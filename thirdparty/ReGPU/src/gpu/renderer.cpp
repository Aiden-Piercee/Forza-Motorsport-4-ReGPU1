// Native D3D12 renderer driven by the command stream (phase 2, first cut).
//
// Runs on the command processor thread. For each draw it translates the
// active Xenos shaders to HLSL (cached), uploads the referenced vertex data
// and an expanded index list into a per-frame upload ring, decodes the
// bound textures (cached by content), and draws into one 1280x720 colour
// target with depth. Resolves and other non-colour EDRAM passes are
// skipped; render-target configuration (EDRAM surfaces, MSAA, tiles) comes
// next.

#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <shared_mutex>
#include <unordered_set>
#include <unordered_map>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/hlsl.h"
#include "gpu/dlss.h"
#include "gpu/fsr3.h"
#include "gpu/recorded_list.h"
#include "fsr1_cs.h"
#include "smaa_cs.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include <DirectXPackedVector.h>
#include "live_switch.h"
#include "settings.h"
#include "gpu/regs.h"
#include "gpu/texture.h"
#include "gpu/ucode.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "debug.h"
#include "log.h"
#include "png.h"
#include "render/presenter.h"
#include "gpu/renderer.h"
#include "gpu/shader_scan.h"
#include "gpu/edram.h"
#include "gpu/gamma_ramp.h"
#include "gpu/edram_targets.h"
#include "gpu/resolve_pack.h"

extern "C" __attribute__((weak)) void RegpuOnLongWait(uint64_t ms, uint32_t hostThreadId); // (the host dumps its threads; see gpu.cpp)

namespace regpu
{
using Microsoft::WRL::ComPtr;


std::atomic<bool> g_gameWidescreen{ false }; // regpu::SetGameWidescreen
uint64_t RendererEditPixelShader(uint64_t hash, const std::string& expr); // the debug server's "ps"

namespace
{
constexpr UINT kWidth = 1280, kHeight = 720;
// Output size (RenderSize: REGPU_RESOLUTION or REGPU_RENDER_SCALE). Everything else
// (registers, scissors, resolve and clear rectangles, shader constants) stays in the game's
// own pixels; only the D3D calls scale. Vertically that is output height / 720. A pass at
// the game's full 1280-pixel width (surface pitch 1280: the main view and its full-screen
// post-processing) scales horizontally to the full output width, wider than 16:9 on an
// ultrawide; every other pass (reflection faces, shadow maps, reduced-size post passes)
// scales uniformly, so square targets stay square. Draws, clears and resolves of a pass
// all read the same pitch, so they agree.
uint32_t OutputWidth()
{
    static const uint32_t w = [] { uint32_t w, h; RenderSize(w, h); return w; }();
    return w;
}
uint32_t OutputHeight()
{
    static const uint32_t h = [] { uint32_t w, h; RenderSize(w, h); return h; }();
    return h;
}
// Upscaling: the 3D scene of a frame renders at this fraction of the output (1 otherwise);
// after the scene is upscaled (UpscaleScene), the rest of the frame renders at full size.
float g_sceneScale = 1.0f;
// The scene of this frame is still being drawn and is upscaled when it is done (any
// upscaler; with a temporal one, also at native scale for anti-aliasing).
bool g_scenePhase = false;
// A temporal upscaler this frame: the scene's main view draws with motion vectors and jitter.
bool g_temporal = false;
// What reaches the temporal upscaler's motion and depth paths, summed and logged once a second
// (BeginTemporalFrame): which of the conditions for a draw's motion vectors and for the scene's depth copy
// holds, and how often.
struct TemporalStats
{
    std::atomic<uint32_t> frames{ 0 }, draws{ 0 }, edram4{ 0 }, widePass{ 0 }, wantMotion{ 0 }, shaderMissing{ 0 }, shaderNotReady{ 0 }, noPipeline{ 0 },
        motion{ 0 }, known{ 0 }, resolves{ 0 }, resolveFullWidth{ 0 }, sceneDone{ 0 }, depthCopies{ 0 }, depthNoTarget{ 0 }, depthEmpty{ 0 }, rearmed{ 0 };
} g_tstat;
// Per vertex shader, what the scene phase's draws did about motion vectors (MotionStatsLog, every 5 s): to see
// which draws get none (and why) and whether the ones that do have constants that change between frames.
struct MotionDrawStat
{
    uint32_t draws = 0, motion = 0, known = 0, changed = 0, changedRegs = 0, firstChanged = 0, vsConstants = 0, vfetch = 0, pitch = 0, colorBase = 0;
    uint64_t psHash = 0; // the last pixel shader seen with it (regpuctl.py ps HASH tint R,G,B paints its draws)
    const char* why = "";
};
std::unordered_map<uint64_t, MotionDrawStat> g_motionDrawStats;
// The last draws' shaders, dumped when the device is lost (RendererLogDred), so DRED's "operation N of the list" can be tied to
// shaders; and the pipeline the command processor is waiting for (a watchdog names it if the wait is long, even if the game never
// gets past it).
struct DrawRecord
{
    uint64_t frame, vs, ps;
    uint32_t pitch, colorBase;
    bool motion;
};
constexpr uint32_t kDrawRing = 64;
DrawRecord g_drawRing[kDrawRing];
std::atomic<uint32_t> g_drawRingNext{ 0 };
std::atomic<int64_t> g_pipelineWaitStartNs{ 0 };
std::atomic<uint64_t> g_pipelineWaitVs{ 0 }, g_pipelineWaitPs{ 0 };
std::atomic<uint32_t> g_drawThreadId{ 0 }; // the host thread that last recorded a draw (sampled by the stall watchdog)
uint32_t g_motionStatFrames = 0, g_motionSampledFrames = 0;
// The constants comparison reads last frame's vertex constants back from the upload ring, which is
// write-combined memory (slow to read): it runs on about one frame in a hundred.
bool g_motionSampleThisFrame = false;
struct Upscale
{
    bool tried = false, ready = false;
    ComPtr<ID3D12RootSignature> root, gammaRoot; // gammaRoot: two SRVs (the image, the display curve)
    ComPtr<ID3D12PipelineState> easu, rcas, depthCopy, gamma, enhance, view;
    ComPtr<ID3D12Resource> dlaaOut; // DLAA output (scene size), copied back into the scene image
    ComPtr<ID3D12Resource> depth; // temporal: the scene's depth as R32 (render size, top left)
    ComPtr<ID3D12Resource> temp; // EASU output (RGBA16F)
    uint32_t tempWidth = 0, tempHeight = 0;
    bool switchedThisFrame = false;
    uint64_t retryFrame = 0; // after a frame with no scene to upscale (menus): native until then
    // Temporal upscalers: the motion target (RG16F, output size; UV offsets to last frame),
    // this frame's jitter (pixels and NDC) and the scene's render size.
    ComPtr<ID3D12Resource> motion;
    ComPtr<ID3D12DescriptorHeap> motionRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE motionRtv{};
    float jitterPx[2] = {}, jitterNdc[2] = {};
    uint32_t renderWidth = 0, renderHeight = 0;
    // The scene phase's draws this frame, the full-width draws after it (last frame's tells whether a real scene
    // follows a first, nearly empty resolve), and the re-arm bookkeeping (SceneResolved).
    uint32_t sceneDraws = 0, postDraws = 0, prevPostDraws = 0, rearms = 0, rearmFailures = 0;
    uint64_t rearmBlockedUntil = 0;
    bool viewValid = false; // this frame's motion and depth were handed to the upscaler (ViewFrontBuffer)
    uint32_t viewWidth = 0, viewHeight = 0; // their render size
    bool reset = true; // no usable history (first frame, after menus)
    // Each draw's vertex constants, kept for the same draw in the next frame (keyed by
    // shaders, vertex and index data and occurrence).
    struct DrawConstants
    {
        uint32_t frame = 0;                    // the temporal frame that last drew it
        D3D12_GPU_VIRTUAL_ADDRESS address[2]{}; // its vertex constants in that frame's ring, by frame parity
    };
    std::unordered_map<uint64_t, DrawConstants> constants;
    std::unordered_map<uint64_t, uint32_t> occurrences;
    uint32_t constantsFrame = 0;
} g_upscale;

// A pass taller than the screen (a 1024x1024 render target, drawn 720 rows at a time) is scaled as a whole
// by kHeight / its height so it fits the surface, and the vertex shader divides by its height.
// It is found by its window scissor (WindowScissor) and recognised by its surface info.
uint32_t g_tallInfo = 0, g_tallHeight = 0, g_tallKey = ~0u;
std::atomic<bool> g_tallSeen{ false }; // REGPU_CAPTURE_TALL: a tall pass was drawn
// A surface wider than the screen (a 2048x2048 composite target, drawn in 256-row
// tiles) is a square pass of its pitch less the tile padding (2080 -> 2048, 1040 -> 1024).
// The MSAA layout each EDRAM surface is drawn in (its last draw that is not a texture-less
// rectangle), by colour base and row width in samples: views of the same EDRAM with another
// MSAA mode (clears) are mapped onto it. Samples per pixel: 1x 1x1, 2x 1x2, 4x 2x2.
std::atomic<uint32_t> g_nativeMsaa[0x1000]; // key << 2 | MSAA mode
uint32_t MsaaAcross(uint32_t msaa) { return msaa == 2 ? 2u : 1u; }
uint32_t MsaaDown(uint32_t msaa) { return msaa ? 2u : 1u; }
uint32_t LayoutKey(uint32_t surface)
{
    return (gpureg::Get(0x2001 /* RB_COLOR_INFO */) & 0xFFF) | (surface & 0x3FFF) * MsaaAcross((surface >> 16) & 3) << 12;
}
std::atomic<uint32_t>& NativeSlot(uint32_t key) { return g_nativeMsaa[(key ^ key >> 12) & 0xFFF]; }
// The view (RB_SURFACE_INFO) each host depth surface is drawn in, by EDRAM depth base: set by
// every depth or stencil draw placed on it directly; 0 before the first.
std::atomic<uint32_t> g_depthOwner[0x1000];
// EDRAM tiles hold 80x16 samples (32 bits each) and follow each other along the surface's rows:
// the same tile index is a different place in a view of another row width.
constexpr uint32_t kEdramTileW = 80, kEdramTileH = 16;
// A depth-only draw through a view of another row width than its host depth surface: drawn
// once per run of EDRAM tiles that stays together in both layouts, moved to where those tiles
// lie on the host surface.
struct EdramAlias
{
    bool on = false;
    uint32_t viewSurface = 0, hostSurface = 0;
};
// The runs of EDRAM tiles a view rectangle (view pixels) covers through an alias: fn(x0, y0, x1, y1, dx,
// dy) per run, in view samples, (dx, dy) being where the run's tiles lie on the host surface relative
// to the view (samples). A run stays on one tile row of the view and of the host layout.
template <typename Fn>
void ForEachAliasRun(const EdramAlias& alias, const D3D12_RECT& r, Fn&& fn)
{
    const uint32_t viewMsaa = (alias.viewSurface >> 16) & 3, hostMsaa = (alias.hostSurface >> 16) & 3;
    const int32_t va = int32_t(MsaaAcross(viewMsaa)), vd = int32_t(MsaaDown(viewMsaa)), ha = int32_t(MsaaAcross(hostMsaa));
    const int32_t viewTiles = int32_t((alias.viewSurface & 0x3FFF) * va / kEdramTileW);
    const int32_t hostTiles = int32_t((alias.hostSurface & 0x3FFF) * ha / kEdramTileW);
    const int32_t tw = int32_t(kEdramTileW), th = int32_t(kEdramTileH);
    const int32_t left = r.left * va, top = r.top * vd, right = r.right * va, bottom = r.bottom * vd;
    if (viewTiles <= 0 || hostTiles <= 0)
        return;
    for (int32_t row = top / th; row * th < bottom; row++)
        for (int32_t col = left / tw; col * tw < right && col < viewTiles;)
        {
            const int32_t tile = row * viewTiles + col, hostRow = tile / hostTiles, hostCol = tile % hostTiles;
            const int32_t run = std::min(viewTiles - col, hostTiles - hostCol); // tiles that stay together
            const int32_t x0 = std::max(left, col * tw), x1 = std::min(right, (col + run) * tw);
            const int32_t y0 = std::max(top, row * th), y1 = std::min(bottom, (row + 1) * th);
            const int32_t dx = (hostCol - col) * tw, dy = (hostRow - row) * th;
            col += run;
            if (x1 > x0 && y1 > y0)
                fn(x0, y0, x1, y1, dx, dy);
        }
}
std::atomic<uint32_t> g_nativeMsaaGeneration{ 0 }; // bumped when an entry changes (BigPassSize's memo)
void SetNativeMsaa(uint32_t key, uint32_t msaa)
{
    if (NativeSlot(key).exchange(key << 2 | msaa, std::memory_order_relaxed) != (key << 2 | msaa))
        g_nativeMsaaGeneration.fetch_add(1, std::memory_order_relaxed);
}
// The surface's MSAA layout when the current view is another one of it (same EDRAM base and row width in
// samples, another MSAA mode), else -1. That includes an MSAA view of a 1x surface: a title clears a 1x
// 256x256 target mostly through a 4x view (120x128 pixels cover 240x256 samples, the 1x pixels), plus a
// 16-pixel 1x strip; mapped as 1x pixels only a 120x128 corner was cleared and the rest kept the previous
// pass's image. REGPU_NO_MSAA_VIEW_OF_1X=1: only views of MSAA surfaces are mapped, as before (A/B).
int ForeignNativeMsaa()
{
    static LiveSwitch off("REGPU_NO_FOREIGN_MSAA"); // A/B
    static LiveSwitch noViewOf1x("REGPU_NO_MSAA_VIEW_OF_1X");
    if (off)
        return -1;
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */), key = LayoutKey(surface);
    const uint32_t native = NativeSlot(key).load(std::memory_order_relaxed);
    return native >> 2 == key && (native & 3) != ((surface >> 16) & 3) && ((native & 3) != 0 || !noViewOf1x) ? int(native & 3) : -1;
}

uint32_t BigPassSizeUncached()
{
    uint32_t pitch = gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF;
    // A view of a big MSAA surface in another MSAA mode belongs to the same pass.
    if (const int native = ForeignNativeMsaa(); native >= 0)
        pitch = pitch * MsaaAcross((gpureg::Get(0x2000) >> 16) & 3) / MsaaAcross(uint32_t(native));
    static LiveSwitch off("REGPU_NO_BIG_PASS");
    return !off && pitch > kWidth ? std::min<uint32_t>(2048, pitch * 64 / 65) : 0;
}
// Asked several times per draw (PassWidth, PassHeight, ScaleX, ScaleY): the answer only changes
// with the surface registers or the layout table.
uint32_t BigPassSize()
{
    struct Memo { uint32_t surface = ~0u, color = ~0u, generation = ~0u, result = 0; };
    thread_local Memo memo;
    const uint32_t surface = gpureg::Get(0x2000), color = gpureg::Get(0x2001) & 0xFFF, generation = g_nativeMsaaGeneration.load(std::memory_order_relaxed);
    if (surface != memo.surface || color != memo.color || generation != memo.generation)
        memo = { surface, color, generation, BigPassSizeUncached() };
    return memo.result;
}
uint32_t PassWidth() { const uint32_t big = BigPassSize(); return big ? big : kWidth; }
// Another MSAA view of the tall pass's EDRAM: a title can clear the stencil of its 1x
// 1024x1024 target through a 4x view (480x512, pitch 520) while a layer is being moved.
bool TallView()
{
    static LiveSwitch off("REGPU_NO_TALL_VIEW"); // A/B
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    return !off && g_tallHeight && surface != g_tallInfo && ((surface ^ g_tallInfo) >> 16 & 3) && LayoutKey(surface) == g_tallKey;
}
uint32_t PassHeight()
{
    if (const uint32_t big = BigPassSize())
        return big;
    const uint32_t surface = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    return g_tallHeight && (surface == g_tallInfo || TallView()) ? g_tallHeight : kHeight;
}
float PassScale()
{
    // (Asked for every scaled coordinate: memoised on what it depends on.)
    struct Memo { uint32_t surface = ~0u, color = ~0u, generation = ~0u, tallInfo = ~0u, tallHeight = ~0u; float result = 1; };
    thread_local Memo memo;
    const uint32_t surface = gpureg::Get(0x2000), color = gpureg::Get(0x2001) & 0xFFF, generation = g_nativeMsaaGeneration.load(std::memory_order_relaxed);
    if (surface != memo.surface || color != memo.color || generation != memo.generation || g_tallInfo != memo.tallInfo || g_tallHeight != memo.tallHeight)
        memo = { surface, color, generation, g_tallInfo, g_tallHeight,
            std::min(float(kWidth) / float(PassWidth()), float(kHeight) / float(PassHeight())) };
    return memo.result;
}
float ScaleY() { return float(OutputHeight()) / float(kHeight) * g_sceneScale * PassScale(); }
float ScaleWide() { return float(OutputWidth()) / float(kWidth) * g_sceneScale * PassScale(); }
bool WidePass() { return (gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF) == kWidth; }
float ScaleX() { return WidePass() ? ScaleWide() : ScaleY(); }
// Row width of an EDRAM view in samples (RB_SURFACE_INFO: pitch in pixels, MSAA in bits 16-17).
uint32_t RowSamples(uint32_t info) { return (info & 0x3FFF) * MsaaAcross((info >> 16) & 3); }
// A depth access through view `view` of a depth base whose host surface is laid out as `host`
// (another row width) is placed tile run by tile run (EdramAlias) unless this names why it can't.
const char* ForeignDepthUnplaceable(uint32_t view, uint32_t host)
{
    static LiveSwitch noAlias("REGPU_NO_EDRAM_ALIAS");
    return noAlias ? "aliasing off (REGPU_NO_EDRAM_ALIAS)"
        : RowSamples(view) % kEdramTileW || RowSamples(host) % kEdramTileW ? "row width not whole tiles"
        : (view & 0x3FFF) > kWidth || (host & 0x3FFF) > kWidth || host == g_tallInfo || view == g_tallInfo ? "a scaled (big or tall) pass"
        : ScaleWide() != ScaleY() && ((view & 0x3FFF) == kWidth || (host & 0x3FFF) == kWidth) ? "a wide pass"
        : nullptr;
}
LONG HostX(LONG v) { return LONG(std::lround(double(v) * ScaleX())); }
LONG HostY(LONG v) { return LONG(std::lround(double(v) * ScaleY())); }
D3D12_RECT ToHost(const D3D12_RECT& r)
{
    return { HostX(r.left), HostY(r.top), HostX(r.right), HostY(r.bottom) };
}

// Wider than 16:9: the output is `wide` times as wide as 16:9 for its height. The view
// extends horizontally by `extent` (1 + (wide - 1) x REGPU_WIDE_FOV%): 100 is full Hor+ (the
// most side view, but rectilinear stretch at the edges grows with a wide camera); 0
// keeps the 16:9 horizontal view and zooms in vertically instead.
float OutputWide() { return float(OutputWidth()) * kHeight / (float(OutputHeight()) * kWidth); }
float ViewExtent()
{
    static const float widePercent = [] {
        const char* v = getenv("REGPU_WIDE_FOV");
        return v ? std::clamp(float(atof(v)), 0.0f, 100.0f) : 100.0f;
    }();
    return 1.0f + (std::max(OutputWide(), 1.0f) - 1.0f) * widePercent / 100.0f;
}
constexpr UINT kFrames = 3;
// The per-frame upload ring. Without guest memory mapped for the GPU (drivers without
// ID3D12Device13, e.g. older Windows 10 builds) every vertex buffer is copied into it: the
// menus alone then need well over 64 MB a frame, and draws past the end were dropped (black
// bands and missing letters). REGPU_RING_MB overrides.
UINT64 kRingSize = 64ull << 20;
UINT64 g_ringPeak = 0;
constexpr UINT kDescriptorsPerFrame = 8192; // per frame: ring + guest memory views, then resolve-blit views
constexpr UINT kBindless = 1u << 16;      // permanent texture descriptors (0 null 2D, 1 null cube, 2 null 3D)
constexpr UINT kBindlessBase = kDescriptorsPerFrame * 3;
// The EDRAM colour target: 32-bit floats. Xenos 16_16 and 16_16_16_16 targets are 16-bit
// fixed point over -32..32 (a step of 2^-10) and 32_FLOAT / 32_32_FLOAT targets full floats;
// half floats keep 1/16 of that fixed-point precision near 32 and overflow past 65504. A
// variance shadow map (depth and depth squared in a 16_16 target, written through an
// exponent bias) needs every bit: its variance, E[d^2] - E[d]^2, is lost in half-float
// rounding and the receivers read as lit (no shadow, a checkered road). Resolves into
// non-8888 destinations use the same format. REGPU_FP16_SURFACES=1 (read at start) keeps
// half floats, for A/B tests and memory.
const DXGI_FORMAT g_colorFormat = getenv("REGPU_FP16_SURFACES") ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
// REGPU_EXACT_FP16_SURFACES=1 (read at start, with 32-bit surfaces): a surface whose EDRAM base is
// first drawn as 8_8_8_8 (and _GAMMA), 2_10_10_10 or 2_10_10_10_FLOAT (7e3, and their _AS_ views)
// is kept in half floats: half the memory and bandwidth. Every value those formats store comes back
// unchanged: an 8- or 10-bit fixed-point step near 1 is 2^-8 or 2^-10, half floats resolve 2^-11
// there, and 7e3 (7-bit mantissa, [0, 31.875]) is a subset of half floats. The draws' output is
// clamped to the target's range after the exponent bias (info2), so nothing overflows. A base later
// bound with any other format (16_16 or 16_16_16_16 fixed point, a variance shadow map, 32-bit
// floats) is moved to 32-bit floats for good, its pixels copied. Single-sample surfaces on the
// serial recording path only.
constexpr DXGI_FORMAT kHalfColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
bool ExactInHalf(uint32_t colorFormat)
{
    const uint32_t f = edram::StorageFormat(colorFormat);
    return f == edram::k8888 || f == edram::k8888Gamma || f == edram::k2101010 || f == edram::k2101010Float;
}
uint32_t g_halfSurfaces = 0; // colour surfaces in half floats now
// Bytes per texel of a host colour or resolve texture.
UINT HostBpp(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}
bool HostColorTexture(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == g_colorFormat;
}
constexpr UINT kMaxSurfaces = 64; // EDRAM bases with host surfaces
// Samples per pixel of the scene surfaces (anti-aliasing, Settings::msaa). The blits that
// read them use multisampled shaders when this is above 1.
uint32_t g_msaa = 1;
constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
constexpr uint32_t kMaxFetchZeroBytes = 4u << 20; // a vertex copy zero-padded past its fetch constant's size (see the draw's vf loop)
constexpr uint32_t kDirectFetchBytes = 16 * 1024; // buffers that change (UI quads, particles) are all under 3 KB (REGPU_TRACE_MIDSIZE); REGPU_DIRECT_FETCH_KB overrides

// Registers.
constexpr uint32_t RB_STENCILREFMASK = 0x210D;
constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_ALPHA_REF = 0x210E, RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111, PA_CL_VPORT_YOFFSET = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205, PA_SC_VIZ_QUERY = 0x2293;
constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B;
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180, SQ_CONTEXT_MISC = 0x2181, PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E;

void LogDebugMessages();

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LogDebugMessages();
        Fatal("renderer: %s failed (%08lX)", what, hr);
    }
}

float RegFloat(uint32_t index)
{
    const uint32_t v = gpureg::Get(index);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

struct CompiledShader
{
    enum State { Pending, Ready, Failed };
    std::atomic<int> state{ Pending }; // set by the compile worker
    uint64_t hash = 0, cacheKey = 0;
    bool pixel = false;
    std::vector<uint32_t> ucodeBE; // saved to the shader cache once a pipeline uses it
    ComPtr<ID3DBlob> code;
    TranslatedShader info;
    const CompiledShader* hlslSource = nullptr; // canonical variants: the shader whose code (and HLSL) this reuses
    mutable const CompiledShader* motion = nullptr; // its motion-vector variant, once looked up
    uint32_t tag = 0; // the game's mark for this shader (Host::rewriteShader), passed to Host::onDraw
    bool IsReady() const { return state.load(std::memory_order_acquire) == Ready; }
};

struct CachedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint64_t contentHash = 0;
    uint64_t checkedFrame = ~0ull; // content already validated in this frame
    uint32_t unchanged = 0;        // checks in a row that found the same content
    uint32_t changes = 0;          // times the content changed after the first upload
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // cubes: matches their faces
    bool native = false; // block-compressed on the GPU: the shader applies the fetch swizzle
    uint64_t faceSerials = 0; // cubes the title rendered: the resolves of the faces last copied in
    bool allFaces = false;    // (all six faces rendered, last time it was put together)
    bool everLive = false;    // cubes: the title rendered (some of) its faces at least once
    uint64_t lastLiveFrame = 0; // cubes: the renderer frame of the last put-together with rendered faces
};

// A resolve destination backed by a host texture: fetches of this guest
// address read the copy of the render target instead of guest memory.
struct ResolvedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;          // game pixels
    uint32_t hostWidth = 0, hostHeight = 0;  // the texture (see ScaleX/ScaleY)
    uint64_t frame = ~0ull;                  // renderer frame of the last resolve into it
    uint64_t serial = 0;                     // stamp of the last resolve into it (g_r.resolveSerial)
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // R32_FLOAT for depth resolves
    bool swapped = false; // resolved with the copy swap bit: guest memory has red and blue exchanged
    // Guest memory under the image when it was resolved (our resolves don't write it): if the title
    // later puts something else there (a CPU-made texture), the resolve is stale (see ResolvedFor).
    uint64_t memHash = 0, memChecked = 0;
    bool stale = false;
    bool ownWrite = false; // memHash is of our own write-back of the image (WriteResolveToGuest, deferred write-back)
    uint32_t guestBpp = 4; // bytes per pixel of the guest image: the hash covers only its memory
    uint32_t guestFormat = 6; // RB_COPY_DEST_INFO format of the last colour resolve (6: 8_8_8_8, 7: 2_10_10_10)
    // The title resolves differently sized images (bloom chain) to one address within a
    // frame: keep earlier sizes here and swap them in rather than recreating them.
    std::vector<ResolvedTexture> spares;
};
const ResolvedTexture* CurrentResolve(uint32_t address); // the resolve at an address unless stale (defined below)

// REGPU_DUMP_RESOLVES readback of one resolved texture.
struct ResolveDump
{
    ComPtr<ID3D12Resource> buffer;
    uint32_t base = 0;
    int variant = 0;
    UINT width = 0, height = 0, pitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float minDepth = 0, maxDepth = 0;
    uint64_t midCount = 0;
};

struct FrameResources
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> ring;
    uint8_t* ringCpu = nullptr;
    UINT64 ringSize = 0; // bytes in `ring` (kRingSize when it was made; a grown ring reaches the next frames)
    std::atomic<UINT64> ringUsed{ 0 }; // draw workers allocate concurrently
    std::atomic<uint32_t> ringRefused{ 0 }; // allocations the full ring refused this frame (a draw skipped or a texture missing each)
    // Guest ranges already uploaded this frame: (address << 32 | bytes) -> (offset, sample hash).
    std::unordered_map<uint64_t, std::pair<UINT64, uint64_t>> uploads;
    UINT descriptorBase = 0;
    UINT descriptorsUsed = 0;
    UINT64 fence = 0;
    std::vector<ComPtr<ID3D12Resource>> garbage; // released once this frame's work completes
    bool timed = false; // REGPU_PERF: GPU timestamps were written for this frame
    // Extra command lists of this frame (parallel draw pieces, main list continuations).
    std::vector<std::pair<ComPtr<ID3D12CommandAllocator>, ComPtr<ID3D12GraphicsCommandList>>> pool;
    size_t poolUsed = 0;
};

// The pipeline this thread is handing to the driver compiler, named in the crash report if the driver faults.
struct CompilingNow { uint64_t vs, ps; bool active; };
thread_local CompilingNow g_compiling{};

// Pipelines whose compile crashed the driver (AMD's amdxc64 faults on a few): written by the crash report to
// regpu-bad-pipelines.txt and refused on the next start, so a draw using one is dropped instead of killing the game.
const char* const kBadPipelinesFile = "regpu-bad-pipelines.txt";
bool PipelineIsPoisoned(uint64_t vs, uint64_t ps)
{
    static const std::set<std::pair<uint64_t, uint64_t>> bad = [] {
        std::set<std::pair<uint64_t, uint64_t>> out;
        if (FILE* f = fopen(kBadPipelinesFile, "r"))
        {
            unsigned long long v, p;
            while (fscanf(f, "%llx %llx", &v, &p) == 2)
                out.insert({ v, p });
            fclose(f);
        }
        return out;
    }();
    return bad.count({ vs, ps }) != 0;
}

struct PipelineKey
{
    uint64_t vs, ps;
    uint32_t blend, depth, stencilMaskMode, flags;
    bool operator==(const PipelineKey&) const = default;
};
struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& k) const
    {
        uint64_t h = k.vs * 0x9E3779B97F4A7C15ull ^ k.ps;
        h ^= (uint64_t(k.blend) << 32 | k.depth) * 0xC2B2AE3D27D4EB4Full;
        h ^= (uint64_t(k.stencilMaskMode) << 32 | k.flags) * 0x165667B19E3779F9ull;
        return size_t(h ^ (h >> 29));
    }
};

struct BoundTexture
{
    ID3D12Resource* texture;
    DXGI_FORMAT format;
    bool cube;
};

// A pipeline state being (or already) built. The driver compiles the translated
// shaders to GPU code inside CreateGraphicsPipelineState, which takes 40-700 ms for
// the larger ones: that is done on worker threads and draws wait until it is ready.
struct PipelineEntry
{
    ComPtr<ID3D12PipelineState> pso;
    std::atomic<int> state{ 0 }; // 0 building, 1 ready, 2 failed
    // The build, run once by whoever claims it first: a pool thread, or a draw that must wait for it
    // (WaitForPipeline), so that draw doesn't wait behind every build queued before it.
    std::function<void()> build;
    std::atomic<bool> claimed{ false };
    void Build()
    {
        if (!claimed.exchange(true, std::memory_order_acq_rel))
            build();
    }
};

struct Renderer
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12GraphicsCommandList> realList;
    RecordedList recorder; // the main thread's command list (see recorded_list.h)
    ComPtr<ID3D12Heap> physicalHeap;     // the guest's physical memory section, mapped for the GPU
    ComPtr<ID3D12Resource> physical;     // 512 MB raw buffer over it (null if unsupported)
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 nextFence = 1;
    FrameResources frames[kFrames];
    UINT frameIndex = 0;
    bool listOpen = false;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12DescriptorHeap> srvHeap;     // shader visible
    ComPtr<ID3D12DescriptorHeap> stagingHeap; // CPU: null texture SRV
    UINT srvStride = 0;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    ComPtr<ID3D12DescriptorHeap> blitRtvHeap;             // one RTV, rewritten per depth resolve
    ComPtr<ID3D12RootSignature> blitRootSignature;
    ComPtr<ID3D12RootSignature> motionRootSignature; // root signature + b2 (motion-vector pipelines)
    ComPtr<ID3D12PipelineState> depthBlit;                // depth buffer -> R32_FLOAT
    ComPtr<ID3D12PipelineState> colorBlit8, colorBlit16;  // colour -> 8888 / RGBA16F, scaled
    ComPtr<ID3D12PipelineState> depthBlitMs, colorBlitMs8, colorBlitMs16; // the same from multisampled surfaces
    // EDRAM surfaces: one host colour and one host depth target per EDRAM base
    // (RB_COLOR_INFO / RB_DEPTH_INFO bits 0-11), so passes that the title
    // places at different EDRAM offsets do not overwrite each other.
    struct Surface
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_CPU_DESCRIPTOR_HANDLE view{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // colour: g_colorFormat or kHalfColor
    };
    std::unordered_map<uint32_t, Surface> colorSurfaces, depthSurfaces;
    ComPtr<ID3D12Resource> readback;

    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<PipelineKey, std::shared_ptr<PipelineEntry>, PipelineKeyHash> pipelines;
    // Keyed by address, format, width and height (TextureKey): looked up for every bound
    // texture of every draw.
    std::unordered_map<uint64_t, CachedTexture> textures;
    std::unordered_map<uint64_t, CachedTexture> cubes;
    std::unordered_map<uint64_t, CachedTexture> volumes; // 3D textures
    std::unordered_map<uint64_t, CachedTexture> mipChains; // resolved textures with their mips, by base and shape
    uint64_t resolveSerial = 0; // counts resolves: what a mip chain was assembled from
    // Bindless texture descriptors.
    struct BindlessEntry
    {
        UINT index;
        ComPtr<ID3D12Resource> resource; // kept alive while the descriptor may be used
        uint64_t lastFrame;
    };
    std::unordered_map<uint64_t, BindlessEntry> bindless; // by resource pointer, format, cube
    std::vector<UINT> bindlessFree;
    UINT bindlessNext = 3;
    std::unordered_map<uint32_t, ResolvedTexture> resolved; // by physical address

    uint64_t frame = 0;
    std::atomic<uint32_t> drawsThisFrame{ 0 }, skippedThisFrame{ 0 };
    bool ok = false;
};

Renderer g_r;

// Shared caches used by draws on several threads (see DrawContext).
std::recursive_mutex g_shaderMutex;  // shaders, pipelines, the compiler's maps
std::recursive_mutex g_textureMutex; // textures, cubes, mip chains, bindless, texture memory, index arena
std::mutex g_surfaceMutex;           // EDRAM surfaces
// Parallel segments: texture uploads and copies a worker makes go to this list, executed
// before the segment's draws (another worker's earlier draws may use the same texture).
RecordedList* g_prepList = nullptr;

// What a thread recording draws keeps for itself: its command list, the bound surfaces and
// shaders. The command processor thread uses the main context (the frame's list); draw
// workers each have their own (see the parallel draw segments in gpu.cpp).
struct DrawContext
{
    RecordedList* list = nullptr;
    uint32_t colorBase = ~0u, depthBase = ~0u;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
    // The bound surfaces.
    ComPtr<ID3D12Resource> color, depth;
    DXGI_FORMAT colorFormat = g_colorFormat;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // Active shaders: the last microcode loaded per stage.
    std::vector<uint32_t> vsCode, psCode;
    uint64_t vsHash = 0, psHash = 0;
    bool worker = false; // a draw worker: frame-level work (begin, resolves, swaps) is not its job
};
DrawContext g_mainContext;
thread_local DrawContext* t_context = nullptr;
inline DrawContext& C() { return t_context ? *t_context : g_mainContext; }

// Holds the texture lock and, on a draw worker, records into the segment's prep list.
struct TextureScope
{
    std::lock_guard<std::recursive_mutex> lock{ g_textureMutex };
    RecordedList* saved = nullptr;
    TextureScope()
    {
        if (C().worker && g_prepList)
        {
            saved = C().list;
            C().list = g_prepList;
        }
    }
    ~TextureScope()
    {
        if (saved)
            C().list = saved;
    }
};

ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state);


void LogDebugMessages()
{
    if (g_r.device)
        Log("renderer: device removed reason %08lX", g_r.device->GetDeviceRemovedReason());
    RendererLogDred();
    ComPtr<ID3D12InfoQueue> queue;
    if (!g_r.device || FAILED(g_r.device->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    const UINT64 n = queue->GetNumStoredMessages();
    for (UINT64 i = n > 20 ? n - 20 : 0; i < n; i++)
    {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        queue->GetMessage(i, message, &size);
        Log("d3d12: %s", message->pDescription);
    }
    queue->ClearStoredMessages();
}
double g_pipelineMs = 0; // REGPU_PERF: time spent creating pipeline states this second
uint32_t g_pipelinesCreated = 0;
uint32_t g_skippedPipeline = 0;
const char* g_waitWhat = "?";
// The current draw samples the full-screen image the title resolved this frame (the
// post-processing composite): its flat geometry is a screen layer, not HUD (see vwide.w).
thread_local bool g_drawSamplesResolve = false;
// REGPU_PERF: GPU time per frame from timestamps at the start and end of each frame's list.
ComPtr<ID3D12QueryHeap> g_timestamps;

// REGPU_GPU_PASSES=1: GPU time per pass (between render target switches and resolves),
// summed by pass and logged every 2 s, to find where GPU time goes.
const bool g_gpuPasses = getenv("REGPU_GPU_PASSES") != nullptr;
constexpr UINT kPassMarks = 1024;
ComPtr<ID3D12QueryHeap> g_passQueries;
ComPtr<ID3D12Resource> g_passReadback;
struct PassMarkInfo
{
    char label[48];
    uint32_t draws; // draws in the pass this mark starts (filled at the next mark)
};
std::vector<PassMarkInfo> g_passMarks[3];
uint32_t g_passDraws = 0;

// Occlusion queries (EVENT_WRITE_ZPD: the sun flare's visibility). Each guest query becomes a
// D3D12 one; the guest gets the result of the same query from the last frame that has
// completed, so the command processor never waits for the GPU. A query is known by its
// sample-count block (RB_SAMPLE_COUNT_ADDR), not by its order in the frame: the number of
// queries a frame issues changes as lights come into view, and matched by order every later
// answer landed on another query's count (the sun flare flared and vanished while driving).
// REGPU_OCCLUSION_BY_ORDER=1: matched by order, as before (A/B).
constexpr UINT kOcclusionPerFrame = 256;
ComPtr<ID3D12QueryHeap> g_occlusionHeap;
ComPtr<ID3D12Resource> g_occlusionReadback;
std::vector<std::pair<uint32_t, UINT>> g_occlusionPending[3]; // per frame slot: (key, query index)
std::unordered_map<uint32_t, int64_t> g_occlusionResult;     // by key: host samples of its last completed frame
uint64_t g_occlusionFrame = ~0ull;
UINT g_occlusionCount = 0, g_occlusionIndex = 0;
uint32_t g_occlusionKey = 0;
bool g_occlusionOpen = false;
bool OcclusionByOrder()
{
    static LiveSwitch byOrder("REGPU_OCCLUSION_BY_ORDER");
    return byOrder;
}
std::map<std::string, std::pair<double, uint64_t>> g_passTotals; // ms, draws
uint64_t g_passFrames = 0;
ComPtr<ID3D12Resource> g_timestampReadback;
double g_gpuFrameMs = 0, g_gpuFrameMaxMs = 0;
uint32_t g_gpuFrames = 0;
uint32_t g_waitBlend = 0;
std::map<std::string, uint64_t> g_created; // REGPU_PERF: committed resources created, by kind
// REGPU_PERF: what happened during the current frame, for the slow-frame log.
struct FrameEvents
{
    double waitMs = 0;          // CP thread waiting on shader/pipeline builds
    uint32_t waits = 0, pipelines = 0, created = 0;
    double section[8] = {};     // draw path sections (see Section), ms
    double worstDrawMs = 0;     // the single slowest draw
} g_frameEvents;
void CountCreate(const char* what)
{
    g_created[what]++;
    g_frameEvents.created++;
}

const bool g_dumpHlsl = getenv("REGPU_DUMP_HLSL") != nullptr;

// Texture memory: textures are placed in large heaps instead of each getting a committed
// resource (its own heap). Creating a committed resource costs the driver milliseconds,
// which showed as hitches whenever new textures streamed in on the road ahead. Blocks are
// 64 KB multiples, reused by size once the GPU is done with the texture that held them.
struct TexturePool
{
    static constexpr uint64_t kHeapBytes = 128ull << 20;
    struct Block
    {
        ID3D12Heap* heap = nullptr;
        uint64_t offset = 0, size = 0;
    };
    std::vector<ComPtr<ID3D12Heap>> heaps;
    uint64_t used = kHeapBytes; // in heaps.back()
    std::unordered_map<uint64_t, std::vector<Block>> free; // by size
    std::unordered_map<ID3D12Resource*, Block> live;
    std::deque<std::pair<uint64_t, Block>> retired; // frame retired, block
} g_texturePool;

// A texture in the pool (or committed, if it does not fit a heap). Starts in `state`.
ComPtr<ID3D12Resource> CreateTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, const char* kind)
{
    static LiveSwitch committed("REGPU_COMMITTED_TEXTURES"); // A/B
    TexturePool& pool = g_texturePool;
    const D3D12_RESOURCE_ALLOCATION_INFO info = g_r.device->GetResourceAllocationInfo(0, 1, &desc);
    const uint64_t size = (info.SizeInBytes + 0xFFFF) & ~0xFFFFull;
    ComPtr<ID3D12Resource> r;
    if (!committed && size <= TexturePool::kHeapBytes && info.Alignment <= 0x10000)
    {
        TexturePool::Block block;
        auto& list = pool.free[size];
        if (!list.empty())
        {
            block = list.back();
            list.pop_back();
        }
        else
        {
            if (pool.used + size > TexturePool::kHeapBytes)
            {
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = TexturePool::kHeapBytes;
                hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                hd.Alignment = 0x10000;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                ComPtr<ID3D12Heap> heap;
                Check(g_r.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "CreateHeap(textures)");
                CountCreate("texture heap");
                pool.heaps.push_back(heap);
                pool.used = 0;
            }
            block = { pool.heaps.back().Get(), pool.used, size };
            pool.used += size;
        }
        if (SUCCEEDED(g_r.device->CreatePlacedResource(block.heap, block.offset, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        {
            pool.live[r.Get()] = block;
            return r;
        }
        pool.free[size].push_back(block);
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
              IID_PPV_ARGS(&r)),
        kind);
    CountCreate(kind);
    return r;
}

FrameResources& Frame();

// REGPU_GARBAGE_DELAY=<n> (A/B, for a GPU hang that needs the GPU frames behind): everything retired
// stays alive n frames longer than the frame slots require. Each part can be left out:
// REGPU_DELAY_NO_GARBAGE (resources in the frame garbage), REGPU_DELAY_NO_TEXBLOCKS (texture pool
// blocks), REGPU_DELAY_NO_BINDLESS (bindless descriptor slots).
uint64_t RetireDelay(const char* without)
{
    static const uint64_t n = getenv("REGPU_GARBAGE_DELAY") ? uint64_t(std::max(0, atoi(getenv("REGPU_GARBAGE_DELAY")))) : 0;
    return n && !getenv(without) ? n : 0;
}

// Hands a texture to the frame's garbage; its pool block is reused once no frame in flight
// can still read it.
void RetireTexture(ComPtr<ID3D12Resource>&& r)
{
    if (!r)
        return;
    if (auto it = g_texturePool.live.find(r.Get()); it != g_texturePool.live.end())
    {
        g_texturePool.retired.push_back({ g_r.frame, it->second });
        g_texturePool.live.erase(it);
    }
    Frame().garbage.push_back(std::move(r));
}

void ReclaimTextureBlocks()
{
    auto& retired = g_texturePool.retired;
    static const uint64_t extra = RetireDelay("REGPU_DELAY_NO_TEXBLOCKS");
    while (!retired.empty() && retired.front().first + kFrames + 2 + extra < g_r.frame)
    {
        g_texturePool.free[retired.front().second.size].push_back(retired.front().second);
        retired.pop_front();
    }
}

// Bumped by every resolve: the per-slot texture bindings below are only reused while no
// resolve has changed what an address maps to.
uint64_t g_resolveEpoch = 0;

// The constants a texture fetch slot produced for the previous draw: draws usually bind
// the same fetch constants as their neighbours, and a repeat within the frame (same fetch,
// no resolve since) gives the same binding.
struct SlotBinding
{
    uint32_t fetch[6] = {};
    uint64_t frame = ~0ull, epoch = ~0ull;
    uint32_t tinfo[4] = {};
    float tbias[4] = {};
    bool samplesResolve = false;
};
// A cache valid for one frame: entries carry the frame they were made in instead of the map
// being cleared each frame (re-inserting freed nodes cost a heap allocation per entry).
template <class V>
struct FrameMap
{
    struct Entry
    {
        uint64_t frame;
        V value;
    };
    std::unordered_map<uint64_t, Entry> map;
    V* Find(uint64_t key, uint64_t frame)
    {
        auto it = map.find(key);
        return it != map.end() && it->second.frame == frame ? &it->second.value : nullptr;
    }
    void Put(uint64_t key, uint64_t frame, const V& value)
    {
        if (map.size() > (1u << 15)) // (stale entries of old frames)
            map.clear();
        map[key] = { frame, value };
    }
};

// Bindings by fetch constant: a texture bound by many draws (in any slot) is looked up once
// per frame and resolve epoch (textures are still checked once per frame for changes).
// (A direct-mapped table: an entry holds the fetch words it was made for, so a collision only
// recomputes. The hash map it replaced was 2-3% of the command processor.)
constexpr uint32_t kBindingSlots = 8192;
thread_local std::unique_ptr<SlotBinding[]> g_bindings;
thread_local uint64_t g_bindingsFrame = ~0ull, g_bindingsEpoch = ~0ull;

// The frame to trace (draws and resolves logged, the image and resolves written out):
// REGPU_TRACE_DRAWS=<frame>, or the one after F12 is pressed in the game window.
std::atomic<uint64_t> g_hotkeyFrame{ 0 };
uint64_t TraceFrame()
{
    static const uint64_t env = getenv("REGPU_TRACE_DRAWS") ? strtoull(getenv("REGPU_TRACE_DRAWS"), nullptr, 10) : 0;
    const uint64_t hot = g_hotkeyFrame.load(std::memory_order_relaxed);
    return hot ? hot : env;
}

// Why GetTexture last found nothing to bind (the draw then samples the white fallback): shown in
// the F12 capture (out/capture/textures.txt).
thread_local char g_textureFailure[96] = "";
void NoteTextureFailure(const char* why, const TextureFetch& fetch)
{
    snprintf(g_textureFailure, sizeof(g_textureFailure), "%s (type %u dim %u format %u)", why, fetch.Type(), fetch.Dimension(), fetch.Format());
}

// Per-draw timing (several clock reads per draw) only for REGPU_PERF.
const bool g_perfTiming = getenv("REGPU_PERF") != nullptr;
double g_textureHashSeconds = 0; // REGPU_PERF: texture change checks this second (hashing guest memory), and how many
uint64_t g_textureChecks = 0;

// The last draw state set on the command list: draws skip setting what has not changed
// (each D3D12 call costs driver time, and most draws share their neighbours' state).
// Invalidated wherever the list is reset or the root signature changes.
struct DrawState
{
    D3D12_VIEWPORT viewport{ -1, -1, -1, -1, -1, -1 };
    D3D12_RECT scissor{ -1, -1, -1, -1 };
    uint32_t stencilRef = ~0u;
    float blendFactor[4] = { -1, -1, -1, -1 };
    ID3D12PipelineState* pso = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE table2{}, table3{};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool motionTargets = false; // the motion target is bound as the second render target
    bool motionRoot = false;    // the motion root signature is set
    D3D12_INDEX_BUFFER_VIEW indexBuffer{};
    void Invalidate() { *this = DrawState{}; }
};
thread_local DrawState g_drawState;

// Texture cache key: address (4 KB aligned), format (6 bits), width and height (13 bits each).
uint64_t TextureKey(uint32_t address, uint32_t format, uint32_t w, uint32_t h)
{
    return uint64_t(address >> 12) << 32 | uint64_t(format & 0x3F) << 26 | uint64_t((w - 1) & 0x1FFF) << 13 | ((h - 1) & 0x1FFF);
}

uint64_t Fnv(const void* data, size_t bytes, uint64_t h = 0xCBF29CE484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// FullHash with AVX2: 64 bytes a step in two 256-bit accumulators (each 32-bit half of the data, mixed
// with a key, multiplied into the other half: XXH3's accumulate). FH1 hashes ~16 MB of vertex and index
// data a frame (buffers reused within the frame are checked again before each draw); the scalar lanes
// took ~1 ms of it.
__attribute__((target("avx2"))) uint64_t FullHashAvx2(const uint8_t* data, size_t bytes)
{
    const __m256i k0 = _mm256_set_epi64x(0x1CAD21F72C81017Cull, 0xBE4BA423396CFEB8ull, 0x7C01812CF721AD1Cull, 0xDB979083E96DD4DEull);
    const __m256i k1 = _mm256_set_epi64x(0x1F67B3B7A4A44072ull, 0x78E5C0CC4EE679CBull, 0xD8ACDEA946EF1938ull, 0x3F349CE33F76FAA8ull);
    __m256i a = _mm256_set1_epi64x(int64_t(0x9E3779B97F4A7C15ull ^ bytes)), b = _mm256_set1_epi64x(int64_t(0xC2B2AE3D27D4EB4Full));
    size_t i = 0;
    for (; i + 64 <= bytes; i += 64)
    {
        const __m256i d0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        const __m256i d1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i + 32));
        const __m256i x0 = _mm256_xor_si256(d0, k0), x1 = _mm256_xor_si256(d1, k1);
        a = _mm256_add_epi64(a, _mm256_add_epi64(_mm256_mul_epu32(x0, _mm256_srli_epi64(x0, 32)), _mm256_shuffle_epi32(d0, 0x4E)));
        b = _mm256_add_epi64(b, _mm256_add_epi64(_mm256_mul_epu32(x1, _mm256_srli_epi64(x1, 32)), _mm256_shuffle_epi32(d1, 0x4E)));
    }
    alignas(32) uint64_t lanes[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), a);
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes + 4), b);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t v : lanes)
        h = (h ^ v) * 0xFF51AFD7ED558CCDull, h ^= h >> 29;
    for (; i < bytes; i++)
        h = (h ^ data[i]) * 0x100000001B3ull;
    h = (h ^ (h >> 33)) * 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 33);
}

// FullHashAvx2's accumulate in 128-bit halves, for CPUs without AVX2 (FX-8350: its 64-bit multiplies
// are slow, the scalar lanes below were ~13% of the draw thread).
uint64_t FullHashSse2(const uint8_t* data, size_t bytes)
{
    const __m128i k0 = _mm_set_epi64x(0x7C01812CF721AD1Cull, 0xDB979083E96DD4DEull), k1 = _mm_set_epi64x(0x1CAD21F72C81017Cull, 0xBE4BA423396CFEB8ull);
    const __m128i k2 = _mm_set_epi64x(0xD8ACDEA946EF1938ull, 0x3F349CE33F76FAA8ull), k3 = _mm_set_epi64x(0x1F67B3B7A4A44072ull, 0x78E5C0CC4EE679CBull);
    __m128i a0 = _mm_set1_epi64x(int64_t(0x9E3779B97F4A7C15ull ^ bytes)), a1 = a0;
    __m128i b0 = _mm_set1_epi64x(int64_t(0xC2B2AE3D27D4EB4Full)), b1 = b0;
    const auto acc = [](__m128i& s, __m128i d, __m128i k) {
        const __m128i x = _mm_xor_si128(d, k);
        s = _mm_add_epi64(s, _mm_add_epi64(_mm_mul_epu32(x, _mm_srli_epi64(x, 32)), _mm_shuffle_epi32(d, 0x4E)));
    };
    size_t i = 0;
    for (; i + 64 <= bytes; i += 64)
    {
        acc(a0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), k0);
        acc(a1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 16)), k1);
        acc(b0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 32)), k2);
        acc(b1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 48)), k3);
    }
    alignas(16) uint64_t lanes[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), a0);
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 2), a1);
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 4), b0);
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 6), b1);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t v : lanes)
        h = (h ^ v) * 0xFF51AFD7ED558CCDull, h ^= h >> 29;
    for (; i < bytes; i++)
        h = (h ^ data[i]) * 0x100000001B3ull;
    h = (h ^ (h >> 33)) * 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 33);
}

// Every byte of the data, four independent 64-bit lanes at a time (a dword-by-dword chain waits on each
// multiply). For data whose every byte matters and that is checked every frame: index buffers.
// (AVX2 where the CPU has it; REGPU_SCALAR_HASH=1: the scalar lanes.)
uint64_t FullHash(const uint8_t* data, size_t bytes)
{
    static const bool avx2 = IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */) && !getenv("REGPU_SCALAR_HASH");
    if (avx2 && bytes >= 256)
        return FullHashAvx2(data, bytes);
    static LiveSwitch scalar("REGPU_SCALAR_HASH_LIVE"); // the scalar lanes on a CPU without AVX2 (A/B)
    if (!avx2 && bytes >= 256 && !scalar)
        return FullHashSse2(data, bytes);
    uint64_t a = 0xCBF29CE484222325ull ^ bytes, b = 0x9E3779B97F4A7C15ull, c = 0xC2B2AE3D27D4EB4Full, d = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32)
    {
        uint64_t v[4];
        memcpy(v, data + i, 32);
        a = (a ^ v[0]) * 0xFF51AFD7ED558CCDull;
        b = (b ^ v[1]) * 0xC4CEB9FE1A85EC53ull;
        c = (c ^ v[2]) * 0xFF51AFD7ED558CCDull;
        d = (d ^ v[3]) * 0xC4CEB9FE1A85EC53ull;
        a ^= a >> 29; b ^= b >> 31; c ^= c >> 29; d ^= d >> 31;
    }
    for (; i < bytes; i++)
        a = (a ^ data[i]) * 0x100000001B3ull;
    uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull);
    h = (h ^ (h >> 33)) * 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

// Sampled hash for large texture sources: every 64th dword plus the ends.
// (Word-wise: it runs for every small buffer of every draw.)
uint64_t ContentHash(const uint8_t* data, size_t bytes, size_t stride = 256)
{
    auto mix = [](uint64_t h, uint64_t v) {
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        return h ^ (h >> 32);
    };
    uint64_t h = mix(0xCBF29CE484222325ull, bytes);
    for (size_t i = 0; i + 4 <= bytes; i += stride)
    {
        uint32_t v;
        memcpy(&v, data + i, 4);
        h = mix(h, v);
    }
    if (bytes >= 64)
        for (size_t i = bytes - 64; i < bytes; i += 8)
        {
            uint64_t v;
            memcpy(&v, data + i, 8);
            h = mix(h, v);
        }
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, UINT index, UINT stride)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* heap, UINT index, UINT stride)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * stride;
    return h;
}

ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource(buffer)"); CountCreate("buffer");
    return r;
}

// Depth resolves: a depth-stencil buffer can only be copied whole, so the
// resolve rectangle is drawn into an R32_FLOAT texture by a tiny pass that
// reads the depth buffer at the same pixel.
void InitDepthBlit()
{
    auto* d = g_r.device;
    // Anti-aliasing: the highest sample count the setting asks for that the device supports
    // for both the colour and depth formats (1 when none).
    g_msaa = 1;
    for (uint32_t want = GetSettings().msaa; want > 1; want /= 2)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS colour{ g_colorFormat, want }, depth{ DXGI_FORMAT_D32_FLOAT_S8X24_UINT, want };
        if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &colour, sizeof colour)) && colour.NumQualityLevels &&
            SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth, sizeof depth)) && depth.NumQualityLevels)
        {
            g_msaa = want;
            break;
        }
    }
    Log("renderer: anti-aliasing %ux (setting %u)", g_msaa, GetSettings().msaa);
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 10;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: blit root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.blitRootSignature)),
        "CreateRootSignature(blit)");

    static const char kSource[] = R"(
Texture2D<float> depthTex : register(t0);
Texture2D<float4> colorTex : register(t0);
cbuffer Blit : register(b0) { float scale; uint swapRB; uint clampDest; float ceiling; float destMin; float destMax; float2 srcScale; float2 srcOffset; };
// The source texel for a destination pixel: the same one, or through srcScale/srcOffset (a depth copy
// through a foreign EDRAM layout reads where the tiles lie on the host surface).
int2 Source(float4 pos) { return int2(pos.xy * srcScale + srcOffset); }
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthTex.Load(int3(Source(pos), 0));
}
float4 psColor(float4 pos : SV_Position) : SV_Target
{
    float4 c = colorTex.Load(int3(Source(pos), 0));
    if (ceiling > 0) c = float4(clamp(c.rgb, 0.0, ceiling), saturate(c.a));
    c *= scale;
    if (clampDest) c = clamp(c, destMin, destMax);
    return swapRB ? c.bgra : c;
}
)";
    ComPtr<ID3DBlob> vs, ps;
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &error), "D3DCompile(blit vs)");
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &error), "D3DCompile(blit ps)");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_r.blitRootSignature.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    static LiveSwitch noCull("REGPU_NO_CULL");
    if (noCull)
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    pd.SampleDesc.Count = 1;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.depthBlit)), "CreateGraphicsPipelineState(depth blit)");
    ComPtr<ID3DBlob> psColor;
    Check(D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "psColor", "ps_5_0", 0, 0, &psColor, &error), "D3DCompile(blit psColor)");
    pd.PS = { psColor->GetBufferPointer(), psColor->GetBufferSize() };
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlit8)), "CreateGraphicsPipelineState(colour blit 8)");
    pd.RTVFormats[0] = g_colorFormat;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlit16)), "CreateGraphicsPipelineState(colour blit 16)");
    if (g_msaa > 1)
    {
        // The same passes reading a multisampled surface: depth takes sample 0, colour averages all samples.
        const std::string msSource = "#define SAMPLES " + std::to_string(g_msaa) + R"(
Texture2DMS<float, SAMPLES> depthMs : register(t0);
Texture2DMS<float4, SAMPLES> colorMs : register(t0);
cbuffer Blit : register(b0) { float scale; uint swapRB; uint clampDest; float ceiling; float destMin; float destMax; float2 srcScale; float2 srcOffset; };
// The source texel for a destination pixel: the same one, or through srcScale/srcOffset (a depth copy
// through a foreign EDRAM layout reads where the tiles lie on the host surface).
int2 Source(float4 pos) { return int2(pos.xy * srcScale + srcOffset); }
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthMs.Load(Source(pos), 0);
}
float4 psColor(float4 pos : SV_Position) : SV_Target
{
    float4 c = 0;
    [unroll] for (int i = 0; i < SAMPLES; i++)
    {
        // Each sample as the console stored it (a 7e3 source's range), then averaged.
        float4 s = colorMs.Load(Source(pos), i);
        if (ceiling > 0) s = float4(clamp(s.rgb, 0.0, ceiling), saturate(s.a));
        c += s;
    }
    c = c / SAMPLES * scale;
    if (clampDest) c = clamp(c, destMin, destMax);
    return swapRB ? c.bgra : c;
}
)";
        ComPtr<ID3DBlob> msVs, msPs, msColor;
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &msVs, &error), "D3DCompile(blit ms vs)");
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &msPs, &error), "D3DCompile(blit ms depth)");
        Check(D3DCompile(msSource.data(), msSource.size(), nullptr, nullptr, nullptr, "psColor", "ps_5_0", 0, 0, &msColor, &error), "D3DCompile(blit ms colour)");
        pd.VS = { msVs->GetBufferPointer(), msVs->GetBufferSize() };
        pd.PS = { msPs->GetBufferPointer(), msPs->GetBufferSize() };
        pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.depthBlitMs)), "CreateGraphicsPipelineState(depth blit ms)");
        pd.PS = { msColor->GetBufferPointer(), msColor->GetBufferSize() };
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlitMs8)), "CreateGraphicsPipelineState(colour blit ms 8)");
        pd.RTVFormats[0] = g_colorFormat;
        Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlitMs16)), "CreateGraphicsPipelineState(colour blit ms 16)");
    }
}

void InitPipelineLibrary(ID3D12Device* device);

// The EDRAM render target cache (gpu/edram_targets.h), started by REGPU_EDRAM_TARGETS=1
// (REGPU_EDRAM_SCALE=1..4 its resolution scale). Until draws are bound to its targets, it runs
// beside the screen-space surfaces, which still draw: each draw's targets are created and
// claimed and the transfers recorded, to bring the cache up without changing the picture.
// Startup only: the ownership it tracks depends on every draw since the start.
edram::TargetCache g_edramTargets;
bool g_edramTargetsOn = false;

void InitEdramTargets()
{
    const char* on = getenv("REGPU_EDRAM_TARGETS");
    if (!on || !atoi(on))
        return;
    const char* scale = getenv("REGPU_EDRAM_SCALE");
    const uint32_t s = scale ? uint32_t(std::clamp(atoi(scale), 1, 4)) : 1u;
    g_edramTargetsOn = g_edramTargets.Init(g_r.device, s, s);
    if (!g_edramTargetsOn)
        Log("renderer: the EDRAM render target cache did not start; screen-space surfaces only");
}

void Init()
{
    g_r.device = RenderDevice();
    g_r.queue = RenderQueue();
    auto* d = g_r.device;

    // Root signature: b0, b1 root CBVs; one table t0-t32; four static samplers.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2; // t0 upload ring, t1 guest memory
    D3D12_DESCRIPTOR_RANGE bindless[3]{};
    for (UINT i = 0; i < 3; i++)
    {
        // 2D textures in space1, cubes in space2, 3D textures in space3, over the same descriptors.
        bindless[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindless[i].NumDescriptors = kBindless;
        bindless[i].RegisterSpace = 1 + i;
        bindless[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 3;
    params[3].DescriptorTable.pDescriptorRanges = bindless;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    const D3D12_FILTER filters[4] = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_FILTER_MIN_MAG_MIP_POINT };
    const D3D12_TEXTURE_ADDRESS_MODE modes[4] = { D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP };
    for (UINT i = 0; i < 4; i++)
    {
        samplers[i].Filter = filters[i];
        // Wrapping linear filtering (surface textures: road, ground) is anisotropic: plain
        // trilinear picks blurred levels at grazing angles (blotchy asphalt). Not the clamped
        // sampler: it also reads shadow maps and other render targets, where averaging depth
        // over a long footprint breaks the shadows (black patches, glitchy object shadows).
        static LiveSwitch noAniso("REGPU_NO_ANISO"); // A/B
        if (filters[i] == D3D12_FILTER_MIN_MAG_MIP_LINEAR && modes[i] == D3D12_TEXTURE_ADDRESS_MODE_WRAP && !noAniso)
        {
            samplers[i].Filter = D3D12_FILTER_ANISOTROPIC;
            samplers[i].MaxAnisotropy = 16;
        }
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = modes[i];
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 4;
    rs.pParameters = params;
    rs.NumStaticSamplers = 4;
    rs.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.rootSignature)), "CreateRootSignature");
    // Motion-vector pipelines: the same plus b2, last frame's vertex constants of the draw.
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].Descriptor.ShaderRegister = 2;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rs.NumParameters = 5;
    blob.Reset();
    error.Reset();
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: motion root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.motionRootSignature)),
        "CreateRootSignature(motion)");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kBindlessBase + kBindless;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.srvHeap)), "CreateDescriptorHeap(srv)");
    g_r.srvStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hd.NumDescriptors = 2 + 2 * kFrames; // null 2D, null cube, per frame: ring and guest-memory raw views
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.stagingHeap)), "CreateDescriptorHeap(staging)");
    hd.NumDescriptors = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride));
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    nullSrv.TextureCube.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.stagingHeap.Get(), 1, g_r.srvStride));
    d->CopyDescriptorsSimple(2, Cpu(g_r.srvHeap.Get(), kBindlessBase, g_r.srvStride), Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    nullSrv.Texture3D.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.srvHeap.Get(), kBindlessBase + 2, g_r.srvStride));

    InitPipelineLibrary(d);
    // Guest physical memory for the GPU, in place: vertex fetches read it
    // without copies.
    ComPtr<ID3D12Device13> d13;
    HRESULT hr = d->QueryInterface(IID_PPV_ARGS(&d13));
    if (SUCCEEDED(hr))
        hr = d13->OpenExistingHeapFromFileMapping(PhysicalSection(), IID_PPV_ARGS(&g_r.physicalHeap));
    if (FAILED(hr) && d13)
    {
        Log("renderer: OpenExistingHeapFromFileMapping failed %08X; trying the view address", unsigned(hr));
        hr = d13->OpenExistingHeapFromAddress1(GuestPtr(0xA0000000), 0x20000000, IID_PPV_ARGS(&g_r.physicalHeap));
    }
    if (FAILED(hr))
        Log("renderer: mapping guest memory: %s failed %08X", d13 ? "OpenExistingHeapFromAddress1" : "ID3D12Device13", unsigned(hr));
    else
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_r.physicalHeap->GetDesc().SizeInBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        if (FAILED(hr = d->CreatePlacedResource(g_r.physicalHeap.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_r.physical))))
        {
            Log("renderer: placing the guest memory buffer failed %08X", unsigned(hr));
            g_r.physical.Reset();
        }
    }
    Log("renderer: guest memory %s", g_r.physical ? "mapped for the GPU (zero-copy vertex fetch)" : "not mappable; vertex data is copied");

    // Views for the EDRAM surfaces (created on first use).
    hd.NumDescriptors = kMaxSurfaces;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.rtvHeap)), "CreateDescriptorHeap(rtv)");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.dsvHeap)), "CreateDescriptorHeap(dsv)");
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.blitRtvHeap)), "CreateDescriptorHeap(blit rtv)");
    InitDepthBlit();
    InitEdramTargets();
    g_r.readback = CreateBuffer(UINT64((OutputWidth() * 4 + 255) & ~255u) * OutputHeight(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // The upload ring: per frame, it holds draw constants, copied vertex and index data and texture
    // uploads. A frame that outgrows it loses draws (see RingAlloc), so it starts at 256 MB above
    // 1080p output (a 4K race needed between 64 and 128 MB) and grows after a frame that ran out or
    // came near (BeginFrameIfNeeded). REGPU_RING_MB=<n> fixes the size (no growing).
    kRingSize = (getenv("REGPU_RING_MB") ? std::clamp<UINT64>(UINT64(atoi(getenv("REGPU_RING_MB"))), 16, 2048)
        : g_r.physical && !getenv("REGPU_NO_DIRECT_FETCH") ? (OutputHeight() > 1080 ? 256 : 64) : 384) << 20;
    Log("renderer: upload ring %llu MB per frame", (unsigned long long)(kRingSize >> 20));
    for (UINT i = 0; i < kFrames; i++)
    {
        FrameResources& f = g_r.frames[i];
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)), "CreateCommandAllocator");
        f.ring = CreateBuffer(kRingSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        f.ring->Map(0, nullptr, reinterpret_cast<void**>(&f.ringCpu));
        f.ringSize = kRingSize;
        f.descriptorBase = i * kDescriptorsPerFrame;
    }
    Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_r.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g_r.realList)),
        "CreateCommandList");
    g_r.realList->Close();
    g_r.recorder.Init(g_r.realList.Get());
    g_mainContext.list = &g_r.recorder;
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence)), "CreateFence");
    g_r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PresenterUseRenderer();
    g_r.ok = true;
    Log("renderer: D3D12 renderer ready");
}

double g_gpuWaitSeconds = 0; // CP thread blocked on the host GPU (frame resources)

FrameResources& Frame() { return g_r.frames[g_r.frameIndex]; }

void TransitionColor(D3D12_RESOURCE_STATES to);

// Gets (creating on first use) the host surface for an EDRAM base.
bool TemporalInit();
void BeginTemporalFrame();
void MotionStatsLog();

// REGPU_EXACT_FP16_SURFACES (see ExactInHalf): asked once, after the device and settings are up.
bool HalfSurfaces()
{
    static const bool on = [] {
        if (!getenv("REGPU_EXACT_FP16_SURFACES"))
            return false;
        const bool ok = g_colorFormat == DXGI_FORMAT_R32G32B32A32_FLOAT && g_msaa == 1 && RendererDrawWorkers() == 0;
        Log("renderer: exact fp16 surfaces %s", ok ? "on" : "off (needs 32-bit surfaces, no MSAA and no draw workers)");
        return ok;
    }();
    return on;
}
void LogHalfSurfaces()
{
    // Each half-float surface saves 8 bytes a pixel against 32-bit floats.
    auto mb = [](uint32_t w, uint32_t h) { return double(g_halfSurfaces) * w * h * 8.0 / (1024.0 * 1024.0); };
    Log("renderer: exact fp16: %u colour surfaces in half floats, %.1f MB saved at %ux%u (%.1f MB at 3840x2160, %.1f MB at 1280x720)",
        g_halfSurfaces, mb(OutputWidth(), OutputHeight()), OutputWidth(), OutputHeight(), mb(3840, 2160), mb(1280, 720));
}
void PromoteSurface(Renderer::Surface& s, uint32_t base);

// half: a colour surface created now may be half floats (HalfSurfaces and ExactInHalf); a half-float
// one asked for without it is moved to g_colorFormat.
Renderer::Surface& GetSurface(bool depth, uint32_t base, bool half = false)
{
    std::lock_guard surfaceLock(g_surfaceMutex);
    auto& map = depth ? g_r.depthSurfaces : g_r.colorSurfaces;
    if (auto it = map.find(base); it != map.end())
    {
        if (!depth && !half && it->second.format == kHalfColor)
            PromoteSurface(it->second, base);
        return it->second;
    }
    if (map.size() >= kMaxSurfaces)
        return map.begin()->second; // out of views: share one (should not happen)
    Renderer::Surface& s = map[base];
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = OutputWidth();
    td.Height = OutputHeight();
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = g_msaa;
    D3D12_CLEAR_VALUE cv{};
    const UINT index = UINT(map.size() - 1);
    if (depth)
    {
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS; // float depth (the title uses D24FS8, reverse-Z); typeless for resolves
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        cv.DepthStencil.Depth = 1.0f;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                  IID_PPV_ARGS(&s.resource)),
            "CreateCommittedResource(depth surface)"); CountCreate("depth surface");
        s.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        s.view = g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        s.view.ptr += SIZE_T(index) * g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = g_msaa > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        C().list->CreateDepthStencilView(g_r.device, s.resource.Get(), dsvDesc, s.view); // in order with the recorded commands
    }
    else
    {
        td.Format = half ? kHalfColor : g_colorFormat;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                  IID_PPV_ARGS(&s.resource)),
            "CreateCommittedResource(colour surface)"); CountCreate("colour surface");
        s.format = td.Format;
        s.view = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        s.view.ptr += SIZE_T(index) * g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        C().list->CreateRenderTargetView(g_r.device, s.resource.Get(), s.view); // in order with the recorded commands
    }
    Log("renderer: EDRAM %s surface at base %03X%s", depth ? "depth" : "colour", base, !depth && half ? " (half floats)" : "");
    if (!depth && half)
    {
        g_halfSurfaces++;
        LogHalfSurfaces();
    }
    if (getenv("REGPU_D3D12_DEBUG"))
    {
        wchar_t name[48];
        swprintf(name, 48, L"EDRAM %ls %03X", depth ? L"depth" : L"colour", base);
        s.resource->SetName(name);
    }
    return s;
}

// Binds the surfaces for the current RB_COLOR_INFO / RB_DEPTH_INFO bases
// (the list must be open).
FrameResources& Frame();

void PassMark(const char* fmt, uint32_t a, uint32_t b)
{
    if (!g_gpuPasses || !g_passQueries)
        return;
    const UINT frameIndex = UINT(&Frame() - g_r.frames);
    auto& marks = g_passMarks[frameIndex];
    if (marks.size() >= kPassMarks)
        return;
    if (!marks.empty())
        marks.back().draws = g_passDraws;
    g_passDraws = 0;
    PassMarkInfo m{};
    snprintf(m.label, sizeof(m.label), fmt, a, b);
    C().list->EndQuery(g_passQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * kPassMarks + UINT(marks.size()));
    marks.push_back(m);
}

// Debug: REGPU_SYNC_PASSES=1 submits the recorded work and waits for the GPU to finish it
// whenever the render targets change; REGPU_SYNC_RESOLVES=1 does the same after every resolve and
// REGPU_SPLIT_RESOLVES=1 only submits there. Slow; they tell whether a fault comes from GPU work
// overlapping that should not (a missing barrier).
void SelectSurfaces(bool force)
{
    if (!force)
    {
        static LiveSwitch syncPasses("REGPU_SYNC_PASSES");
        if (syncPasses && !C().worker && ((gpureg::Get(RB_COLOR_INFO) & 0xFFF) != C().colorBase || (gpureg::Get(RB_DEPTH_INFO) & 0xFFF) != C().depthBase))
            RendererFenceSyncForced(); // (binds the new targets: SplitMainList)
    }
    const uint32_t colorInfo = gpureg::Get(RB_COLOR_INFO);
    // REGPU_COLOR_SURFACE_PER_BPP=1 (test, live): 64-bit colour formats (16_16_16_16, 16_16_16_16_FLOAT,
    // 32_32_FLOAT) get a host surface of their own at a base, apart from the 32-bit ones. A title that
    // keeps an image in EDRAM in a 64-bit layout across frames and draws a 32-bit pass over the same base
    // in between (an accumulation: copied out before the next blend) found it overwritten by that pass,
    // as one host surface per base holds both; in EDRAM the two layouts don't cover the same bytes.
    static LiveSwitch perBpp("REGPU_COLOR_SURFACE_PER_BPP");
    const uint32_t colorFormatReg = (colorInfo >> 16) & 0xF;
    const bool wide = colorFormatReg == 5 || colorFormatReg == 7 || colorFormatReg == 15;
    const uint32_t colorBase = (colorInfo & 0xFFF) | (perBpp && wide ? 0x1000u : 0u), depthBase = gpureg::Get(RB_DEPTH_INFO) & 0xFFF;
    // (A half-float surface rebound in a format it can't hold exactly is moved to 32-bit floats.)
    const bool half = HalfSurfaces() && ExactInHalf((colorInfo >> 16) & 0xF);
    if (!force && colorBase == C().colorBase && depthBase == C().depthBase && (half || C().colorFormat != kHalfColor))
        return;
    if (C().colorBase != ~0u && !C().worker) // (workers leave surfaces as render targets)
        g_r.colorSurfaces[C().colorBase].state = C().colorState;
    Renderer::Surface& c = GetSurface(false, colorBase, half);
    Renderer::Surface& z = GetSurface(true, depthBase);
    C().color = c.resource;
    C().colorFormat = c.format;
    C().colorState = c.state;
    C().rtv = c.view;
    C().depth = z.resource;
    C().dsv = z.view;
    C().colorBase = colorBase;
    C().depthBase = depthBase;
    PassMark("pass colour %03X depth %03X", colorBase, depthBase);
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    C().list->OMSetRenderTargets(1, &C().rtv, FALSE, &C().dsv);
    g_drawState.motionTargets = false;
}

// This draw's render targets in the EDRAM target cache (see InitEdramTargets).
void UpdateEdramTargets(const CompiledShader& ps)
{
    edram::PassRegisters r;
    r.surfaceInfo = gpureg::Get(RB_SURFACE_INFO);
    r.colorInfo[0] = gpureg::Get(RB_COLOR_INFO);
    for (uint32_t i = 1; i < 4; i++)
        r.colorInfo[i] = gpureg::Get(0x2002 + i); // RB_COLOR1..3_INFO
    r.depthInfo = gpureg::Get(RB_DEPTH_INFO);
    r.modeControl = gpureg::Get(RB_MODECONTROL);
    r.colorMask = gpureg::Get(RB_COLOR_MASK);
    r.depthControl = gpureg::Get(RB_DEPTHCONTROL);
    r.windowOffset = gpureg::Get(PA_SC_WINDOW_OFFSET);
    r.scissorTL = gpureg::Get(PA_SC_WINDOW_SCISSOR_TL);
    r.scissorBR = gpureg::Get(PA_SC_WINDOW_SCISSOR_BR);
    r.screenScissorTL = gpureg::Get(0x200E); // PA_SC_SCREEN_SCISSOR_TL
    r.screenScissorBR = gpureg::Get(0x200F); // PA_SC_SCREEN_SCISSOR_BR
    r.modeCntl = gpureg::Get(PA_SU_SC_MODE_CNTL);
    r.vteCntl = gpureg::Get(PA_CL_VTE_CNTL);
    r.vtxCntl = gpureg::Get(0x2302); // PA_SU_VTX_CNTL
    r.viewportYScale = RegFloat(PA_CL_VPORT_YSCALE);
    r.viewportYOffset = RegFloat(PA_CL_VPORT_YOFFSET);
    r.psColorTargets = ps.info.colorTargetsWritten;
    const edram::Pass pass = edram::DescribePass(r);
    if (!pass.used)
        return;
    edram::RecordContext rc;
    rc.list = C().list;
    rc.descriptors = [](uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) {
        FrameResources& f = Frame();
        if (f.descriptorsUsed + count > kDescriptorsPerFrame)
            return false;
        const UINT slot = f.descriptorBase + f.descriptorsUsed;
        f.descriptorsUsed += count;
        cpu = Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride);
        gpu = Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride);
        return true;
    };
    edram::HostTarget* targets[5];
    if (g_edramTargets.Update(pass, rc, targets))
    {
        // Back to the screen-space surfaces and the draw's root signature (as after a resolve blit).
        C().list->OMSetRenderTargets(1, &C().rtv, FALSE, &C().dsv);
        C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
        g_drawState.Invalidate();
    }
}

// Bindless texture descriptors: one per (resource, format, dimension), kept
// while the texture is in use. Entries unused for a few frames (replaced or
// dropped textures) go back to the free list; their resources are released
// once the GPU is done with the frame that last used them.
UINT BindlessIndexLocked(ID3D12Resource* resource, DXGI_FORMAT format, bool cube, bool volume);
// Each thread remembers this frame's descriptors (eviction only takes ones unused for frames).
UINT BindlessIndex(ID3D12Resource* resource, DXGI_FORMAT format, bool cube, bool volume = false)
{
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource)) << 8 ^ uint64_t(format) << 2 ^ uint64_t(cube) ^ uint64_t(volume) << 1;
    thread_local FrameMap<UINT> known;
    if (const UINT* index = known.Find(key, g_r.frame))
        return *index;
    std::lock_guard textureLock(g_textureMutex);
    const UINT index = BindlessIndexLocked(resource, format, cube, volume);
    known.Put(key, g_r.frame, index);
    return index;
}

UINT BindlessIndexLocked(ID3D12Resource* resource, DXGI_FORMAT format, bool cube, bool volume)
{
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource)) << 8 ^ uint64_t(format) << 2 ^ uint64_t(cube) ^ uint64_t(volume) << 1;
    auto it = g_r.bindless.find(key);
    if (it != g_r.bindless.end())
    {
        it->second.lastFrame = g_r.frame;
        return it->second.index;
    }
    UINT index;
    if (!g_r.bindlessFree.empty())
    {
        index = g_r.bindlessFree.back();
        g_r.bindlessFree.pop_back();
    }
    else if (g_r.bindlessNext < kBindless)
        index = g_r.bindlessNext++;
    else
        return volume ? 2u : cube ? 1u : 0u; // full: draw with a null texture
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.Shader4ComponentMapping = format == DXGI_FORMAT_BC4_UNORM
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0) // DXT5A: its one channel in all four
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (volume)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = UINT(-1);
    }
    else if (cube)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = UINT(-1);
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = UINT(-1); // all levels (resolved mip chains)
    }
    g_r.device->CreateShaderResourceView(resource, &srv, Cpu(g_r.srvHeap.Get(), kBindlessBase + index, g_r.srvStride));
    g_r.bindless[key] = { index, resource, g_r.frame };
    return index;
}

void EvictBindless()
{
    if (g_r.frame % 16 != 0)
        return;
    for (auto it = g_r.bindless.begin(); it != g_r.bindless.end();)
    {
        static const uint64_t extra = RetireDelay("REGPU_DELAY_NO_BINDLESS");
        if (it->second.lastFrame + kFrames + 2 + extra < g_r.frame)
        {
            Frame().garbage.push_back(std::move(it->second.resource));
            g_r.bindlessFree.push_back(it->second.index);
            it = g_r.bindless.erase(it);
        }
        else
            ++it;
    }
}

// Waits for the queue fence to reach a value, on an event of the calling thread: one shared event let two waiting
// threads steal each other's wake-up, and the loser sat out its whole timeout (2 s stalls in a race).
void WaitFenceValue(UINT64 value, DWORD timeoutMs)
{
    thread_local HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_r.fence->GetCompletedValue() >= value)
        return;
    g_r.fence->SetEventOnCompletion(value, event);
    if (WaitForSingleObject(event, timeoutMs) == WAIT_TIMEOUT && timeoutMs != INFINITE)
        Log("perf: a fence wait timed out after %lu ms (value %llu, completed %llu)", timeoutMs, (unsigned long long)value, (unsigned long long)g_r.fence->GetCompletedValue());
}

void BeginFrameIfNeeded()
{
    if (g_r.listOpen)
        return;
    FrameResources& f = Frame();
    if (g_r.fence->GetCompletedValue() < f.fence)
    {
        const auto w0 = std::chrono::steady_clock::now();
        WaitFenceValue(f.fence, INFINITE);
        g_gpuWaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    }
    // REGPU_FRAMES_AHEAD=<n> (1..kFrames-1): before a frame starts, the host GPU has finished the frame
    // n before it, so at most n frames of work wait on the GPU (unset: kFrames, as before).
    static const UINT framesAhead = getenv("REGPU_FRAMES_AHEAD") ? UINT(std::clamp(atoi(getenv("REGPU_FRAMES_AHEAD")), 1, int(kFrames))) : kFrames;
    if (framesAhead < kFrames)
    {
        const FrameResources& earlier = g_r.frames[(g_r.frameIndex + kFrames - framesAhead) % kFrames];
        if (g_r.fence->GetCompletedValue() < earlier.fence)
        {
            const auto w0 = std::chrono::steady_clock::now();
            WaitFenceValue(earlier.fence, INFINITE);
            g_gpuWaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        }
    }
    {
        // The slot's garbage is free once its frame has completed; with REGPU_GARBAGE_DELAY it waits
        // that many frames more.
        static const uint64_t extra = RetireDelay("REGPU_DELAY_NO_GARBAGE");
        static std::deque<std::pair<uint64_t, ComPtr<ID3D12Resource>>> delayed;
        if (extra)
        {
            for (auto& r : f.garbage)
                delayed.emplace_back(g_r.frame, std::move(r));
            while (!delayed.empty() && delayed.front().first + extra < g_r.frame)
                delayed.pop_front();
        }
        f.garbage.clear();
    }
    void FinishDeferredWritebacks(UINT frameIndex);
    FinishDeferredWritebacks(UINT(&f - g_r.frames)); // this slot's previous frame has completed
    static LiveSwitch perf("REGPU_PERF");
    const UINT frameIndex = UINT(&f - g_r.frames);
    if (!g_occlusionPending[frameIndex].empty())
    {
        // This slot's previous frame has completed: keep its occlusion results.
        uint64_t* counts = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * kOcclusionPerFrame * 8, size_t(frameIndex + 1) * kOcclusionPerFrame * 8 };
        if (SUCCEEDED(g_occlusionReadback->Map(0, &range, reinterpret_cast<void**>(&counts))))
        {
            // A block can be counted once per tile in a frame (predicated tiling draws the scene
            // once per tile, each pass wrapped in the same query): the console accumulates the
            // block's counts across the tiles, so the frame's counts are summed per key.
            std::unordered_map<uint32_t, int64_t> frameSums;
            for (const auto& [key, index] : g_occlusionPending[frameIndex])
                frameSums[key] += int64_t(counts[index]);
            for (const auto& [key, sum] : frameSums)
                g_occlusionResult[key] = sum;
            const D3D12_RANGE none{ 0, 0 };
            g_occlusionReadback->Unmap(0, &none);
        }
        g_occlusionPending[frameIndex].clear();
    }
    if (perf && !g_timestamps)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2 * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_timestamps));
        g_timestampReadback = CreateBuffer(8 * 2 * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (g_gpuPasses && !g_passQueries)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kPassMarks * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_passQueries));
        g_passReadback = CreateBuffer(8ull * kPassMarks * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (g_gpuPasses && g_passMarks[frameIndex].size() > 1)
    {
        // This slot's previous frame has completed: add up its passes.
        auto& marks = g_passMarks[frameIndex];
        uint64_t* t = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * kPassMarks * 8, size_t(frameIndex + 1) * kPassMarks * 8 };
        if (SUCCEEDED(g_passReadback->Map(0, &range, reinterpret_cast<void**>(&t))))
        {
            UINT64 frequency = 1;
            g_r.queue->GetTimestampFrequency(&frequency);
            const uint64_t* ts = t + size_t(frameIndex) * kPassMarks;
            for (size_t i = 0; i + 1 < marks.size(); i++)
            {
                auto& total = g_passTotals[marks[i].label];
                total.first += double(ts[i + 1] - ts[i]) * 1000.0 / double(frequency);
                total.second += marks[i].draws;
            }
            const D3D12_RANGE none{ 0, 0 };
            g_passReadback->Unmap(0, &none);
            g_passFrames++;
        }
        marks.clear();
        static auto lastLog = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(2) && g_passFrames)
        {
            lastLog = std::chrono::steady_clock::now();
            std::vector<std::pair<double, std::string>> top;
            double sum = 0;
            for (auto& [label, total] : g_passTotals)
            {
                top.push_back({ total.first, label });
                sum += total.first;
            }
            std::sort(top.rbegin(), top.rend());
            Log("gpu passes: %.2f ms/frame over %llu frames", sum / g_passFrames, (unsigned long long)g_passFrames);
            for (size_t i = 0; i < top.size() && i < 14; i++)
                Log("gpu passes: %6.3f ms  %5.0f draws  %s", top[i].first / g_passFrames, double(g_passTotals[top[i].second].second) / g_passFrames,
                    top[i].second.c_str());
            g_passTotals.clear();
            g_passFrames = 0;
        }
    }
    g_passMarks[frameIndex].clear();
    if (perf && f.timed && g_timestampReadback)
    {
        // This slot's previous frame has completed (waited for above).
        uint64_t* t = nullptr;
        const D3D12_RANGE range{ size_t(frameIndex) * 16, size_t(frameIndex) * 16 + 16 };
        if (SUCCEEDED(g_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&t))))
        {
            UINT64 frequency = 1;
            g_r.queue->GetTimestampFrequency(&frequency);
            const double ms = double(t[frameIndex * 2 + 1] - t[frameIndex * 2]) * 1000.0 / double(frequency);
            const D3D12_RANGE none{ 0, 0 };
            g_timestampReadback->Unmap(0, &none);
            if (ms > 0 && ms < 1000)
            {
                g_gpuFrameMs += ms;
                g_gpuFrameMaxMs = std::max(g_gpuFrameMaxMs, ms);
                g_gpuFrames++;
            }
        }
        f.timed = false;
    }
    f.allocator->Reset();
    f.poolUsed = 0;
    g_r.recorder.Rebind(g_r.realList.Get()); // (a pool list may have continued last frame)
    C().list->Reset(f.allocator.Get(), nullptr);
    // This frame's scene phase and scale (see UpscaleScene).
    {
        const Settings& st = GetSettings();
        const bool temporalUpscaler = st.upscaler == Settings::Upscaler::Dlss || st.upscaler == Settings::Upscaler::Fsr3 ||
            st.upscaler == Settings::Upscaler::Fsr4;
        const float scale = st.SceneScale();
        g_upscale.switchedThisFrame = false;
        g_scenePhase = st.upscaler != Settings::Upscaler::Off && (temporalUpscaler || scale < 1.0f) && g_r.frame >= g_upscale.retryFrame &&
            (!temporalUpscaler || TemporalInit());
        g_temporal = g_scenePhase && temporalUpscaler;
        g_sceneScale = g_scenePhase ? scale : 1.0f;
        if (g_temporal)
            BeginTemporalFrame();
    }
    PassMark("frame start %u%u", 0, 0);
    if (perf && g_timestamps)
        C().list->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2);
    // A frame that ran out of ring: logged at most once a second, whatever else is logged (the ring
    // full messages stop after 20), since every refused allocation drops a draw or a texture.
    const uint32_t refusedThisFrame = f.ringRefused.exchange(0);
    if (const uint32_t refused = refusedThisFrame)
    {
        static auto lastRingLog = std::chrono::steady_clock::time_point{};
        if (const auto now = std::chrono::steady_clock::now(); now - lastRingLog >= std::chrono::seconds(1))
        {
            lastRingLog = now;
            Log("ring: frame %llu refused %u uploads, draws skipped or textures missing (used %llu MB of %llu)", (unsigned long long)g_r.frame,
                refused, (unsigned long long)(f.ringUsed.load() >> 20), (unsigned long long)(f.ringSize >> 20));
        }
    }
    if (const UINT64 used = f.ringUsed.load(); used > g_ringPeak)
    {
        g_ringPeak = used;
        if (used > f.ringSize / 2)
            Log("renderer: upload ring peak %llu MB of %llu", (unsigned long long)(used >> 20), (unsigned long long)(f.ringSize >> 20));
    }
    // A frame that ran out of ring, or used more than three quarters of it, doubles the ring for the
    // frames after it (to 2048 MB at most): each frame's ring is replaced here, at its own start, once
    // the GPU is done with it, so a single frame can still have run short but the next ones don't.
    // REGPU_RING_MB fixes the size; REGPU_RING_NO_GROW=1 keeps the start size, refusing what doesn't
    // fit (as before; A/B).
    {
        static LiveSwitch noGrow("REGPU_RING_NO_GROW");
        static const bool fixedSize = getenv("REGPU_RING_MB") != nullptr;
        const UINT64 used = f.ringUsed.load();
        // (Only a frame already at the current size decides: one still on a smaller ring is about to get the bigger one.)
        if (!noGrow && !fixedSize && f.ringSize >= kRingSize && (refusedThisFrame || used > f.ringSize / 4 * 3) && kRingSize < (2048ull << 20))
        {
            const UINT64 grown = std::min<UINT64>(kRingSize * 2, 2048ull << 20);
            Log("ring: growing the upload ring from %llu to %llu MB (frame %llu used %llu MB%s)", (unsigned long long)(kRingSize >> 20),
                (unsigned long long)(grown >> 20), (unsigned long long)g_r.frame, (unsigned long long)(used >> 20), refusedThisFrame ? ", refused uploads" : "");
            kRingSize = grown;
        }
        if (f.ringSize < kRingSize && !noGrow)
        {
            f.garbage.push_back(std::move(f.ring));
            f.ring = CreateBuffer(kRingSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            f.ring->Map(0, nullptr, reinterpret_cast<void**>(&f.ringCpu));
            f.ringSize = kRingSize;
        }
    }
    f.ringUsed = 0;
    f.uploads.clear();
    // Per-frame table: t0 this frame's ring, t1 guest physical memory.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.NumElements = UINT(f.ringSize / 4);
        raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g_r.device->CreateShaderResourceView(f.ring.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase, g_r.srvStride));
        if (g_r.physical)
        {
            raw.Buffer.NumElements = UINT(g_r.physical->GetDesc().Width / 4);
            g_r.device->CreateShaderResourceView(g_r.physical.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase + 1, g_r.srvStride));
        }
        else
            g_r.device->CreateShaderResourceView(f.ring.Get(), &raw, Cpu(g_r.srvHeap.Get(), f.descriptorBase + 1, g_r.srvStride));
    }
    f.descriptorsUsed = 2;
    EvictBindless();
    ReclaimTextureBlocks();
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    C().list->SetDescriptorHeaps(1, heaps);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    g_drawState.Invalidate();
    SelectSurfaces(true);
    C().list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_r.listOpen = true;
}

// Allocates from the frame's upload ring; returns the offset (or ~0). `what` names the use in the
// log when the ring is full.
UINT64 RingAlloc(UINT64 bytes, UINT64 alignment = 256, const char* what = "constants")
{
    FrameResources& f = Frame();
    UINT64 used = f.ringUsed.load(std::memory_order_relaxed), offset;
    for (;;)
    {
        offset = (used + alignment - 1) & ~(alignment - 1);
        if (offset + bytes > f.ringSize || f.ringUsed.compare_exchange_weak(used, offset + bytes, std::memory_order_relaxed))
            break;
    }
    if (offset + bytes > f.ringSize)
    {
        f.ringRefused.fetch_add(1, std::memory_order_relaxed);
        static UINT64 lastFull = 0;
        static int fullLogs = 0;
        if ((RendererTracing() || fullLogs < 20) && lastFull != g_r.frame && ++fullLogs)
        {
            lastFull = g_r.frame;
            Log("ring full: %llu bytes requested (%s), %llu used, %llu uploads cached", (unsigned long long)bytes, what, (unsigned long long)f.ringUsed.load(),
                (unsigned long long)f.uploads.size());
        }
        return ~0ull;
    }
    return offset;
}

// A large vertex buffer the title rewrites from frame to frame (skinning palettes): read in
// place, the title's next frame can overwrite it before the host GPU runs this one (its fences
// are signalled when recorded), so it is copied while it keeps changing. Checked once per frame
// with the sampled hash. REGPU_DIRECT_DYNAMIC keeps them in place (A/B).
bool DynamicBuffer(uint32_t address, uint32_t bytes)
{
    static LiveSwitch keepDirect("REGPU_DIRECT_DYNAMIC");
    if (keepDirect)
        return false;
    struct Seen
    {
        uint64_t hash = 0, frame = ~0ull, dynamicUntil = 0;
    };
    static std::mutex mutex; // draw workers record segments in parallel
    static std::unordered_map<uint64_t, Seen> seen;
    std::lock_guard lock(mutex);
    Seen& s = seen[uint64_t(address) << 32 | bytes];
    // Hashing is the cost: a buffer that stopped changing (static geometry, most of them) is
    // checked every 16th frame, one that changes every frame.
    const bool due = s.frame == ~0ull || g_r.frame <= s.dynamicUntil || g_r.frame >= s.frame + 16;
    if (s.frame != g_r.frame && due)
    {
        const uint64_t hash = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(address)), bytes);
        if (s.frame != ~0ull && hash != s.hash)
            s.dynamicUntil = g_r.frame + 120;
        s.hash = hash;
        s.frame = g_r.frame;
        // REGPU_TRACE_DYNAMIC=1: the buffers being copied, every 600 frames.
        static LiveSwitch trace("REGPU_TRACE_DYNAMIC");
        static uint64_t lastLog = 0;
        if (trace && g_r.frame >= lastLog + 600)
        {
            lastLog = g_r.frame;
            uint32_t n = 0, total = 0;
            uint64_t bytesTotal = 0;
            for (const auto& [key, e] : seen)
            {
                total++;
                if (g_r.frame <= e.dynamicUntil)
                {
                    n++;
                    bytesTotal += uint32_t(key);
                    if (n <= 12)
                        Log("dynamic: buffer %08X %u bytes", uint32_t(key >> 32), uint32_t(key));
                }
            }
            Log("dynamic: %u of %u large buffers copied, %llu KB", n, total, (unsigned long long)(bytesTotal >> 10));
        }
    }
    return g_r.frame <= s.dynamicUntil;
}

// REGPU_PERF: UploadGuest's bytes hashed and copied (logged with the frame stats).
struct UploadStats
{
    std::atomic<uint64_t> hashed{ 0 }, copied{ 0 }, hits{ 0 }, copies{ 0 };
} g_uploadStats;

// Copies a guest range into the ring once per frame; draws sharing a vertex
// buffer reuse the copy. A hash of all its bytes catches ranges rewritten mid-frame.
// Zeros follow the copy up to `zeroTo` bytes.
UINT64 UploadGuest(uint32_t guest, uint32_t bytes, uint32_t zeroTo)
{
    FrameResources& f = Frame();
    const uint8_t* src = GuestPtr<uint8_t>(guest);
    // Every copy is checked with a hash of all its bytes before a draw reuses it: a title can still be
    // filling a buffer when the first draw of the frame copies it, and a sampled check let later draws
    // reuse that half-filled copy (unwritten vertices: missing geometry, or huge triangles that hung the
    // GPU). REGPU_UPLOAD_SAMPLED_HASH=1 (live): one dword per 256 bytes and the last 64 bytes (ContentHash's
    // default), which was all this check read before. REGPU_UPLOAD_FULL_HASH_KB=<n>: buffers over n KB are
    // sampled at 16 points (A/B; 16 was the old size).
    uint64_t sample = bytes;
    static const uint32_t fullHashBytes = getenv("REGPU_UPLOAD_FULL_HASH_KB") ? uint32_t(atoi(getenv("REGPU_UPLOAD_FULL_HASH_KB"))) * 1024u : ~0u;
    static LiveSwitch uploadSampled("REGPU_UPLOAD_SAMPLED_HASH");
    if (bytes <= fullHashBytes)
        sample = uploadSampled ? ContentHash(src, bytes) : FullHash(src, bytes);
    else
        for (uint32_t i = 0; i < 16 && bytes >= 8; i++)
        {
            uint64_t v;
            memcpy(&v, src + (uint64_t(bytes - 8) * i / 15), 8);
            sample = (sample ^ v) * 0x100000001B3ull;
        }
    const uint32_t total = std::max(bytes, zeroTo);
    const uint64_t key = (uint64_t(guest) << 32 | bytes) ^ (uint64_t(total - bytes) * 0x9E3779B97F4A7C15ull);
    // Each recording thread keeps its own cache (cleared when the frame changes).
    thread_local FrameMap<std::pair<UINT64, uint64_t>> uploads;
    g_uploadStats.hashed += bytes;
    if (const auto* known = uploads.Find(key, g_r.frame); known && known->second == sample)
    {
        g_uploadStats.hits++;
        return known->first;
    }
    g_uploadStats.copies++;
    g_uploadStats.copied += bytes;
    if (RendererTracing() && bytes > (16u << 10))
        Log("upload: %08X %u bytes (ring %llu)", guest, bytes, (unsigned long long)f.ringUsed.load());
    const UINT64 offset = RingAlloc(std::max<uint32_t>(total, 16) + 16, 16, "vertex data");
    if (offset == ~0ull)
        return offset;
    memcpy(f.ringCpu + offset, src, bytes);
    memset(f.ringCpu + offset + bytes, 0, total - bytes);
    uploads.Put(key, g_r.frame, { offset, sample });
    return offset;
}

// Index buffers for indexed draws: the guest's big-endian indices, swapped once into an
// arena on the GPU and checked for changes once per frame (sampled, like textures). Real
// indexed draws let the GPU reuse transformed vertices; the vertex shader then gets the
// index as its vertex ID ("auto" index mode, see hlsl.cpp).
struct IndexArena
{
    static constexpr uint64_t kBytes = 128ull << 20;
    ComPtr<ID3D12Resource> buffer;
    uint64_t used = 0;
    std::unordered_map<uint32_t, std::vector<uint64_t>> free; // offsets by size class (log2)
    struct Entry
    {
        uint64_t offset = 0, hash = 0, checkedFrame = ~0ull;
        uint32_t sizeClass = 0;
        D3D12_GPU_VIRTUAL_ADDRESS staged = 0; // this frame's copy in the upload ring (frame stagedFrame)
        uint64_t stagedFrame = ~0ull;
    };
    std::unordered_map<uint64_t, Entry> entries;
    bool full = false;
    // Copies into the arena, made at the end of the frame in one batch (a barrier
    // on the arena mid-frame would drain the GPU each time new geometry streams in).
    struct Copy
    {
        ID3D12Resource* ring;
        uint64_t from, to, bytes;
    };
    std::vector<Copy> pending;
} g_indexArena;

// This frame's index uploads, copied into the arena once its draws are recorded.
void FlushIndexCopies()
{
    IndexArena& arena = g_indexArena;
    if (arena.pending.empty())
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = arena.buffer.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    C().list->ResourceBarrier(1, &b);
    for (const auto& c : arena.pending)
        C().list->CopyBufferRegion(arena.buffer.Get(), c.to, c.ring, c.from, c.bytes);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    arena.pending.clear();
}

// The view of the guest index buffer at physical address `base` (count indices), swapped
// into the arena; false when it cannot be cached (the draw fetches indices itself).
bool GetIndexBufferLocked(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view);
// Each thread remembers this frame's index buffers (checked once per frame).
bool GetIndexBuffer(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view)
{
    thread_local FrameMap<D3D12_INDEX_BUFFER_VIEW> known;
    const uint64_t key = uint64_t(base) << 32 ^ uint64_t(count) << 3 ^ uint64_t(is32) << 2 ^ endian;
    if (const auto* v = known.Find(key, g_r.frame))
    {
        view = *v;
        return true;
    }
    std::lock_guard textureLock(g_textureMutex);
    if (!GetIndexBufferLocked(base, count, is32, endian, view))
        return false;
    known.Put(key, g_r.frame, view);
    return true;
}

bool GetIndexBufferLocked(uint32_t base, uint32_t count, bool is32, uint32_t endian, D3D12_INDEX_BUFFER_VIEW& view)
{
    IndexArena& arena = g_indexArena;
    const uint32_t bytes = count * (is32 ? 4 : 2);
    if (!bytes || bytes > (16u << 20))
        return false;
    if (arena.full && arena.entries.size())
    {
        // Start over: copies in this frame run after the frames in flight are done.
        arena.entries.clear();
        arena.free.clear();
        arena.used = 0;
        arena.full = false;
    }
    if (!arena.buffer)
    {
        arena.buffer = CreateBuffer(IndexArena::kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        if (!arena.buffer)
            return false;
    }
    const uint64_t key = uint64_t(base) << 32 ^ uint64_t(bytes) << 3 ^ uint64_t(is32) << 2 ^ endian;
    auto [it, inserted] = arena.entries.try_emplace(key);
    IndexArena::Entry& e = it->second;
    if (inserted)
    {
        uint32_t sizeClass = 8; // 256 bytes
        while ((1ull << sizeClass) < bytes)
            sizeClass++;
        auto& list = arena.free[sizeClass];
        if (!list.empty())
        {
            e.offset = list.back();
            list.pop_back();
        }
        else if (arena.used + (1ull << sizeClass) <= IndexArena::kBytes)
        {
            e.offset = arena.used;
            arena.used += 1ull << sizeClass;
        }
        else
        {
            arena.entries.erase(it);
            arena.full = true;
            return false;
        }
        e.sizeClass = sizeClass;
    }
    view = { arena.buffer->GetGPUVirtualAddress() + e.offset, bytes, is32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
    if (e.checkedFrame == g_r.frame)
    {
        if (e.stagedFrame == g_r.frame)
            view.BufferLocation = e.staged;
        return true;
    }
    e.checkedFrame = g_r.frame;
    const uint8_t* src = GuestPtr<uint8_t>(GpuPhysToGuest(base));
    // Every index, not a sample: a game that streams new geometry into the same address and size
    // can change only words between the samples, and the old indices drew on (FM2 port, report #4:
    // black wedges of track). REGPU_INDEX_SAMPLED_HASH=1 samples every 256 bytes as before (A/B).
    static LiveSwitch sampledIndexHash("REGPU_INDEX_SAMPLED_HASH");
    const uint64_t hash = sampledIndexHash ? ContentHash(src, bytes) : FullHash(src, bytes);
    if (!inserted && hash == e.hash)
        return true;
    // Swap into the upload ring (as the vertex shader would read them), then copy.
    const UINT64 offset = RingAlloc((bytes + 15) & ~15u, 16, "index buffer");
    if (offset == ~0ull)
        return false;
    uint8_t* dst = Frame().ringCpu + offset;
    if (is32)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t v;
            memcpy(&v, src + i * 4, 4);
            if (endian == 2)
                v = __builtin_bswap32(v);
            else if (endian == 1)
                v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
            memcpy(dst + i * 4, &v, 4);
        }
    else
        for (uint32_t i = 0; i < count; i++)
        {
            uint16_t v;
            memcpy(&v, src + i * 2, 2);
            if (endian == 1 || endian == 2)
                v = __builtin_bswap16(v);
            memcpy(dst + i * 2, &v, 2);
        }
    // This frame draws from the upload ring; the arena gets the copy at the end of the frame.
    e.staged = Frame().ring->GetGPUVirtualAddress() + offset;
    e.stagedFrame = g_r.frame;
    view.BufferLocation = e.staged;
    arena.pending.push_back({ Frame().ring.Get(), offset, e.offset, bytes });
    e.hash = hash;
    return true;
}

// Compile activity, for the on-screen overlay and REGPU_PERF.
struct CompileStats
{
    std::atomic<int> busy{ 0 };            // jobs (shader compiles, pipeline builds) in flight
    std::atomic<uint32_t> shadersBuilt{ 0 }, shadersCached{ 0 }, pipelinesBuilt{ 0 }, pipelinesLoaded{ 0 };
    std::atomic<uint64_t> lastColdMs{ 0 };  // steady-clock ms of the last real compile
    std::atomic<uint32_t> warmTotal{ 0 }, warmDone{ 0 };
    std::atomic<uint32_t> staticDone{ 0 }, staticTotal{ 0 }; // first boot: static shaders compiled / to compile
    std::mutex mutex;
    std::string current;                    // what the oldest in-flight job is
    std::chrono::steady_clock::time_point currentSince;
    void Begin(const std::string& what)
    {
        std::lock_guard lock(mutex);
        if (busy.load() == 0)
            currentSince = std::chrono::steady_clock::now();
        current = what;
        busy++;
    }
    void End(bool cold)
    {
        busy--;
        if (cold)
            lastColdMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
} g_stats;

// Everything the title has ever bound is recorded (shader microcode as files in
// shadercache/, pipeline states in pipelines.manifest) so later runs rebuild all of
// it in the background at boot instead of stalling the first time a level needs it.
// Cache files written off the command processor thread: opening a file can take
// milliseconds (more with a virus scanner), which showed as frame spikes whenever a level
// met a new pipeline.
struct DiskWriter
{
    std::mutex mutex;
    std::condition_variable cv, idle;
    std::deque<std::function<void()>> queue;
    bool busy = false;

    void Push(std::function<void()> job)
    {
        static std::once_flag started;
        std::call_once(started, [this] {
            std::thread([this] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;)
                {
                    std::function<void()> next;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait(lock, [this] { return !queue.empty(); });
                        next = std::move(queue.front());
                        queue.pop_front();
                        busy = true;
                    }
                    next();
                    std::lock_guard lock(mutex);
                    busy = false;
                    if (queue.empty())
                        idle.notify_all();
                }
            }).detach();
        });
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }
    // Waits until everything queued is written (at exit).
    void Flush()
    {
        std::unique_lock lock(mutex);
        idle.wait_for(lock, std::chrono::seconds(5), [this] { return queue.empty() && !busy; });
    }
} g_diskWriter;

struct Manifest
{
    std::mutex mutex;
    std::unordered_set<PipelineKey, PipelineKeyHash> known;
    std::vector<PipelineKey> pending; // loaded at start, not built yet
    size_t next = 0;
    std::unordered_set<uint64_t> shaderFiles;

    static std::filesystem::path Path() { return UserDir() / "pipelines.manifest"; }
    static std::filesystem::path UcodePath(bool pixel, uint64_t hash)
    {
        char name[48];
        snprintf(name, sizeof(name), "%s_%016llx.ucode", pixel ? "ps" : "vs", (unsigned long long)hash);
        return UserDir() / "shadercache" / name;
    }
    void Load()
    {
        std::ifstream in(Path());
        std::string line;
        while (std::getline(in, line))
        {
            PipelineKey k{};
            unsigned long long vs, ps;
            if (sscanf(line.c_str(), "P %llx %llx %x %x %x %x", &vs, &ps, &k.blend, &k.depth, &k.stencilMaskMode, &k.flags) == 6)
            {
                k.vs = vs;
                k.ps = ps;
                if (known.insert(k).second)
                    pending.push_back(k);
            }
        }
        g_stats.warmTotal = uint32_t(pending.size());
        Log("renderer: manifest lists %zu pipelines to rebuild", pending.size());
    }
    void RecordPipeline(const PipelineKey& k)
    {
        std::lock_guard lock(mutex);
        if (!known.insert(k).second)
            return;
        g_diskWriter.Push([k] {
            std::ofstream(Path(), std::ios::app) << "P " << std::hex << k.vs << ' ' << k.ps << ' ' << k.blend << ' ' << k.depth << ' '
                                                  << k.stencilMaskMode << ' ' << k.flags << '\n';
        });
    }
    void RecordShader(bool pixel, uint64_t hash, const std::vector<uint32_t>& codeBE)
    {
        std::lock_guard lock(mutex);
        if (codeBE.empty() || !shaderFiles.insert(hash).second)
            return;
        g_diskWriter.Push([path = UcodePath(pixel, hash), code = codeBE] {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
        });
    }
    static bool LoadShader(bool pixel, uint64_t hash, std::vector<uint32_t>& out)
    {
        std::ifstream in(UcodePath(pixel, hash), std::ios::binary | std::ios::ate);
        if (!in)
            return false;
        out.resize(size_t(in.tellg()) / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size() * 4));
        return !out.empty();
    }
} g_manifest;

// Shader compilation runs on worker threads so the command processor never
// stalls on fxc: a draw whose shaders are still compiling is skipped. The
// bytecode is cached on disk by HLSL hash, so later runs start warm.
// Compiles that something waits for skip fxc's optimiser (a large shader can take seconds with
// it). REGPU_OPTIMIZE_SHADERS=1 also builds each shader optimised on a background thread and
// stores it beside the unoptimised one (<key>.o.dxbc); later runs load that. The running
// session keeps the bytecode its pipelines were built from.
const bool g_optimizeShaders = getenv("REGPU_OPTIMIZE_SHADERS") != nullptr;
// REGPU_OPTIMIZE_LEVEL (1-3, default 1): the fxc optimisation level of REGPU_OPTIMIZE_SHADERS' background builds. Higher
// levels take longer to compile and may run faster on a weak GPU; other levels cache in files of their own.
int OptimizeLevel()
{
    static const int level = [] {
        const char* v = getenv("REGPU_OPTIMIZE_LEVEL");
        const int n = v ? atoi(v) : 1;
        return n < 1 ? 1 : n > 3 ? 3 : n;
    }();
    return level;
}

std::filesystem::path ShaderCachePath(uint64_t key, bool optimized = false)
{
    char name[40];
    if (optimized && OptimizeLevel() != 1)
        snprintf(name, sizeof(name), "%016llx.o%d.dxbc", (unsigned long long)key, OptimizeLevel());
    else
        snprintf(name, sizeof(name), optimized ? "%016llx.o.dxbc" : "%016llx.dxbc", (unsigned long long)key);
    return UserDir() / "shadercache" / name;
}

// REGPU_OPTIMIZE_SHADERS: optimised builds, one at a time at the lowest priority.
struct ShaderOptimizer
{
    struct Job
    {
        uint64_t key;
        bool pixel;
        std::string hlsl;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::unordered_set<uint64_t> queued;
    std::atomic<uint32_t> built{ 0 };

    void Push(uint64_t key, bool pixel, const std::string& hlsl)
    {
        {
            std::lock_guard lock(mutex);
            if (!queued.insert(key).second)
                return;
            jobs.push_back({ key, pixel, hlsl });
        }
        static std::once_flag started;
        std::call_once(started, [this] { std::thread([this] { Run(); }).detach(); });
        cv.notify_one();
    }

    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(job.hlsl.data(), job.hlsl.size(), nullptr, nullptr, nullptr, "main",
                    job.pixel ? "ps_5_1" : "vs_5_1",
                    (OptimizeLevel() == 3 ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : OptimizeLevel() == 2 ? D3DCOMPILE_OPTIMIZATION_LEVEL2 : D3DCOMPILE_OPTIMIZATION_LEVEL1) |
                        D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0,
                    &code, &errors)))
                continue; // the unoptimised build stays
            // Written under a temporary name: a run that reads it never sees half a file.
            const auto path = ShaderCachePath(job.key, true);
            auto tmp = path;
            tmp += ".tmp";
            std::ofstream(tmp, std::ios::binary).write(static_cast<const char*>(code->GetBufferPointer()), std::streamsize(code->GetBufferSize()));
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (const uint32_t n = ++built; n == 1 || n % 100 == 0)
                Log("renderer: %u shaders optimised in the background", n);
        }
    }
} g_optimizer;

// The stored bytecode for cs.cacheKey, optimised first; an unoptimised one is queued for its
// optimised build.
bool LoadCachedBytecode(CompiledShader& cs)
{
    for (const bool optimized : { true, false })
    {
        if (optimized && !g_optimizeShaders)
            continue;
        std::ifstream in{ ShaderCachePath(cs.cacheKey, optimized), std::ios::binary | std::ios::ate };
        if (!in)
            continue;
        const auto size = size_t(in.tellg());
        if (!size || FAILED(D3DCreateBlob(size, &cs.code)))
            continue;
        in.seekg(0);
        in.read(static_cast<char*>(cs.code->GetBufferPointer()), std::streamsize(size));
        if (!optimized && g_optimizeShaders)
            g_optimizer.Push(cs.cacheKey, cs.pixel, cs.info.hlsl);
        return true;
    }
    return false;
}

struct ShaderCompiler
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<CompiledShader*> queue;
    std::deque<CompiledShader*> background; // static shaders: compiled when nothing urgent waits
    std::vector<std::thread> workers;
    std::atomic<int> jobs{ 0 };   // urgent, queued or compiling (holds wait for these)
    std::atomic<int> waiting{ 0 }; // background, queued or parked
    // Once the game runs, background pushes (memory-scan finds: mostly stale copies the
    // game never draws) are parked instead: translated, compiled only if a draw promotes them.
    std::atomic<bool> park{ false };
    std::unordered_set<const CompiledShader*> parked;

    void Start()
    {
        std::filesystem::create_directories(UserDir() / "shadercache");
        // All but one core: the first boot compiles the whole game behind the overlay.
        const unsigned n = std::max(2u, std::thread::hardware_concurrency() - 1);
        for (unsigned i = 0; i < n; i++)
            workers.emplace_back([this] { Run(); });
        for (auto& w : workers)
            w.detach();
    }

    void Push(CompiledShader* cs, bool later = false)
    {
        {
            std::lock_guard lock(mutex);
            if (later)
            {
                waiting++;
                if (park.load())
                    parked.insert(cs);
                else
                    background.push_back(cs);
            }
            else
            {
                jobs++;
                queue.push_back(cs);
            }
        }
        cv.notify_one();
    }

    // Moves a background shader something now needs to the front of the urgent queue.
    void Promote(const CompiledShader& cs)
    {
        if (cs.state.load(std::memory_order_relaxed) != CompiledShader::Pending || !waiting.load(std::memory_order_relaxed))
            return;
        std::lock_guard lock(mutex);
        bool found = false;
        if (parked.erase(&cs))
            found = true;
        else if (auto it = std::find(background.begin(), background.end(), &cs); it != background.end())
        {
            background.erase(it);
            found = true;
        }
        if (found)
        {
            waiting--;
            jobs++;
            queue.push_front(const_cast<CompiledShader*>(&cs));
        }
    }

    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            CompiledShader* cs;
            bool urgent;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !queue.empty() || !background.empty(); });
                urgent = !queue.empty();
                auto& from = urgent ? queue : background;
                cs = from.front();
                from.pop_front();
                if (!urgent)
                    waiting--;
            }
            if (urgent)
            {
                char label[64];
                snprintf(label, sizeof(label), "%s %08llx", cs->pixel ? "pixel shader" : "vertex shader", (unsigned long long)(cs->hash >> 32));
                g_stats.Begin(label);
                Compile(*cs);
                g_stats.End(true);
                jobs--;
            }
            else
                Compile(*cs);
        }
    }

    static void Compile(CompiledShader& cs)
    {
        ComPtr<ID3DBlob> errors;
        // Runtime compiles skip optimisation: fxc can take minutes on the
        // larger translated shaders.
        const auto start = std::chrono::steady_clock::now();
        const HRESULT hr = D3DCompile(cs.info.hlsl.data(), cs.info.hlsl.size(), nullptr, nullptr, nullptr, "main",
            cs.pixel ? "ps_5_1" : "vs_5_1", D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &cs.code, &errors);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (FAILED(hr))
        {
            Log("renderer: %s %016llx failed to compile:\n%s", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            cs.state.store(CompiledShader::Failed, std::memory_order_release);
            return;
        }
        g_stats.shadersBuilt++;
        if (ms > 200)
            Log("renderer: %s %016llx compiled in %.0f ms (background)", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash, ms);
        std::ofstream(ShaderCachePath(cs.cacheKey), std::ios::binary)
            .write(static_cast<const char*>(cs.code->GetBufferPointer()), std::streamsize(cs.code->GetBufferSize()));
        cs.state.store(CompiledShader::Ready, std::memory_order_release);
    }
} g_compiler;

// Shaders by canonical key: the microcode with the vertex fetch fields the title patches at
// load masked out (the prefetch count too: bits 27-29 of the fetch's first dword). A patched
// copy of a translated shader shares its HLSL and bytecode and only needs its own fetch layout
// (TranslateVertexFetchLayout), not a full translation (which took up to ~18 ms on the command
// processor for a large shader).
std::unordered_map<uint64_t, const CompiledShader*> g_canonical;
// Bytecode already loaded, by HLSL hash: a shader whose HLSL matches one in memory doesn't
// read the shader cache from disk again.
std::unordered_map<uint64_t, ComPtr<ID3DBlob>> g_bytecode;

uint64_t CanonicalKey(const ucode::Shader& parsed)
{
    std::vector<uint32_t> w = parsed.words;
    for (const ucode::Cf& cf : parsed.cf)
    {
        if (cf.IsExec())
            for (uint32_t k = 0; k < cf.count(); k++)
            {
                const uint32_t index = cf.address() + k;
                if (!((cf.sequence() >> (2 * k)) & 1) || (index + 1) * 3 > w.size() || (w[index * 3] & 0x1F) != 0)
                    continue;
                w[index * 3] &= ~0x3FF00000u;   // fetch constant, its index select, prefetch count
                w[index * 3 + 1] &= 0x80000000u; // swizzle, format, sign, rounding, exponent, mini
                w[index * 3 + 2] &= 0x80000000u; // stride, offset
            }
        if (cf.IsEnd())
            break;
    }
    return Fnv(w.data(), w.size() * 4, 0x5A17);
}

// background: queued behind everything a draw needs (the static shader set).
// Live pixel shader edits (the debug server's "ps HASH EXPR"): draws with that shader use an alias hash
// instead, translated afresh with its output replaced, so they get their own pipelines and the original
// shader is untouched (shaders are never removed). A new edit gets a new alias; clearing it goes back.
std::mutex g_psEditMutex;
std::unordered_map<uint64_t, uint64_t> g_psEditAlias; // shader -> alias
std::unordered_map<uint64_t, std::string> g_psEditExpr; // alias -> the expression it returns
std::atomic<bool> g_psEdits{ false };
std::unordered_set<uint64_t> g_psEditEver; // every alias made (kept out of the pipeline manifest)
// The debug server's drawstats: draws per (vs, ps) over one frame.
std::atomic<int> g_drawStats{ 0 }; // 1 armed for the next frame, 2 counting, 0 off
std::mutex g_drawStatsMutex;
std::map<std::pair<uint64_t, uint64_t>, uint32_t> g_drawStatsCounts;
bool IsPsEditAlias(uint64_t hash)
{
    std::lock_guard lock(g_psEditMutex);
    return g_psEditEver.count(hash) != 0;
}
uint64_t PsEditAlias(uint64_t hash)
{
    if (!g_psEdits.load(std::memory_order_relaxed))
        return hash;
    std::lock_guard lock(g_psEditMutex);
    auto it = g_psEditAlias.find(hash);
    return it == g_psEditAlias.end() ? hash : it->second;
}

const CompiledShader& GetShaderLocked(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background);
// Shaders are never removed: each thread remembers the ones it looked up (no lock per draw).
const CompiledShader& GetShader(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background = false)
{
    thread_local std::unordered_map<uint64_t, const CompiledShader*> known;
    if (auto it = known.find(hash); it != known.end())
        return *it->second;
    std::lock_guard shaderLock(g_shaderMutex);
    const CompiledShader& cs = GetShaderLocked(hash, codeBE, pixel, background);
    known.emplace(hash, &cs);
    return cs;
}

const CompiledShader& GetShaderLocked(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel, bool background)
{
    auto it = g_r.shaders.find(hash);
    if (it != g_r.shaders.end())
    {
        if (!background)
            g_compiler.Promote(*it->second);
        return *it->second;
    }
    static std::once_flag started;
    std::call_once(started, [] { g_compiler.Start(); });
    auto& slot = g_r.shaders[hash];
    slot = std::make_unique<CompiledShader>();
    CompiledShader& cs = *slot;
    cs.hash = hash;
    cs.pixel = pixel;
    // Not saved yet: the memory scan also finds stale copies in freed blocks (heap links
    // over the first words), so only shaders a pipeline uses go into the cache.
    cs.ucodeBE = codeBE;
    const auto getStart = std::chrono::steady_clock::now();
    static LiveSwitch perfLog("REGPU_PERF");
    const char* path = "full";
    struct LogSlow
    {
        std::chrono::steady_clock::time_point start;
        const CompiledShader& cs;
        const char*& path;
        ~LogSlow()
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (perfLog && ms > 5.0)
                Log("perf: GetShader %s %016llx took %.1f ms (%s, %zu dwords)", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash, ms, path,
                    cs.ucodeBE.size());
        }
    } logSlow{ getStart, cs, path };
    const ucode::Shader parsed = ucode::Parse(codeBE.data(), uint32_t(codeBE.size()), pixel);
    static LiveSwitch noCanonical("REGPU_NO_CANONICAL");
    const uint64_t canonical = pixel || noCanonical || IsPsEditAlias(hash) ? 0 : CanonicalKey(parsed);
    if (canonical)
        path = g_canonical.count(canonical) ? (g_canonical[canonical]->IsReady() ? "full, layout fallback" : "full, base not ready") : "full, new canonical";
    if (canonical)
        if (auto c = g_canonical.find(canonical); c != g_canonical.end() && c->second->IsReady())
        {
            const CompiledShader& base = *c->second;
            TranslatedShader layout;
            if (TranslateVertexFetchLayout(parsed, layout))
            {
                cs.info.ok = true;
                cs.info.usedTextureSlots = base.info.usedTextureSlots;
                cs.info.constantsUsed = base.info.constantsUsed; // same ALU code
                cs.info.vertexFetchLayout = std::move(layout.vertexFetchLayout);
                cs.info.vertexFetchConstants = std::move(layout.vertexFetchConstants);
                cs.cacheKey = base.cacheKey;
                cs.code = base.code;
                cs.hlslSource = &base;
                cs.tag = base.tag;
                cs.state = CompiledShader::Ready;
                g_stats.shadersCached++;
                path = "variant";
                return cs;
            }
        }
    cs.info = TranslateShader(parsed);
    // Only a translation whose fetches all read vfl can stand in for its patched copies; one
    // with baked fetch fields would hand them the wrong layout.
    if (canonical && cs.info.ok)
        if (TranslatedShader check; TranslateVertexFetchLayout(parsed, check))
            g_canonical.emplace(canonical, &cs);
    // Memory export writes guest memory through u0, which the root signature doesn't bind yet
    // (and the CPU doesn't see the written range): not drawn until that is in place.
    if (cs.info.ok && cs.info.memExport)
    {
        cs.info.ok = false;
        cs.info.error = "memory export (eM#) needs the u0 guest-memory binding";
    }
    if (!cs.info.ok)
    {
        Log("renderer: %s %016llx not translated: %s", pixel ? "ps" : "vs", (unsigned long long)hash, cs.info.error.c_str());
        cs.state = CompiledShader::Failed;
        return cs;
    }
    // The game's own fixes to this shader (regpu.h Host::rewriteShader).
    if (const auto rewrite = regpu::GetHost().rewriteShader)
        cs.tag = rewrite(hash, cs.info.hlsl, pixel);
    if (g_dumpHlsl)
    {
        std::filesystem::create_directories("out/hlsl");
        char name[64];
        snprintf(name, sizeof(name), "out/hlsl/%s_%016llx.hlsl", pixel ? "ps" : "vs", (unsigned long long)hash);
        if (FILE* f = fopen(name, "wb"))
        {
            fwrite(cs.info.hlsl.data(), 1, cs.info.hlsl.size(), f);
            fclose(f);
        }
    }
    // Debug: REGPU_DEBUG_VS=<hash>:<statement> appends a statement to that vertex
    // shader's guest program (it can read r[] and write o[]).
    if (const char* dbg = getenv("REGPU_DEBUG_VS"); dbg && !pixel && strtoull(dbg, nullptr, 16) == hash)
        if (const char* stmt = strchr(dbg, ':'))
        {
            // "<hash>:@<block>:<statement>" inserts at the start of control-flow block <block>.
            std::string s = stmt + 1, marker = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
            if (s[0] == '@')
            {
                const size_t colon = s.find(':');
                marker = "        if (cfpc == " + s.substr(1, colon - 1) + "u)\n        {\n";
                s = s.substr(colon + 1);
            }
            if (size_t at = cs.info.hlsl.find(marker); at != std::string::npos)
                cs.info.hlsl.insert(marker[0] == '}' ? at : at + marker.size(), "            " + s + "\n");
        }
    // Debug: REGPU_TINT_PS=h1,h2,... paints those pixel shaders in distinct flat
    // colours (red, green, blue, yellow, magenta, cyan, white, orange, ...).
    if (const char* list = getenv("REGPU_TINT_PS"); list && pixel)
    {
        static const char* kTints[] = { "1,0,0", "0,1,0", "0,0,1", "1,1,0", "1,0,1", "0,1,1", "1,1,1", "1,0.5,0", "0.5,0,1", "0,0.5,0.3",
            "0.5,0.5,0.5", "0.3,0.15,0" };
        int n = 0;
        for (const char* s = list; *s && n < 12; n++)
        {
            char* end;
            const uint64_t h = strtoull(s, &end, 16);
            if (h == hash)
                if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                    cs.info.hlsl.replace(at, 13, std::string("return float4(") + kTints[n] + ", 1);");
            s = *end == ',' ? end + 1 : end;
            if (end == s && *end != ',')
                break;
        }
    }
    // Debug: REGPU_DEBUG_PS=<hash>:<expr> replaces that pixel shader's output, e.g.
    // "a62c809452fcc71b:float4(input.o[4].xyz, 1)".
    // REGPU_DEBUG_PS_ALL=<expr> does the same for every pixel shader.
    if (const char* all = getenv("REGPU_DEBUG_PS_ALL"); all && pixel)
        if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
            cs.info.hlsl.replace(at, 13, std::string("return ") + all + ";");
    if (const char* dbg = getenv("REGPU_DEBUG_PS"); dbg && pixel && strtoull(dbg, nullptr, 16) == hash)
        if (const char* expr = strchr(dbg, ':'))
            if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                cs.info.hlsl.replace(at, 13, std::string("return ") + (expr + 1) + ";");
    if (!pixel && g_psEdits.load(std::memory_order_relaxed))
    {
        std::string stmt;
        {
            std::lock_guard lock(g_psEditMutex);
            if (auto e = g_psEditExpr.find(hash); e != g_psEditExpr.end())
                stmt = e->second;
        }
        const std::string marker = "}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n";
        // "@TEXT|STATEMENT" runs it after the first line holding TEXT instead of at the end.
        size_t at = std::string::npos;
        if (!stmt.empty() && stmt[0] == '@')
        {
            const size_t bar = stmt.find('|');
            if (bar != std::string::npos)
                if (size_t hit = cs.info.hlsl.find(stmt.substr(1, bar - 1)); hit != std::string::npos)
                    at = cs.info.hlsl.find('\n', hit) + 1;
            stmt = bar == std::string::npos ? "" : stmt.substr(bar + 1);
        }
        else if (!stmt.empty())
            at = cs.info.hlsl.find(marker);
        if (!stmt.empty() && at != std::string::npos && at != 0)
        {
            cs.info.hlsl.insert(at, "            " + stmt + "\n");
            Log("debug: vs alias %016llx runs %s", (unsigned long long)hash, stmt.c_str());
        }
        else if (!stmt.empty())
            Log("debug: vs alias %016llx: the anchor wasn't found", (unsigned long long)hash);
    }
    if (pixel && g_psEdits.load(std::memory_order_relaxed))
    {
        std::string expr;
        {
            std::lock_guard lock(g_psEditMutex);
            if (auto e = g_psEditExpr.find(hash); e != g_psEditExpr.end())
                expr = e->second;
        }
        if (!expr.empty())
        {
            if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                cs.info.hlsl.replace(at, 13, "return " + expr + ";");
            Log("debug: ps alias %016llx returns %s", (unsigned long long)hash, expr.c_str());
        }
    }
    // Debug edits name temporaries r[N], as the microcode does; the translation declares them as
    // r0..r63 unless REGPU_INDEXED_TEMPS=1 (hlsl.cpp IndexedTemps). The generated code itself
    // never writes r[<number>].
    if (!IndexedTemps() && (g_psEdits.load(std::memory_order_relaxed) || getenv("REGPU_DEBUG_VS") || getenv("REGPU_DEBUG_PS") || getenv("REGPU_DEBUG_PS_ALL")))
    {
        static const std::regex numbered(R"((^|[^A-Za-z0-9_])r\[(\d+)\])");
        cs.info.hlsl = std::regex_replace(cs.info.hlsl, numbered, "$1r$2");
    }
    cs.cacheKey = Fnv(cs.info.hlsl.data(), cs.info.hlsl.size(), pixel ? 1 : 2);
    if (auto b = g_bytecode.find(cs.cacheKey); b != g_bytecode.end())
    {
        cs.code = b->second;
        cs.state = CompiledShader::Ready;
        g_stats.shadersCached++;
        return cs;
    }
    if (LoadCachedBytecode(cs))
    {
        cs.state = CompiledShader::Ready;
        g_bytecode.emplace(cs.cacheKey, cs.code);
        g_stats.shadersCached++;
        return cs;
    }
    g_compiler.Push(&cs, background);
    return cs;
}

// Motion-vector variant of a translated shader (temporal upscalers). Built by rewriting the
// generated HLSL, so default shaders (and their caches) stay as they are:
// - the vertex constants gain last frame's c[] (vcPrev, see DrawImpl) and the frame's
//   jitter (vjitter, NDC units);
// - the vertex shader runs the guest program a second time with vcPrev (GuestVSPrev), puts
//   both clip positions in two more interpolants and jitters only the rasterized position;
// - the pixel shader also writes the motion to SV_Target1 as a UV offset (previous minus
//   current, in the 0..1 range of the target).
// Empty when the shader's text is not in the expected shape.
std::string MotionVariant(const std::string& hlsl, bool pixel)
{
    std::string h = hlsl;
    auto replace = [&](const std::string& from, const std::string& to) {
        const size_t at = h.find(from);
        if (at == std::string::npos)
            return false;
        h.replace(at, from.size(), to);
        return true;
    };
    if (!replace("    float4 vwide;\n};", "    float4 vwide;\n    float4 vjitter;\n};\ncbuffer VSPrevious : register(b2)\n{\n    float4 vcPrev[256];\n};") ||
        !replace("    float2 pcoord : TEXCOORD16; // point sprite coordinate\n",
            "    float2 pcoord : TEXCOORD16; // point sprite coordinate\n    float4 mcur : TEXCOORD17;\n    float4 mprev : TEXCOORD18;\n"))
        return {};
    if (pixel)
    {
        if (!replace("float4 main(VSOut input) : SV_Target0\n{", "struct PSMotionOut { float4 c : SV_Target0; float2 mv : SV_Target1; };\n"
                                                                    "PSMotionOut main(VSOut input)\n{") &&
            !replace("float4 main(VSOut input, out float oDepthOut : SV_Depth) : SV_Target0\n{",
                "struct PSMotionOut { float4 c : SV_Target0; float2 mv : SV_Target1; };\n"
                "PSMotionOut main(VSOut input, out float oDepthOut : SV_Depth)\n{"))
            return {};
        const size_t at = h.rfind("    return oC[0];\n}");
        if (at == std::string::npos)
            return {};
        h.replace(at, std::string("    return oC[0];\n}").size(),
            "    PSMotionOut motionOut;\n"
            "    motionOut.c = oC[0];\n"
            "    float2 cur = input.mcur.xy / input.mcur.w, prev = input.mprev.xy / input.mprev.w;\n"
            "    motionOut.mv = (prev - cur) * float2(0.5, -0.5);\n"
            "    return motionOut;\n}");
        return h;
    }
    // Vertex shader: GuestVSPrev, a copy of the guest program reading last frame's constants.
    const std::string sig = "void GuestVS(uint vindex, out float4 opos, out float4 o[16], out float4 opts)\n{";
    const size_t start = h.find(sig);
    const size_t end = h.find("}\nvoid GuestVS(uint vindex, out float4 opos, out float4 o[16])\n", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    std::string prev = h.substr(start, end + 2 - start);
    prev.replace(0, std::string("void GuestVS(").size(), "void GuestVSPrev(");
    for (size_t at = 0; (at = prev.find("vc[", at)) != std::string::npos; at += 7)
        prev.replace(at, 3, "vcPrev[");
    // The second run only computes last frame's position: it must not export to memory again.
    static const std::string exportCall = "MemExport(eA, eM, eMw);";
    for (size_t at = 0; (at = prev.find(exportCall, at)) != std::string::npos;)
        prev.erase(at, exportCall.size());
    h.insert(end + 2, prev);
    // The viewport transform (from the target size to the clip position) as a function.
    const std::string from = "    float2 size = float2(vinfo.zw);\n";
    const std::string last = "    r.pos = float4(ndc * w, p.z * w, w);\n";
    const size_t a = h.find(from), b = h.find(last);
    const size_t mainAt = h.find("VSOut main(uint id : SV_VertexID)\n{");
    if (a == std::string::npos || b == std::string::npos || mainAt == std::string::npos || b < a || a < mainAt)
        return {};
    std::string body = h.substr(a, b - a);
    for (size_t at = 0; (at = body.find("r.pos", at)) != std::string::npos; at += 3)
        body.replace(at, 5, "pos");
    const std::string fn = "float4 ToClip(float4 pos, float2 pointOffset)\n{\n" + body + "    return float4(ndc * w, p.z * w, w);\n}\n";
    h.replace(a, b + last.size() - a,
        "    r.pos = ToClip(r.pos, pointOffset);\n"
        "    r.mcur = r.pos;\n"
        "    r.mprev = r.pos;\n"
        "    if (!isPoint && !rect)\n"
        "    {\n"
        "        float4 q, qo[16], qpts;\n"
        "        GuestVSPrev(e, q, qo, qpts);\n"
        "        r.mprev = ToClip(q, pointOffset);\n"
        "    }\n"
        "    r.pos.xy += vjitter.xy * r.pos.w;\n");
    h.insert(mainAt, fn);
    // VSOut members the original code never sets.
    return h;
}

// The motion-vector variant of a shader (compiled on first use; nullptr if it cannot be made).
std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> g_motionShaders;
const CompiledShader* MotionShaderLookup(const CompiledShader& base);
const CompiledShader* MotionShader(const CompiledShader& base)
{
    if (base.motion && base.motion->state.load(std::memory_order_acquire) != CompiledShader::Pending)
        return base.motion->state.load(std::memory_order_acquire) == CompiledShader::Failed ? nullptr : base.motion;
    const CompiledShader* m = MotionShaderLookup(base);
    base.motion = m;
    return m;
}

const CompiledShader* MotionShaderLookup(const CompiledShader& base)
{
    const CompiledShader& source = base.hlslSource ? *base.hlslSource : base;
    auto [it, inserted] = g_motionShaders.try_emplace(source.cacheKey);
    if (!inserted)
        return it->second->state.load(std::memory_order_acquire) == CompiledShader::Failed ? nullptr : it->second.get();
    it->second = std::make_unique<CompiledShader>();
    CompiledShader& cs = *it->second;
    cs.hash = source.hash;
    cs.pixel = source.pixel;
    cs.info = source.info;
    cs.info.hlsl = MotionVariant(source.info.hlsl, source.pixel);
    if (cs.info.hlsl.empty())
    {
        LOG_ONCE("upscale: no motion-vector variant for %s %016llx", source.pixel ? "ps" : "vs", (unsigned long long)source.hash);
        cs.state = CompiledShader::Failed;
        return nullptr;
    }
    cs.cacheKey = Fnv(cs.info.hlsl.data(), cs.info.hlsl.size(), source.pixel ? 1 : 2);
    if (auto b = g_bytecode.find(cs.cacheKey); b != g_bytecode.end())
    {
        cs.code = b->second;
        cs.state = CompiledShader::Ready;
        return &cs;
    }
    if (LoadCachedBytecode(cs))
    {
        cs.state = CompiledShader::Ready;
        g_bytecode.emplace(cs.cacheKey, cs.code);
        return &cs;
    }
    g_compiler.Push(&cs, false);
    return &cs;
}

D3D12_BLEND Blend(uint32_t f)
{
    switch (f)
    {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: return D3D12_BLEND_BLEND_FACTOR;
    case 14: return D3D12_BLEND_ALPHA_FACTOR;
    case 13: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return D3D12_BLEND_INV_ALPHA_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND AlphaBlend(uint32_t f)
{
    // Colour factors are invalid for alpha in D3D12: use their alpha forms.
    switch (f)
    {
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_ALPHA;
    case 9: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return Blend(f);
    }
}

D3D12_BLEND_OP BlendOp(uint32_t op)
{
    switch (op)
    {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
    }
}

// Persistent pipeline library: compiled GPU code survives between runs
// (Config::userDir/pipelines.bin; keyed by the driver, so a driver update just misses).
struct PipelineLibrary
{
    ComPtr<ID3D12PipelineLibrary> library;
    std::atomic<int> stored{ 0 };
    std::mutex saveMutex;
    // Stores and loads share the library; serializing takes it alone. Serialize running
    // during a StorePipeline is not something the runtime promises to handle, and a cold
    // load with many pipelines building at once locked up (Mugello).
    std::shared_mutex access;
    std::chrono::steady_clock::time_point lastSave = std::chrono::steady_clock::now();
    std::vector<char> loaded;
    // Compaction: entries are never removed, so every translator or driver change leaves the old ones
    // behind (FM2 port: 574 MB read at every boot). Every pipeline this session loads or builds also
    // goes into `fresh`; once that holds every pipeline the manifest lists and is at least 10% smaller,
    // it replaces the old library and is what gets saved. REGPU_NO_LIBRARY_COMPACT=1 keeps the old one.
    ComPtr<ID3D12PipelineLibrary> fresh;
    std::atomic<size_t> freshCount{ 0 };

    static std::filesystem::path Path() { return UserDir() / "pipelines.bin"; }

    void Init(ID3D12Device* device)
    {
        ComPtr<ID3D12Device1> d1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d1))))
            return;
        if (std::ifstream in{ Path(), std::ios::binary | std::ios::ate })
        {
            loaded.resize(size_t(in.tellg()));
            in.seekg(0);
            in.read(loaded.data(), std::streamsize(loaded.size()));
        }
        if (FAILED(d1->CreatePipelineLibrary(loaded.data(), loaded.size(), IID_PPV_ARGS(&library))))
        {
            loaded.clear();
            if (FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))))
                library.Reset();
        }
        Log("renderer: pipeline library %s (%zu bytes loaded)", library ? "ready" : "unavailable", loaded.size());
        if (library && !loaded.empty() && !getenv("REGPU_NO_LIBRARY_COMPACT") && FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&fresh))))
            fresh.Reset();
    }

    // r3: the shaders' compiled code and the fixed-function state are part of the name. An entry only
    // loads with the description it was stored with, so when a translation or the renderer's state for
    // a key changed (depth bias, formats, sample count), the old entry kept the name and the new pipeline
    // was rebuilt at every launch instead of stored. REGPU_PIPELINE_NAME_SHADERS_ONLY=1 names by the key
    // alone, as before (r2: the root signature with the 3D texture range).
    static std::wstring Name(const PipelineKey& k, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd)
    {
        static const bool shadersOnly = getenv("REGPU_PIPELINE_NAME_SHADERS_ONLY") != nullptr;
        wchar_t name[128];
        if (shadersOnly)
        {
            swprintf(name, 128, L"r2_%016llx_%016llx_%08x_%08x_%08x_%08x", (unsigned long long)k.vs, (unsigned long long)k.ps, k.blend, k.depth,
                k.stencilMaskMode, k.flags);
            return name;
        }
        uint64_t code = Fnv(pd.VS.pShaderBytecode, pd.VS.BytecodeLength, 3) * 0x9E3779B97F4A7C15ull ^
            (pd.PS.pShaderBytecode ? Fnv(pd.PS.pShaderBytecode, pd.PS.BytecodeLength, 4) : 0);
        code = (code ^ Fnv(&pd.BlendState, sizeof(pd.BlendState), 5)) * 0x9E3779B97F4A7C15ull;
        code = (code ^ Fnv(&pd.RasterizerState, sizeof(pd.RasterizerState), 6)) * 0x9E3779B97F4A7C15ull;
        code = (code ^ Fnv(&pd.DepthStencilState, sizeof(pd.DepthStencilState), 7)) * 0x9E3779B97F4A7C15ull;
        code = (code ^ Fnv(pd.RTVFormats, sizeof(pd.RTVFormats), 8)) * 0x9E3779B97F4A7C15ull;
        const uint32_t misc[] = { pd.SampleMask, uint32_t(pd.IBStripCutValue), uint32_t(pd.PrimitiveTopologyType), pd.NumRenderTargets,
            uint32_t(pd.DSVFormat), pd.SampleDesc.Count, pd.SampleDesc.Quality, pd.InputLayout.NumElements };
        code = (code ^ Fnv(misc, sizeof(misc), 9)) * 0x9E3779B97F4A7C15ull;
        swprintf(name, 128, L"r3_%016llx_%016llx_%08x_%08x_%08x_%08x_%016llx", (unsigned long long)k.vs, (unsigned long long)k.ps, k.blend, k.depth,
            k.stencilMaskMode, k.flags, (unsigned long long)code);
        return name;
    }

    HRESULT CreateOrLoad(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, const PipelineKey& key, ComPtr<ID3D12PipelineState>& pso, bool* hit)
    {
        *hit = false;
        if (PipelineIsPoisoned(key.vs, key.ps))
            return E_FAIL; // (see kBadPipelinesFile)
        if (library)
        {
            const std::wstring name = Name(key, pd);
            {
                std::shared_lock lock(access);
                *hit = SUCCEEDED(library->LoadGraphicsPipeline(name.c_str(), &pd, IID_PPV_ARGS(&pso)));
            }
            if (*hit)
            {
                KeepFresh(name, pso.Get());
                return S_OK;
            }
            // The slow part (the driver compiling) runs outside the lock.
            g_compiling = { key.vs, key.ps, true };
            const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
            g_compiling.active = false;
            if (SUCCEEDED(hr))
            {
                std::shared_lock lock(access);
                if (SUCCEEDED(library->StorePipeline(name.c_str(), pso.Get())))
                    stored++;
            }
            if (SUCCEEDED(hr))
                KeepFresh(name, pso.Get());
            return hr;
        }
        g_compiling = { key.vs, key.ps, true };
        const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        g_compiling.active = false;
        return hr;
    }

    void KeepFresh(const std::wstring& name, ID3D12PipelineState* pso)
    {
        if (!fresh)
            return;
        {
            std::shared_lock lock(access); // (stores share the library, as in CreateOrLoad)
            if (!fresh)
                return;
            if (SUCCEEDED(fresh->StorePipeline(name.c_str(), pso)))
                freshCount++;
        }
        size_t listed;
        {
            std::lock_guard manifestLock(g_manifest.mutex);
            listed = g_manifest.known.size();
        }
        if (!listed || freshCount < listed)
            return;
        bool swap = false;
        {
            std::unique_lock lock(access);
            // Only when it saves something: else the old library stays and is not written again.
            if (fresh && fresh->GetSerializedSize() * 10 < loaded.size() * 9)
            {
                library = fresh;
                swap = true;
            }
            fresh.Reset();
        }
        if (swap)
        {
            Log("renderer: pipeline library compacted to this session's %zu pipelines; saved at the next save", freshCount.load());
            stored++;
        }
    }

    // Writes the library out when new pipelines were stored (at most every 10 s). Never on
    // the calling thread: serializing and writing the whole library (tens to hundreds of MB)
    // there stalled the command processor for one frame every 10 s of gameplay. force (exit)
    // writes synchronously.
    std::atomic<bool> saving{ false };

    void MaybeSave(bool force = false)
    {
        if (!library || !stored.load())
            return;
        if (force)
        {
            SaveNow();
            return;
        }
        if (std::chrono::steady_clock::now() - lastSave < std::chrono::seconds(10) || saving.exchange(true))
            return;
        lastSave = std::chrono::steady_clock::now();
        std::thread([this] {
            // Not lower: it holds the library alone while serializing, and starving it then
            // would stall every pipeline build.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SaveNow();
            saving = false;
        }).detach();
    }

    void SaveNow()
    {
        std::lock_guard lock(saveMutex);
        const int pending = stored.exchange(0);
        if (!pending)
            return;
        const auto start = std::chrono::steady_clock::now();
        std::vector<char> blob;
        {
            std::unique_lock lock(access);
            blob.resize(library->GetSerializedSize());
            if (blob.empty() || FAILED(library->Serialize(blob.data(), blob.size())))
            {
                stored += pending;
                return;
            }
        }
        const auto serialized = std::chrono::steady_clock::now();
        const auto tmp = Path().string() + ".tmp";
        { std::ofstream(tmp, std::ios::binary).write(blob.data(), std::streamsize(blob.size())); }
        std::error_code ec;
        std::filesystem::rename(tmp, Path(), ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
        if (getenv("REGPU_PERF"))
            Log("perf: pipeline library saved in the background (%.0f MB): serialize %.0f ms, write %.0f ms", blob.size() / 1048576.0,
                std::chrono::duration<double, std::milli>(serialized - start).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - serialized).count());
    }
} g_pipelineLibrary;

void InitPipelineLibrary(ID3D12Device* device)
{
    g_pipelineLibrary.Init(device);
    g_manifest.Load();
    ShaderScanStart();
}

struct PipelinePool
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;

    PipelinePool()
    {
        const unsigned n = std::clamp(std::thread::hardware_concurrency() * 3 / 4, 2u, 12u);
        for (unsigned i = 0; i < n; i++)
            std::thread([this] { Run(); }).detach();
    }
    void Push(std::function<void()> job)
    {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }
    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !queue.empty(); });
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
        }
    }
};

std::shared_ptr<PipelineEntry> EnsurePipeline(const CompiledShader& vs, const CompiledShader& ps, const PipelineKey& key)
{
    if (auto it = g_r.pipelines.find(key); it != g_r.pipelines.end())
        return it->second;
    const uint32_t blend = key.blend, depth = key.depth;
    const uint32_t mask = (key.stencilMaskMode >> 24) & 0xF, mode = (key.stencilMaskMode >> 28) & 7, stencil = key.stencilMaskMode & 0xFFFFFF;
    const bool alphaToMask = key.flags & 1, cullable = (key.flags >> 1) & 1;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_r.rootSignature.Get();
    pd.VS = { vs.code->GetBufferPointer(), vs.code->GetBufferSize() };
    pd.PS = { ps.code->GetBufferPointer(), ps.code->GetBufferSize() };
    auto& rt = pd.BlendState.RenderTarget[0];
    const uint32_t srcC = blend & 31, opC = (blend >> 5) & 7, dstC = (blend >> 8) & 31;
    const uint32_t srcA = (blend >> 16) & 31, opA = (blend >> 21) & 7, dstA = (blend >> 24) & 31;
    rt.BlendEnable = !(srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0);
    rt.SrcBlend = Blend(srcC);
    rt.DestBlend = Blend(dstC);
    rt.BlendOp = BlendOp(opC);
    rt.SrcBlendAlpha = AlphaBlend(srcA);
    rt.DestBlendAlpha = AlphaBlend(dstA);
    rt.BlendOpAlpha = BlendOp(opA);
    rt.RenderTargetWriteMask = UINT8(mask);
    pd.BlendState.AlphaToCoverageEnable = alphaToMask; // one sample: coverage where alpha >= 0.5
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    // Polygon offset slope (MakePipelineKey flags bits 8-23, 1/16 subpixel units).
    pd.RasterizerState.SlopeScaledDepthBias = float(int16_t((key.flags >> 8) & 0xFFFF)) / 256.0f;
    pd.DepthStencilState.DepthEnable = (depth >> 1) & 1;
    pd.DepthStencilState.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
    // Stencil: Xenos ops (the D3D9 order) 0 keep, 1 zero, 2 replace, 3 incr clamp, 4 decr clamp,
    // 5 invert, 6 incr wrap, 7 decr wrap. Back faces use their own ops when enabled.
    static const D3D12_STENCIL_OP kOps[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
        D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
    static LiveSwitch noStencil("REGPU_NO_STENCIL");
    if ((depth & 1) && !noStencil)
    {
        auto& ds = pd.DepthStencilState;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = UINT8(stencil >> 8);
        ds.StencilWriteMask = UINT8(stencil >> 16);
        auto faceOps = [&](uint32_t shift) {
            D3D12_DEPTH_STENCILOP_DESC op{};
            op.StencilFunc = D3D12_COMPARISON_FUNC(((depth >> shift) & 7) + 1);
            op.StencilFailOp = kOps[(depth >> (shift + 3)) & 7];
            op.StencilPassOp = kOps[(depth >> (shift + 6)) & 7];
            op.StencilDepthFailOp = kOps[(depth >> (shift + 9)) & 7];
            return op;
        };
        ds.FrontFace = faceOps(8);
        // Rectangles and point sprites aren't polygons to the Xenos: they have no back face, so
        // their (host) second triangles and mirrored corners take the front ops too (the facing
        // test applies to triangles only: PA_SU_SC_MODE_CNTL's cull and the back stencil ops do not
        // touch them). A rectangle list drawn clockwise with two-sided stencil (a
        // depth reset under a windscreen: NOTEQUAL front, ALWAYS back) otherwise applied the back
        // ops and reset the whole screen. REGPU_RECT_BACK_STENCIL=1: the facing decides (A/B).
        static LiveSwitch rectBackStencil("REGPU_RECT_BACK_STENCIL");
        ds.BackFace = (depth >> 7) & 1 && (cullable || rectBackStencil) ? faceOps(20) : ds.FrontFace;
    }
    // Culling: bit 0 front, bit 1 back; bit 2 set means clockwise faces are front.
    static LiveSwitch flip("REGPU_CULL_FLIP");
    pd.RasterizerState.FrontCounterClockwise = ((mode & 4) == 0) != flip;
    // Only real triangle primitives keep the guest winding; generated geometry
    // (rectangle corners, point sprites) is never culled.
    static LiveSwitch noCull("REGPU_NO_CULL"); // A/B
    pd.RasterizerState.CullMode = !cullable || noCull ? D3D12_CULL_MODE_NONE
        : (mode & 3) == 1 ? D3D12_CULL_MODE_FRONT : (mode & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = key.flags & 8 ? kHalfColor : g_colorFormat;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    pd.SampleDesc.Count = g_msaa;
    const bool motion = key.flags & 4; // motion-vector variants: a second target for the motion
    if (motion)
    {
        pd.pRootSignature = g_r.motionRootSignature.Get();
        pd.NumRenderTargets = 2;
        pd.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        pd.BlendState.IndependentBlendEnable = TRUE;
        auto& mv = pd.BlendState.RenderTarget[1];
        mv = D3D12_RENDER_TARGET_BLEND_DESC{};
        // Blended draws (glass, smoke) keep the motion of what is behind them.
        mv.RenderTargetWriteMask = rt.BlendEnable ? 0 : D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
    }
    auto entry = std::make_shared<PipelineEntry>();
    g_r.pipelines[key] = entry;
    if (!motion && !IsPsEditAlias(key.ps) && !IsPsEditAlias(key.vs)) // the manifest rebuilds pipelines from the base shaders (not live edits)
    {
        g_manifest.RecordShader(false, key.vs, vs.ucodeBE);
        g_manifest.RecordShader(true, key.ps, ps.ucodeBE);
        g_manifest.RecordPipeline(key);
    }
    static PipelinePool pool;
    char label[64];
    snprintf(label, sizeof(label), "pipeline %08llx/%08llx", (unsigned long long)(key.vs >> 32), (unsigned long long)(key.ps >> 32));
    g_stats.Begin(label);
    // (A plain pointer back to the entry: pipelines are never removed.)
    entry->build =[e = entry.get(), pd, vsKeep = vs.code, psKeep = ps.code, key]() mutable {
        PipelineEntry* entry = e;
        const auto createStart = std::chrono::steady_clock::now();
        bool hit = false;
        const HRESULT hr = g_pipelineLibrary.CreateOrLoad(g_r.device, pd, key, entry->pso, &hit);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStart).count();
        if (ms > 30.0 && getenv("REGPU_PERF"))
            Log("perf: pipeline %016llx/%016llx took %.1f ms to create (background)", (unsigned long long)key.vs, (unsigned long long)key.ps, ms);
        if (FAILED(hr))
            Log("renderer: pipeline %016llx/%016llx failed", (unsigned long long)key.vs, (unsigned long long)key.ps);
        (hit ? g_stats.pipelinesLoaded : g_stats.pipelinesBuilt)++;
        entry->state.store(FAILED(hr) ? 2 : 1, std::memory_order_release);
        g_stats.End(!hit);
    };
    pool.Push([entry] { entry->Build(); });
    g_pipelinesCreated++;
    g_frameEvents.pipelines++;
    return entry;
}

void WaitHoldingImpl(const std::function<bool()>& done);

// Waits for the build (draws are never dropped: a missing pipeline used to leave
// shadow passes undrawn). Cold builds can take 100+ ms; the overlay says so.
ID3D12PipelineState* WaitForPipeline(const std::shared_ptr<PipelineEntry>& entry)
{
    // Not started yet: built here, now. Queued behind the pool's other builds, a draw that must wait
    // for its pipeline waited for all of them (a cold cache: over 20 s, and the title never queued
    // work again). REGPU_NO_INLINE_PIPELINE=1: wait in the queue, as before.
    if (static LiveSwitch noInline("REGPU_NO_INLINE_PIPELINE"); !noInline)
        entry->Build();
    WaitHoldingImpl([&] { return entry->state.load(std::memory_order_acquire) != 0; });
    return entry->state.load() == 1 ? entry->pso.Get() : nullptr;
}

PipelineKey MakePipelineKey(uint64_t vsHash, uint64_t psHash, bool depthOnly, bool cullable)
{
    const uint32_t blend = gpureg::Get(RB_BLENDCONTROL0);
    uint32_t depth = gpureg::Get(RB_DEPTHCONTROL);
    // Debug: REGPU_NO_ZTEST=a-b turns the depth test off for draws a..b-1 of the REGPU_DUMP_FRAME frame.
    static uint32_t noZFrom = 0, noZTo = 0;
    static const bool noZParsed = [] { if (const char* s = getenv("REGPU_NO_ZTEST")) sscanf(s, "%u-%u", &noZFrom, &noZTo); return true; }();
    static const uint64_t noZFrame = getenv("REGPU_DUMP_FRAME") ? strtoull(getenv("REGPU_DUMP_FRAME"), nullptr, 10) : 0;
    (void)noZParsed;
    if (noZTo && g_r.frame + 1 == noZFrame && g_r.drawsThisFrame >= noZFrom && g_r.drawsThisFrame < noZTo)
        depth &= ~(getenv("REGPU_NO_ZWRITE") ? 4u : 2u);
    static LiveSwitch zFuncAlways("REGPU_ZFUNC_ALWAYS");
    if (zFuncAlways)
        depth |= 0x70; // debug: depth func ALWAYS for every draw
    const uint32_t mask = depthOnly ? 0 : gpureg::Get(RB_COLOR_MASK) & 0xF;
    // Stencil masks (not the reference, which is dynamic) and culling are pipeline state.
    const uint32_t stencil = gpureg::Get(RB_STENCILREFMASK) & 0xFFFF00, mode = gpureg::Get(PA_SU_SC_MODE_CNTL) & 7;
    // Alpha-to-mask (RB_COLORCONTROL bit 4): cut-out foliage and fences.
    const bool alphaToMask = (gpureg::Get(RB_COLORCONTROL) >> 4) & 1;
    // Bit 3: the bound colour surface is half floats (REGPU_EXACT_FP16_SURFACES), the pipeline's target format.
    // Bits 8-23: the polygon offset's slope term (PA_SU_POLY_OFFSET_*_SCALE) in 1/16 subpixel units,
    // the pipeline's slope-scaled depth bias (the constant term is in the vertex shader, DrawImpl).
    uint32_t slope = 0;
    {
        static LiveSwitch noPolyOffset("REGPU_NO_POLY_OFFSET");
        const uint32_t sc = gpureg::Get(PA_SU_SC_MODE_CNTL);
        if (!noPolyOffset && cullable && (sc & (3u << 11)))
        {
            const float scale = (sc >> 11) & 1 ? RegFloat(0x2380) : RegFloat(0x2382); // PA_SU_POLY_OFFSET_FRONT/BACK_SCALE
            slope = uint32_t(int32_t(std::clamp(scale * 16.0f, -32768.0f, 32767.0f))) & 0xFFFF;
        }
    }
    return PipelineKey{ vsHash, psHash, blend, depth, stencil | (mask << 24) | (mode << 28),
        uint32_t(alphaToMask) | (uint32_t(cullable) << 1) | (uint32_t(C().colorFormat == kHalfColor) << 3) | (slope << 8) };
}

std::chrono::steady_clock::time_point g_lastLongHold{}; // end of the last hold of 2 s or more

// The game is running: a loading hold has finished at least 4 s ago (draw workers start then).
bool GameSettled()
{
    return g_lastLongHold.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - g_lastLongHold > std::chrono::seconds(4);
}

ID3D12PipelineState* GetPipelineImpl(const CompiledShader& vs, const CompiledShader& ps, bool depthOnly, bool cullable, bool motion)
{
    PipelineKey key = MakePipelineKey(C().vsHash, C().psHash, depthOnly, cullable);
    if (motion)
        key.flags |= 4;
    thread_local std::unordered_map<PipelineKey, ID3D12PipelineState*, PipelineKeyHash> ready; // (pipelines are never removed)
    // Runs of draws share a pipeline: the last one's answer first, before the map.
    thread_local PipelineKey lastKey{};
    thread_local ID3D12PipelineState* lastPso = nullptr;
    if (lastPso && memcmp(&key, &lastKey, sizeof(key)) == 0)
        return lastPso;
    if (auto r = ready.find(key); r != ready.end())
    {
        lastKey = key;
        lastPso = r->second;
        return r->second;
    }
    std::shared_ptr<PipelineEntry> entry;
    {
        const auto lockStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::lock_guard shaderLock(g_shaderMutex);
        if (g_perfTiming)
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lockStart).count(); ms > 2.0)
                Log("perf: GetPipeline waited %.1f ms for the shader lock", ms);
        auto it = g_r.pipelines.find(key);
        if (it != g_r.pipelines.end() && it->second->state.load(std::memory_order_acquire) == 1)
        {
            ready.emplace(key, it->second->pso.Get());
            return it->second->pso.Get();
        }
        entry = EnsurePipeline(vs, ps, key);
    }
    // Every draw waits for its pipeline: a draw is never dropped because it is still building.
    // REGPU_SKIP_BUILDING_DRAWS=1 (test, live): once the game runs (GameSettled), a draw whose pipeline is
    // still building is skipped instead (a first-time pipeline in a race froze the frame for up to ~2 s).
    static LiveSwitch skipBuilding("REGPU_SKIP_BUILDING_DRAWS");
    if (skipBuilding && GameSettled() && entry->state.load(std::memory_order_acquire) != 1)
        return nullptr;
    g_waitWhat = depthOnly ? "pipeline (depth only)" : "pipeline";
    g_waitBlend = key.blend;
    const auto waitStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ID3D12PipelineState* pso = WaitForPipeline(entry);
    if (g_perfTiming)
        if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count(); ms > 2.0)
            Log("perf: GetPipeline waited %.1f ms for pipeline %016llx/%016llx (state was %d)", ms, (unsigned long long)key.vs, (unsigned long long)key.ps, 0);
    return pso;
}

// GetPipeline with a long-wait report: a draw that waits over 250 ms for its pipeline is logged with its shaders, and a
// watchdog logs a wait still going after a second (so it is in the log even if the game then hangs or the device is lost).
ID3D12PipelineState* GetPipeline(const CompiledShader& vs, const CompiledShader& ps, bool depthOnly, bool cullable, bool motion = false)
{
    static std::once_flag watchdogStarted;
    std::call_once(watchdogStarted, [] {
        std::thread([] {
            SetThreadDescription(GetCurrentThread(), L"pipeline watchdog");
            int64_t reported = 0;
            for (;;)
            {
                Sleep(250);
                {
                    // A frame stall (the 2 s hitches): the frame counter has not moved for two polls.
                    static uint64_t lastFrame = ~0ull, reportedFrame = ~0ull;
                    static int still = 0;
                    const uint64_t frame = g_r.frame;
                    still = frame == lastFrame ? still + 1 : 0;
                    lastFrame = frame;
                    static int samples = 0;
                    if (still < 2)
                        samples = 0;
                    if (still >= 2 && g_r.ok && frame > 200 && samples++ < 6 && frame == reportedFrame && RegpuOnLongWait)
                        RegpuOnLongWait(uint64_t(still) * 250, g_drawThreadId.load(std::memory_order_relaxed));
                    if (still >= 2 && g_r.ok && frame != reportedFrame && frame > 200)
                    {
                        reportedFrame = frame;
                        Log("STALL: frame %llu has not finished for %d ms. renderer waiting on: %s; GPU fence completed %llu of %llu; pipeline wait vs %016llx ps %016llx (%s); pipeline builds busy %d",
                            (unsigned long long)frame, still * 250, g_waitWhat, (unsigned long long)g_r.fence->GetCompletedValue(), (unsigned long long)g_r.nextFence,
                            (unsigned long long)g_pipelineWaitVs.load(), (unsigned long long)g_pipelineWaitPs.load(),
                            g_pipelineWaitStartNs.load() ? "in GetPipeline" : "not in GetPipeline", int(g_stats.busy.load()));
                        if (RegpuOnLongWait)
                            RegpuOnLongWait(uint64_t(still) * 250, g_drawThreadId.load(std::memory_order_relaxed));
                    }
                }
                const int64_t start = g_pipelineWaitStartNs.load(std::memory_order_relaxed);
                if (!start || start == reported)
                    continue;
                const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
                const double ms = double(now - start) / 1e6;
                if (ms > 1000.0)
                {
                    reported = start;
                    Log("perf: the command processor has waited %.0f ms for pipeline %016llx/%016llx (still waiting)", ms,
                        (unsigned long long)g_pipelineWaitVs.load(), (unsigned long long)g_pipelineWaitPs.load());
                }
            }
        }).detach();
    });
    const auto start = std::chrono::steady_clock::now();
    g_pipelineWaitVs.store(C().vsHash, std::memory_order_relaxed);
    g_pipelineWaitPs.store(C().psHash, std::memory_order_relaxed);
    g_pipelineWaitStartNs.store(start.time_since_epoch().count(), std::memory_order_relaxed);
    ID3D12PipelineState* pso = GetPipelineImpl(vs, ps, depthOnly, cullable, motion);
    g_pipelineWaitStartNs.store(0, std::memory_order_relaxed);
    if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); ms > 250.0)
        Log("perf: a draw spent %.0f ms getting pipeline %016llx/%016llx (%s, %s)", ms, (unsigned long long)C().vsHash, (unsigned long long)C().psHash,
            motion ? "motion variant" : "plain", pso ? "ok" : "none");
    return pso;
}

// Cube textures: six faces, each a padded 2D slice. Faces the title rendered
// and resolved (dynamic reflections) are copied from their host textures every
// frame; static faces are decoded once and re-decoded when their data changes.
// Once a second (CubeStatsLog): how the cubes the title renders (the cars' reflections) were put together: all six faces
// from the title's own renders ("live"), some of them, or none (read from guest memory, which holds no picture of a cube the
// title renders), and how often the last live picture was kept instead (REGPU_NO_CUBE_KEEP=1 turns that off, live).
struct CubeStats
{
    uint32_t builds = 0, liveAll = 0, livePartial = 0, none = 0, kept = 0, lost = 0;
};
CubeStats g_cubeStats;
void CubeStatsLog()
{
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1))
        return;
    last = now;
    const CubeStats s = std::exchange(g_cubeStats, CubeStats{});
    if (s.builds)
        Log("cube: %u builds a second: all six faces live %u, some live %u, none live (from memory) %u; kept the last live picture %u; cubes that lost their live faces %u",
            s.builds, s.liveAll, s.livePartial, s.none, s.kept, s.lost);
}

ID3D12Resource* GetCubeTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    TextureScope textureScope;
    CubeStatsLog();
    const uint32_t w = fetch.Width(), h = fetch.Height();
    if (w != h || w > 2048)
        return nullptr;
    const uint32_t stride = TextureFaceStride(fetch);
    // One cube copy per address, format and size was shared by every fetch of it, though fetches
    // differ in what they make of it (swizzle, endian, mip levels): the first fetch of a frame made
    // the copy all the frame's draws then read, so draws saw another fetch's cube whenever the draw
    // order changed. REGPU_CUBE_VARIANT_KEY=1 (test) keys them by those fields too; it did not change the
    // car flicker, so one copy per address stays the default.
    static LiveSwitch variantKey("REGPU_CUBE_VARIANT_KEY");
    uint64_t cubeKey = TextureKey(fetch.Address(), fetch.Format(), w, h);
    if (variantKey)
        cubeKey ^= (uint64_t(fetch.MipAddress()) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(fetch.MipMaxLevel()) << 58) ^ (uint64_t(fetch.Swizzle()) << 40) ^
            (uint64_t(fetch.Endian()) << 54);
    CachedTexture& cached = g_r.cubes[cubeKey];
    // A cube the title rendered is put together once a frame, at its first fetch: faces it resolves
    // later in the frame show from the next frame on. REGPU_CUBE_RECHECK=1 (live) puts it together
    // again at a fetch after any face was resolved anew (as the hardware reads memory at each fetch).
    static LiveSwitch recheck("REGPU_CUBE_RECHECK");
    // REGPU_FREEZE_CUBES=1 (live, debug): a rendered cube that had all six faces keeps that picture
    // (later face resolves are ignored).
    static LiveSwitch freeze("REGPU_FREEZE_CUBES");
    if (freeze && cached.resource && cached.allFaces)
    {
        format = cached.format;
        return cached.resource.Get();
    }
    uint64_t serials = 0;
    if (recheck && cached.resource && cached.checkedFrame == g_r.frame)
        for (uint32_t i = 0; i < 6; i++)
            if (const ResolvedTexture* face = CurrentResolve(fetch.Address() + i * stride))
                serials = serials * 0x100000001B3ull ^ face->serial;
    if (cached.resource && cached.checkedFrame == g_r.frame && (!recheck || serials == cached.faceSerials))
    {
        format = cached.format;
        return cached.resource.Get();
    }
    cached.checkedFrame = g_r.frame;

    // Faces the title resolved into?
    const ResolvedTexture* faces[6]{};
    bool resolved = false;
    cached.faceSerials = 0;
    for (uint32_t i = 0; i < 6; i++)
    {
        // (Through the stale check: a cube the title rendered, then replaced with a loaded one at the
        // same address, kept showing the old surroundings.)
        const ResolvedTexture* face = CurrentResolve(fetch.Address() + i * stride);
        if (face)
            cached.faceSerials = cached.faceSerials * 0x100000001B3ull ^ face->serial;
        // REGPU_NO_RENDERED_CUBES=1 (live, debug): a cube is always read from guest memory, never from
        // the faces the title rendered (reflections go stale or blank, but tell whether a fault comes
        // from the rendered ones).
        static LiveSwitch noRendered("REGPU_NO_RENDERED_CUBES");
        if (face && !noRendered && face->format != DXGI_FORMAT_R32_FLOAT && face->width >= w && face->height >= h)
        {
            faces[i] = face;
            resolved = true;
        }
    }
    DXGI_FORMAT want = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < 6 && resolved; i++)
        if (faces[i])
        {
            want = faces[i]->format;
            break;
        }
    const bool hadAllFaces = cached.allFaces;
    cached.allFaces = resolved;
    for (uint32_t i = 0; i < 6; i++)
        if (!faces[i] || faces[i]->format != want)
            cached.allFaces = false;
    g_cubeStats.builds++;
    if (resolved)
    {
        (cached.allFaces ? g_cubeStats.liveAll : g_cubeStats.livePartial)++;
        cached.everLive = true;
        cached.lastLiveFrame = g_r.frame;
    }
    else
        g_cubeStats.none++;
    if (hadAllFaces && !cached.allFaces)
    {
        g_cubeStats.lost++;
        static uint32_t lostLogged = 0;
        if (lostLogged++ < 40)
            Log("cube: %08X lost its live faces in frame %llu (%s)", fetch.Address(), (unsigned long long)g_r.frame, resolved ? "some faces are still live" : "none live now");
    }
    // A cube the title renders that has no live face for a moment (its resolves late, stale or replaced) used to be read from
    // guest memory, which holds no picture of it: the paint reflected an empty cube (purple, no reflections). The last live
    // picture stays for up to 3 s (180 frames) instead. REGPU_NO_CUBE_KEEP=1 (live) = the old behaviour (A/B).
    static LiveSwitch noKeep("REGPU_NO_CUBE_KEEP");
    if (!resolved && cached.resource && cached.everLive && !noKeep && g_r.frame - cached.lastLiveFrame < 180)
    {
        g_cubeStats.kept++;
        format = cached.format;
        return cached.resource.Get();
    }
    // Faces the title rendered are at the render scale; the cube matches them (their own host
    // size: the current pass's scale differs when the cube is first fetched in another pass,
    // which rebuilt the cube mid-level, a 4-5 ms draw).
    uint32_t cw = w, ch = h;
    for (uint32_t i = 0; i < 6 && resolved; i++)
        if (faces[i] && faces[i]->format == want && faces[i]->hostWidth && faces[i]->width)
        {
            cw = uint32_t(std::lround(double(w) * faces[i]->hostWidth / faces[i]->width));
            ch = uint32_t(std::lround(double(h) * faces[i]->hostHeight / faces[i]->height));
            break;
        }
    // A cube read from memory gets the title's own mip levels (the faces of each level, from the mip
    // address, down to the packed tail): a surface that samples a rough reflection reads the smaller,
    // averaged levels (car paint, by its computed LOD). With level 0 alone every reflection was the
    // sharpest one, noisy on rough paint. Cubes the title rendered keep one level (their mips are
    // resolves of their own). REGPU_CUBE_NO_MIPS=1: level 0 only, as before (A/B).
    static LiveSwitch noCubeMips("REGPU_CUBE_NO_MIPS");
    std::vector<TextureFetch> levels{ fetch };
    for (uint32_t l = 1; !resolved && !noCubeMips; l++)
    {
        TextureFetch level;
        if (!TextureCubeMipLevel(fetch, l, level))
            break;
        levels.push_back(level);
    }
    const UINT16 mipCount = UINT16(levels.size());
    if (!cached.resource || cached.width != cw || cached.height != ch || cached.format != want || cached.resource->GetDesc().MipLevels != mipCount)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = cw;
        td.Height = ch;
        td.DepthOrArraySize = 6;
        td.MipLevels = mipCount;
        td.SampleDesc.Count = 1;
        td.Format = want;
        if (cached.resource)
            Frame().garbage.push_back(std::move(cached.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&cached.resource)),
            "CreateCommittedResource(cube)"); CountCreate("cube");
        cached.width = cw;
        cached.height = ch;
        cached.format = want;
        cached.contentHash = 0;
    }
    format = want;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    if (resolved)
    {
        D3D12_RESOURCE_BARRIER fb[6]{};
        UINT n = 0;
        for (uint32_t i = 0; i < 6; i++)
        {
            if (!faces[i] || faces[i]->format != want)
                continue;
            fb[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fb[n].Transition.pResource = faces[i]->resource.Get();
            fb[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fb[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            n++;
        }
        C().list->ResourceBarrier(1, &b);
        if (n)
            C().list->ResourceBarrier(n, fb);
        for (uint32_t i = 0; i < 6; i++)
        {
            if (!faces[i] || faces[i]->format != want)
                continue;
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            src.pResource = faces[i]->resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{ 0, 0, 0, std::min(cw, faces[i]->hostWidth), std::min(ch, faces[i]->hostHeight), 1 };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        for (UINT k = 0; k < n; k++)
            std::swap(fb[k].Transition.StateBefore, fb[k].Transition.StateAfter);
        if (n)
            C().list->ResourceBarrier(n, fb);
    }
    else
    {
        // Static: hash all six faces, decode on change.
        // REGPU_FREEZE_MEMORY_CUBES=1 (live, debug): a cube read from memory keeps its first upload.
        static LiveSwitch freezeMemory("REGPU_FREEZE_MEMORY_CUBES");
        if (freezeMemory && cached.resource && cached.contentHash)
            return cached.resource.Get();
        const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
        // REGPU_CUBE_FULL_HASH=1 (live, debug): every byte of the faces is checked for changes (not one
        // word in 64), and each change is logged with the faces it touched.
        static LiveSwitch fullHash("REGPU_CUBE_FULL_HASH");
        uint64_t hash = ContentHash(source, size_t(stride) * 6, fullHash ? 4 : 256) ^ (uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Endian()) << 44) ^ mipCount;
        if (fullHash && cached.contentHash != hash)
        {
            static std::unordered_map<uint32_t, std::array<uint64_t, 6>> faceHashes;
            auto& fh = faceHashes[fetch.Address()];
            char changed[16] = {};
            int n = 0;
            for (uint32_t i = 0; i < 6; i++)
            {
                const uint64_t h = ContentHash(source + size_t(stride) * i, stride, 4);
                if (h != fh[i])
                    changed[n++] = char('0' + i);
                fh[i] = h;
            }
            static uint32_t logged = 0;
            if (logged++ < 2000)
                Log("cube: %08X %ux%u from memory changed in frame %llu (draw %u), faces %s (levels %u, mip address %08X max level %u, swizzle %03X, endian %u)",
                    fetch.Address(), w, h, (unsigned long long)g_r.frame, g_r.drawsThisFrame.load(), n ? changed : "none (levels or layout)", unsigned(mipCount),
                    fetch.MipAddress(), fetch.MipMaxLevel(), fetch.Swizzle(), fetch.Endian());
        }
        for (uint32_t l = 1; l < mipCount; l++) // (the levels change with the image)
            hash = (hash ^ ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(levels[l].Address())), size_t(TextureFaceStride(levels[l])) * 6)) * 0x100000001B3ull;
        if (cached.contentHash == hash)
            return cached.resource.Get();
        UINT64 total = 0;
        for (const TextureFetch& level : levels)
            total += ((UINT64((level.Width() * 4 + 255) & ~255u) * level.Height() + 511) & ~511ull) * 6;
        const UINT64 offset = RingAlloc(total, 512, "cube texture");
        if (offset == ~0ull)
            return nullptr;
        C().list->ResourceBarrier(1, &b);
        UINT64 at = offset;
        for (uint32_t l = 0; l < mipCount; l++)
        {
            const uint32_t lw = levels[l].Width(), lh = levels[l].Height(), levelStride = l ? TextureFaceStride(levels[l]) : stride;
            const UINT rowPitch = (lw * 4 + 255) & ~255u;
            const UINT64 faceBytes = (UINT64(rowPitch) * lh + 511) & ~511ull;
            for (uint32_t i = 0; i < 6; i++)
            {
                TextureFetch face = levels[l];
                face.dwords[1] = (face.dwords[1] & 0xFFF) | ((levels[l].Address() + i * levelStride) & 0xFFFFF000);
                face.dwords[5] = (face.dwords[5] & ~(3u << 9)) | (1u << 9); // decode as 2D
                std::vector<uint8_t> rgba;
                if (!DecodeTexture(face, rgba))
                    rgba.assign(size_t(lw) * lh * 4, 0);
                if (getenv("REGPU_DUMP_CUBES"))
                {
                    char name[96];
                    std::filesystem::create_directories("out/cube");
                    snprintf(name, sizeof(name), "out/cube/%08X_face%u_level%u_stride%X.png", fetch.Address(), i, l, levelStride);
                    WritePng(name, lw, lh, rgba);
                }
                for (uint32_t y = 0; y < lh; y++)
                    memcpy(Frame().ringCpu + at + UINT64(y) * rowPitch, &rgba[size_t(y) * lw * 4], size_t(lw) * 4);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = cached.resource.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = l + i * mipCount; // D3D12: mip + array slice x mip count
                src.pResource = Frame().ring.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = at;
                src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, lw, lh, 1, rowPitch };
                C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                at += faceBytes;
            }
        }
        cached.contentHash = hash;
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    return cached.resource.Get();
}

// 3D textures (colour-grading lookup tables and the like): decoded to an RGBA8 volume, all slices,
// and decoded again when their data changes. Mip levels are not read.
ID3D12Resource* GetVolumeTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    TextureScope textureScope;
    const uint32_t w = fetch.Width(), h = fetch.Height(), depth = fetch.Depth();
    format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (w > 2048 || h > 2048 || depth > 2048)
        return nullptr;
    CachedTexture& cached = g_r.volumes[TextureKey(fetch.Address(), fetch.Format(), w, h) ^ uint64_t(depth) << 52];
    if (cached.resource && cached.checkedFrame == g_r.frame)
        return cached.resource.Get();
    cached.checkedFrame = g_r.frame;
    std::vector<uint8_t> rgba;
    if (!DecodeTexture3D(fetch, rgba))
    {
        NoteTextureFailure("not a supported 3D texture", fetch);
        return nullptr;
    }
    const uint64_t hash = ContentHash(rgba.data(), rgba.size());
    if (cached.resource && cached.contentHash == hash)
        return cached.resource.Get();
    if (!cached.resource || cached.width != w || cached.height != h || cached.resource->GetDesc().DepthOrArraySize != depth)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = UINT16(depth);
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = format;
        if (cached.resource)
            Frame().garbage.push_back(std::move(cached.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&cached.resource)),
            "CreateCommittedResource(volume)"); CountCreate("volume");
        cached.width = w;
        cached.height = h;
        cached.format = format;
    }
    const UINT rowPitch = (w * 4 + 255) & ~255u;
    const UINT64 sliceBytes = UINT64(rowPitch) * h;
    const UINT64 offset = RingAlloc(sliceBytes * depth, 512, "volume texture");
    if (offset == ~0ull)
        return nullptr;
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < h; y++)
            memcpy(Frame().ringCpu + offset + sliceBytes * z + UINT64(y) * rowPitch, &rgba[(size_t(z) * h + y) * w * 4], size_t(w) * 4);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    C().list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = cached.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = Frame().ring.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint = { format, w, h, depth, rowPitch };
    C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    cached.contentHash = hash;
    return cached.resource.Get();
}

// A render target the title resolved together with its mip chain (it
// downsamples and resolves each level itself): levels 1+ follow each other
// from the fetch constant's mip address, each padded to 32x32 texels and
// 4 KB. Assembled into one mipmapped host texture, again whenever one of its levels has been
// resolved since: a fetch reads memory as it is at the draw (one title blurs a shadow in two
// transposed passes, both resolved to level 0, and the chain built after the first was kept).
ID3D12Resource* ResolvedMipChain(const TextureFetch& fetch, const ResolvedTexture& level0)
{
    TextureScope textureScope;
    const uint32_t w = std::min(fetch.Width(), level0.width), h = std::min(fetch.Height(), level0.height);
    const UINT bpp = level0.format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
    std::vector<const ResolvedTexture*> levels{ &level0 };
    uint32_t address = fetch.MipAddress();
    for (uint32_t i = 1; i <= fetch.MipMaxLevel() && (w >> i || h >> i); i++)
    {
        const uint32_t lw = std::max(w >> i, 1u), lh = std::max(h >> i, 1u);
        const ResolvedTexture* level = CurrentResolve(address);
        if (!level || level->format != level0.format || level->width < lw || level->height < lh)
            break;
        levels.push_back(level);
        // Each level takes the memory the title gave it: guest bytes per texel, not the host format's
        // (a 2-channel 8-bit image is 2 bytes in memory and 4 in our RGBA copy: with 4, every level after
        // the first was looked for at twice the address and the chain stopped at 2 levels, so the road
        // was minified without mips). REGPU_MIP_CHAIN_HOST_BPP=1 steps by the host format again (A/B).
        static LiveSwitch hostBpp("REGPU_MIP_CHAIN_HOST_BPP");
        const uint32_t stride = hostBpp ? uint32_t(bpp) : std::max(level0.guestBpp, 1u);
        const uint32_t bytes = ((lw + 31) & ~31u) * ((lh + 31) & ~31u) * stride;
        address += std::max((bytes + 4095) & ~4095u, 4096u);
    }
    if (levels.size() < 2)
        return nullptr;
    // The levels are host textures (scaled with the render resolution): the chain is too.
    // (At game size it held each level's top-left corner only: one title's level-wide
    // shadow map read from the wrong place, black patches on the road.)
    const uint32_t hw = std::max(uint32_t(uint64_t(level0.hostWidth) * w / std::max(level0.width, 1u)), 1u);
    const uint32_t hh = std::max(uint32_t(uint64_t(level0.hostHeight) * h / std::max(level0.height, 1u)), 1u);

    const UINT count = UINT(levels.size());
    // One chain per base and shape: a base fetched with two sizes or level counts in a frame recreated
    // its chain at each change. REGPU_MIP_CHAIN_BY_BASE=1: one per base, as before (A/B).
    static LiveSwitch byBase("REGPU_MIP_CHAIN_BY_BASE");
    auto& chain = g_r.mipChains[byBase ? uint64_t(fetch.Address())
        : uint64_t(fetch.Address()) << 32 ^ (uint64_t(hw) << 18 ^ uint64_t(hh) << 4 ^ count) * 0x9E3779B1u ^ uint64_t(level0.format)];
    // The resolves the chain holds. REGPU_MIP_CHAIN_PER_FRAME=1: once per frame, as before (A/B).
    static LiveSwitch perFrame("REGPU_MIP_CHAIN_PER_FRAME");
    uint64_t sources = count;
    for (const ResolvedTexture* level : levels)
        sources = (sources ^ level->serial) * 0x100000001B3ull;
    if (chain.resource && (perFrame ? chain.checkedFrame == g_r.frame : chain.contentHash == sources))
        return chain.resource.Get();
    chain.checkedFrame = g_r.frame;
    chain.contentHash = sources;
    if (!chain.resource || chain.width != hw || chain.height != hh || chain.format != level0.format ||
        chain.resource->GetDesc().MipLevels != count)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = hw;
        td.Height = hh;
        td.DepthOrArraySize = 1;
        td.MipLevels = UINT16(count);
        td.SampleDesc.Count = 1;
        td.Format = level0.format;
        if (chain.resource)
            Frame().garbage.push_back(std::move(chain.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&chain.resource)),
            "CreateCommittedResource(mip chain)"); CountCreate("mip chain");
        chain.width = hw;
        chain.height = hh;
        chain.format = level0.format;
    }
    std::vector<D3D12_RESOURCE_BARRIER> b(count + 1);
    for (UINT i = 0; i <= count; i++)
    {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = i < count ? levels[i]->resource.Get() : chain.resource.Get();
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[i].Transition.StateAfter = i < count ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    }
    // Several levels can share one host texture only if the title resolved
    // them to the same address, which it does not; still, transition each once.
    C().list->ResourceBarrier(UINT(b.size()), b.data());
    for (UINT i = 0; i < count; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = chain.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        src.pResource = levels[i]->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, std::min(std::max(hw >> i, 1u), levels[i]->hostWidth), std::min(std::max(hh >> i, 1u), levels[i]->hostHeight), 1 };
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    for (auto& x : b)
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    C().list->ResourceBarrier(UINT(b.size()), b.data());
    return chain.resource.Get();
}

// The resolved texture a fetch reads, if any. Resolves are never block compressed,
// so a compressed fetch at an address we resolved to earlier is a different image the
// title has since loaded there (menu icons reusing a shadow image's memory).
// A hash of the guest memory a resolved image covers (sampled: it only has to notice new contents).
// (Only the image's own bytes: an 8-bit shadow image hashed as 32-bit took in the memory after it,
// which changed with unrelated game state, and the shadow fell back to that memory as noise.)
uint64_t GuestImageHash(uint32_t base, uint32_t width, uint32_t height, uint32_t bpp)
{
    const size_t bytes = std::min<size_t>(size_t(width) * height * bpp, 4u << 20);
    return ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(base)), std::max<size_t>(bytes, 64), 256);
}

// Records what guest memory holds under a resolved image (after a resolve, or after we write it back:
// ownWrite).
void NoteResolvedMemory(uint32_t base, ResolvedTexture& r, bool ownWrite = false)
{
    r.memHash = GuestImageHash(base, r.width, r.height, r.guestBpp);
    r.memChecked = g_r.frame;
    r.stale = false;
    r.ownWrite |= ownWrite;
}

// The resolve at a guest address, unless it is stale. A resolve not repeated lately whose memory has
// changed since: the title reused the address for a texture of its own (menu icons that sat on an old
// menu resolve showed as white boxes; a homespace's reflection cube loaded where the previous
// homespace's rendered one was). Checked every 32 frames; a new resolve there makes it current again.
// Every lookup of a resolve a fetch reads goes through here: whole textures, cube faces, mip levels.
const ResolvedTexture* CurrentResolve(uint32_t address)
{
    auto it = g_r.resolved.find(address & 0x1FFFFFFF);
    if (it == g_r.resolved.end())
        return nullptr;
    static LiveSwitch noStaleCheck("REGPU_NO_STALE_RESOLVES"); // A/B
    ResolvedTexture& r = it->second;
    // A resolve we write back to memory ourselves (as the console's resolve does) can be checked while
    // the title keeps resolving it: what the memory should hold is known (our write), so a change is the
    // title's own write. A title that restarts an accumulation by clearing the image's memory from the
    // CPU (photo mode's picture) needs it: re-resolved every frame, the gate below never let the clear be
    // seen, and the old accumulation was kept. REGPU_STALE_CHECK_WRITTEN=1: check such resolves every
    // frame, whatever their age (test).
    static LiveSwitch checkWritten("REGPU_STALE_CHECK_WRITTEN");
    const bool watched = checkWritten && r.ownWrite;
    if (!noStaleCheck && ((r.frame + 8 < g_r.frame && r.frame != ~0ull) || watched))
    {
        if (!r.stale && g_r.frame - r.memChecked >= (watched ? 1u : 32u))
        {
            r.memChecked = g_r.frame;
            if (GuestImageHash(it->first, r.width, r.height, r.guestBpp) != r.memHash)
            {
                r.stale = true;
                Log("renderer: resolve at %08X (%ux%u) is stale: the memory was reused", it->first, r.width, r.height);
            }
        }
        if (r.stale)
            return nullptr;
    }
    return &r;
}

const ResolvedTexture* ResolvedFor(const TextureFetch& fetch)
{
    const char* name = TextureFormatName(fetch.Format());
    if (name && (strncmp(name, "DXT", 3) == 0 || strncmp(name, "DXN", 3) == 0 || strncmp(name, "CTX", 3) == 0))
        return nullptr;
    return CurrentResolve(fetch.Address());
}

// REGPU_TRACE_BINDS=<address>[,<address>...] (hex, read at start): each frame, log every bind of those
// resolve destinations (frame and serial of the resolve behind it, per mip level) and every resolve
// into them, with the draw number, to compare frames where an artefact shows with frames where it
// doesn't. A bind also logs the draw's vertex fetch constant 95 (address, size, content hash).
const std::vector<uint32_t>& TracedBindAddresses()
{
    static const std::vector<uint32_t> list = [] {
        std::vector<uint32_t> v;
        if (const char* s = getenv("REGPU_TRACE_BINDS"); s && *s)
            for (char* end = nullptr;; s = end + 1)
            {
                v.push_back(uint32_t(strtoul(s, &end, 16)) & 0x1FFFFFFF);
                if (*end != ',')
                    break;
            }
        return v;
    }();
    return list;
}

void TraceResolvedBind(const TextureFetch& fetch, uint32_t slot, const ResolvedTexture* resolved)
{
    const auto& list = TracedBindAddresses();
    const uint32_t address = fetch.Address() & 0x1FFFFFFF;
    if (list.empty() || std::find(list.begin(), list.end(), address) == list.end())
        return;
    std::string levels;
    uint32_t mip = fetch.MipAddress() & 0x1FFFFFFF;
    const uint32_t w = fetch.Width(), h = fetch.Height();
    for (uint32_t i = 1; mip && i <= fetch.MipMaxLevel() && (w >> i || h >> i); i++)
    {
        const uint32_t lw = std::max(w >> i, 1u), lh = std::max(h >> i, 1u);
        char e[96];
        if (auto it = g_r.resolved.find(mip); it != g_r.resolved.end())
            snprintf(e, sizeof(e), " L%u %08X f%lld s%llu", i, mip, (long long)(g_r.frame - it->second.frame), (unsigned long long)it->second.serial);
        else
            snprintf(e, sizeof(e), " L%u %08X none", i, mip);
        levels += e;
        const uint32_t bytes = ((lw + 31) & ~31u) * ((lh + 31) & ~31u) * (resolved && resolved->format != DXGI_FORMAT_R8G8B8A8_UNORM ? 8 : 4);
        mip += std::max((bytes + 4095) & ~4095u, 4096u);
    }
    const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + 95 * 2), d1 = gpureg::Get(gpureg::FetchConstants + 95 * 2 + 1);
    const uint32_t vb = d0 & 0xFFFFFFFC, vbBytes = ((d1 >> 2) & 0xFFFFFF) * 4;
    const uint64_t vbHash = vbBytes && vbBytes <= (1u << 20) ? ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(vb)), vbBytes) : 0;
    // Logged when what it reports (frame ages, staleness, the vertex data) differs from the last
    // line for the same draw number, slot and address: serials advance every frame by themselves.
    std::string ages = resolved ? "f" + std::to_string((long long)(g_r.frame - resolved->frame)) + (resolved->stale ? " STALE" : "") : "mem";
    for (size_t at = levels.find(" f"); at != std::string::npos; at = levels.find(" f", at + 2))
        ages += levels.substr(at, levels.find(' ', at + 1) - at);
    ages += " " + std::to_string(vbHash);
    static std::mutex m;
    static std::unordered_map<uint64_t, std::string> last;
    {
        std::lock_guard lock(m);
        std::string& previous = last[uint64_t(g_r.drawsThisFrame.load()) << 40 ^ uint64_t(slot) << 32 ^ address];
        if (previous == ages)
            return;
        previous = ages;
    }
    Log("bind: frame %llu draw %u vs %016llx ps %016llx t%u %08X %ux%u: %s%s | vf95 %08X %u bytes hash %016llx",
        (unsigned long long)g_r.frame, g_r.drawsThisFrame.load(), (unsigned long long)C().vsHash, (unsigned long long)C().psHash, slot, address, w, h,
        resolved ? ("resolve f" + std::to_string((long long)(g_r.frame - resolved->frame)) + " s" + std::to_string(resolved->serial) +
                    (resolved->stale ? " STALE" : "")).c_str() : "guest memory",
        levels.c_str(), vb, vbBytes, (unsigned long long)vbHash);
}


// Decodes (if changed) and uploads a texture; returns its resource.
ID3D12Resource* GetTextureLocked(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native);
// Textures are checked and uploaded once per frame; each thread remembers what it got this
// frame, so repeated bindings take no lock.
ID3D12Resource* GetTexture(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native)
{
    struct Known
    {
        ID3D12Resource* resource;
        DXGI_FORMAT format;
        bool native;
    };
    thread_local FrameMap<Known> known;
    // (Resolved textures can change within a frame: not cached.)
    const bool cacheable = !ResolvedFor(fetch) && !RendererTracing();
    const uint64_t key = TextureKey(fetch.Address(), fetch.Format(), fetch.Width(), fetch.Height()) ^ uint64_t(fetch.dwords[0]) << 7 ^
        uint64_t(fetch.dwords[3] & 0xFFF) << 41 ^ uint64_t(fetch.dwords[1] & 0xC0) << 50;
    if (cacheable)
        if (const Known* k = known.Find(key, g_r.frame))
        {
            format = k->format;
            native = k->native;
            return k->resource;
        }
    ID3D12Resource* r = GetTextureLocked(fetch, format, native);
    if (cacheable && r)
        known.Put(key, g_r.frame, { r, format, native });
    return r;
}

ID3D12Resource* GetTextureLocked(const TextureFetch& fetch, DXGI_FORMAT& format, bool& native)
{
    TextureScope textureScope;
    native = false;
    format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (const ResolvedTexture* resolvedTexture = ResolvedFor(fetch))
    {
        // The full-screen scene image rendered this frame: the draw is a screen layer (the
        // post-processing composite, a grid of rectangles). HUD elements sample only atlases
        // and reduced-size buffers (a 320x180 blur of the scene behind their panels).
        // (By the size the draw fetches it at: the HUD samples the scene image too, at 320x180.)
        if (resolvedTexture->frame == g_r.frame && fetch.Width() >= kWidth)
            g_drawSamplesResolve = true;
        format = resolvedTexture->format;
        if (fetch.MipMaxLevel() > 0 && fetch.MipAddress())
            if (ID3D12Resource* chain = ResolvedMipChain(fetch, *resolvedTexture))
                return chain;
        return resolvedTexture->resource.Get();
    }
    if (fetch.Type() != 2 || fetch.Dimension() != 1 || !TextureFormatSupported(fetch.Format()))
    {
        NoteTextureFailure("not a supported 2D texture", fetch);
        return nullptr;
    }
    const uint32_t w = fetch.Width(), h = fetch.Height();
    auto& cached = g_r.textures[TextureKey(fetch.Address(), fetch.Format(), w, h)];
    static LiveSwitch dumpAll("REGPU_DUMP_TEXTURES");
    const bool forceDecode = dumpAll && RendererTracing();
    // REGPU_TEXTURE_CHECK_EVERY_DRAW=1 (test): checked at every draw, not once a frame (a texture the
    // title rewrites between two draws of a frame).
    static LiveSwitch checkEveryDraw("REGPU_TEXTURE_CHECK_EVERY_DRAW");
    if (cached.resource && cached.checkedFrame == g_r.frame && !forceDecode && !checkEveryDraw)
    {
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    // A texture unchanged for many checks (most of them: loaded once) is checked every 4th
    // frame (staggered), not every frame: hashing all textures in use cost ~0.5 ms a frame.
    // Ones that change (video, render-to-texture on the CPU) never get here.
    // Every frame by default: titles that stream textures reuse their memory, and a texture
    // rechecked every 4th or 16th frame showed the previous one there for a few frames.
    // REGPU_TEXTURE_CHECK_STAGGER=1 brings the staggered checks back (A/B; faster).
    static LiveSwitch stagger("REGPU_TEXTURE_CHECK_STAGGER");
    const bool checkAlways = !stagger;
    const uint64_t textureKey = TextureKey(fetch.Address(), fetch.Format(), w, h);
    // Unchanged for 300 checks (a few seconds): every 16th frame (a CPU-side update shows up at most
    // ~0.1 s late). REGPU_TEXTURE_CHECK_EVERY4=1 keeps every 4th (A/B).
    static LiveSwitch every4("REGPU_TEXTURE_CHECK_EVERY4");
    const uint64_t checkMask = !every4 && cached.unchanged >= 300 ? 15 : 3;
    // Textures that keep changing (8 times or more) aren't staggered. A paused video's planes sit unchanged
    // long enough to be, each on its own frames, and when it played again its brightness and colour came
    // from different frames for a frame or two (FM2 port, report #15). REGPU_STAGGER_CHANGING=1: as before.
    static LiveSwitch noChangingCheck("REGPU_STAGGER_CHANGING");
    if (cached.resource && !checkAlways && !forceDecode && cached.unchanged >= 30 &&
        (cached.changes < 8 || noChangingCheck) && ((g_r.frame + (textureKey >> 32)) & checkMask) != 0)
    {
        cached.checkedFrame = g_r.frame;
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    cached.checkedFrame = g_r.frame;
    // DXT formats stay compressed on the GPU (BC1-BC5): no decode, a quarter to an eighth of
    // the upload. D3D12 needs block-aligned sizes; others are decoded to RGBA8.
    static LiveSwitch noNativeBC("REGPU_NO_NATIVE_BC");
    const uint32_t kind = TextureBCKind(fetch.Format());
    const uint32_t bc = !noNativeBC && ((w % 4 == 0 && h % 4 == 0) || kind == 6) && !forceDecode ? kind : 0;
    const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
    // The whole base level: the old guess (pitch * height / 2) covered only the first half of a
    // one-byte-per-texel texture (DXT5), so a thumbnail filled in later went unnoticed.
    const size_t sourceBytes = std::max<size_t>({ size_t(std::max(fetch.Pitch(), w)) * h / 2, TextureSourceSize(fetch), 64 });
    // Only what changes the decoded image: sampler state (clamp, filter) varies per draw.
    // Small textures (icons, thumbnails) are hashed densely, big ones sampled: a change confined
    // to a few dwords must not slip between the samples.
    // REGPU_TEXTURE_FULL_HASH=1: every texture hashed densely (test: a change between the samples).
    static LiveSwitch fullTextureHash("REGPU_TEXTURE_FULL_HASH");
    const size_t hashStride = fullTextureHash || sourceBytes <= (64u << 10) ? 4 : sourceBytes <= (512u << 10) ? 64 : 256;
    const auto hashStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint64_t hash = ContentHash(source, sourceBytes, hashStride) ^ (bc ? 0 : uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Pitch()) << 24) ^
        (uint64_t(fetch.Tiled()) << 40) ^ (uint64_t(fetch.Endian()) << 44);
    if (g_perfTiming)
    {
        g_textureHashSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - hashStart).count();
        g_textureChecks++;
    }
    if (cached.resource && cached.contentHash == hash && !forceDecode)
    {
        cached.unchanged++;
        format = cached.format;
        native = cached.native;
        return cached.resource.Get();
    }
    // REGPU_TRACE_TEXCHANGE=1: textures whose contents changed (address, frame, checks unchanged before).
    if (static LiveSwitch traceChange("REGPU_TRACE_TEXCHANGE"); traceChange && cached.resource)
        Log("texchange: %08X %s %ux%u frame %llu after %u unchanged", fetch.Address(), TextureFormatName(fetch.Format()), w, h,
            (unsigned long long)g_r.frame, cached.unchanged);
    const auto uploadStart = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool wasNew = !cached.resource;
    cached.unchanged = 0;
    if (!wasNew)
        cached.changes++;
    struct UploadLog
    {
        std::chrono::steady_clock::time_point start;
        const TextureFetch& fetch;
        bool isNew;
        ~UploadLog()
        {
            if (!g_perfTiming)
                return;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms > 2.0)
                Log("perf: texture %08X %s %ux%u %s took %.1f ms", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height(),
                    isNew ? "new" : "changed", ms);
        }
    } uploadLog{ uploadStart, fetch, wasNew };
    // The title's own mip levels (distant surfaces sample the smaller, averaged ones: without
    // them the road and grass shimmer and look grainy). Down to the packed tail.
    static LiveSwitch noMips("REGPU_NO_MIPS"); // A/B
    std::vector<TextureFetch> levels{ fetch };
    for (uint32_t l = 1; !noMips && !forceDecode; l++)
    {
        TextureFetch level;
        if (!TextureMipLevel(fetch, l, level))
            break;
        levels.push_back(level);
    }
    const UINT16 mipCount = UINT16(levels.size());
    if (bc)
    {
        static const DXGI_FORMAT bcFormats[] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM,
            DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
        const DXGI_FORMAT bcFormat = bcFormats[bc];
        // Kind 6 (16_16_16_16_FLOAT) is uncompressed: blocks of one texel, 8 bytes.
        const uint32_t blockBytes = bc == 1 || bc == 4 || bc == 6 ? 8 : 16, bs = bc == 6 ? 1 : 4;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!cached.resource || cached.width != w || cached.height != h || cached.format != bcFormat || cached.resource->GetDesc().MipLevels != mipCount)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w;
            td.Height = h;
            td.DepthOrArraySize = 1;
            td.MipLevels = mipCount;
            td.SampleDesc.Count = 1;
            td.Format = bcFormat;
            RetireTexture(std::move(cached.resource));
            cached.resource = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST, "texture");
            cached.width = w;
            cached.height = h;
            cached.format = bcFormat;
        }
        else
        {
            b.Transition.pResource = cached.resource.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            C().list->ResourceBarrier(1, &b);
        }
        for (UINT16 l = 0; l < mipCount; l++)
        {
            const uint32_t lw = levels[l].Width(), lh = levels[l].Height();
            const UINT rowPitch = (((lw + bs - 1) / bs) * blockBytes + 255) & ~255u;
            const UINT64 offset = RingAlloc(UINT64(rowPitch) * ((lh + bs - 1) / bs), 512, "compressed texture");
            if (offset == ~0ull)
            {
                NoteTextureFailure("upload ring full (compressed)", fetch);
                return nullptr;
            }
            CopyTextureBlocks(levels[l], Frame().ringCpu + offset, rowPitch);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = l;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = Frame().ring.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = offset;
            src.PlacedFootprint.Footprint = { bcFormat, (lw + bs - 1) / bs * bs, (lh + bs - 1) / bs * bs, 1, rowPitch };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        b.Transition.pResource = cached.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        C().list->ResourceBarrier(1, &b);
        cached.contentHash = hash;
        cached.native = true;
        format = bcFormat;
        native = true;
        return cached.resource.Get();
    }

    std::vector<uint8_t> rgba;
    if (!DecodeTexture(fetch, rgba))
    {
        NoteTextureFailure("decode failed", fetch);
        return nullptr;
    }
    if (RendererTracing())
        Log("texture upload: %08X %s %ux%u (%s)", fetch.Address(), TextureFormatName(fetch.Format()), w, h, cached.resource ? "changed" : "new");
    static const uint32_t dumpAddress = getenv("REGPU_DUMP_TEXTURE") ? uint32_t(strtoul(getenv("REGPU_DUMP_TEXTURE"), nullptr, 16)) : 0;
    if (RendererTracing() && getenv("REGPU_DUMP_TEXTURES"))
    {
        char name[64];
        snprintf(name, sizeof(name), "out/tex/%08X_%s_%ux%u.png", fetch.Address(), TextureFormatName(fetch.Format()), w, h);
        std::filesystem::create_directories("out/tex");
        WritePng(name, w, h, rgba);
    }
    if (dumpAddress && fetch.Address() == dumpAddress)
    {
        WritePng("texture.png", w, h, rgba);
        Log("renderer: wrote texture.png (%s %ux%u, endian %u, tiled %u, pitch %u)", TextureFormatName(fetch.Format()), w, h, fetch.Endian(),
            fetch.Tiled(), fetch.Pitch());
    }
    if (!cached.resource || cached.width != w || cached.height != h || cached.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        cached.resource->GetDesc().MipLevels != mipCount)
    {
        cached.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cached.native = false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = mipCount;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        RetireTexture(std::move(cached.resource));
        cached.resource = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST, "texture");
        cached.width = w;
        cached.height = h;
    }
    else
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = cached.resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        C().list->ResourceBarrier(1, &b);
    }
    for (UINT16 l = 0; l < mipCount; l++)
    {
        if (l > 0 && !DecodeTexture(levels[l], rgba))
        {
            NoteTextureFailure("mip decode failed", fetch);
            return nullptr;
        }
        const uint32_t lw = levels[l].Width(), lh = levels[l].Height();
        const UINT rowPitch = (lw * 4 + 255) & ~255u;
        const UINT64 offset = RingAlloc(UINT64(rowPitch) * lh, 512, "texture");
        if (offset == ~0ull)
        {
            NoteTextureFailure("upload ring full", fetch);
            return nullptr;
        }
        for (uint32_t y = 0; y < lh; y++)
            memcpy(Frame().ringCpu + offset + UINT64(y) * rowPitch, &rgba[size_t(y) * lw * 4], size_t(lw) * 4);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = cached.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = l;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = Frame().ring.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, lw, lh, 1, rowPitch };
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    C().list->ResourceBarrier(1, &b);
    cached.contentHash = hash;
    return cached.resource.Get();
}

// F12 capture: every distinct texture a draw of the traced frame binds goes into
// out/capture/textures.txt with its decoded image in out/capture/tex/, and a texture that could not
// be bound (the draw samples the white fallback) is named in out/play.log with the reason.
void CaptureTexture(const TextureFetch& fetch, uint32_t slot, bool cube, bool bound, uint32_t index, bool native, const char* why)
{
    static uint64_t captureFrame = 0;
    static std::set<uint64_t> seen;
    static std::ofstream summary;
    const uint64_t frame = TraceFrame();
    if (frame != captureFrame)
    {
        captureFrame = frame;
        seen.clear();
        std::filesystem::create_directories("out/capture/tex");
        std::filesystem::remove_all("out/capture/mem");
        summary.close();
        summary.open("out/capture/textures.txt");
        summary << "frame " << frame << " (draw numbers are those of the 'draw #N' lines in out/play.log)\n";
        summary << "draw slot address format size pitch tiled swizzle: status [image]\n";
    }
    const bool resolved = ResolvedFor(fetch) != nullptr;
    if (!bound && !cube)
        Log("    texture t%u %08X %s %ux%u is not bound: the draw samples white (%s)", slot, fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(),
            fetch.Height(), why[0] ? why : "unknown");
    const uint64_t key = (uint64_t(fetch.Address()) << 24) ^ (uint64_t(fetch.Format()) << 16) ^ (uint64_t(fetch.Width()) << 4) ^ fetch.Height() ^
        (uint64_t(cube) << 62);
    if (!seen.insert(key).second)
        return;
    char status[160];
    if (!bound)
        snprintf(status, sizeof(status), "WHITE FALLBACK: %s", why[0] ? why : "unknown");
    else if (resolved)
        snprintf(status, sizeof(status), "a resolved render target (out/res)");
    else
        snprintf(status, sizeof(status), "bound%s, bindless %u", native ? " (compressed on the GPU)" : "", index);
    std::string image;
    std::vector<uint8_t> rgba;
    if (!cube && !resolved && fetch.Type() == 2 && fetch.Dimension() == 1 && TextureFormatSupported(fetch.Format()) && DecodeTexture(fetch, rgba))
    {
        char name[160];
        snprintf(name, sizeof(name), "out/capture/tex/%08X_%s_%ux%u.png", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height());
        if (WritePng(name, fetch.Width(), fetch.Height(), rgba))
            image = name;
    }
    // The raw bytes of the base level as the game left them (out/capture/mem/), to look at a texture the
    // decode shows as noise: wrong layout, or not written yet.
    if (!cube && !resolved && fetch.Type() == 2)
    {
        const size_t bytes = std::min<size_t>(TextureSourceSize(fetch), 16u << 20);
        if (bytes)
        {
            std::filesystem::create_directories("out/capture/mem");
            char name[160];
            snprintf(name, sizeof(name), "out/capture/mem/%08X_%s_%ux%u.bin", fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height());
            std::ofstream raw(name, std::ios::binary);
            raw.write(reinterpret_cast<const char*>(GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()))), std::streamsize(bytes));
        }
    }
    char line[400];
    snprintf(line, sizeof(line), "%u t%u %08X %s %ux%u pitch %u %s sw%03X: %s %s%s\n", g_r.drawsThisFrame.load(), slot, fetch.Address(),
        TextureFormatName(fetch.Format()), fetch.Width(), fetch.Height(), fetch.Pitch(), fetch.Tiled() ? "tiled" : "linear", fetch.Swizzle(), status,
        cube ? "(cube map) " : "", image.c_str());
    summary << line;
    summary.flush();
}

uint32_t ReadIndex(uint32_t base, uint32_t i, bool is32, uint32_t endian)
{
    if (is32)
    {
        uint32_t v = *GuestPtr<uint32_t>(base + i * 4);
        if (endian == 2)
            v = __builtin_bswap32(v);
        else if (endian == 1)
            v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
        return v;
    }
    uint16_t v = *GuestPtr<uint16_t>(base + i * 2);
    if (endian == 1 || endian == 2)
        v = __builtin_bswap16(v);
    return v;
}

// Expanded host vertex list: guest indices, or rectangle markers
// (bit 31 | role << 29 | list position of the rectangle's first vertex).
bool BuildIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize, std::vector<uint32_t>& out)
{
    const uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    const bool is32 = (initiator >> 11) & 1;
    const uint32_t offset = gpureg::Get(VGT_INDX_OFFSET);
    // Reused between draws (no allocation per draw).
    thread_local std::vector<uint32_t> g;
    thread_local std::vector<uint8_t> cut;
    g.resize(count);
    cut.clear();
    // Primitive restart: a raw index equal to the reset value ends the strip/fan.
    const bool restart = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    const uint32_t resetIndex = gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX) & (is32 ? 0xFFFFFF : 0xFFFF);
    if (source == 0) // DMA
    {
        const uint32_t base = GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u));
        const uint32_t endian = dmaSize >> 30;
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        for (uint32_t i = 0; i < count && i < available; i++)
        {
            const uint32_t raw = ReadIndex(base, i, is32, endian);
            if (restart && (raw & (is32 ? 0xFFFFFF : 0xFFFF)) == resetIndex)
            {
                cut.resize(count);
                cut[i] = 1;
            }
            g[i] = raw + offset;
        }
        // A draw whose count runs past the index buffer's size (DMA size, dword 3 bits 0-23) reads
        // zero indices there, as the indexed path's buffer view does; the vector is reused between
        // draws, so without this those entries kept an earlier draw's indices (stray triangles).
        // REGPU_STALE_INDICES=1: leave them, as before (A/B). REGPU_TRACE_SHORT_INDICES=1: log such draws.
        static LiveSwitch staleIndices("REGPU_STALE_INDICES");
        static LiveSwitch traceShort("REGPU_TRACE_SHORT_INDICES");
        if (count > available)
        {
            static uint32_t logged = 0;
            if (traceShort && logged++ < 40)
                Log("indices: initiator %08X count %u past the DMA size %u (dma %08X)", initiator, count, available, dmaBase);
            if (!staleIndices)
                for (uint32_t i = available; i < count; i++)
                    g[i] = offset;
        }
    }
    else if (source == 2) // auto
    {
        for (uint32_t i = 0; i < count; i++)
            g[i] = i + offset;
    }
    else
        return false;

    out.clear();
    switch (prim)
    {
    case 1: // point list: a quad per point, corner in bits 29-30
    {
        out.resize(size_t(count) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t corner : { 0u, 1u, 2u, 2u, 1u, 3u })
                *o++ = (corner << 29) | (g[i] & 0x1FFFFFFF);
        return true;
    }
    case 4: // triangle list
        out.assign(g.begin(), g.begin() + (count / 3) * 3);
        return true;
    case 6: // triangle strip(s), split at restart indices
    case 5: // triangle fan(s)
    {
        out.resize(count >= 3 ? size_t(count - 2) * 3 : 0); // upper bound
        uint32_t* o = out.data();
        for (uint32_t start = 0; start < count;)
        {
            uint32_t end = start;
            if (cut.empty())
                end = count;
            else
                while (end < count && !cut[end])
                    end++;
            for (uint32_t i = start; i + 2 < end; i++)
            {
                const uint32_t k = i - start;
                if (prim == 5)
                    o[0] = g[start], o[1] = g[i + 1], o[2] = g[i + 2];
                else if (k & 1)
                    o[0] = g[i + 1], o[1] = g[i], o[2] = g[i + 2];
                else
                    o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2];
                o += 3;
            }
            start = end + 1;
        }
        out.resize(size_t(o - out.data()));
        return true;
    }
    case 13: // quad list
    {
        out.resize(size_t(count / 4) * 6);
        uint32_t* o = out.data();
        for (uint32_t i = 0; i + 3 < count; i += 4, o += 6)
            o[0] = g[i], o[1] = g[i + 1], o[2] = g[i + 2], o[3] = g[i], o[4] = g[i + 2], o[5] = g[i + 3];
        return true;
    }
    case 8: // rectangle list: first triangle as given, second from the
            // hypotenuse ends and the computed fourth corner
        // The Xenos draws each rectangle as one primitive with the given triangle's facing. The second
        // triangle is ordered (first hypotenuse end, fourth corner, second end), which keeps that winding:
        // in the other order it faced the other way, so a draw with separate back-face stencil ops (a
        // depth reset under the windscreen, stencil NOTEQUAL front, ALWAYS back) applied them to half of
        // every rectangle. REGPU_OLD_RECT_WINDING=1 (read at start) restores the old order (A/B).
        static const bool oldWinding = getenv("REGPU_OLD_RECT_WINDING") != nullptr;
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t pos = uint32_t(out.size());
            out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
            for (uint32_t role : oldWinding ? std::initializer_list<uint32_t>{ 1u, 2u, 0u } : std::initializer_list<uint32_t>{ 1u, 0u, 2u })
                out.push_back(0x80000000u | (role << 29) | pos);
        }
        return true;
    default:
        return false;
    }
}
}

namespace
{
// Expanded index lists (strips with restarts, fans, points, rectangles), kept across frames
// and keyed by the draw's index parameters and a sampled hash of the guest indices (like
// index buffers); each is copied into the ring once per frame.
struct IndexListEntry
{
    bool valid = false, ok = false;
    uint64_t sample = 0;
    std::vector<uint32_t> list;
    uint32_t maxIndex = 0;  // the highest vertex index in `list` (an upper bound for rectangles and points)
    uint64_t frame = ~0ull; // the frame `offset` belongs to
    UINT64 offset = 0;
};

IndexListEntry* CachedIndexList(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    const uint32_t source = (initiator >> 6) & 3, count = initiator >> 16;
    const bool is32 = (initiator >> 11) & 1;
    uint64_t key = 0xCBF29CE484222325ull;
    for (uint32_t v : { initiator, dmaBase, dmaSize, gpureg::Get(VGT_INDX_OFFSET), gpureg::Get(PA_SU_SC_MODE_CNTL) & (1u << 21),
             gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX) })
        key = (key ^ v) * 0x100000001B3ull;
    uint64_t sample = 0;
    if (source == 0)
    {
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        const uint32_t bytes = std::min(count, available) * (is32 ? 4 : 2);
        sample = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u))), bytes);
    }
    thread_local std::unordered_map<uint64_t, IndexListEntry> cache;
    if (cache.size() > 8192)
        cache.clear();
    IndexListEntry& e = cache[key];
    if (!e.valid || e.sample != sample)
    {
        e.valid = true;
        e.sample = sample;
        e.frame = ~0ull;
        e.ok = BuildIndexList(initiator, dmaBase, dmaSize, e.list);
        // Rectangle and point entries carry flags in bits 29-31 over a list position or index.
        e.maxIndex = 0;
        for (uint32_t v : e.list)
            e.maxIndex = std::max(e.maxIndex, v & 0x1FFFFFFFu);
    }
    return e.ok ? &e : nullptr;
}

// Origin (screen rows) and screen rectangle of the tile pass being replayed.
struct TileState
{
    int originX = 0, originY = 0;
    D3D12_RECT rect{ 0, 0, 1 << 14, 1 << 14 };
    uint32_t surface = ~0u; // RB_SURFACE_INFO of the tiled pass (~0: any)
};
thread_local TileState g_tile;
// Tiles side by side (window offset x) are placed like tiles one above another; REGPU_NO_TILE_X
// keeps x at the screen origin (rows only) for an A/B.
bool TileColumns()
{
    static LiveSwitch noTileX("REGPU_NO_TILE_X");
    return !noTileX;
}
static LiveSwitch g_noForeignTile("REGPU_NO_FOREIGN_TILE"); // A/B for the paint-editor tile fix

// Tiling: each tile pass is shifted into EDRAM by the window offset and
// clipped to the tile. We render in screen space, so draws (and resolves)
// stay unshifted and are clipped to the tile's screen rectangle.
D3D12_RECT WindowScissor()
{
    const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
    const int32_t wx = int32_t(offsetReg << 17) >> 17, wy = int32_t((offsetReg >> 16) << 17) >> 17;
    const bool vertexOffset = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1;
    const uint32_t tl = gpureg::Get(PA_SC_WINDOW_SCISSOR_TL), br = gpureg::Get(PA_SC_WINDOW_SCISSOR_BR);
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF), x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!(tl >> 31))
    {
        x0 += wx; x1 += wx; y0 += wy; y1 += wy; // to EDRAM space
    }
    if (vertexOffset)
    {
        x0 -= wx; x1 -= wx; y0 -= wy; y1 -= wy; // back to screen space
    }
    // Taller than the screen on a pass narrower than it: a tall render target (see PassHeight).
    const uint32_t surfaceInfo = gpureg::Get(0x2000 /* RB_SURFACE_INFO */);
    static LiveSwitch noTall("REGPU_NO_TALL_PASS");
    if (!noTall && y1 > int32_t(kHeight) && y1 <= 2048 && (surfaceInfo & 0x3FFF) < kWidth)
    {
        if (g_tallInfo != surfaceInfo || g_tallHeight != uint32_t(y1))
            Log("tall pass: scissor y %d..%d x %d..%d window offset %d,%d vertexOffset %d surface %08X", y0, y1, x0, x1, wx, wy, int(vertexOffset), surfaceInfo);
        g_tallInfo = surfaceInfo;
        g_tallHeight = uint32_t(y1);
        g_tallKey = LayoutKey(surfaceInfo);
        // REGPU_CAPTURE_TALL=1: trace this very frame from here on (its dump is taken at the swap).
        static LiveSwitch captureTall("REGPU_CAPTURE_TALL");
        static bool traced = false;
        if (captureTall && !traced)
        {
            traced = true;
            g_hotkeyFrame = g_r.frame + 1;
            g_tallSeen = true;
        }
    }
    const LONG maxY = LONG(PassHeight());
    const LONG maxX = LONG(PassWidth());
    return { std::clamp<LONG>(x0, 0, maxX), std::clamp<LONG>(y0, 0, maxY), std::clamp<LONG>(x1, 0, maxX), std::clamp<LONG>(y1, 0, maxY) };
}

void TransitionColor(D3D12_RESOURCE_STATES to)
{
    if (C().colorState == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = C().color.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = C().colorState;
    b.Transition.StateAfter = to;
    C().list->ResourceBarrier(1, &b);
    C().colorState = to;
}

// Draws the resolve rectangle of the colour (or depth) buffer into the
// destination's host texture at the same pixels, converting the format and
// scaling colour by the resolve's exponent bias.
// Makes `dest` the variant of that size and format (an earlier one from its spares, or new).
void EnsureResolved(ResolvedTexture& dest, uint32_t width, uint32_t height, uint32_t hostWidth, uint32_t hostHeight, DXGI_FORMAT format)
{
    auto swapVariant = [](ResolvedTexture& a, ResolvedTexture& b) {
        std::swap(a.resource, b.resource);
        std::swap(a.width, b.width);
        std::swap(a.height, b.height);
        std::swap(a.hostWidth, b.hostWidth);
        std::swap(a.hostHeight, b.hostHeight);
        std::swap(a.frame, b.frame);
        std::swap(a.serial, b.serial);
        std::swap(a.format, b.format);
        std::swap(a.swapped, b.swapped);
    };
    if (!dest.resource || dest.width != width || dest.height != height || dest.hostWidth != hostWidth || dest.hostHeight != hostHeight ||
        dest.format != format)
    {
        bool found = false;
        for (ResolvedTexture& spare : dest.spares)
            if (spare.resource && spare.width == width && spare.height == height && spare.hostWidth == hostWidth &&
                spare.hostHeight == hostHeight && spare.format == format)
            {
                swapVariant(dest, spare);
                found = true;
                break;
            }
        if (!found)
        {
            if (dest.resource)
            {
                // The displaced variant stays available; bound the pool.
                if (dest.spares.size() >= 6)
                {
                    Frame().garbage.push_back(std::move(dest.spares.front().resource));
                    dest.spares.erase(dest.spares.begin());
                }
                dest.spares.emplace_back();
                swapVariant(dest, dest.spares.back());
            }
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = hostWidth;
            td.Height = hostHeight;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = format;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            // Colour images can also be an upscaler's output (written by compute).
            if (HostColorTexture(format))
                td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&dest.resource)),
                "CreateCommittedResource(resolve)");
            CountCreate("resolve");
            dest.width = width;
            dest.height = height;
            dest.hostWidth = hostWidth;
            dest.hostHeight = hostHeight;
            dest.format = format;
            dest.swapped = false;
        }
    }
}

// unorm: the destination format is fixed point, so the copy saturates (after the exponent bias).
// ceiling: a 7e3 source's stored range (each colour channel held to [0, ceiling], alpha saturated) before the scale; 0 none.
// destRange: the destination's fixed-point range after the scale ([0, 1] for unorm, the 16-bit integer range for 16-bit
// fixed formats); null none.
// alias: a depth copy through a view of another row width than the depth base's host surface (see
// EdramAlias): the draws placed its tiles where they lie in the host layout, so the copy reads them
// from there, run by run.
void BlitResolve(bool depth, uint32_t base, uint32_t width, uint32_t height, DXGI_FORMAT format, const D3D12_RECT& rect, float scale, bool swapRB = false,
    const float* destRange = nullptr, float ceiling = 0.0f, const EdramAlias* alias = nullptr)
{
    ResolvedTexture& dest = g_r.resolved[base];
    const uint32_t hostWidth = uint32_t(HostX(LONG(width))), hostHeight = uint32_t(HostY(LONG(height)));
    EnsureResolved(dest, width, height, hostWidth, hostHeight, format);
    if (static const bool named = getenv("REGPU_D3D12_DEBUG") != nullptr; named && dest.resource)
    {
        // (Debug layer messages name the resource.)
        wchar_t name[64];
        swprintf(name, 64, L"resolve %08X %ux%u", base, width, height);
        dest.resource->SetName(name);
    }
    if (dest.frame != g_r.frame) // (once per frame: tiles land in the same image)
        NoteResolvedMemory(base, dest);
    dest.frame = g_r.frame;
    dest.serial = ++g_r.resolveSerial;
    // (Logged when the draw number it happens at changes for that address.)
    static std::unordered_map<uint32_t, uint32_t> lastResolveDraw;
    if (const auto& traced = TracedBindAddresses(); !traced.empty() && std::find(traced.begin(), traced.end(), base & 0x1FFFFFFF) != traced.end() &&
        std::exchange(lastResolveDraw[base & 0x1FFFFFFF], g_r.drawsThisFrame.load() + 1) != g_r.drawsThisFrame.load() + 1)
        Log("bind: frame %llu draw %u resolve into %08X %ux%u%s serial %llu", (unsigned long long)g_r.frame, g_r.drawsThisFrame.load(), base & 0x1FFFFFFF,
            width, height, depth ? " (depth)" : "", (unsigned long long)dest.serial);
    const D3D12_RECT sr{ rect.left, rect.top, std::min<LONG>(rect.right, width), std::min<LONG>(rect.bottom, height) };
    FrameResources& f = Frame();
    if (sr.right <= sr.left || sr.bottom <= sr.top || f.descriptorsUsed >= kDescriptorsPerFrame)
        return;

    const UINT slot = f.descriptorBase + f.descriptorsUsed++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : C().colorFormat;
    srv.ViewDimension = g_msaa > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(depth ? C().depth.Get() : C().color.Get(), &srv, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    const auto rtv = g_r.blitRtvHeap->GetCPUDescriptorHandleForHeapStart();
    C().list->CreateRenderTargetView(g_r.device, dest.resource.Get(), rtv); // in order with the recorded commands

    D3D12_RESOURCE_BARRIER b[2]{};
    UINT barriers = 1;
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = dest.resource.Get();
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (depth)
    {
        b[1].Transition.pResource = C().depth.Get();
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers = 2;
    }
    else
        TransitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    C().list->ResourceBarrier(barriers, b);

    const bool ms = g_msaa > 1;
    ID3D12PipelineState* pso = depth ? (ms ? g_r.depthBlitMs.Get() : g_r.depthBlit.Get())
        : format == DXGI_FORMAT_R8G8B8A8_UNORM ? (ms ? g_r.colorBlitMs8.Get() : g_r.colorBlit8.Get())
        : (ms ? g_r.colorBlitMs16.Get() : g_r.colorBlit16.Get());
    C().list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    C().list->SetGraphicsRootSignature(g_r.blitRootSignature.Get());
    C().list->SetPipelineState(pso);
    C().list->SetGraphicsRootDescriptorTable(0, Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    // Constants 6-9: the source texel's scale and offset (identity unless through an alias).
    uint32_t blitConstants[10] = { std::bit_cast<uint32_t>(scale), uint32_t(swapRB), uint32_t(destRange != nullptr), std::bit_cast<uint32_t>(ceiling),
        std::bit_cast<uint32_t>(destRange ? destRange[0] : 0.0f), std::bit_cast<uint32_t>(destRange ? destRange[1] : 0.0f),
        std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(1.0f), 0, 0 };
    // Width and height stay in game pixels (the size the title knows); the texture and the
    // rectangle are scaled.
    const D3D12_VIEWPORT vp{ 0, 0, float(hostWidth), float(hostHeight), 0, 1 };
    C().list->RSSetViewports(1, &vp);
    if (!alias)
    {
        const D3D12_RECT hostRect = ToHost(sr);
        C().list->SetGraphicsRoot32BitConstants(1, 10, blitConstants, 0);
        C().list->RSSetScissorRects(1, &hostRect);
        C().list->DrawInstanced(3, 1, 0, 0);
    }
    else
    {
        // As the draws place them (DrawImpl): each run of tiles along a view tile row that stays on
        // one row of the host layout lies (dx, dy) samples away on the host surface. A destination
        // pixel P (host scale S) is view sample P / S x the view's MSAA; plus (dx, dy), over the
        // host's MSAA, times S, is the host texel: P x va / ha + S x dx / ha (and down alike).
        const uint32_t viewMsaa = (alias->viewSurface >> 16) & 3, hostMsaa = (alias->hostSurface >> 16) & 3;
        const int32_t va = int32_t(MsaaAcross(viewMsaa)), vd = int32_t(MsaaDown(viewMsaa));
        const int32_t ha = int32_t(MsaaAcross(hostMsaa)), hd = int32_t(MsaaDown(hostMsaa));
        const float s = ScaleY();
        blitConstants[6] = std::bit_cast<uint32_t>(float(va) / float(ha));
        blitConstants[7] = std::bit_cast<uint32_t>(float(vd) / float(hd));
        ForEachAliasRun(*alias, sr, [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t dx, int32_t dy) {
            blitConstants[8] = std::bit_cast<uint32_t>(s * float(dx) / float(ha));
            blitConstants[9] = std::bit_cast<uint32_t>(s * float(dy) / float(hd));
            const D3D12_RECT runRect{ LONG(std::lround(std::floor(float(x0) / float(va) * s))), LONG(std::lround(std::floor(float(y0) / float(vd) * s))),
                LONG(std::lround(std::ceil(float(x1) / float(va) * s))), LONG(std::lround(std::ceil(float(y1) / float(vd) * s))) };
            C().list->SetGraphicsRoot32BitConstants(1, 10, blitConstants, 0);
            C().list->RSSetScissorRects(1, &runRect);
            C().list->DrawInstanced(3, 1, 0, 0);
        });
    }

    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    C().list->ResourceBarrier(barriers, b);
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Back to the main targets and root signature.
    C().list->OMSetRenderTargets(1, &C().rtv, FALSE, &C().dsv);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    g_drawState.Invalidate();
}

// Moves a half-float colour surface (see ExactInHalf) to g_colorFormat for good, its pixels copied
// (the list must be open; the caller binds the targets again).
void PromoteSurface(Renderer::Surface& s, uint32_t base)
{
    ComPtr<ID3D12Resource> old = std::move(s.resource);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = old->GetDesc();
    td.Format = g_colorFormat;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&s.resource)),
        "CreateCommittedResource(colour surface)"); CountCreate("colour surface");
    s.format = td.Format;
    C().list->CreateRenderTargetView(g_r.device, s.resource.Get(), s.view); // in order with the recorded commands
    FrameResources& f = Frame();
    const bool copied = f.descriptorsUsed < kDescriptorsPerFrame;
    if (copied)
    {
        const UINT slot = f.descriptorBase + f.descriptorsUsed++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kHalfColor;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(old.Get(), &srv, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
        if (s.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = old.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = s.state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            C().list->ResourceBarrier(1, &b);
        }
        C().list->OMSetRenderTargets(1, &s.view, FALSE, nullptr);
        C().list->SetGraphicsRootSignature(g_r.blitRootSignature.Get());
        C().list->SetPipelineState(g_r.colorBlit16.Get());
        C().list->SetGraphicsRootDescriptorTable(0, Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
        const uint32_t blitConstants[10] = { std::bit_cast<uint32_t>(1.0f), 0, 0, 0, 0, 0, std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(1.0f), 0, 0 };
        C().list->SetGraphicsRoot32BitConstants(1, 10, blitConstants, 0);
        const D3D12_VIEWPORT vp{ 0, 0, float(td.Width), float(td.Height), 0, 1 };
        const D3D12_RECT all{ 0, 0, LONG(td.Width), LONG(td.Height) };
        C().list->RSSetViewports(1, &vp);
        C().list->RSSetScissorRects(1, &all);
        C().list->DrawInstanced(3, 1, 0, 0);
        C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
        g_drawState.Invalidate();
    }
    s.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    f.garbage.push_back(std::move(old));
    g_halfSurfaces--;
    Log("renderer: EDRAM colour surface at base %03X moved to 32-bit floats (bound as format %u)%s", base,
        (gpureg::Get(RB_COLOR_INFO) >> 16) & 0xF, copied ? "" : ", not copied: out of descriptors");
    LogHalfSurfaces();
}

// EDRAM resolve: copy a rectangle of the render target to the destination
// (as a host texture), then optionally clear colour and depth.
// ---- Upscaling ---------------------------------------------------------------------------
// Temporal upscalers (DLSS, FSR 3): the motion target, jitter, and the evaluation.
bool InitUpscale();
bool TemporalInit()
{
    if (!InitUpscale())
        return false;
    if (!g_upscale.motion)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = OutputWidth();
        td.Height = OutputHeight();
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = td.Format;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                IID_PPV_ARGS(&g_upscale.motion))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(g_r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_upscale.motionRtvHeap))))
            return false;
        g_upscale.motionRtv = g_upscale.motionRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_r.device->CreateRenderTargetView(g_upscale.motion.Get(), nullptr, g_upscale.motionRtv);
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&g_upscale.depth))))
            return false;
    }
    return true;
}

// A tile of the scene was resolved (its depth may be cleared next): keep its depth.
void CopySceneDepth(const D3D12_RECT& gameRect)
{
    const D3D12_RECT r = ToHost(gameRect);
    FrameResources& f = Frame();
    if (!g_upscale.depth || !C().depth)
    {
        g_tstat.depthNoTarget++;
        return;
    }
    if (r.right <= r.left || r.bottom <= r.top || f.descriptorsUsed + 2 > kDescriptorsPerFrame)
    {
        g_tstat.depthEmpty++;
        return;
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(C().depth.Get(), &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_upscale.depth.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = C().depth.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    C().list->ResourceBarrier(1, &b);
    uint32_t c[24] = {};
    c[0] = uint32_t(r.left);
    c[1] = uint32_t(r.top);
    c[20] = uint32_t(r.right - r.left);
    c[21] = uint32_t(r.bottom - r.top);
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.depthCopy.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch((c[20] + 7) / 8, (c[21] + 7) / 8, 1);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    g_tstat.depthCopies++;
}

float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index; index /= base)
    {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// Start of a temporal frame: jitter, a cleared motion target, the next constants map.
void BeginTemporalFrame()
{
    g_upscale.prevPostDraws = g_upscale.postDraws;
    g_upscale.postDraws = 0;
    g_upscale.sceneDraws = 0;
    g_upscale.rearms = 0;
    g_tstat.frames++;
    g_motionStatFrames++;
    // (The constants comparison reads write-combined memory: 40-130 ms on every sampled frame, a hitch
    // every ~1.7 s in a busy scene. Diagnostics only, so opt-in.)
    static LiveSwitch motionConstants("REGPU_MOTION_CONSTANTS");
    g_motionSampleThisFrame = motionConstants && g_motionStatFrames % 100 == 2;
    if (g_motionSampleThisFrame)
        g_motionSampledFrames++;
    MotionStatsLog();
    static auto statsLogged = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - statsLogged >= std::chrono::seconds(1))
    {
        statsLogged = now;
        auto take = [](std::atomic<uint32_t>& v) { return v.exchange(0); };
        const uint32_t frames = take(g_tstat.frames), draws = take(g_tstat.draws), edram4 = take(g_tstat.edram4), wide = take(g_tstat.widePass),
            want = take(g_tstat.wantMotion), missing = take(g_tstat.shaderMissing), notReady = take(g_tstat.shaderNotReady), noPso = take(g_tstat.noPipeline),
            motion = take(g_tstat.motion), known = take(g_tstat.known), resolves = take(g_tstat.resolves), fullWidth = take(g_tstat.resolveFullWidth),
            done = take(g_tstat.sceneDone), copies = take(g_tstat.depthCopies), noTarget = take(g_tstat.depthNoTarget), empty = take(g_tstat.depthEmpty),
            rearmed = take(g_tstat.rearmed);
        Log("temporal: %u frames: scene draws %u (edram mode 4: %u, wide pass: %u, want motion: %u; motion shader missing %u, not ready %u, no pipeline %u; "
            "writing motion %u, known from last frame %u); colour resolves %u (full width %u), scenes done %u (re-armed %u), depth copies %u (skipped: no target %u, empty %u); full-width draws after the scene %u",
            frames, draws, edram4, wide, want, missing, notReady, noPso, motion, known, resolves, fullWidth, done, rearmed, copies, noTarget, empty,
            g_upscale.prevPostDraws);
    }
    g_upscale.renderWidth = uint32_t(std::lround(OutputWidth() * double(g_sceneScale)));
    g_upscale.renderHeight = uint32_t(std::lround(OutputHeight() * double(g_sceneScale)));
    // Phases: 8 per pixel of upscale area (DLSS guidance), at most 64.
    const double ratio = double(OutputWidth()) / std::max(1u, g_upscale.renderWidth);
    const uint32_t phases = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
    const uint32_t index = uint32_t(g_r.frame % phases) + 1;
    static LiveSwitch noJitter("REGPU_NO_JITTER");
    g_upscale.jitterPx[0] = noJitter ? 0.0f : Halton(index, 2) - 0.5f;
    g_upscale.jitterPx[1] = noJitter ? 0.0f : Halton(index, 3) - 0.5f;
    g_upscale.jitterNdc[0] = 2.0f * g_upscale.jitterPx[0] / float(std::max(1u, g_upscale.renderWidth));
    g_upscale.jitterNdc[1] = -2.0f * g_upscale.jitterPx[1] / float(std::max(1u, g_upscale.renderHeight));
    g_upscale.constantsFrame++;
    g_upscale.occurrences.clear();
    if (g_upscale.constantsFrame % 600 == 0) // draws not seen for a while
        std::erase_if(g_upscale.constants, [](const auto& e) { return e.second.frame + 2 < g_upscale.constantsFrame; });
    const float zero[4] = {};
    C().list->ClearRenderTargetView(g_upscale.motionRtv, zero, 0, nullptr);
}

void UpscaleResolved(uint32_t base);
void ViewUpscalerInput(ID3D12Resource* source, DXGI_FORMAT sourceFormat, ID3D12Resource* out, bool depth, uint32_t renderWidth, uint32_t renderHeight,
    uint32_t width, uint32_t height);

// DLSS or FSR 3 on the scene image at `base`: colour (render size), depth and motion (top left of
// output-sized textures); the upscaled image replaces it like UpscaleResolved.
bool EvaluateTemporal(uint32_t base, bool dlss)
{
    if (dlss && !DlssInit(g_r.device))
        return false;
    auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource)
        return false;
    ResolvedTexture& dest = it->second;
    if (!HostColorTexture(dest.format))
        return false;
    const ComPtr<ID3D12Resource> color = dest.resource;
    const uint32_t inWidth = dest.hostWidth, inHeight = dest.hostHeight;
    const float savedScale = g_sceneScale;
    g_sceneScale = 1.0f;
    const uint32_t outWidth = uint32_t(HostX(LONG(dest.width))), outHeight = uint32_t(HostY(LONG(dest.height)));
    g_sceneScale = savedScale;
    if (outWidth < inWidth || outHeight < inHeight)
        return false;
    const uint64_t frame = dest.frame;
    const DXGI_FORMAT format = dest.format;
    const bool swapped = dest.swapped;
    // DLAA (native scale): the output must be another texture; the pool keys variants on
    // size, so a same-size output gets its own texture, copied back after.
    const bool dlaa = outWidth == inWidth && outHeight == inHeight;
    if (dlaa)
    {
        if (!g_upscale.dlaaOut || g_upscale.dlaaOut->GetDesc().Width != outWidth || g_upscale.dlaaOut->GetDesc().Height != outHeight ||
            g_upscale.dlaaOut->GetDesc().Format != format)
        {
            if (g_upscale.dlaaOut)
                Frame().garbage.push_back(std::move(g_upscale.dlaaOut));
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = color->GetDesc();
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_upscale.dlaaOut))))
                return false;
        }
    }
    else
        EnsureResolved(dest, dest.width, dest.height, outWidth, outHeight, format);
    dest.frame = frame;
    dest.swapped = swapped;

    DlssSetup setup;
    setup.renderWidth = inWidth;
    setup.renderHeight = inHeight;
    setup.outputWidth = outWidth;
    setup.outputHeight = outHeight;
    setup.quality = GetSettings().quality;
    setup.hdr = format != DXGI_FORMAT_R8G8B8A8_UNORM;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    DlssFrame in{};
    in.color = color.Get();
    in.depth = g_upscale.depth.Get();
    in.motion = g_upscale.motion.Get();
    in.output = dlaa ? g_upscale.dlaaOut.Get() : dest.resource.Get();
    static const float jitterSign = getenv("REGPU_DLSS_JITTER_FLIP") ? -1.0f : 1.0f;
    in.jitterX = g_upscale.jitterPx[0] * jitterSign;
    in.jitterY = g_upscale.jitterPx[1] * jitterSign;
    in.motionScaleX = float(inWidth);
    in.motionScaleY = float(inHeight);
    in.frameTimeMs = float(std::chrono::duration<double, std::milli>(now - last).count());
    in.reset = g_upscale.reset;
    last = now;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier(g_upscale.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(g_upscale.motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(in.output, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // NGX records into the real command list (after everything recorded so far).
    const bool ok = dlss ? DlssEvaluate(C().list->Real(), setup, in)
                         : GetSettings().upscaler == Settings::Upscaler::Fsr4 ? Fsr4Evaluate(g_r.device, C().list->Real(), setup, in, GetSettings().sharpness)
                                                             : Fsr3Evaluate(g_r.device, C().list->Real(), setup, in, GetSettings().sharpness);
    g_upscale.viewValid = ok; // (REGPU_VIEW_MOTION / REGPU_VIEW_DEPTH show it at the end of the frame, ViewFrontBuffer)
    g_upscale.viewWidth = inWidth;
    g_upscale.viewHeight = inHeight;
    barrier(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    if (dlaa && ok)
    {
        barrier(in.output, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(color.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        C().list->Real()->CopyResource(color.Get(), in.output);
        barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
        barrier(in.output, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    }
    barrier(g_upscale.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(g_upscale.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // NGX may have changed the list's heaps, root signatures and pipeline: set ours again.
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    C().list->SetDescriptorHeaps(1, heaps);
    C().list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    C().list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_drawState.Invalidate();
    SelectSurfaces(true);
    if (!ok && !dlaa) // keep the image: show the small one upscaled by FSR 1 instead
    {
        dest.resource = color;
        dest.hostWidth = inWidth;
        dest.hostHeight = inHeight;
        return false;
    }
    return true;
}

// REGPU_VIEW_MOTION / REGPU_VIEW_DEPTH (live, see tools/regpuctl.py switch): show the upscaler's
// motion or depth input in place of the finished frame (ViewFrontBuffer), to tune the motion path.
// Motion: grey is still, red and green are motion (about +-8 render pixels over the full range);
// depth: the stored value (reversed depth reads white when near). `source` is in
// NON_PIXEL_SHADER_RESOURCE, `out` in UNORDERED_ACCESS; the data is in the top left renderWidth x
// renderHeight of `source`, and fills the width x height of `out`.
void ViewUpscalerInput(ID3D12Resource* source, DXGI_FORMAT sourceFormat, ID3D12Resource* out, bool depth, uint32_t renderWidth, uint32_t renderHeight,
    uint32_t width, uint32_t height)
{
    FrameResources& f = Frame();
    if (!g_upscale.view || f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return;
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = sourceFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(source, &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = out->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(out, nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    uint32_t c[24] = {};
    c[16] = depth ? 1u : 0u; // rcasCon.x
    const float gain = float(renderWidth) / 16.0f; // one render pixel of motion is 1/renderWidth: +-8 px fills the range
    memcpy(&c[17], &gain, sizeof(gain));           // rcasCon.y
    c[18] = renderWidth;                           // rcasCon.zw
    c[19] = renderHeight;
    c[20] = width;
    c[21] = height;
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() }; // (the upscaler may have changed them)
    C().list->SetDescriptorHeaps(1, heaps);
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.view.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = out;
    C().list->ResourceBarrier(1, &b);
}

// The temporal upscale of the scene image at `base` (colour, depth, motion, jitter); FSR 1
// where the chosen upscaler is not available.
void TemporalEvaluate(uint32_t base)
{
    const bool done = EvaluateTemporal(base, GetSettings().upscaler == Settings::Upscaler::Dlss);
    if (!done)
        UpscaleResolved(base);
    g_upscale.reset = false;
}

// The scene (everything up to the full-screen resolve of the main view) renders at
// g_sceneScale; that resolved image is upscaled to the output size (FSR 1: EASU, then RCAS
// sharpening) and post-processing and the HUD render at full size from it.

bool InitUpscale()
{
    if (g_upscale.tried)
        return g_upscale.ready;
    g_upscale.tried = true;
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 1;
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 24;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g_r.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_upscale.root))))
    {
        Log("upscale: root signature failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_upscale.root.Get();
    pd.CS = { kEasuMain, sizeof(kEasuMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.easu))))
        return false;
    pd.CS = { kRcasMain, sizeof(kRcasMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.rcas))))
        return false;
    pd.CS = { kDepthCopyMain, sizeof(kDepthCopyMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.depthCopy))))
        return false;
    srv.NumDescriptors = 2;
    blob.Reset();
    error.Reset();
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g_r.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_upscale.gammaRoot))))
    {
        Log("upscale: gamma root signature failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    pd.pRootSignature = g_upscale.gammaRoot.Get();
    pd.CS = { kGammaMain, sizeof(kGammaMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.gamma))))
        return false;
    pd.pRootSignature = g_upscale.root.Get();
    pd.CS = { kEnhanceMain, sizeof(kEnhanceMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.enhance))))
        return false;
    pd.CS = { kViewMain, sizeof(kViewMain) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_upscale.view))))
        return false;
    g_upscale.ready = true;
    Log("upscale: FSR 1 ready (scene at %.0f%% of %ux%u)", GetSettings().SceneScale() * 100.0f, OutputWidth(), OutputHeight());
    return true;
}

// ---- Display gamma ---------------------------------------------------------------------
// The console's display maps each front buffer value through a ramp the title uploads (gamma_ramp.h):
// the 256-entry table for an 8-bit front buffer, the piecewise-linear ramp for a 10-bit one
// (2_10_10_10), per channel. Often not the identity (one title's table darkens by about x^1.15, with a
// darker toe: without it the picture was ~20% brighter than the console's). REGPU_NO_GAMMA=1 shows
// the front buffer as is; REGPU_GAMMA_TABLE_ONLY=1 applies the table to every front buffer (A/B).
ComPtr<ID3D12Resource> g_gammaOut;
ComPtr<ID3D12Resource> g_gammaCurve; // 1024x1 RGBA32F: the curve, `size` entries used
uint64_t g_gammaCurveKey = ~0ull;
uint32_t g_gammaCurveSize = 256;
bool g_gammaCurveIdentity = true;
ComPtr<ID3D12Resource> g_enhanceOut; // enhanced lighting output (Settings::enhance)
ComPtr<ID3D12Resource> g_viewOut; // REGPU_VIEW_MOTION / REGPU_VIEW_DEPTH output (ViewFrontBuffer)

// The display curve for this front buffer, uploaded when a ramp changed; false if it can't be.
bool UpdateGammaCurve(bool tenBit, bool off)
{
    const uint64_t key = (gamma::Generation() << 2) | (tenBit ? 2u : 0u) | (off ? 1u : 0u);
    if (g_gammaCurve && key == g_gammaCurveKey)
        return true;
    gamma::Curve curve;
    gamma::Build(tenBit, curve);
    if (off)
    {
        curve.identity = true;
        for (uint32_t c = 0; c < 3; c++)
            for (uint32_t v = 0; v < curve.size; v++)
                curve.rgb[c][v] = float(v) / float(curve.size - 1);
    }
    constexpr UINT kEntries = 1024, kRowBytes = kEntries * 16;
    if (!g_gammaCurve)
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kEntries;
        td.Height = 1;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        g_gammaCurve = CreateTexture(td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "gamma curve");
        if (!g_gammaCurve)
            return false;
    }
    const UINT64 offset = RingAlloc(kRowBytes, 512, "gamma curve");
    if (offset == ~0ull)
        return false;
    float* row = reinterpret_cast<float*>(Frame().ringCpu + offset);
    for (uint32_t v = 0; v < kEntries; v++)
    {
        const uint32_t at = std::min(v, curve.size - 1);
        row[v * 4 + 0] = curve.rgb[0][at];
        row[v * 4 + 1] = curve.rgb[1][at];
        row[v * 4 + 2] = curve.rgb[2][at];
        row[v * 4 + 3] = 1.0f;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_gammaCurve.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    C().list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = g_gammaCurve.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = Frame().ring.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, kEntries, 1, 1, kRowBytes };
    C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    g_gammaCurveKey = key;
    g_gammaCurveSize = curve.size;
    g_gammaCurveIdentity = curve.identity;
    return true;
}

// REGPU_VIEW_MOTION / REGPU_VIEW_DEPTH: the upscaler's motion or depth input of this frame as the
// finished frame (HUD included, hidden), at the shown image's size, written after the post-processing
// and the display's gamma so its colours are as written. Null when off, or this frame had no temporal
// scene. The result is in COPY_SOURCE state.
ID3D12Resource* ViewFrontBuffer(ID3D12Resource* shown)
{
    static LiveSwitch viewMotion("REGPU_VIEW_MOTION"), viewDepth("REGPU_VIEW_DEPTH");
    if (!shown || !g_upscale.viewValid || !(viewMotion || viewDepth) || !InitUpscale() || !g_upscale.view || !g_upscale.motion || !g_upscale.depth)
        return nullptr;
    g_upscale.viewValid = false;
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return nullptr;
    const D3D12_RESOURCE_DESC desc = shown->GetDesc();
    if (!g_viewOut || g_viewOut->GetDesc().Width != desc.Width || g_viewOut->GetDesc().Height != desc.Height)
    {
        D3D12_RESOURCE_DESC td = desc;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_viewOut));
        g_viewOut = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_SOURCE, "view");
        if (!g_viewOut)
            return nullptr;
    }
    const bool motion = viewMotion;
    ID3D12Resource* source = motion ? g_upscale.motion.Get() : g_upscale.depth.Get();
    // Motion rests as a render target, depth as UAV; the output in COPY_SOURCE (as the gamma copy does).
    const D3D12_RESOURCE_STATES sourceRest = motion ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    barrier(source, sourceRest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(g_viewOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ViewUpscalerInput(source, motion ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT, g_viewOut.Get(), !motion, g_upscale.viewWidth, g_upscale.viewHeight,
        uint32_t(desc.Width), desc.Height);
    barrier(g_viewOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, sourceRest);
    g_drawState.Invalidate();
    return g_viewOut.Get();
}

// Returns the graded copy of `image` (in COPY_SOURCE state), or null to show it as is. guestFormat:
// the front buffer's resolve format (6 8_8_8_8, 7 2_10_10_10). A front buffer that isn't 8888 on the
// host (2_10_10_10 and the float formats) always goes through it: the display scans those out too,
// and the swap chain takes 8-bit colour.
ID3D12Resource* GradeFrontBuffer(ID3D12Resource* image, uint32_t guestFormat)
{
    static LiveSwitch off("REGPU_NO_GAMMA"), tableOnly("REGPU_GAMMA_TABLE_ONLY");
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const bool convert = desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool tenBit = guestFormat == 7 && !tableOnly;
    if (!InitUpscale() || !UpdateGammaCurve(tenBit, off))
        return nullptr;
    if (g_gammaCurveIdentity && !convert)
        return nullptr;
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 3 > kDescriptorsPerFrame)
        return nullptr;
    if (!g_gammaOut || g_gammaOut->GetDesc().Width != desc.Width || g_gammaOut->GetDesc().Height != desc.Height)
    {
        D3D12_RESOURCE_DESC td = desc;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_gammaOut));
        g_gammaOut = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_SOURCE, "gamma");
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 3;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(image, &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    g_r.device->CreateShaderResourceView(g_gammaCurve.Get(), &srv, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_gammaOut.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 2, g_r.srvStride));
    uint32_t c[24] = {};
    c[20] = uint32_t(desc.Width);
    c[21] = desc.Height;
    c[22] = g_gammaCurveSize;
    C().list->SetComputeRootSignature(g_upscale.gammaRoot.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.gamma.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 2, g_r.srvStride));
    // The output rests in COPY_SOURCE (for the present); to UAV and back around the write, here, so
    // no frame can write it in the copy state whatever the swap did with it.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_gammaOut.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    C().list->ResourceBarrier(1, &b);
    C().list->Dispatch(UINT((desc.Width + 7) / 8), (desc.Height + 7) / 8, 1);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    C().list->ResourceBarrier(1, &b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_gammaOut.Get();
}

// ---- SMAA 1x -------------------------------------------------------------------------------
// Config::smaa or REGPU_SMAA=<n> (0 off, 1 low, 2 medium, 3 high, 4 ultra, the SMAA presets; REGPU_NO_SMAA=1
// turns it off live): SMAA 1x on the whole frame as it is shown, HUD included. It runs on the 8-bit
// display-encoded image, after the display's gamma (GradeFrontBuffer): a front buffer in another format
// (2_10_10_10) only becomes that there, and luma edges are found on what the eye sees. Three compute
// passes: luma edges, blending weights (SMAA's area and search lookup textures), neighbourhood blending
// into a texture of its own, shown instead.
struct Smaa
{
    bool tried = false, ready = false, uploaded = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> edges[4], weights[4], blend[4]; // per preset: low, medium, high, ultra
    ComPtr<ID3D12PipelineState> view; // REGPU_SMAA_VIEW
    // The once-a-second log line: GPU time from timestamps around the passes, read when the frame slot
    // comes round again.
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    bool timed[kFrames] = {};
    uint64_t frames = 0, passes = 0, timedFrames = 0;
    double gpuMs = 0;
    std::chrono::steady_clock::time_point logged = std::chrono::steady_clock::now();
    ComPtr<ID3D12Resource> area, search, areaUpload, searchUpload;
    ComPtr<ID3D12Resource> edgesTex, weightsTex, out; // at the image's size
    uint32_t width = 0, height = 0;
} g_smaa;

bool InitSmaa()
{
    if (g_smaa.tried)
        return g_smaa.ready;
    g_smaa.tried = true;
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 3;
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav;
    // SMAA's samplers: linear and point, clamped.
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < 2; i++)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(g_r.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_smaa.root))))
    {
        Log("smaa: root signature failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    struct Bytes { const unsigned char* data; size_t size; };
    const Bytes shaders[4][3] = {
        { { kSmaaEdgesLow, sizeof(kSmaaEdgesLow) }, { kSmaaWeightsLow, sizeof(kSmaaWeightsLow) }, { kSmaaBlendLow, sizeof(kSmaaBlendLow) } },
        { { kSmaaEdgesMedium, sizeof(kSmaaEdgesMedium) }, { kSmaaWeightsMedium, sizeof(kSmaaWeightsMedium) }, { kSmaaBlendMedium, sizeof(kSmaaBlendMedium) } },
        { { kSmaaEdgesHigh, sizeof(kSmaaEdgesHigh) }, { kSmaaWeightsHigh, sizeof(kSmaaWeightsHigh) }, { kSmaaBlendHigh, sizeof(kSmaaBlendHigh) } },
        { { kSmaaEdgesUltra, sizeof(kSmaaEdgesUltra) }, { kSmaaWeightsUltra, sizeof(kSmaaWeightsUltra) }, { kSmaaBlendUltra, sizeof(kSmaaBlendUltra) } },
    };
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_smaa.root.Get();
    for (int p = 0; p < 4; p++)
    {
        ComPtr<ID3D12PipelineState>* out[3] = { std::addressof(g_smaa.edges[p]), std::addressof(g_smaa.weights[p]), std::addressof(g_smaa.blend[p]) };
        for (int s = 0; s < 3; s++)
        {
            pd.CS = { shaders[p][s].data, shaders[p][s].size };
            if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(out[s]->ReleaseAndGetAddressOf()))))
            {
                Log("smaa: pipeline %d/%d failed", p, s);
                return false;
            }
        }
    }
    pd.CS = { kSmaaView, sizeof(kSmaaView) };
    if (FAILED(g_r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_smaa.view))))
    {
        Log("smaa: view pipeline failed");
        return false;
    }
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kFrames;
    if (FAILED(g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_smaa.queries))) ||
        !(g_smaa.readback = CreateBuffer(8 * 2 * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)))
        g_smaa.queries.Reset(); // (only the log line's GPU time goes)
    // The lookup textures (AreaTex.h: RG8 160x560, SearchTex.h: R8 64x16), uploaded on first use.
    auto lookup = [](uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t pitch, const unsigned char* bytes, ComPtr<ID3D12Resource>& tex,
                      ComPtr<ID3D12Resource>& upload) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = format;
        tex = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST, "smaa lookup");
        const UINT aligned = (pitch + 255) & ~255u;
        upload = CreateBuffer(UINT64(aligned) * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* cpu = nullptr;
        if (!tex || !upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&cpu))))
            return false;
        for (uint32_t y = 0; y < h; y++)
            memcpy(cpu + size_t(y) * aligned, bytes + size_t(y) * pitch, pitch);
        upload->Unmap(0, nullptr);
        return true;
    };
    if (!lookup(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, AREATEX_PITCH, areaTexBytes, g_smaa.area, g_smaa.areaUpload) ||
        !lookup(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, SEARCHTEX_PITCH, searchTexBytes, g_smaa.search, g_smaa.searchUpload))
    {
        Log("smaa: lookup textures failed");
        return false;
    }
    g_smaa.ready = true;
    Log("smaa: ready");
    return true;
}

// Once a second, with SMAA on: its passes per shown frame (3 when every frame went through it) and their GPU time.
void SmaaLog(uint32_t preset)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_smaa.logged < std::chrono::seconds(1))
        return;
    g_smaa.logged = now;
    Log("smaa: preset %u, %.1f passes/frame over %llu frames, %.2f ms GPU", preset, g_smaa.frames ? double(g_smaa.passes) / g_smaa.frames : 0.0,
        (unsigned long long)g_smaa.frames, g_smaa.timedFrames ? g_smaa.gpuMs / g_smaa.timedFrames : 0.0);
    g_smaa.frames = g_smaa.passes = g_smaa.timedFrames = 0;
    g_smaa.gpuMs = 0;
}

uint32_t SmaaPreset()
{
    static const uint32_t preset = std::min<uint32_t>(getenv("REGPU_SMAA") ? uint32_t(atoi(getenv("REGPU_SMAA"))) : GetSettings().smaa, 4);
    return preset;
}

// Returns SMAA's output for `image` (8-bit, display-encoded), or null to show the image as it is (SMAA
// off, or something failed). `image` is in COPY_SOURCE and so is the result.
ID3D12Resource* SmaaFrontBuffer(ID3D12Resource* image)
{
    static LiveSwitch noSmaa("REGPU_NO_SMAA"); // A/B
    const uint32_t preset = SmaaPreset();
    if (!preset)
        return nullptr;
    g_smaa.frames++;
    SmaaLog(preset);
    if (noSmaa || !image || image->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM || !InitSmaa())
        return nullptr;
    FrameResources& f = Frame();
    if (const UINT slot = g_r.frameIndex; std::exchange(g_smaa.timed[slot], false))
    {
        uint64_t* t = nullptr;
        const D3D12_RANGE range{ slot * 16, slot * 16 + 16 }, none{ 0, 0 };
        if (SUCCEEDED(g_smaa.readback->Map(0, &range, reinterpret_cast<void**>(&t))))
        {
            uint64_t frequency = 1;
            g_r.queue->GetTimestampFrequency(&frequency);
            g_smaa.gpuMs += double(t[slot * 2 + 1] - t[slot * 2]) * 1000.0 / double(frequency);
            g_smaa.timedFrames++;
            g_smaa.readback->Unmap(0, &none);
        }
    }
    if (f.descriptorsUsed + 12 > kDescriptorsPerFrame) // three passes of 3 SRVs and a UAV
        return nullptr;
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const uint32_t w = uint32_t(desc.Width), h = desc.Height;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    if (!g_smaa.uploaded)
    {
        auto copy = [&](ID3D12Resource* tex, ID3D12Resource* upload, DXGI_FORMAT format, UINT width, UINT height, UINT pitch) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { format, width, height, 1, (pitch + 255) & ~255u };
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, read);
        };
        copy(g_smaa.area.Get(), g_smaa.areaUpload.Get(), DXGI_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH);
        copy(g_smaa.search.Get(), g_smaa.searchUpload.Get(), DXGI_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH);
        // The upload buffers stay alive until the frame that used them has finished.
        f.garbage.push_back(std::move(g_smaa.areaUpload));
        f.garbage.push_back(std::move(g_smaa.searchUpload));
        g_smaa.uploaded = true;
    }
    if (!g_smaa.out || g_smaa.width != w || g_smaa.height != h)
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_smaa.edgesTex));
        RetireTexture(std::move(g_smaa.weightsTex));
        RetireTexture(std::move(g_smaa.out));
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        g_smaa.edgesTex = CreateTexture(td, read, "smaa");
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_smaa.weightsTex = CreateTexture(td, read, "smaa");
        g_smaa.out = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_SOURCE, "smaa");
        g_smaa.width = w;
        g_smaa.height = h;
        if (!g_smaa.edgesTex || !g_smaa.weightsTex || !g_smaa.out)
            return nullptr;
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 12;
    auto srvOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(r, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    auto uavOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_r.device->CreateUnorderedAccessView(r, nullptr, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    // Edges: image -> edges. Weights: edges, area, search -> weights. Blend: image, weights -> out.
    // Unused SRV slots repeat a valid view (every descriptor of a bound table must be valid).
    srvOf(image, DXGI_FORMAT_R8G8B8A8_UNORM, slots + 0);
    srvOf(image, DXGI_FORMAT_R8G8B8A8_UNORM, slots + 1);
    srvOf(image, DXGI_FORMAT_R8G8B8A8_UNORM, slots + 2);
    uavOf(g_smaa.edgesTex.Get(), DXGI_FORMAT_R8G8_UNORM, slots + 3);
    srvOf(g_smaa.edgesTex.Get(), DXGI_FORMAT_R8G8_UNORM, slots + 4);
    srvOf(g_smaa.area.Get(), DXGI_FORMAT_R8G8_UNORM, slots + 5);
    srvOf(g_smaa.search.Get(), DXGI_FORMAT_R8_UNORM, slots + 6);
    uavOf(g_smaa.weightsTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots + 7);
    srvOf(image, DXGI_FORMAT_R8G8B8A8_UNORM, slots + 8);
    srvOf(g_smaa.weightsTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots + 9);
    srvOf(image, DXGI_FORMAT_R8G8B8A8_UNORM, slots + 10);
    uavOf(g_smaa.out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slots + 11);

    const float c[4] = { 1.0f / float(w), 1.0f / float(h), float(w), float(h) };
    const int p = int(preset) - 1;
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    // REGPU_SMAA_VIEW=1 (live): every pixel SMAA blends shown in red, to see what it found.
    static LiveSwitch view("REGPU_SMAA_VIEW");
    barrier(image, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    if (g_smaa.queries)
        C().list->EndQuery(g_smaa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_r.frameIndex * 2);
    C().list->SetComputeRootSignature(g_smaa.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 4, c, 0);
    const struct { ID3D12PipelineState* pso; ID3D12Resource* target; UINT base; D3D12_RESOURCE_STATES rest; } passes[] = {
        { g_smaa.edges[p].Get(), g_smaa.edgesTex.Get(), slots + 0, read },
        { g_smaa.weights[p].Get(), g_smaa.weightsTex.Get(), slots + 4, read },
        { view ? g_smaa.view.Get() : g_smaa.blend[p].Get(), g_smaa.out.Get(), slots + 8, D3D12_RESOURCE_STATE_COPY_SOURCE },
    };
    for (const auto& pass : passes)
    {
        barrier(pass.target, pass.rest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        C().list->SetPipelineState(pass.pso);
        C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), pass.base, g_r.srvStride));
        C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), pass.base + 3, g_r.srvStride));
        C().list->Dispatch(gx, gy, 1);
        barrier(pass.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pass.rest);
        g_smaa.passes++;
    }
    if (g_smaa.queries)
    {
        C().list->EndQuery(g_smaa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_r.frameIndex * 2 + 1);
        C().list->ResolveQueryData(g_smaa.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_r.frameIndex * 2, 2, g_smaa.readback.Get(), g_r.frameIndex * 16ull);
        g_smaa.timed[g_r.frameIndex] = true;
    }
    barrier(image, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_smaa.out.Get();
}

// Enhanced lighting (Settings::enhance): the frame through the local-contrast pass. `image` is
// in COPY_SOURCE and so is the result; returns null when the option is off.
ID3D12Resource* EnhanceFrontBuffer(ID3D12Resource* image)
{
    if (!GetSettings().enhance || !image || !InitUpscale())
        return nullptr;
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 2 > kDescriptorsPerFrame)
        return nullptr;
    if (!g_enhanceOut || g_enhanceOut->GetDesc().Width != desc.Width || g_enhanceOut->GetDesc().Height != desc.Height)
    {
        D3D12_RESOURCE_DESC td = desc;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        RetireTexture(std::move(g_enhanceOut));
        g_enhanceOut = CreateTexture(td, D3D12_RESOURCE_STATE_COPY_SOURCE, "enhance");
    }
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 2;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(image, &srv, Cpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_r.device->CreateUnorderedAccessView(g_enhanceOut.Get(), nullptr, &uav, Cpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));

    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b)
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = image;
    b[0].Transition.Subresource = 0;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = g_enhanceOut.Get();
    b[1].Transition.Subresource = 0;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    C().list->ResourceBarrier(2, b);

    // The strength (0.35) rides in the rcasCon slot of the constants (index 16), as float bits.
    const float strength = 0.35f;
    uint32_t c[24] = {};
    c[16] = std::bit_cast<uint32_t>(strength);
    c[20] = uint32_t(desc.Width);
    c[21] = desc.Height;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.enhance.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch(UINT((desc.Width + 7) / 8), (desc.Height + 7) / 8, 1);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(2, b);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
    return g_enhanceOut.Get();
}

// Upscales the resolved image at `base` (rendered at the scene scale) to the output size in
// place: the resolve keeps the small image among its spares and shows the upscaled one.
void UpscaleResolved(uint32_t base)
{
    auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || !InitUpscale())
        return;
    ResolvedTexture& dest = it->second;
    if (!HostColorTexture(dest.format))
        return;
    const ComPtr<ID3D12Resource> source = dest.resource;
    const uint32_t inWidth = dest.hostWidth, inHeight = dest.hostHeight;
    const float savedScale = g_sceneScale;
    g_sceneScale = 1.0f;
    const uint32_t outWidth = uint32_t(HostX(LONG(dest.width))), outHeight = uint32_t(HostY(LONG(dest.height)));
    g_sceneScale = savedScale;
    if (outWidth <= inWidth || outHeight <= inHeight)
        return;
    const uint64_t frame = dest.frame;
    const DXGI_FORMAT format = dest.format;
    const bool swapped = dest.swapped;
    EnsureResolved(dest, dest.width, dest.height, outWidth, outHeight, format);
    dest.frame = frame;
    dest.swapped = swapped;
    if (!g_upscale.temp || g_upscale.tempWidth != outWidth || g_upscale.tempHeight != outHeight)
    {
        if (g_upscale.temp)
            Frame().garbage.push_back(std::move(g_upscale.temp));
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = outWidth;
        td.Height = outHeight;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                  IID_PPV_ARGS(&g_upscale.temp)),
            "CreateCommittedResource(upscale)");
        g_upscale.tempWidth = outWidth;
        g_upscale.tempHeight = outHeight;
    }
    FrameResources& f = Frame();
    if (f.descriptorsUsed + 4 > kDescriptorsPerFrame)
        return;
    const UINT slots = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += 4;
    auto srvOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(r, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    auto uavOf = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = fmt;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_r.device->CreateUnorderedAccessView(r, nullptr, &v, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    };
    srvOf(source.Get(), format, slots);
    uavOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 1);
    srvOf(g_upscale.temp.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slots + 2);
    uavOf(dest.resource.Get(), format, slots + 3);

    // FSR 1 constants (FsrEasuCon / FsrRcasCon): the whole source texture is the input.
    auto bits = [](float v) { return std::bit_cast<uint32_t>(v); };
    uint32_t c[24] = {};
    const float iw = float(inWidth), ih = float(inHeight), ow = float(outWidth), oh = float(outHeight);
    c[0] = bits(iw / ow); c[1] = bits(ih / oh); c[2] = bits(0.5f * iw / ow - 0.5f); c[3] = bits(0.5f * ih / oh - 0.5f);
    c[4] = bits(1.0f / iw); c[5] = bits(1.0f / ih); c[6] = bits(1.0f / iw); c[7] = bits(-1.0f / ih);
    c[8] = bits(-1.0f / iw); c[9] = bits(2.0f / ih); c[10] = bits(1.0f / iw); c[11] = bits(2.0f / ih);
    c[12] = bits(0.0f); c[13] = bits(4.0f / ih);
    // RCAS: sharpness in stops (0 sharpest); the setting's 1 is sharpest.
    const float sharp = std::exp2(-2.0f * (1.0f - GetSettings().sharpness));
    c[16] = bits(sharp);
    const uint16_t h = DirectX::PackedVector::XMConvertFloatToHalf(sharp);
    c[17] = uint32_t(h) | uint32_t(h) << 16;
    c[20] = outWidth;
    c[21] = outHeight;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        C().list->ResourceBarrier(1, &b);
    };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const UINT gx = (outWidth + 15) / 16, gy = (outHeight + 15) / 16;
    C().list->SetComputeRootSignature(g_upscale.root.Get());
    C().list->SetComputeRoot32BitConstants(0, 24, c, 0);
    C().list->SetPipelineState(g_upscale.easu.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 1, g_r.srvStride));
    C().list->Dispatch(gx, gy, 1);
    barrier(g_upscale.temp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(dest.resource.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    C().list->SetPipelineState(g_upscale.rcas.Get());
    C().list->SetComputeRootDescriptorTable(1, Gpu(g_r.srvHeap.Get(), slots + 2, g_r.srvStride));
    C().list->SetComputeRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), slots + 3, g_r.srvStride));
    C().list->Dispatch(gx, gy, 1);
    barrier(dest.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(g_upscale.temp.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_drawState.Invalidate(); // the compute pipeline replaced the draw's
}

// The scene is complete (its full-screen image resolved): upscale it, and render the rest of
// the frame (post-processing, HUD) at full size.
void UpscaleScene(uint32_t base)
{
    if (g_temporal)
        TemporalEvaluate(base);
    else
        UpscaleResolved(base);
    g_sceneScale = 1.0f;
    g_scenePhase = false;
    g_upscale.switchedThisFrame = true;
}

// The vertex constants a draw wrote to an earlier frame's ring (CPU side), from their GPU address.
const float* PreviousRingFloats(D3D12_GPU_VIRTUAL_ADDRESS va)
{
    for (auto& fr : g_r.frames)
    {
        if (!fr.ring || !fr.ringCpu)
            continue;
        const D3D12_GPU_VIRTUAL_ADDRESS base = fr.ring->GetGPUVirtualAddress();
        if (va >= base && va < base + fr.ringSize)
            return reinterpret_cast<const float*>(fr.ringCpu + (va - base));
    }
    return nullptr;
}

void MotionStatsLog()
{
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5))
        return;
    last = now;
    const double frames = std::max(1u, g_motionStatFrames), sampled = std::max(1u, g_motionSampledFrames);
    std::vector<std::pair<uint64_t, MotionDrawStat>> v(g_motionDrawStats.begin(), g_motionDrawStats.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
    double total = 0, withMotion = 0;
    std::map<std::string, double> missing; // why -> draws per frame
    for (const auto& e : v)
    {
        total += e.second.draws;
        withMotion += e.second.motion;
        if (e.second.draws > e.second.motion)
        {
            char key[96];
            snprintf(key, sizeof(key), "%s (target pitch %u)", e.second.why, e.second.pitch);
            missing[key] += double(e.second.draws - e.second.motion);
        }
    }
    Log("motion: %.0f frames, %zu vertex shaders in the scene phase, %.0f draws/frame, %.0f with motion", frames, v.size(), total / frames, withMotion / frames);
    for (const auto& [why, n] : missing)
        Log("motion:   no motion: %.1f draws/frame: %s", n / frames, why.c_str());
    for (size_t i = 0; i < v.size() && i < 14; i++)
    {
        const MotionDrawStat& s = v[i].second;
        Log("motion:   vs %016llx ps %016llx: %.1f draws/frame, %.1f with motion, %.1f known, %.1f/frame with changed constants (%.1f registers each, last first at c%u; "
            "sampled on %.0f frames); constants used %u, vertex fetches %u, target pitch %u base %u%s%s",
            (unsigned long long)v[i].first, (unsigned long long)s.psHash, s.draws / frames, s.motion / frames, s.known / frames, s.changed / sampled,
            s.changed ? double(s.changedRegs) / double(s.changed) : 0.0, s.firstChanged, sampled, s.vsConstants, s.vfetch, s.pitch, s.colorBase,
            s.draws > s.motion ? ", no motion: " : "", s.draws > s.motion ? s.why : "");
    }
    g_motionDrawStats.clear();
    g_motionStatFrames = 0;
    g_motionSampledFrames = 0;
}

// The scene's full-screen resolve. The first one of a frame is not always the main view: FM4's garage resolves
// a nearly empty pass (a draw or two, in three tiles) before the 3D scene, which then drew at full size with no
// jitter, depth or motion for the upscaler. A temporal scene of few draws is upscaled on its own (as a menu
// image is) and the scene phase carries on for the real one, when last frame drew a lot at full width after its
// scene; if no scene resolves after such a re-arm, re-arming is off for a growing number of frames.
// Off by default: FM4's garage and races already put their 3D scene in the scene phase (the first resolve of
// a frame with few draws is its menus). REGPU_SCENE_REARM=1 turns it on (live), REGPU_SCENE_MIN_DRAWS=<n> sets
// what counts as few (default 32).
void SceneResolved(uint32_t base)
{
    static LiveSwitch rearmOn("REGPU_SCENE_REARM");
    static const uint32_t minDraws = getenv("REGPU_SCENE_MIN_DRAWS") ? uint32_t(atoi(getenv("REGPU_SCENE_MIN_DRAWS"))) : 32;
    static uint32_t logged = 0;
    const bool few = g_temporal && g_upscale.sceneDraws < minDraws;
    const bool expected = g_upscale.prevPostDraws >= 4 * minDraws;
    const bool allowed = rearmOn && g_r.frame >= g_upscale.rearmBlockedUntil && g_upscale.rearms < 2;
    if (few && expected && allowed)
    {
        if (logged++ < 40 || logged % 600 == 0)
            Log("temporal: resolve %08X after %u draws is not the main view (%u full-width draws followed last frame): upscaled on its own, the scene goes on",
                base, g_upscale.sceneDraws, g_upscale.prevPostDraws);
        UpscaleResolved(base);
        g_upscale.rearms++;
        g_upscale.sceneDraws = 0;
        g_tstat.rearmed++;
        return;
    }
    if (g_temporal && (logged++ < 40 || logged % 600 == 0))
        Log("temporal: scene done at resolve %08X after %u draws (%s)", base, g_upscale.sceneDraws,
            !few ? "main view" : !rearmOn ? "few draws, re-arm off (REGPU_SCENE_REARM=1)" : !expected ? "no full-width draws after the scene last frame" : !allowed ? "re-arm blocked" : "?");
    if (g_upscale.rearms)
        g_upscale.rearmFailures = 0; // the real scene resolved after a re-arm
    UpscaleScene(base);
}

// Copies the render target resolved to `base` into guest memory, tiled, for the title to read.
void WriteResolveToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info);
void QueueDeferredWriteback(uint32_t base, uint32_t width, uint32_t height, uint32_t info, bool depth, const D3D12_RECT& rect,
    const regpu::Writeback& request);

void SplitMainList();
void ResolveImpl();
void Resolve()
{
    ResolveImpl();
    static LiveSwitch syncResolves("REGPU_SYNC_RESOLVES"), splitResolves("REGPU_SPLIT_RESOLVES");
    if (C().worker || !g_r.ok)
        return;
    if (syncResolves)
        RendererFenceSyncForced();
    else if (splitResolves)
    {
        BeginFrameIfNeeded();
        SplitMainList();
    }
}
void ResolveImpl()
{
    g_resolveEpoch++; // texture bindings may now map to a new resolve
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    SelectSurfaces(false);
    PassMark("resolve to %08X (colour %03X)", gpureg::Get(RB_COPY_DEST_BASE), C().colorBase);
    const uint32_t control = gpureg::Get(RB_COPY_CONTROL);
    const uint32_t command = (control >> 20) & 3;

    // The rectangle: three float2 vertices in vertex fetch constant 0.
    const uint32_t d0 = gpureg::Get(gpureg::FetchConstants), d1 = gpureg::Get(gpureg::FetchConstants + 1);
    const uint32_t vertices = GpuPhysToGuest(d0 & 0xFFFFFFFC);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 3; i++)
    {
        uint32_t xv = *GuestPtr<uint32_t>(vertices + i * 8), yv = *GuestPtr<uint32_t>(vertices + i * 8 + 4);
        if ((d1 & 3) == 2)
        {
            xv = __builtin_bswap32(xv);
            yv = __builtin_bswap32(yv);
        }
        float x, y;
        memcpy(&x, &xv, 4);
        memcpy(&y, &yv, 4);
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    // Clamped to the tile, as the hardware does: in a tile pass the
    // resolves for the other tiles become empty.
    const D3D12_RECT tile = WindowScissor();
    D3D12_RECT rect{ std::clamp<LONG>(LONG(std::lround(x0)), tile.left, tile.right), std::clamp<LONG>(LONG(std::lround(y0)), tile.top, tile.bottom),
        std::clamp<LONG>(LONG(std::lround(x1)), tile.left, tile.right), std::clamp<LONG>(LONG(std::lround(y1)), tile.top, tile.bottom) };
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    const uint32_t sourceSelect = control & 7;
    const uint64_t traceFrame = TraceFrame();
    // REGPU_TRACE_RESOLVES=1: every resolve (not just those of the traced frame), with its time.
    static LiveSwitch traceAllResolves("REGPU_TRACE_RESOLVES");
    // REGPU_TRACE_RESOLVES_MIN=<width>: only resolves at least that wide (less slowdown).
    static const long traceMinWidth = getenv("REGPU_TRACE_RESOLVES_MIN") ? atol(getenv("REGPU_TRACE_RESOLVES_MIN")) : 0;
    if ((traceAllResolves && rect.right - rect.left >= traceMinWidth) || (traceFrame && g_r.frame + 1 == traceFrame))
        Log("resolve: control %08X dest %08X pitch %08X info %08X rect %ld,%ld-%ld,%ld clear %08X depth clear %08X (color info %08X depth info %08X surface %08X)", control,
            gpureg::Get(RB_COPY_DEST_BASE), gpureg::Get(RB_COPY_DEST_PITCH), gpureg::Get(RB_COPY_DEST_INFO), rect.left, rect.top, rect.right, rect.bottom,
            gpureg::Get(RB_COLOR_CLEAR), gpureg::Get(RB_DEPTH_CLEAR), gpureg::Get(RB_COLOR_INFO), gpureg::Get(RB_DEPTH_INFO), gpureg::Get(RB_SURFACE_INFO));
    // REGPU_CAPTURE_ON_RESOLVE=<dest hex>[,<skip>]: trace the frame after the skip-th resolve to that address
    // (passes that run only now and then, like an editor's preview).
    if (static const char* onResolve = getenv("REGPU_CAPTURE_ON_RESOLVE"); onResolve)
    {
        static const uint32_t dest = uint32_t(strtoul(onResolve, nullptr, 16));
        static int skip = strchr(onResolve, ',') ? atoi(strchr(onResolve, ',') + 1) : 0;
        if ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) == dest && skip-- == 0)
        {
            g_hotkeyFrame = g_r.frame + 2;
            Log("capture: resolve to %08X seen, tracing frame %llu", dest, (unsigned long long)(g_r.frame + 2));
        }
    }
    // The depth base's host surface may be laid out in another row width than this view (see
    // EdramAlias): depth-only draws through such views were placed tile run by tile run (DrawImpl),
    // so a depth copy reads the tiles from there and a depth clear clears them there. (A plain
    // rectangle took the host layout's tiles for the view's: a caster depth copy came out with a
    // stray hard-edged wedge, which the floor shadow then showed.) REGPU_NO_ALIAS_RESOLVE=1 copies
    // and clears the plain rectangle, as before (A/B).
    EdramAlias resolveAlias;
    {
        static LiveSwitch noAliasResolve("REGPU_NO_ALIAS_RESOLVE");
        static LiveSwitch noForeignDrop("REGPU_NO_FOREIGN_DEPTH_DROP");
        const uint32_t surface = gpureg::Get(RB_SURFACE_INFO);
        const uint32_t host = g_depthOwner[gpureg::Get(RB_DEPTH_INFO) & 0xFFF].load(std::memory_order_relaxed);
        if (!noAliasResolve && !noForeignDrop && host && RowSamples(host) != RowSamples(surface) && !ForeignDepthUnplaceable(surface, host))
            resolveAlias = { true, surface, host };
    }
    if (command != 3 && sourceSelect <= 4)
    {
        // Each tile resolves to the destination row it starts at; key the
        // host texture by the image's own base so the tiles land together.
        const uint32_t pitchReg = gpureg::Get(RB_COPY_DEST_PITCH);
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t info = gpureg::Get(RB_COPY_DEST_INFO);
        const bool depth = sourceSelect == 4;
        const uint32_t destFormat = (info >> 7) & 0x3F;
        // Guest bytes per pixel (Xenos formats: k_8 2; 16-bit k_1_5_5_5/k_5_6_5/k_6_5_5 3-5, k_8_8 10,
        // k_4_4_4_4 15, k_16 24, k_16_FLOAT 30; 64-bit k_16_16_16_16 26, k_16_16_16_16_FLOAT 32,
        // k_32_32 34, k_32_32_FLOAT 37; 128-bit k_32_32_32_32 35, k_32_32_32_32_FLOAT 38; else 32-bit).
        // A tile's rows are pitch * guestBpp bytes each, so a lower tile of a non-32-bit image finds
        // the image's base at its own bytes per pixel (at 4 bytes a pixel, a 64-bit image's lower tile
        // landed in a separate image and its part of the image stayed black). REGPU_NO_TILE_BPP=1
        // steps back 4 bytes a pixel and keeps the earlier table (16_FLOAT, 16_16_16_16_FLOAT,
        // 32_32 and 32_32_32_32 as 4 bytes), as before (A/B).
        static LiveSwitch noTileBpp("REGPU_NO_TILE_BPP");
        const bool tileBpp = !noTileBpp;
        const uint32_t guestBpp = depth ? 4
            : destFormat == 2 ? 1
            : (destFormat >= 3 && destFormat <= 5) || destFormat == 10 || destFormat == 15 || destFormat == 24 || (tileBpp && destFormat == 30) ? 2
            : destFormat == 26 || destFormat == 37 || (tileBpp && (destFormat == 32 || destFormat == 34)) ? 8
            : destFormat == 38 || (tileBpp && destFormat == 35) ? 16 : 4;
        // The copy writes EDRAM row e to destination row e from RB_COPY_DEST_BASE. The rectangle's rows
        // are in screen space, moved into EDRAM by the window offset (PA_SC_WINDOW_OFFSET y, when
        // PA_SU_SC_MODE_CNTL enables it), as for draws: a predicated tile's rows start at EDRAM row 0
        // and its destination base is advanced to them, while a pass that is not tiled copies its
        // lower rows at their own row from the image's own base (an image resolved in two halves to
        // one destination). We keep a resolved image whole, keyed by its base: step back from the
        // destination base by the rows the window offset moved. REGPU_RESOLVE_ROW_FROM_RECT=1 steps
        // back by the rectangle's top row instead, whatever the offset (as before; A/B).
        static LiveSwitch rowFromRect("REGPU_RESOLVE_ROW_FROM_RECT");
        const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
        const bool vertexOffset = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1;
        const int32_t wy = vertexOffset ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
        const uint32_t rowsBack = rowFromRect ? uint32_t(rect.top) : uint32_t(std::clamp<int32_t>(-wy, 0, int32_t(rect.top)));
        const uint32_t base = ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) - rowsBack * pitch * (tileBpp ? guestBpp : 4)) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, std::max<uint32_t>(kWidth, BigPassSize()));
        // Taller than the screen is possible: a 1024x1024 target drawn as tall passes is resolved
        // 720 rows at a time and sampled as 1024 rows. The texture keeps all its rows (clamped to
        // kHeight it was stretched 1.4x vertically when sampled).
        constexpr uint32_t kMaxResolveHeight = 2048;
        uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + rowsBack, 1, kMaxResolveHeight);
        // A later tile into an image that already holds its rows keeps the image: some copies give
        // the full image's rows for every tile (720, then 976 and 1232 with the tile's offset), and
        // each "taller" copy recreated the texture, dropping the tiles before it (the lower two
        // thirds of the image came out black). It grows only when the rows don't fit.
        static LiveSwitch noKeep("REGPU_NO_HEIGHT_KEEP");
        if (auto it = g_r.resolved.find(base); !noKeep && it != g_r.resolved.end() && it->second.resource && rowsBack > 0 &&
                                                uint32_t(rect.bottom) <= it->second.height && it->second.height < height)
            height = it->second.height;
        // Destination: 8888 stays 8-bit (clamped, as the hardware does); other
        // formats keep the HDR range. The exponent bias scales colour copies.
        const DXGI_FORMAT format = depth ? DXGI_FORMAT_R32_FLOAT
            : destFormat == 6 ? DXGI_FORMAT_R8G8B8A8_UNORM : g_colorFormat;
        const int bias = int32_t(info << 10) >> 26; // bits 16-21, signed
        // Bit 24 (copy swap): the destination stores red and blue exchanged; the
        // fetch swizzle swaps them back when the title samples it.
        const bool swapRB = !depth && ((info >> 24) & 1);
        g_r.resolved[base].guestFormat = depth ? 0 : destFormat;
        g_r.resolved[base].guestBpp = guestBpp;
        // Fixed-point destinations hold [0, 1]. Not the float ones (16_FLOAT, 16_16_FLOAT,
        // 16_16_16_16_FLOAT, 32_FLOAT, 32_32_FLOAT, 32_32_32_32_FLOAT), nor the 16-bit ones
        // (16, 16_16, 16_16_16_16), copied from 16_16 targets whose -32..32 range this
        // surface model doesn't keep yet.
        static LiveSwitch noUnormResolve("REGPU_NO_UNORM_CLAMP");
        const bool keepRange = destFormat == 30 || destFormat == 31 || destFormat == 32 || destFormat == 36 || destFormat == 37 ||
            destFormat == 38 || destFormat == 24 || destFormat == 25 || destFormat == 26;
        // A 7e3 source (colour formats 3 and 12, 2_10_10_10_FLOAT) stores colour in [0, 31.875] and alpha in 2 bits:
        // what blending added past that isn't in the console's EDRAM (it saturates after every blend). Draws clamp
        // their own output (hlsl.cpp), but the host surface keeps the blended sum, and a host blend can't saturate in
        // 7e3 without programmable blending, so the copy holds it to the stored range as it leaves EDRAM. Alpha is
        // saturated, not quantised to its 2 bits (a known gap). The format is that of the colour target the copy
        // selects (RB_COPY_CONTROL source select: RB_COLOR_INFO, RB_COLOR1_INFO .. RB_COLOR3_INFO).
        // REGPU_NO_7E3_RESOLVE_CLAMP leaves the copy unclamped (live, for A/B).
        static LiveSwitch no7e3Clamp("REGPU_NO_7E3_RESOLVE_CLAMP");
        static constexpr uint32_t kColorInfo[4] = { RB_COLOR_INFO, 0x2003, 0x2004, 0x2005 };
        const uint32_t sourceFormat = depth ? 0 : (gpureg::Get(kColorInfo[sourceSelect & 3]) >> 16) & 0xF;
        const float ceiling = !depth && !no7e3Clamp && (sourceFormat == 3 || sourceFormat == 12) ? 31.875f : 0.0f;
        // 16-bit fixed destinations (k_16, k_16_16, k_16_16_16_16: formats 24, 25, 26) hold integers: the console's copy
        // converts each value (after the exponent bias) to a 16-bit integer and saturates, signed or unsigned by the copy's
        // number format (RB_COPY_DEST_INFO bits 13-15: 1 snorm, 3 sint signed; 0 unorm, 2 uint unsigned). Our texture keeps
        // the value as a float in the same units (the fetch's exponent adjust scales it back), so it is held to that
        // integer range here; unclamped, values past it (a 16_16 shadow map written through a colour exponent bias and
        // copied with bias +10) reached the fetch where the console had saturated them. REGPU_NO_FIXED16_RESOLVE_CLAMP
        // leaves them unclamped (live, for A/B).
        static LiveSwitch noFixed16Clamp("REGPU_NO_FIXED16_RESOLVE_CLAMP");
        const uint32_t numberFormat = (info >> 13) & 7;
        const bool fixed16 = !depth && (destFormat == 24 || destFormat == 25 || destFormat == 26) && !noFixed16Clamp;
        static constexpr float kUnorm[2] = { 0.0f, 1.0f }, kSigned16[2] = { -32768.0f, 32767.0f }, kUnsigned16[2] = { 0.0f, 65535.0f };
        const float* destRange = !depth && !keepRange && !noUnormResolve ? kUnorm
            : fixed16 ? (numberFormat == 1 || numberFormat == 3 ? kSigned16 : kUnsigned16) : nullptr;
        BlitResolve(depth, base, width, height, format, rect, depth ? 1.0f : std::ldexp(1.0f, bias), false, destRange, ceiling,
            depth && resolveAlias.on ? &resolveAlias : nullptr);
        // Temporal upscaling: each tile of the main view keeps its depth before it is cleared.
        if (g_temporal && g_scenePhase && !depth)
        {
            g_tstat.resolves++;
            if (width >= kWidth && rect.left == 0 && rect.right >= LONG(kWidth))
                g_tstat.resolveFullWidth++;
        }
        if (g_temporal && g_scenePhase && !depth && width >= kWidth && rect.left == 0 && rect.right >= LONG(kWidth))
            CopySceneDepth(rect);
        // The main view's full image (its last tile reaches the bottom): the scene is done.
        if (g_scenePhase && !depth && width >= kWidth && rect.left == 0 && rect.right >= LONG(kWidth) && rect.bottom >= LONG(kHeight))
        {
            if (g_temporal)
                g_tstat.sceneDone++;
            SceneResolved(base);
        }
        g_r.resolved[base].swapped = swapRB;
        // Images the game reads on the CPU are also written to memory (regpu.h Host::resolveWriteback;
        // without it, REGPU_RESOLVE_WRITEBACK's "WxH,WxH" sizes of 8888 colour, at once).
        regpu::Writeback request;
        if (const auto ask = regpu::GetHost().resolveWriteback)
        {
            const uint32_t msaa = (gpureg::Get(RB_SURFACE_INFO) >> 16) & 3;
            request = ask({ base, width, height, destFormat, depth, msaa == 2 ? 4u : msaa == 1 ? 2u : 1u, int32_t(rect.top), int32_t(rect.bottom) });
        }
        else
        {
            static const std::vector<std::pair<uint32_t, uint32_t>> sizes = [] {
                std::vector<std::pair<uint32_t, uint32_t>> v;
                for (const char* p = getenv("REGPU_RESOLVE_WRITEBACK"); p && *p;)
                {
                    uint32_t w = 0, h = 0;
                    if (sscanf(p, "%ux%u", &w, &h) == 2)
                        v.emplace_back(w, h);
                    p = strchr(p, ',');
                    if (p)
                        p++;
                }
                return v;
            }();
            if (!depth && rect.left == 0 && rect.top == 0 &&
                std::find(sizes.begin(), sizes.end(), std::pair<uint32_t, uint32_t>(width, height)) != sizes.end())
                request.mode = regpu::Writeback::Mode::Now;
        }
        // REGPU_RESOLVE_WRITEBACK_MAX=<pixels>: every smaller resolve the game didn't ask for is also
        // written, deferred (the console writes every resolve to memory; small ones, like an
        // average-luminance target, are what a title reads back on the CPU).
        static const uint64_t writebackMax = getenv("REGPU_RESOLVE_WRITEBACK_MAX") ? strtoull(getenv("REGPU_RESOLVE_WRITEBACK_MAX"), nullptr, 10) : 0;
        if (request.mode == regpu::Writeback::Mode::None && uint64_t(width) * height <= writebackMax)
            request.mode = regpu::Writeback::Mode::Deferred;
        if (request.mode == regpu::Writeback::Mode::Now && !depth)
            WriteResolveToGuest(base, width, height, info);
        else if (request.mode == regpu::Writeback::Mode::Deferred)
            QueueDeferredWriteback(base, width, height, info, depth, rect, request);
    }
    // Clears after a tile's resolve prepare the EDRAM for the next tile, which
    // we draw lower on the same host surface: a full-width clear covers every
    // row from the tile down.
    D3D12_RECT clearRect = rect;
    const uint32_t surfacePitch = gpureg::Get(RB_SURFACE_INFO) & 0x3FFF;
    static LiveSwitch noClearExtend("REGPU_NO_CLEAR_EXTEND");
    if (!noClearExtend && surfacePitch && uint32_t(rect.right - rect.left) >= std::min<uint32_t>(surfacePitch, kWidth) && rect.left == 0)
        clearRect.bottom = PassHeight();
    if ((control >> 8) & 1) // colour clear (ARGB)
    {
        const uint32_t c = gpureg::Get(RB_COLOR_CLEAR);
        const float color[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_RECT hostClear = ToHost(clearRect);
        C().list->ClearRenderTargetView(C().rtv, color, 1, &hostClear);
    }
    if ((control >> 9) & 1) // depth/stencil clear: depth 24 bits | stencil 8
    {
        const uint32_t v = gpureg::Get(RB_DEPTH_CLEAR), d = v >> 8;
        float depth = float(d) / 16777215.0f;
        if ((gpureg::Get(RB_DEPTH_INFO) >> 16) & 1)
        {
            // D24FS8: 20e4 float (4-bit exponent, bias 15, 20-bit mantissa).
            const uint32_t e = d >> 20, m = d & 0xFFFFF;
            depth = e ? std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15) : std::ldexp(float(m) / 1048576.0f, -14);
            depth = std::clamp(depth, 0.0f, 1.0f);
        }
        // Through an alias (as the copy above): clear the tiles where the draws placed them.
        if (resolveAlias.on)
        {
            std::vector<D3D12_RECT> runs;
            const uint32_t hostMsaa = (resolveAlias.hostSurface >> 16) & 3;
            const float ha = float(MsaaAcross(hostMsaa)), hd = float(MsaaDown(hostMsaa)), s = ScaleY();
            ForEachAliasRun(resolveAlias, clearRect, [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t dx, int32_t dy) {
                runs.push_back({ LONG(std::lround(std::floor(float(x0 + dx) / ha * s))), LONG(std::lround(std::floor(float(y0 + dy) / hd * s))),
                    LONG(std::lround(std::ceil(float(x1 + dx) / ha * s))), LONG(std::lround(std::ceil(float(y1 + dy) / hd * s))) });
            });
            if (!runs.empty())
                C().list->ClearDepthStencilView(C().dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, UINT8(v & 0xFF), UINT(runs.size()), runs.data());
        }
        else
        {
            const D3D12_RECT hostClear = ToHost(clearRect);
            C().list->ClearDepthStencilView(C().dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, UINT8(v & 0xFF), 1, &hostClear);
        }
    }
}
}

namespace
{
// ---- Lookahead: pipelines the command stream will need, found by GpuLookahead.
struct LookItem
{
    PipelineKey key;
    std::shared_ptr<std::vector<uint32_t>> vsCode, psCode; // only for shaders not translated yet
    std::shared_ptr<PipelineEntry> entry; // set once the build has been started
    bool dropped = false;
};
std::vector<LookItem> g_look;
std::unordered_set<PipelineKey, PipelineKeyHash> g_lookKeys;
uint64_t g_predVs = 0, g_predPs = 0;
const uint32_t *g_predVsCode = nullptr, *g_predPsCode = nullptr;
uint32_t g_predVsDwords = 0, g_predPsDwords = 0;
size_t g_lookNext = 0;
uint32_t g_lookStarted = 0, g_lookFinished = 0;

// Starts what can be started (translating shaders as needed, queueing pipeline builds);
// returns true while anything is unfinished.
bool AdvanceLookahead(double budgetMs)
{
    const auto start = std::chrono::steady_clock::now();
    bool pending = false;
    for (size_t i = 0; i < g_look.size(); i++)
    {
        LookItem& it = g_look[i];
        if (it.dropped)
            continue;
        if (!it.entry)
        {
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs)
            {
                pending = true;
                continue;
            }
            const CompiledShader* vs = nullptr;
            const CompiledShader* ps = nullptr;
            if (auto f = g_r.shaders.find(it.key.vs); f != g_r.shaders.end())
                g_compiler.Promote(*(vs = f->second.get()));
            else
                vs = it.vsCode ? &GetShader(it.key.vs, *it.vsCode, false) : nullptr;
            if (auto f = g_r.shaders.find(it.key.ps); f != g_r.shaders.end())
                g_compiler.Promote(*(ps = f->second.get()));
            else
                ps = it.psCode ? &GetShader(it.key.ps, *it.psCode, true) : nullptr;
            if (!vs || !ps)
            {
                it.dropped = true;
                g_lookFinished++;
                continue;
            }
            if (vs->state.load() == CompiledShader::Failed || ps->state.load() == CompiledShader::Failed)
            {
                it.dropped = true;
                g_lookFinished++;
                continue;
            }
            if (vs->state.load() == CompiledShader::Pending || ps->state.load() == CompiledShader::Pending)
            {
                pending = true;
                continue;
            }
            it.entry = EnsurePipeline(*vs, *ps, it.key);
            g_lookStarted++;
        }
        if (it.entry && it.entry->state.load(std::memory_order_acquire) == 0)
            pending = true;
    }
    // Finished items leave the list so it stays short.
    size_t keep = 0;
    for (size_t i = 0; i < g_look.size(); i++)
    {
        const bool finished = g_look[i].dropped || (g_look[i].entry && g_look[i].entry->state.load(std::memory_order_acquire) != 0);
        if (finished && !g_look[i].dropped)
            g_lookFinished++;
        if (!finished)
            g_look[keep++] = std::move(g_look[i]);
    }
    g_look.resize(keep);
    return pending || !g_look.empty();
}

// Shaders the memory scan found (by renderer hash), to see how much of a level it covers.
std::unordered_set<uint64_t> g_scanKnown;
uint32_t g_scanPrefound = 0, g_scanMissed = 0;

// Hands shaders the scanner found to the compile queue (translation runs here, on the
// command processor thread, so the budget keeps it from eating a frame).
void DrainScan(double budgetMs, std::vector<const CompiledShader*>* queued = nullptr)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<ScannedShader> found;
    while (true)
    {
        found.clear();
        if (!ShaderScanTake(found, 8))
            return;
        for (ScannedShader& s : found)
        {
            const uint64_t hash = Fnv(s.code.data(), s.code.size() * 4) ^ (s.pixel ? 1 : 0);
            g_scanKnown.insert(hash);
            // Speculative (and partly stale memory): compiled when nothing urgent waits,
            // promoted if a draw turns out to need it.
            const CompiledShader& cs = GetShader(hash, s.code, s.pixel, true);
            if (queued && cs.state.load() == CompiledShader::Pending)
                queued->push_back(&cs);
        }
        if (budgetMs > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs)
            return;
    }
}

// ---- Hold: while the command processor waits for a compile it keeps presenting the
// last frame with the overlay on top, so the screen doesn't just freeze.
struct Hold
{
    bool active = false;
    ComPtr<ID3D12Resource> image; // last frame shown (kept alive here)
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> upload;
    uint8_t* uploadCpu = nullptr;
    std::chrono::steady_clock::time_point lastPresent{};
} g_hold;

void HoldPresent();

// Spins until done() is true. Presents the overlay roughly 8 times a second once the wait
// has lasted 150 ms.


void WaitHoldingImpl(const std::function<bool()>& done)
{
    if (done())
        return;
    const auto waitStart = std::chrono::steady_clock::now();
    struct CountWait
    {
        std::chrono::steady_clock::time_point start;
        ~CountWait()
        {
            g_frameEvents.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            g_frameEvents.waits++;
        }
    } countWait{ waitStart };
    // A stall: look at everything queued behind this draw and build it all at once
    // (in parallel), then wait for the whole set, not just this draw's pipeline.
    static auto lastLook = std::chrono::steady_clock::time_point{};
    const auto start = std::chrono::steady_clock::now();
    bool announced = false;
    // Not once the game runs: the scan alone takes ~18 ms and the wait is usually shorter.
    if (!g_compiler.park.load())
    {
        ShaderScanNow(std::max(2u, std::thread::hardware_concurrency() / 2));
        DrainScan(0);
    }
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLook > std::chrono::milliseconds(400))
        {
            lastLook = now;
            GpuLookahead();
        }
        const bool lookPending = AdvanceLookahead(4.0);
        if (done() && !lookPending && g_compiler.jobs.load() == 0)
            break;
        if (now - start > std::chrono::seconds(180))
            break;
        // Nothing legitimate takes this long: say what is still pending and where every
        // thread is, once per wait.
        static std::chrono::steady_clock::time_point reported{};
        if (now - start > std::chrono::seconds(20) && reported != start)
        {
            reported = start;
            int queued = 0, background = 0, parked = 0;
            {
                std::lock_guard lock(g_compiler.mutex);
                queued = int(g_compiler.queue.size());
                background = int(g_compiler.background.size());
                parked = int(g_compiler.parked.size());
            }
            Log("hang: waiting %.0f s on %s: done %d, lookahead pending %d (%zu items), compile jobs %d (queued %d, background %d, parked %d), "
                "pipeline builds in flight %d",
                std::chrono::duration<double>(now - start).count(), g_waitWhat, int(done()), int(lookPending), g_look.size(),
                g_compiler.jobs.load(), queued, background, parked, g_stats.busy.load());
            if (regpu::GetHost().logThreadStacks)
                regpu::GetHost().logThreadStacks("renderer wait over 20 s");
        }
        if (now - start > std::chrono::milliseconds(150))
        {
            g_hold.active = true;
            announced = true;
            DrainScan(0);
            if (now - g_hold.lastPresent > std::chrono::milliseconds(33))
                HoldPresent();
        }
        Sleep(1);
    }
    if (announced)
        g_hold.active = false;
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
        g_lastLongHold = std::chrono::steady_clock::now();
    if (getenv("REGPU_PERF"))
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > 20)
            Log("perf: stall of %.0f ms on %s (blend %08X, lookahead started %u, finished %u)", ms, g_waitWhat, g_waitBlend, g_lookStarted, g_lookFinished);
    }
}

// Lookahead hooks, called from GpuLookahead on shadow register state.
void PredictBeginImpl()
{
    g_predVs = C().vsHash;
    g_predPs = C().psHash;
    g_predVsCode = C().vsCode.data();
    g_predPsCode = C().psCode.data();
    g_predVsDwords = uint32_t(C().vsCode.size());
    g_predPsDwords = uint32_t(C().psCode.size());
}

// ---- Warm-up: rebuilds every shader/pipeline recorded in the manifest in the
// background (on this thread a few ms per frame) while the game boots and sits in menus.
void WarmupStep()
{
    DrainScan(3.0);
    static LiveSwitch off("REGPU_NO_WARMUP");
    if (off || g_manifest.next >= g_manifest.pending.size())
        return;
    const auto start = std::chrono::steady_clock::now();
    int skipped = 0;
    while (g_manifest.next < g_manifest.pending.size() && g_stats.busy.load() < 12)
    {
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > 4.0)
            break;
        const PipelineKey key = g_manifest.pending[g_manifest.next];
        if (g_r.pipelines.count(key))
        {
            g_manifest.next++;
            g_stats.warmDone++;
            continue;
        }
        std::vector<uint32_t> vsCode, psCode;
        if (!Manifest::LoadShader(false, key.vs, vsCode) || !Manifest::LoadShader(true, key.ps, psCode))
        {
            g_manifest.next++;
            g_stats.warmDone++;
            continue;
        }
        const CompiledShader& vs = GetShader(key.vs, vsCode, false);
        const CompiledShader& ps = GetShader(key.ps, psCode, true);
        if (vs.state.load() == CompiledShader::Pending || ps.state.load() == CompiledShader::Pending)
        {
            // Still compiling: look further ahead instead of waiting on it.
            std::rotate(g_manifest.pending.begin() + g_manifest.next, g_manifest.pending.begin() + g_manifest.next + 1, g_manifest.pending.end());
            if (++skipped > 64)
                break;
            continue;
        }
        g_manifest.next++;
        g_stats.warmDone++;
        if (vs.IsReady() && ps.IsReady())
            EnsurePipeline(vs, ps, key);
    }
}

// ---- Overlay: a small panel in the corner while shaders are compiling.
constexpr UINT kOverlayW = 520, kOverlayH = 140;
struct Overlay
{
    ComPtr<ID3D12Resource> texture;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    std::chrono::steady_clock::time_point lastRender{};
    std::string lastText;
    bool visible = false;
} g_overlay;

bool OverlayWanted(std::vector<std::string>& lines)
{
    static LiveSwitch disabled("REGPU_NO_OVERLAY");
    if (disabled)
        return false;
    const uint64_t nowMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool recentCold = g_stats.lastColdMs.load() && nowMs - g_stats.lastColdMs.load() < 4000;
    const bool warming = g_stats.warmTotal.load() && g_stats.warmDone.load() < g_stats.warmTotal.load();
    if (g_stats.busy.load() == 0 && !recentCold && !warming && !g_hold.active)
        return false;
    char l[160];
    lines.push_back(g_hold.active ? "Preparing shaders - please wait" : "Shader compiler");
    if (g_stats.busy.load() > 0)
    {
        std::lock_guard lock(g_stats.mutex);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stats.currentSince).count();
        snprintf(l, sizeof(l), "Compiling %s  (%.1f s, %d in progress)", g_stats.current.c_str(), sec, g_stats.busy.load());
        lines.push_back(l);
    }
    else
        lines.push_back("Idle");
    snprintf(l, sizeof(l), "Shaders    %u compiled, %u from cache", g_stats.shadersBuilt.load(), g_stats.shadersCached.load());
    lines.push_back(l);
    snprintf(l, sizeof(l), "Pipelines  %u built, %u from cache", g_stats.pipelinesBuilt.load(), g_stats.pipelinesLoaded.load());
    lines.push_back(l);
    if (g_hold.active)
    {
        snprintf(l, sizeof(l), "This scene  %u pipelines found, %u done, %zu waiting", g_lookStarted + uint32_t(g_look.size()), g_lookFinished, g_look.size());
        lines.push_back(l);
    }
    if (g_stats.warmTotal.load())
    {
        snprintf(l, sizeof(l), "Warm-up    %u / %u pipelines", std::min(g_stats.warmDone.load(), g_stats.warmTotal.load()), g_stats.warmTotal.load());
        lines.push_back(l);
    }
    if (g_stats.staticTotal.load())
    {
        snprintf(l, sizeof(l), "First boot %u / %u game shaders compiled", std::min(g_stats.staticDone.load(), g_stats.staticTotal.load()), g_stats.staticTotal.load());
        lines.push_back(l);
    }
    return true;
}

// Renders the lines with GDI into RGBA pixels.
void RenderOverlayPixels(const std::vector<std::string>& lines, std::vector<uint32_t>& pixels)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kOverlayW;
    bi.bmiHeader.biHeight = -int(kOverlayH);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT all{ 0, 0, LONG(kOverlayW), LONG(kOverlayH) };
    HBRUSH bg = CreateSolidBrush(RGB(14, 16, 22)), border = CreateSolidBrush(RGB(255, 140, 0));
    FillRect(dc, &all, border);
    RECT inner{ 2, 2, LONG(kOverlayW) - 2, LONG(kOverlayH) - 2 };
    FillRect(dc, &inner, bg);
    HFONT font = CreateFontW(19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < lines.size(); i++)
    {
        SetTextColor(dc, i == 0 ? RGB(255, 170, 40) : RGB(235, 235, 235));
        TextOutA(dc, 12, 8 + int(i) * 22, lines[i].c_str(), int(lines[i].size()));
    }
    GdiFlush();
    pixels.resize(size_t(kOverlayW) * kOverlayH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < pixels.size(); i++)
    {
        const uint32_t p = src[i]; // 0x00RRGGBB
        pixels[i] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bg);
    DeleteObject(border);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// Records the overlay upload (when its text changed) and returns the texture to
// copy onto the back buffer, or null when nothing is showing.
struct OverlayUpload
{
    ID3D12Resource* resource = nullptr;
    uint8_t* cpu = nullptr;
    UINT64 offset = 0;
};

// upload(bytes) returns staging space (resource + CPU pointer + offset) or a null resource.
ID3D12Resource* PrepareOverlayOn(ID3D12GraphicsCommandList* list, const std::function<OverlayUpload(UINT64)>& upload, bool force)
{
    std::vector<std::string> lines;
    g_overlay.visible = OverlayWanted(lines);
    if (!g_overlay.visible)
        return nullptr;
    std::string text;
    for (auto& l : lines)
        text += l + "\n";
    const auto now = std::chrono::steady_clock::now();
    const bool stale = !g_overlay.texture || (text != g_overlay.lastText && (force || now - g_overlay.lastRender > std::chrono::milliseconds(200)));
    if (stale)
    {
        if (!g_overlay.texture)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = kOverlayW;
            td.Height = kOverlayH;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_overlay.texture))))
                return nullptr;
            g_overlay.state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        std::vector<uint32_t> pixels;
        RenderOverlayPixels(lines, pixels);
        static LiveSwitch dumpOverlay("REGPU_OVERLAY_DUMP");
        if (dumpOverlay)
        {
            std::vector<uint8_t> rgba(pixels.size() * 4);
            memcpy(rgba.data(), pixels.data(), rgba.size());
            WritePng("overlay.png", kOverlayW, kOverlayH, rgba);
        }
        const UINT pitch = kOverlayW * 4;
        const UINT alignedPitch = (pitch + 255) & ~255u;
        const OverlayUpload staging = upload(UINT64(alignedPitch) * kOverlayH);
        if (!staging.resource)
            return nullptr;
        for (UINT y = 0; y < kOverlayH; y++)
            memcpy(staging.cpu + staging.offset + UINT64(y) * alignedPitch, pixels.data() + size_t(y) * kOverlayW, pitch);
        if (g_overlay.state != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_overlay.texture.Get();
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = g_overlay.state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = g_overlay.texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = staging.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = staging.offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kOverlayW, kOverlayH, 1, alignedPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_overlay.texture.Get();
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        g_overlay.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_overlay.lastText = text;
        g_overlay.lastRender = now;
    }
    return g_overlay.state == D3D12_RESOURCE_STATE_COPY_SOURCE ? g_overlay.texture.Get() : nullptr;
}

// The frame path stages through the frame's upload ring.
ID3D12Resource* PrepareOverlay()
{
    return PrepareOverlayOn(C().list->Real(), [](UINT64 bytes) {
        OverlayUpload u;
        const UINT64 offset = RingAlloc(bytes, 512, "overlay");
        if (offset == ~0ull)
            return u;
        u.resource = Frame().ring.Get();
        u.cpu = Frame().ringCpu;
        u.offset = offset;
        return u;
    }, false);
}

// Presents the last frame again with the overlay on top, on a separate command list
// (the frame's own list is mid-recording while the command processor waits).
void HoldPresent()
{
    g_hold.lastPresent = std::chrono::steady_clock::now();
    if (!g_hold.image || !g_r.device)
        return;
    if (!g_hold.list)
    {
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_hold.allocator))) ||
            FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_hold.allocator.Get(), nullptr, IID_PPV_ARGS(&g_hold.list))))
            return;
        g_hold.list->Close();
        const UINT64 bytes = UINT64((kOverlayW * 4 + 255) & ~255u) * kOverlayH + 1024;
        g_hold.upload = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        g_hold.upload->Map(0, nullptr, &mapped);
        g_hold.uploadCpu = static_cast<uint8_t*>(mapped);
    }
    // Everything the main queue still runs must be done before this list reuses its allocator.
    g_hold.allocator->Reset();
    g_hold.list->Reset(g_hold.allocator.Get(), nullptr);
    ID3D12Resource* overlay = PrepareOverlayOn(g_hold.list.Get(), [](UINT64) {
        OverlayUpload u;
        u.resource = g_hold.upload.Get();
        u.cpu = g_hold.uploadCpu;
        return u;
    }, true);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_hold.image.Get();
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_hold.list->ResourceBarrier(1, &b);
    const bool ok = PresenterRecordCopy(g_hold.list.Get(), g_hold.image.Get(), overlay, kOverlayW, kOverlayH);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_hold.list->ResourceBarrier(1, &b);
    g_hold.list->Close();
    if (!ok)
        return;
    ID3D12CommandList* lists[] = { g_hold.list.Get() };
    g_r.queue->ExecuteCommandLists(1, lists);
    PresenterPresent();
    // Wait for it: the allocator and upload buffer are reused at the next hold.
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        WaitFenceValue(value, 500);
    }
}
}

// Puts a full 1280x720 RGBA8 frame (the swap chain size) on screen: the first-boot screen,
// shown before the game has drawn anything that HoldPresent could repeat.
void BootPresent(const std::vector<uint8_t>& rgba)
{
    constexpr uint32_t gw = 1280, gh = 720;
    const uint32_t w = OutputWidth(), h = OutputHeight(); // the swap chain size
    static ComPtr<ID3D12Resource> texture, upload;
    static uint8_t* uploadCpu = nullptr;
    if (!g_r.device || rgba.size() != size_t(gw) * gh * 4)
        return;
    if (!g_hold.list)
    {
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_hold.allocator))) ||
            FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_hold.allocator.Get(), nullptr, IID_PPV_ARGS(&g_hold.list))))
            return;
        g_hold.list->Close();
        const UINT64 bytes = UINT64((kOverlayW * 4 + 255) & ~255u) * kOverlayH + 1024;
        g_hold.upload = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        g_hold.upload->Map(0, nullptr, &mapped);
        g_hold.uploadCpu = static_cast<uint8_t*>(mapped);
    }
    if (!texture)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_r.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
            return;
        upload = CreateBuffer(UINT64((w * 4 + 255) & ~255u) * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        uploadCpu = static_cast<uint8_t*>(mapped);
    }
    // Nearest-neighbour to the output size, centred at 16:9 on wider outputs with the
    // background's edge columns carried out to the sides.
    const uint32_t pitch = (w * 4 + 255) & ~255u;
    const uint32_t contentW = std::min(w, h * gw / gh), left = (w - contentW) / 2;
    for (uint32_t y = 0; y < h; y++)
    {
        const uint32_t* in = reinterpret_cast<const uint32_t*>(rgba.data()) + size_t(y * gh / h) * gw;
        uint32_t* out = reinterpret_cast<uint32_t*>(uploadCpu + size_t(y) * pitch);
        for (uint32_t x = 0; x < w; x++)
        {
            const uint32_t cx = x < left ? 0 : std::min(contentW - 1, x - left);
            out[x] = in[size_t(cx) * gw / contentW];
        }
    }
    g_hold.allocator->Reset();
    g_hold.list->Reset(g_hold.allocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, (w * 4 + 255) & ~255u };
    g_hold.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_hold.list->ResourceBarrier(1, &b);
    const bool ok = PresenterRecordCopy(g_hold.list.Get(), texture.Get());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_hold.list->ResourceBarrier(1, &b);
    g_hold.list->Close();
    if (!ok)
        return;
    ID3D12CommandList* lists[] = { g_hold.list.Get() };
    g_r.queue->ExecuteCommandLists(1, lists);
    PresenterPresent();
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        WaitFenceValue(value, 500);
    }
}

void RendererPredictBegin()
{
    PredictBeginImpl();
}

void RendererPredictShader(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    dwords = ucode::UsedDwords(microcodeBE, dwords);
    const uint64_t hash = Fnv(microcodeBE, dwords * 4) ^ type;
    if (type == 0)
    {
        g_predVs = hash;
        g_predVsCode = microcodeBE;
        g_predVsDwords = dwords;
    }
    else
    {
        g_predPs = hash;
        g_predPsCode = microcodeBE;
        g_predPsDwords = dwords;
    }
}

void RendererPredictDraw(uint32_t initiator)
{
    const uint32_t edramMode = gpureg::Get(RB_MODECONTROL) & 7;
    if ((edramMode != 4 && edramMode != 5) || !g_predVs || !g_predPs || !g_predVsDwords || !g_predPsDwords)
        return;
    const uint32_t prim = initiator & 0x3F;
    const PipelineKey key = MakePipelineKey(g_predVs, g_predPs, edramMode == 5, prim != 8 && prim != 1);
    if (g_r.pipelines.count(key) || !g_lookKeys.insert(key).second)
        return;
    LookItem item{ key, nullptr, nullptr, nullptr, false };
    if (!g_r.shaders.count(g_predVs))
        item.vsCode = std::make_shared<std::vector<uint32_t>>(g_predVsCode, g_predVsCode + g_predVsDwords);
    if (!g_r.shaders.count(g_predPs))
        item.psCode = std::make_shared<std::vector<uint32_t>>(g_predPsCode, g_predPsCode + g_predPsDwords);
    g_look.push_back(std::move(item));
}

bool RendererCompileIdle()
{
    const uint64_t nowMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool warming = (g_stats.warmTotal.load() && g_stats.warmDone.load() < g_stats.warmTotal.load()) || g_stats.staticTotal.load();
    return g_stats.busy.load() == 0 && !warming && nowMs - g_stats.lastColdMs.load() > 2000;
}

void RendererSaveCaches()
{
    g_diskWriter.Flush();
    g_pipelineLibrary.MaybeSave(true);
}

void RendererSetShader(uint32_t type, const uint32_t* microcodeBE, uint32_t usedDwords, uint64_t hash)
{
    std::vector<uint32_t>& code = type == 0 ? C().vsCode : C().psCode;
    uint64_t& current = type == 0 ? C().vsHash : C().psHash;
    // Titles reload the bound shader before most draws: the same hash is the same used microcode, so
    // the copy is already there (FH1: ~2% of the draw thread on an FX-8350). REGPU_NO_SAME_SHADER_SKIP=1: copy anyway.
    static LiveSwitch noSkip("REGPU_NO_SAME_SHADER_SKIP");
    if (current == hash && code.size() == usedDwords && !noSkip)
        return;
    code.assign(microcodeBE, microcodeBE + usedDwords);
    current = hash;
}

void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    std::vector<uint32_t>& code = type == 0 ? C().vsCode : C().psCode;
    code.assign(microcodeBE, microcodeBE + ucode::UsedDwords(microcodeBE, dwords));
    (type == 0 ? C().vsHash : C().psHash) = Fnv(code.data(), code.size() * 4) ^ type;
}

namespace
{
void TraceSkip(const char* why, uint32_t initiator)
{
    const uint64_t traceFrame = TraceFrame();
    if (traceFrame && g_r.frame + 1 == traceFrame)
        Log("skip draw (%s): prim %u count %u mode %u vs %016llx ps %016llx", why, initiator & 0x3F, initiator >> 16,
            gpureg::Get(RB_MODECONTROL) & 7, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
}
}

namespace
{
// Performance counters (REGPU_PERF=1 logs them once a second).
struct Perf
{
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    double drawSeconds = 0, swapSeconds = 0;
    double section[8] = {};
    double sub[4] = {}; // inside section 3: index list, constant packing, vertex data, the rest
    uint64_t tablesBuilt = 0, tablesReused = 0;
    std::chrono::steady_clock::time_point mark;
    uint64_t frames = 0, draws = 0, slowFrames = 0;
    double maxFrame = 0;
} g_perf;
void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
}


// ---- Parallel draw pieces --------------------------------------------------------------
namespace
{
std::mutex g_poolMutex;

ID3D12GraphicsCommandList* AcquireList()
{
    FrameResources& f = Frame();
    std::lock_guard lock(g_poolMutex);
    if (f.poolUsed == f.pool.size())
    {
        auto& e = f.pool.emplace_back();
        Check(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.first)), "CreateCommandAllocator(piece)");
        Check(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e.first.Get(), nullptr, IID_PPV_ARGS(&e.second)),
            "CreateCommandList(piece)");
    }
    else
    {
        auto& e = f.pool[f.poolUsed];
        e.first->Reset();
        e.second->Reset(e.first.Get(), nullptr);
    }
    return f.pool[f.poolUsed++].second.Get();
}

// The state every draw list starts with.
void ListPrologue(RecordedList& list)
{
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    list.SetDescriptorHeaps(1, heaps);
    list.SetGraphicsRootSignature(g_r.rootSignature.Get());
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// Executes the main list so far and continues the frame in a fresh one.
bool SuspendOcclusionQuery();
void ResumeOcclusionQuery();

void SplitMainList()
{
    // A query must end in the list it began in: an open one is suspended and continues in the
    // new list, its parts counted under the key it gets when the next event closes it.
    const bool suspended = SuspendOcclusionQuery();
    ID3D12GraphicsCommandList* done = g_r.recorder.Real();
    done->Close();
    ID3D12CommandList* lists[] = { done };
    g_r.queue->ExecuteCommandLists(1, lists);
    g_r.recorder.Rebind(AcquireList());
    ListPrologue(g_r.recorder);
    g_drawState.Invalidate();
    SelectSurfaces(true);
    if (suspended)
        ResumeOcclusionQuery();
}

// Waits until everything submitted to the queue so far has finished.
void WaitForQueueIdle()
{
    const UINT64 value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    if (g_r.fence->GetCompletedValue() < value)
    {
        WaitFenceValue(value, 2000);
    }
}

// The parts of the open segment ended by list splits (SplitMainList), already resolved: they take
// the segment's key when it closes (the frame's counts are summed per key).
std::vector<UINT> g_occlusionSplit;

void EndOcclusionQuery()
{
    g_occlusionOpen = false;
    C().list->EndQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    C().list->ResolveQueryData(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex, 1, g_occlusionReadback.Get(), UINT64(g_occlusionIndex) * 8);
    for (const UINT part : g_occlusionSplit)
        g_occlusionPending[g_r.frameIndex].push_back({ g_occlusionKey, part });
    g_occlusionSplit.clear();
    g_occlusionPending[g_r.frameIndex].push_back({ g_occlusionKey, g_occlusionIndex });
}

// Ends the open query's part in the current list (true if one was open).
bool SuspendOcclusionQuery()
{
    if (!g_occlusionOpen)
        return false;
    C().list->EndQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    C().list->ResolveQueryData(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex, 1, g_occlusionReadback.Get(), UINT64(g_occlusionIndex) * 8);
    g_occlusionSplit.push_back(g_occlusionIndex);
    g_occlusionOpen = false;
    return true;
}

// Continues a suspended segment in the new list, in a query of its own (none left: the parts so
// far still count when the next event closes the segment).
void ResumeOcclusionQuery()
{
    if (g_occlusionCount >= kOcclusionPerFrame)
        return;
    g_occlusionIndex = g_r.frameIndex * kOcclusionPerFrame + g_occlusionCount++;
    C().list->BeginQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    g_occlusionOpen = true;
}

// Deferred write-backs (regpu.h Writeback::Mode::Deferred): the requested part of a resolve is copied to
// a readback buffer with its frame, and written to guest memory when that frame slot comes round again,
// a frame or two late and without a stall (waiting for each one ran a game reading its picture every
// frame at a third of its speed). Depth goes out as the console stores a D24FS8 copy: tiled 32-bit
// words, the 20e4 float depth in the top 24 bits; colour as tiled A8R8G8B8.
struct DeferredWriteback
{
    ComPtr<ID3D12Resource> buffer;
    UINT pitch = 0, hostW = 0, hostH = 0;
    DXGI_FORMAT hostFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t base = 0, width = 0, height = 0, info = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool depth = false;
    // Guest memory the write covers, as it was when the resolve was recorded (see WritebackRange).
    uint32_t memBytes = 0;
    uint64_t memHash = 0;
    // Guest memory doesn't hold our last write-back of this destination (none yet, or the title has put
    // something else there since): the title would find no image of it until this one lands.
    bool first = false;
    // Fence-ordered write-back (FenceWritebacks): generation << 2 | state (0 waiting, 1 being written,
    // 2 written); the generation changes each time the entry is reused, so a late claim cannot take
    // the next use's entry.
    std::atomic<uint64_t> state{ 0 };
};
// A deque: entries keep their address while the fence writer thread may hold them.
std::deque<DeferredWriteback> g_deferredWritebacks[kFrames];
size_t g_deferredUsed[kFrames] = {};
size_t g_deferredFenced[kFrames] = {}; // entries already handed to the fence writer
// Base -> hash of the guest memory after our last write-back there (ClaimAndWrite).
std::mutex g_ownWriteMutex;
std::unordered_map<uint32_t, uint64_t> g_ownWrite;

// REGPU_NO_FENCE_WRITEBACKS=1: deferred write-backs land only when their frame slot comes round again,
// as before. Otherwise, with fences written after the host GPU (the fence writer thread), each one is
// written to guest memory just before the first fence after it: a title that reads a small resolved
// image once it sees the next fence (as on the console) finds it there.
bool FenceWritebacks()
{
    static const bool on = !getenv("REGPU_NO_FENCE_WRITEBACKS");
    return on;
}
struct FencedWriteback
{
    DeferredWriteback* w;
    uint64_t fence, generation;
    UINT slot;
};
std::mutex g_fencedMutex;
std::deque<FencedWriteback> g_fenced;

void WriteResolvePixels(const uint8_t* pixels, UINT pitch, DXGI_FORMAT hostFormat, UINT hostW, UINT hostH, uint32_t base, uint32_t width,
    uint32_t info, bool depth, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1);

// The guest bytes a deferred write-back of rows [0, y1) covers: the tiled destination, 32-texel aligned.
uint32_t WritebackRange(const DeferredWriteback& w)
{
    uint32_t log2Bytes = 2;
    if (!w.depth && !resolve::DestLog2Bytes(w.depth ? 6 : (w.info >> 7) & 0x3F, log2Bytes))
        return 0;
    return ((w.width + 31) & ~31u) * ((w.y1 + 31) & ~31u) << log2Bytes;
}

// Writes one entry's pixels if this caller claims it (state waiting, generation as expected).
// The console writes a resolve to memory as the copy runs; ours lands a frame or more later. If the
// title has put something else in that memory since (a transient image's memory reused for a texture
// the game streamed in), the console's write would long have been overwritten by it: the late write
// is skipped, so it doesn't land on the new data. REGPU_WRITEBACK_OVERWRITE=1 writes it anyway, as before.
bool ClaimAndWrite(DeferredWriteback& w, uint64_t generation)
{
    uint64_t expected = generation << 2;
    if (!w.state.compare_exchange_strong(expected, (generation << 2) | 1))
        return false;
    static LiveSwitch overwrite("REGPU_WRITEBACK_OVERWRITE");
    // (Our own earlier write-back of the same image landing in between is not the title's: the memory
    // then holds what we last wrote there.)
    const uint64_t now = w.memBytes ? ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(w.base)), w.memBytes, 4) : 0;
    bool ours = false;
    {
        std::lock_guard lock(g_ownWriteMutex);
        auto it = g_ownWrite.find(w.base);
        ours = it != g_ownWrite.end() && it->second == now;
    }
    if (!overwrite && w.memBytes && now != w.memHash && !ours)
    {
        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 200)
            Log("renderer: write-back of %08X (%ux%u, %u bytes) skipped: the title has reused that memory since the resolve", w.base, w.width,
                w.height, w.memBytes);
        w.state = (generation << 2) | 2;
        return true;
    }
    const uint8_t* pixels = nullptr;
    if (SUCCEEDED(w.buffer->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))) && pixels)
    {
        WriteResolvePixels(pixels, w.pitch, w.hostFormat, w.hostW, w.hostH, w.base, w.width, w.info, w.depth, w.x0, w.x1, w.y0, w.y1);
        if (w.memBytes)
        {
            std::lock_guard lock(g_ownWriteMutex);
            g_ownWrite[w.base] = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(w.base)), w.memBytes, 4);
        }
        const D3D12_RANGE none{ 0, 0 };
        w.buffer->Unmap(0, &none);
    }
    w.state = (generation << 2) | 2;
    return true;
}

uint32_t EncodeDepth20e4(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (v < std::ldexp(1.0f, -14))
        return std::min<uint32_t>(uint32_t(std::lround(std::ldexp(double(v), 34))), 0xFFFFF); // denormal: m / 2^20 * 2^-14
    int e = 0;
    (void)std::frexp(double(v), &e); // v = f * 2^e, f in [0.5, 1)
    const int exponent = std::clamp(e - 1 + 15, 1, 15);
    const double mantissa = std::ldexp(double(v), 15 - exponent) - 1.0;
    return uint32_t(exponent) << 20 | std::min<uint32_t>(uint32_t(std::lround(mantissa * 1048576.0)), 0xFFFFF);
}

// Writes the game pixels [x0, x1) x [y0, y1) of a resolve to guest memory as the GPU stores its
// destination: tiled (32-texel aligned pitch), in the destination format (RB_COPY_DEST_INFO
// bits 7-12) with the copy's red/blue swap and endian mode. `pixels` is the host texture read
// back (RGBA8, RGBA16F, or R32F for depth), hostW x hostH texels `pitch` bytes apart, covering
// the game pixels at the host's render scale (3x3 texels per game pixel at 4K). Each game pixel
// is the average of the texels it covers for colour, the closest one to its centre for depth
// (the texel that sampled where the console's pixel did; REGPU_WRITEBACK_POINT takes that for
// colour too). Depth goes out as the console stores a D24FS8 copy: 32-bit words, the 20e4 float
// depth in the top 24 bits.
void WriteResolvePixels(const uint8_t* pixels, UINT pitch, DXGI_FORMAT hostFormat, UINT hostW, UINT hostH, uint32_t base, uint32_t width,
    uint32_t info, bool depth, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    static LiveSwitch pointColor("REGPU_WRITEBACK_POINT");
    const uint32_t endian = info & 7, destFormat = depth ? 6 : (info >> 7) & 0x3F;
    const bool swapRB = !depth && ((info >> 24) & 1);
    uint32_t log2Bytes = 2;
    if (!depth && !resolve::DestLog2Bytes(destFormat, log2Bytes))
        return;
    const uint32_t bytes = 1u << log2Bytes, pitchTexels = (width + 31) & ~31u;
    const UINT hostBpp = HostBpp(hostFormat);
    const bool point = depth || pointColor;
    uint8_t* guest = GuestPtr<uint8_t>(GpuPhysToGuest(base));
    // The host texels [first, last) that game pixel i of n covers when the n pixels span `host`
    // texels: at least one, and only the one at its centre when point sampling.
    const auto span = [point](uint32_t i, uint32_t n, UINT host, UINT& first, UINT& last) {
        if (point)
        {
            first = std::min<UINT>(UINT(uint64_t(2 * i + 1) * host / (2 * uint64_t(n))), host - 1);
            last = first + 1;
            return;
        }
        first = std::min<UINT>(UINT(uint64_t(i) * host / n), host - 1);
        last = std::clamp<UINT>(UINT(uint64_t(i + 1) * host / n), first + 1, host);
    };
    for (uint32_t y = y0; y < y1; y++)
    {
        UINT hy0, hy1;
        span(y - y0, y1 - y0, hostH, hy0, hy1);
        for (uint32_t x = x0; x < x1; x++)
        {
            UINT hx0, hx1;
            span(x - x0, x1 - x0, hostW, hx0, hx1);
            uint32_t words[4] = {};
            if (depth)
                words[0] = EncodeDepth20e4(*reinterpret_cast<const float*>(pixels + size_t(hy0) * pitch + size_t(hx0) * 4)) << 8;
            else
            {
                float c[4] = {};
                for (UINT hy = hy0; hy < hy1; hy++)
                    for (UINT hx = hx0; hx < hx1; hx++)
                    {
                        const uint8_t* px = pixels + size_t(hy) * pitch + size_t(hx) * hostBpp;
                        for (int k = 0; k < 4; k++)
                        {
                            if (hostBpp == 16)
                            {
                                float f;
                                memcpy(&f, px + k * 4, 4);
                                c[k] += f;
                            }
                            else if (hostBpp == 8)
                            {
                                uint16_t h;
                                memcpy(&h, px + k * 2, 2);
                                c[k] += resolve::HalfToFloat(h);
                            }
                            else
                                c[k] += float(px[k]) / 255.0f;
                        }
                    }
                const float n = float((hx1 - hx0) * (hy1 - hy0));
                for (float& v : c)
                    v /= n;
                if (swapRB)
                    std::swap(c[0], c[2]);
                resolve::PackPixel(destFormat, c, words);
            }
            const size_t at = TextureTiledOffset(x, y, pitchTexels, log2Bytes);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(words);
            for (uint32_t k = 0; k < bytes; k++)
                guest[resolve::SwappedByte(at + k, endian)] = b[k];
        }
    }
}

void FinishDeferredWritebacks(UINT frameIndex)
{
    {
        // This slot's entries leave the fence writer's list: they are finished here.
        std::lock_guard lock(g_fencedMutex);
        std::erase_if(g_fenced, [&](const FencedWriteback& f) { return f.slot == frameIndex; });
    }
    g_deferredFenced[frameIndex] = 0;
    for (size_t i = 0; i < g_deferredUsed[frameIndex]; i++)
    {
        DeferredWriteback& w = g_deferredWritebacks[frameIndex][i];
        // Written by the fence writer already (or being written: wait for it), else written here.
        const uint64_t generation = w.state.load() >> 2;
        if (!ClaimAndWrite(w, generation))
            while ((w.state.load() & 3) == 1)
                std::this_thread::yield();
        if (auto it = g_r.resolved.find(w.base); it != g_r.resolved.end())
            NoteResolvedMemory(w.base, it->second, true); // our own write: not a reuse of the memory
    }
    g_deferredUsed[frameIndex] = 0;
}

// Queues the rows of this resolve (rect) inside the requested region for a deferred write-back.
void QueueDeferredWriteback(uint32_t base, uint32_t width, uint32_t height, uint32_t info, bool depth, const D3D12_RECT& rect,
    const regpu::Writeback& request)
{
    const auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource ||
        (depth ? it->second.format != DXGI_FORMAT_R32_FLOAT
               : !HostColorTexture(it->second.format)))
        return;
    const uint32_t x0 = std::min(request.x0, width), x1 = std::min(request.x1, width);
    const uint32_t y0 = std::max<uint32_t>(std::min(request.y0, height), uint32_t(std::max<LONG>(rect.top, 0)));
    const uint32_t y1 = std::min<uint32_t>(std::min(request.y1, height), uint32_t(std::max<LONG>(rect.bottom, 0)));
    if (x1 <= x0 || y1 <= y0)
        return;
    ResolvedTexture& r = it->second;
    auto& slot = g_deferredWritebacks[g_r.frameIndex];
    if (g_deferredUsed[g_r.frameIndex] == slot.size())
        slot.emplace_back();
    DeferredWriteback& w = slot[g_deferredUsed[g_r.frameIndex]++];
    w.state = ((w.state.load() >> 2) + 1) << 2; // a new use: next generation, waiting
    const UINT hostX0 = UINT(uint64_t(x0) * r.hostWidth / width), hostY0 = UINT(uint64_t(y0) * r.hostHeight / height);
    w.hostW = std::max<UINT>(1, std::min<UINT>(UINT(uint64_t(x1) * r.hostWidth / width), r.hostWidth) - hostX0);
    w.hostH = std::max<UINT>(1, std::min<UINT>(UINT(uint64_t(y1) * r.hostHeight / height), r.hostHeight) - hostY0);
    w.hostFormat = r.format;
    w.pitch = (w.hostW * HostBpp(r.format) + 255) & ~255u;
    const UINT64 bytes = UINT64(w.pitch) * w.hostH;
    if (!w.buffer || w.buffer->GetDesc().Width < bytes)
        w.buffer = CreateBuffer(std::max<UINT64>(bytes, 1u << 20), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r.resource.Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    C().list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = w.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { r.format, w.hostW, w.hostH, 1, w.pitch };
    src.pResource = r.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{ hostX0, hostY0, 0, hostX0 + w.hostW, hostY0 + w.hostH, 1 };
    C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    C().list->ResourceBarrier(1, &bar);
    w.base = base;
    w.width = width;
    w.height = height;
    w.info = info;
    w.depth = depth;
    w.x0 = x0, w.x1 = x1, w.y0 = y0, w.y1 = y1;
    w.memBytes = WritebackRange(w);
    w.memHash = w.memBytes ? ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(base)), w.memBytes, 4) : 0;
    {
        std::lock_guard lock(g_ownWriteMutex);
        const auto own = g_ownWrite.find(base);
        w.first = own == g_ownWrite.end() || own->second != w.memHash;
    }
}

// Before a fence write from the command stream: what has been recorded is submitted and a host
// fence marks its end, so the title only sees the fence once the draws before it have really read
// their data. Written at record time, it reused vertex and index buffers the GPU had yet to read
// (PGR4: torn geometry spiking into the sky for a frame, crowd poses snapping). The command
// processor used to wait for the GPU here, which kept the CPU and GPU from ever overlapping
// (PGR4 ran at 25 fps); the write now waits on its own thread (gpu.cpp).
// REGPU_EARLY_FENCES writes them at record time again (A/B).
uint64_t FenceSubmitImpl()
{
    static LiveSwitch early("REGPU_EARLY_FENCES");
    if (!g_r.ok)
        return 0;
    // With record-time fences, a fence that follows a small resolve write-back of this frame still goes
    // after the host GPU (and the write-back), on the fence writer: the title finds the image in memory
    // once it sees that fence. Record-time fences behind it queue after it on the writer, so values stay
    // in order. REGPU_NO_WRITEBACK_FENCES=1: every fence at record time (the write-backs land later).
    // Only a write-back whose destination memory doesn't hold our last write-back of it holds its fence:
    // there the title would find no image at all until it lands (a scene start, or memory the title used
    // for something else in between). Others replace an image already in memory, one or two frames old,
    // and land when their frame slot comes round, with no wait: a fence after the host GPU in every frame
    // tied the title to the GPU's pace (race frames went from about 16 ms to 33-50).
    // REGPU_WRITEBACK_FENCES_ALL=1: every write-back holds its fence, as before.
    if (early)
    {
        static LiveSwitch noWritebackFences("REGPU_NO_WRITEBACK_FENCES");
        static LiveSwitch allWritebacks("REGPU_WRITEBACK_FENCES_ALL");
        const UINT slot = g_r.frameIndex;
        if (noWritebackFences || !FenceWritebacks())
            return 0;
        // Write-backs the host GPU has finished land now (not when their frame slot comes round).
        RendererFinishFencedWritebacks(g_r.fence->GetCompletedValue());
        if (g_deferredUsed[slot] <= g_deferredFenced[slot])
            return 0;
        bool hold = allWritebacks;
        for (size_t i = g_deferredFenced[slot]; i < g_deferredUsed[slot] && !hold; i++)
            if (g_deferredWritebacks[slot][i].first)
            {
                hold = true;
                static uint32_t logged = 0;
                const DeferredWriteback& w = g_deferredWritebacks[slot][i];
                if (logged++ < 100)
                    Log("renderer: fence held for the write-back of %08X (%ux%u): its memory doesn't hold our last image of it (frame %llu)", w.base,
                        w.width, w.height, (unsigned long long)g_r.frame);
            }
        if (!hold)
        {
            // Not held: the fence is written now, and these write-backs land as soon as the host GPU has
            // done them (a host fence after them, checked at every later fence), about a frame after the
            // resolve, without the title waiting. REGPU_WRITEBACK_AT_SLOT=1: they land kFrames later, as before.
            static LiveSwitch atSlot("REGPU_WRITEBACK_AT_SLOT");
            if (!atSlot)
            {
                BeginFrameIfNeeded();
                SplitMainList();
                const uint64_t value = g_r.nextFence++;
                g_r.queue->Signal(g_r.fence.Get(), value);
                std::lock_guard lock(g_fencedMutex);
                for (size_t i = g_deferredFenced[slot]; i < g_deferredUsed[slot]; i++)
                {
                    DeferredWriteback& w = g_deferredWritebacks[slot][i];
                    g_fenced.push_back({ &w, value, w.state.load() >> 2, slot });
                }
                g_deferredFenced[slot] = g_deferredUsed[slot];
            }
            return 0;
        }
    }
    // Nothing recorded since the last one: the same point in the queue.
    static uint64_t lastFrame = ~0ull, lastValue = 0;
    static uint32_t lastDraws = ~0u;
    const uint32_t draws = g_r.drawsThisFrame.load();
    const UINT slot = g_r.frameIndex;
    const bool newWritebacks = FenceWritebacks() && g_deferredUsed[slot] > g_deferredFenced[slot];
    if (lastFrame == g_r.frame && lastDraws == draws && !newWritebacks)
        return lastValue;
    lastFrame = g_r.frame;
    lastDraws = draws;
    BeginFrameIfNeeded();
    SplitMainList();
    lastValue = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), lastValue);
    if (FenceWritebacks())
    {
        // The write-backs recorded before this fence: the fence writer lands them before the fence.
        std::lock_guard lock(g_fencedMutex);
        for (size_t i = g_deferredFenced[slot]; i < g_deferredUsed[slot]; i++)
        {
            DeferredWriteback& w = g_deferredWritebacks[slot][i];
            g_fenced.push_back({ &w, lastValue, w.state.load() >> 2, slot });
        }
        g_deferredFenced[slot] = g_deferredUsed[slot];
    }
    return lastValue;
}

void WriteResolveToGuest(uint32_t base, uint32_t width, uint32_t height, uint32_t info)
{
    const auto it = g_r.resolved.find(base);
    if (it == g_r.resolved.end() || !it->second.resource || !width || !height ||
        !HostColorTexture(it->second.format))
        return;
    ResolvedTexture& r = it->second;
    // Run what has been recorded (the resolve's copy included) and let the GPU finish it.
    BeginFrameIfNeeded();
    SplitMainList();
    WaitForQueueIdle();
    const UINT pitch = (r.hostWidth * HostBpp(r.format) + 255) & ~255u;
    ComPtr<ID3D12Resource> buffer = CreateBuffer(UINT64(pitch) * r.hostHeight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList* list = AcquireList();
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r.resource.Get();
    bar.Transition.Subresource = 0;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { r.format, r.hostWidth, r.hostHeight, 1, pitch };
    src.pResource = r.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    list->ResourceBarrier(1, &bar);
    list->Close();
    ID3D12CommandList* lists[] = { list };
    g_r.queue->ExecuteCommandLists(1, lists);
    WaitForQueueIdle();
    uint8_t* pixels = nullptr;
    buffer->Map(0, nullptr, reinterpret_cast<void**>(&pixels));
    if (!pixels)
        return;
    WriteResolvePixels(pixels, pitch, r.format, r.hostWidth, r.hostHeight, base, width, info, false, 0, width, 0, height);
    buffer->Unmap(0, nullptr);
    NoteResolvedMemory(base, r, true); // our own write: not a reuse of the memory
    Log("resolve: wrote %ux%u back to guest memory at %08X (format %u, endian %u, swap %u)", width, height, base, (info >> 7) & 0x3F, info & 7,
        (info >> 24) & 1);
}

struct Workers
{
    bool started = false;
    std::mutex mutex;
    std::condition_variable start, done;
    uint64_t generation = 0;
    const std::function<void(uint32_t)>* job = nullptr;
    uint32_t count = 0;
    std::atomic<uint32_t> next{ 0 }, finished{ 0 };
    std::vector<ID3D12GraphicsCommandList*> lists;
} g_workers;

// One piece on this thread, with a worker context of its own.
std::atomic<uint64_t> g_pieceNs{ 0 }, g_segmentNs{ 0 }, g_pieceCount{ 0 }, g_segmentCount{ 0 };
void TranslatePiece(DrawContext& ctx, uint32_t i)
{
    const auto t0 = std::chrono::steady_clock::now();
    ID3D12GraphicsCommandList* real = AcquireList();
    RecordedList recorder;
    recorder.Init(real, true);
    ctx = DrawContext{};
    ctx.list = &recorder;
    ctx.worker = true;
    DrawContext* saved = t_context;
    t_context = &ctx;
    ListPrologue(recorder);
    g_drawState.Invalidate();
    (*g_workers.job)(i);
    t_context = saved;
    real->Close();
    g_workers.lists[i] = real;
    g_pieceNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_pieceCount++;
}

}
HANDLE g_profiledWorker = nullptr; // REGPU_PROFILE=w
namespace
{
void WorkerMain()
{
    SetThreadDescription(GetCurrentThread(), L"gpu draw piece worker");
    if (!g_profiledWorker)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_profiledWorker, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DrawContext ctx;
    uint64_t seen = 0;
    for (;;)
    {
        {
            std::unique_lock lock(g_workers.mutex);
            g_workers.start.wait(lock, [&] { return g_workers.generation != seen; });
            seen = g_workers.generation;
        }
        for (uint32_t i; (i = g_workers.next.fetch_add(1)) < g_workers.count;)
        {
            TranslatePiece(ctx, i);
            if (g_workers.finished.fetch_add(1) + 1 == g_workers.count)
            {
                std::lock_guard lock(g_workers.mutex);
                g_workers.done.notify_all();
            }
        }
    }
}
}

// Exceptions escaping a thread end the process silently: say where.
void InstallTerminateLog()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::set_terminate([] {
            std::string what = "?";
            if (auto e = std::current_exception())
                try { std::rethrow_exception(e); } catch (const std::exception& x) { what = x.what(); } catch (...) {}
            Log("CRASH: std::terminate (%s): %s", what.c_str(), regpu::GetHost().currentStack ? regpu::GetHost().currentStack(24).c_str() : "");
            fflush(nullptr);
            std::abort();
        });
    });
}

uint32_t RendererDrawWorkers()
{
    InstallTerminateLog();
    static const uint32_t n = [] {
        const char* v = getenv("REGPU_DRAW_WORKERS");
        const uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
        (void)hw;
        // Off by default: measured slower than the serial path so far (per-draw cost roughly
        // doubles across threads and the command processor waits for each segment).
        return v ? std::min<uint32_t>(uint32_t(atoi(v)), hw) : 0u;
    }();
    return n;
}

bool RendererParallelReady()
{
    // (The EDRAM target cache's ownership follows draw order: one thread records.)
    return RendererDrawWorkers() > 0 && !g_edramTargetsOn && g_r.ok && GameSettled() && GetSettings().upscaler == Settings::Upscaler::Off && !RendererTracing();
}

bool RendererIsResolveDraw()
{
    return (gpureg::Get(RB_MODECONTROL) & 7) == 6;
}

void RendererLogCrashContext()
{
    if (g_compiling.active)
    {
        Log("crash: this thread was inside the driver compiling pipeline vs %016llx ps %016llx; it will be skipped on the next start", (unsigned long long)g_compiling.vs, (unsigned long long)g_compiling.ps);
        if (FILE* f = fopen(kBadPipelinesFile, "a"))
        {
            fprintf(f, "%016llx %016llx%c", (unsigned long long)g_compiling.vs, (unsigned long long)g_compiling.ps, 10);
            fclose(f);
        }
    }
    const uint32_t next = g_drawRingNext.load();
    for (uint32_t i = 0; i < 8 && i < next; i++)
    {
        const DrawRecord& d = g_drawRing[(next - 1 - i) % kDrawRing];
        Log("crash: recent draw %u: frame %llu vs %016llx ps %016llx", i, (unsigned long long)d.frame, (unsigned long long)d.vs, (unsigned long long)d.ps);
    }
    LogFlush(); // (the crash report goes through the async log writer)
}

// Before a fence write from the command stream: run what has been recorded and let the host
// GPU finish it, so the title only sees the fence once the draws before it have really read
// their data. Written at record time, a title could reuse vertex and index buffers the GPU had
// yet to read (torn geometry for a frame). The console signals its fences when the GPU gets
// there. REGPU_EARLY_FENCES writes them at record time again (A/B; faster).
void RendererLogDred()
{
    static bool logged = false;
    if (!g_r.device || logged || g_r.device->GetDeviceRemovedReason() == S_OK)
        return;
    logged = true;
    Log("dred: device removed, reason %08lX", g_r.device->GetDeviceRemovedReason());
    if (const int64_t waitStart = g_pipelineWaitStartNs.load(); waitStart)
        Log("dred: the command processor was waiting %.0f ms for pipeline %016llx/%016llx",
            double(std::chrono::steady_clock::now().time_since_epoch().count() - waitStart) / 1e6, (unsigned long long)g_pipelineWaitVs.load(),
            (unsigned long long)g_pipelineWaitPs.load());
    {
        const uint32_t next = g_drawRingNext.load();
        Log("dred: the last %u draws recorded, newest first (frame, vertex shader, pixel shader, target pitch, colour base, motion):", kDrawRing);
        for (uint32_t i = 0; i < kDrawRing && i < next; i++)
        {
            const DrawRecord& d = g_drawRing[(next - 1 - i) % kDrawRing];
            Log("dred:   %2u: frame %llu vs %016llx ps %016llx pitch %u base %u%s", i, (unsigned long long)d.frame, (unsigned long long)d.vs,
                (unsigned long long)d.ps, d.pitch, d.colorBase, d.motion ? " motion" : "");
        }
    }
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(g_r.device->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        Log("dred: not available (start with REGPU_DRED=1)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)) && breadcrumbs.pHeadAutoBreadcrumbNode)
    {
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
        {
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount)
                continue; // that list finished
            Log("dred: command list %ls on %ls stopped at operation %u of %u", node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)", done, node->BreadcrumbCount);
            for (UINT i = done > 4 ? done - 4 : 0; i < node->BreadcrumbCount && i < done + 4; i++)
                Log("dred:   op %u: type %d%s", i, int(node->pCommandHistory[i]), i == done ? "  <- the GPU did not complete this one" : "");
        }
    }
    else
        Log("dred: no breadcrumbs (REGPU_DRED=1 was not set when the device was created)");
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA)
    {
        Log("dred: page fault at GPU address %llX", (unsigned long long)fault.PageFaultVA);
        for (const D3D12_DRED_ALLOCATION_NODE1* n = fault.pHeadExistingAllocationNode; n; n = n->pNext)
            Log("dred:   live allocation near it: %ls (type %d)", n->ObjectNameW ? n->ObjectNameW : L"(unnamed)", int(n->AllocationType));
        for (const D3D12_DRED_ALLOCATION_NODE1* n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
            Log("dred:   recently freed: %ls (type %d)", n->ObjectNameW ? n->ObjectNameW : L"(unnamed)", int(n->AllocationType));
    }
}

void RendererFenceSyncForced()
{
    if (!g_r.ok)
        return;
    BeginFrameIfNeeded();
    SplitMainList();
    WaitForQueueIdle();
}

void RendererFenceSync()
{
    static LiveSwitch early("REGPU_EARLY_FENCES");
    if (early || !g_r.ok)
        return;
    // Nothing recorded since the last sync: nothing to wait for.
    static uint64_t lastFrame = ~0ull;
    static uint32_t lastDraws = ~0u;
    const uint32_t draws = g_r.drawsThisFrame.load();
    if (lastFrame == g_r.frame && lastDraws == draws)
        return;
    lastFrame = g_r.frame;
    lastDraws = draws;
    BeginFrameIfNeeded();
    SplitMainList();
    WaitForQueueIdle();
}

bool OcclusionAnswer(uint32_t key, uint32_t& samples);

// One EVENT_WRITE_ZPD (see renderer.h). The Xenos snapshots its sample counter into the event's
// block at every such event, and D3D reads a query as its end block minus its begin block; a
// title can put several begins before one end (FM4: five, then the end, each frame). Each event
// closes the segment counted since the event before it, keyed by this event's block, and opens
// the next one, except after an end. An end block answers with the samples of the segment that
// ends at it (the draws since the event just before, summed over the frame's tiles, from the
// last completed frame); begin blocks read 0, as D3D's end-minus-begin then gives that segment.
// (One query from the first begin to the end counted everything drawn in between, millions of
// samples, and the sun flare blew the screen white.)
bool RendererOcclusionEvent(uint32_t address, bool end, uint32_t& samples)
{
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    if (g_occlusionOpen)
    {
        g_occlusionKey = address;
        EndOcclusionQuery();
    }
    else if (!g_occlusionSplit.empty())
    {
        // A segment split by a list split that found no query left to continue in.
        for (const UINT part : g_occlusionSplit)
            g_occlusionPending[g_r.frameIndex].push_back({ address, part });
        g_occlusionSplit.clear();
    }
    if (!end)
    {
        RendererOcclusionBegin(address);
        return false;
    }
    return OcclusionAnswer(address, samples);
}

uint64_t RendererFenceSubmit() { return FenceSubmitImpl(); }

// REGPU_FENCE_SPLIT=1 (with record-time fences): at each fence write the work recorded so far is
// submitted (no wait), so the frame reaches the GPU in pieces instead of one list.
// REGPU_MAX_PIECES_IN_FLIGHT=<k>: the caller then waits until at most k such pieces are unfinished on
// the GPU, bounding the backlog by work rather than by frames (0 or unset: no cap).
void RendererSplitAndCap()
{
    static const bool split = getenv("REGPU_FENCE_SPLIT") != nullptr;
    if (!split || !g_r.ok)
        return;
    static const size_t cap = getenv("REGPU_MAX_PIECES_IN_FLIGHT") ? size_t(std::max(0, atoi(getenv("REGPU_MAX_PIECES_IN_FLIGHT")))) : 0;
    static std::deque<uint64_t> pieces;
    static uint64_t lastFrame = ~0ull;
    static uint32_t lastDraws = ~0u;
    const uint32_t draws = g_r.drawsThisFrame.load();
    if (lastFrame == g_r.frame && lastDraws == draws)
        return; // nothing recorded since the last piece
    lastFrame = g_r.frame;
    lastDraws = draws;
    BeginFrameIfNeeded();
    SplitMainList();
    const uint64_t value = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), value);
    pieces.push_back(value);
    const uint64_t done = g_r.fence->GetCompletedValue();
    while (!pieces.empty() && pieces.front() <= done)
        pieces.pop_front();
    while (cap && pieces.size() > cap)
    {
        const auto w0 = std::chrono::steady_clock::now();
        WaitFenceValue(pieces.front(), INFINITE);
        g_gpuWaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        pieces.pop_front();
    }
}

// The fence writer thread, once the host GPU has reached `completed`: the write-backs handed over
// with a fence value up to it are written to guest memory (before the fence itself).
void RendererFinishFencedWritebacks(uint64_t completed)
{
    for (;;)
    {
        FencedWriteback f;
        {
            std::lock_guard lock(g_fencedMutex);
            if (g_fenced.empty() || g_fenced.front().fence > completed)
                return;
            f = g_fenced.front();
            g_fenced.pop_front();
        }
        ClaimAndWrite(*f.w, f.generation);
    }
}

void RendererFenceWait(uint64_t value, void* event)
{
    if (g_r.fence->GetCompletedValue() >= value)
        return;
    g_r.fence->SetEventOnCompletion(value, HANDLE(event));
    if (WaitForSingleObject(HANDLE(event), 2000) == WAIT_TIMEOUT)
        Log("perf: a guest fence wait timed out after 2000 ms (value %llu, completed %llu)", (unsigned long long)value, (unsigned long long)g_r.fence->GetCompletedValue());
}

void RendererOcclusionBegin(uint32_t address)
{
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    if (g_occlusionFrame != g_r.frame)
    {
        g_occlusionFrame = g_r.frame;
        g_occlusionCount = 0;
    }
    if (g_occlusionOpen || g_occlusionCount >= kOcclusionPerFrame)
        return;
    if (!g_occlusionHeap)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qd.Count = kOcclusionPerFrame * kFrames;
        g_r.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_occlusionHeap));
        g_occlusionReadback = CreateBuffer(8ull * kOcclusionPerFrame * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_occlusionHeap || !g_occlusionReadback)
            return;
    }
    g_occlusionKey = OcclusionByOrder() ? g_occlusionCount : address;
    g_occlusionIndex = g_r.frameIndex * kOcclusionPerFrame + g_occlusionCount++;
    C().list->BeginQuery(g_occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_occlusionIndex);
    g_occlusionOpen = true;
}

bool RendererOcclusionEnd(uint32_t address, uint32_t& samples)
{
    // A query that couldn't be opened here (one already open, or the frame's queries used up)
    // still has its block's count from an earlier frame.
    const bool open = g_occlusionOpen;
    if (!open && OcclusionByOrder())
        return false;
    const uint32_t key = open ? g_occlusionKey : address;
    if (open)
        EndOcclusionQuery();
    return OcclusionAnswer(key, samples);
}

// The count stored for a key, in console samples; false while there is none yet.
bool OcclusionAnswer(uint32_t key, uint32_t& samples)
{
    const auto it = g_occlusionResult.find(key);
    if (it == g_occlusionResult.end())
        return false;
    // Host samples to the console's: the output is ScaleX x ScaleY host pixels per game pixel
    // with g_msaa samples each; the guest surface has 1, 2 or 4 samples per pixel.
    const uint32_t guestMsaa = (gpureg::Get(RB_SURFACE_INFO) >> 16) & 3;
    const double perHost = double(1u << guestMsaa) / (double(ScaleX()) * double(ScaleY()) * double(g_msaa));
    samples = uint32_t(std::min(double(it->second) * perHost + 0.5, 4294967295.0));
    return true;
}

void RendererTrackTile(RendererTile& tile)
{
    const uint32_t mode = gpureg::Get(RB_MODECONTROL) & 7;
    if (mode != 4 && mode != 5)
        return;
    const D3D12_RECT scissor = WindowScissor();
    const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
    const bool vertexOffset = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1;
    const int32_t wx = vertexOffset ? int32_t(offsetReg << 17) >> 17 : 0, wy = vertexOffset ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
    const bool full = scissor.left <= 0 && scissor.top <= 0 && scissor.right >= LONG(PassWidth()) && scissor.bottom >= LONG(PassHeight());
    if (!full)
    {
        tile.originX = TileColumns() ? -wx : 0;
        tile.originY = -wy;
        tile.rect[0] = scissor.left;
        tile.rect[1] = scissor.top;
        tile.rect[2] = scissor.right;
        tile.rect[3] = scissor.bottom;
    }
}

void RendererSetTile(const RendererTile& tile)
{
    g_tile.originX = tile.originX;
    g_tile.originY = tile.originY;
    g_tile.rect = { tile.rect[0], tile.rect[1], tile.rect[2], tile.rect[3] };
}

void RendererRunPieces(uint32_t n, const std::function<void(uint32_t)>& translate)
{
    if (!n)
        return;
    const auto segmentStart = std::chrono::steady_clock::now();
    struct Done
    {
        std::chrono::steady_clock::time_point start;
        ~Done()
        {
            g_segmentNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_segmentCount++;
            static auto last = std::chrono::steady_clock::now();
            if (getenv("REGPU_PERF") && std::chrono::steady_clock::now() - last > std::chrono::seconds(2))
            {
                last = std::chrono::steady_clock::now();
                Log("perf: parallel segments %llu: %.1f ms wall, %.1f ms in %llu pieces (%.2fx)", (unsigned long long)g_segmentCount.load(),
                    g_segmentNs / 1e6, g_pieceNs / 1e6, (unsigned long long)g_pieceCount.load(), double(g_pieceNs) / std::max<uint64_t>(1, g_segmentNs));
                g_segmentNs = g_pieceNs = g_pieceCount = g_segmentCount = 0;
            }
        }
    } done{ segmentStart };
    BeginFrameIfNeeded();
    SplitMainList();
    // Texture uploads of the pieces, executed before them.
    ID3D12GraphicsCommandList* prepReal = AcquireList();
    RecordedList prep;
    prep.Init(prepReal, true);
    g_prepList = &prep;
    {
        std::lock_guard lock(g_workers.mutex);
        if (!g_workers.started)
        {
            g_workers.started = true;
            // 16 MB stacks like the command processor's: shader translation and the HLSL
            // compiler can run on these threads, and they need far more than 1 MB.
            for (uint32_t i = 0; i < RendererDrawWorkers(); i++)
                if (HANDLE h = CreateThread(nullptr, 16u << 20, [](void*) -> DWORD { WorkerMain(); return 0; }, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr))
                    CloseHandle(h);
        }
        g_workers.job = &translate;
        g_workers.count = n;
        g_workers.next = 0;
        g_workers.finished = 0;
        g_workers.lists.assign(n, nullptr);
        g_workers.generation++;
    }
    g_workers.start.notify_all();
    // This thread takes pieces too.
    static DrawContext ownContext;
    for (uint32_t i; (i = g_workers.next.fetch_add(1)) < n;)
    {
        TranslatePiece(ownContext, i);
        g_workers.finished.fetch_add(1);
    }
    {
        std::unique_lock lock(g_workers.mutex);
        g_workers.done.wait(lock, [] { return g_workers.finished.load() >= g_workers.count; });
    }
    g_prepList = nullptr;
    prepReal->Close();
    std::vector<ID3D12CommandList*> lists;
    lists.push_back(prepReal);
    for (ID3D12GraphicsCommandList* l : g_workers.lists)
        lists.push_back(l);
    g_r.queue->ExecuteCommandLists(UINT(lists.size()), lists.data());
}

void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    if (!g_perfTiming)
    {
        DrawImpl(initiator, dmaBase, dmaSize);
        g_perf.draws++;
        g_passDraws++;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    DrawImpl(initiator, dmaBase, dmaSize);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_perf.drawSeconds += seconds;
    g_frameEvents.worstDrawMs = std::max(g_frameEvents.worstDrawMs, seconds * 1000.0);
    g_perf.draws++;
}

namespace
{
// REGPU_PERF: the parts of section 3 ("constants" in the log), timed from the last SubSection mark.
std::chrono::steady_clock::time_point g_subMark;
void SubSection(int i)
{
    if (!g_perfTiming)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (i > 0)
        g_perf.sub[i - 1] += std::chrono::duration<double>(now - g_subMark).count();
    g_subMark = now;
}
void Section(int i)
{
    if (!g_perfTiming)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (i > 0)
    {
        const double seconds = std::chrono::duration<double>(now - g_perf.mark).count();
        g_perf.section[i - 1] += seconds;
        g_frameEvents.section[i - 1] += seconds * 1000.0;
    }
    g_perf.mark = now;
}

void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    g_drawThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    Section(0);
    g_drawSamplesResolve = false;
    static LiveSwitch noRender("REGPU_NO_RENDER");
    if (noRender)
        return;
    if (!g_r.ok)
        Init();
    const uint32_t edramMode = gpureg::Get(RB_MODECONTROL) & 7;
    if (edramMode == 6)
    {
        Resolve();
        return;
    }
    // Colour+depth (4) and depth-only (5) passes draw.
    if ((edramMode != 4 && edramMode != 5) || C().vsCode.empty() || C().psCode.empty())
    {
        TraceSkip("edram mode", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    // A draw with PA_SC_VIZ_QUERY.kill_pix_post_hi_z (bit 7) is viz query survey geometry: it only
    // feeds the hierarchical-Z visibility test of its query ID, and the hardware kills its pixels
    // after hi-Z, so it writes no colour, depth or stencil, and the sample counter (ZPD) does not
    // count it (the kill comes before the depth and stencil stages). Nothing here predicates draws on
    // viz queries (every conditional draw runs), so a survey has nothing to feed and isn't drawn.
    // Titles may draw them with debug shaders, which then show as solid boxes.
    // REGPU_VIZ_QUERY_DRAWS=1 rasterises them as before (A/B).
    static LiveSwitch vizQueryDraws("REGPU_VIZ_QUERY_DRAWS");
    if (!vizQueryDraws && (gpureg::Get(PA_SC_VIZ_QUERY) >> 7 & 1))
    {
        TraceSkip("viz query survey", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    // A host depth surface holds its EDRAM in one row layout. Views of the same EDRAM in
    // another MSAA mode but the same row width in samples are mapped onto it (g_nativeMsaa);
    // a view of another row width addresses the same tiles in another arrangement (a shadow
    // map drawn 1040 samples wide, cleared through 560-pixel views): a depth-only draw through
    // it is drawn tile run by tile run onto the host surface (EdramAlias), as the EDRAM lays
    // it out. What that can't place (textured, tile-relative, scaled passes) is dropped and
    // logged once per base and view. REGPU_NO_EDRAM_ALIAS=1 drops every such draw;
    // REGPU_NO_FOREIGN_DEPTH_DROP=1 draws them unmoved, as before (A/B).
    EdramAlias alias;
    const auto dropForeignDepth = [&](const char* why) {
        static std::mutex loggedMutex;
        static std::unordered_set<uint64_t> logged;
        const uint32_t depthBase = gpureg::Get(RB_DEPTH_INFO) & 0xFFF;
        {
            std::lock_guard lock(loggedMutex);
            if (logged.insert(uint64_t(depthBase) << 32 | alias.viewSurface).second)
                Log("renderer: unsupported: depth write through a foreign EDRAM layout (depth base %03X, view %08X, host surface %08X): %s; dropped",
                    depthBase, alias.viewSurface, alias.hostSurface, why);
        }
        TraceSkip("depth through a foreign EDRAM layout", initiator);
        g_r.skippedThisFrame++;
    };
    if (const uint32_t depthControl = gpureg::Get(RB_DEPTHCONTROL); depthControl & 3)
    {
        const uint32_t surface = gpureg::Get(RB_SURFACE_INFO), depthBase = gpureg::Get(RB_DEPTH_INFO) & 0xFFF;
        std::atomic<uint32_t>& owner = g_depthOwner[depthBase];
        const uint32_t host = owner.load(std::memory_order_relaxed);
        static LiveSwitch noForeignDrop("REGPU_NO_FOREIGN_DEPTH_DROP");
        // A depth clear (a rectangle writing depth with the test off) through a view of another row width
        // makes that view the layout of the depth base: what it overwrites need not survive in the old one.
        // A title shares one depth base between a 1x 560-sample caster pass (cleared, drawn, copied out) and a
        // later 4x 1040-sample pass of another object; with the first owner kept for good, the caster pass
        // went through the other layout and the copy took in the later pass's depth (a large stray shadow).
        // REGPU_NO_DEPTH_CLEAR_OWNER=1: the first owner stays, as before (A/B).
        static LiveSwitch noClearOwner("REGPU_NO_DEPTH_CLEAR_OWNER");
        const bool clearTakesBase = !noClearOwner && (initiator & 0x3F) == 8 && (depthControl & 4) && ((depthControl >> 4) & 7) == 7;
        if (edramMode == 5 && host && RowSamples(host) != RowSamples(surface) && !noForeignDrop && !clearTakesBase)
        {
            alias = { true, surface, host };
            if (const char* why = ForeignDepthUnplaceable(surface, host))
            {
                dropForeignDepth(why);
                return;
            }
        }
        else
            owner.store(surface, std::memory_order_relaxed);
    }
    {
        // A draw reached a shader we have not seen: a new level or object has loaded. Before the
        // static shader set existed this scanned guest memory for all of its shader objects
        // right away; now that set covers the game, and the 512 MB scan (~18 ms on this
        // thread) only showed as a frame spike, so once the game runs the low-priority
        // background scan is enough.
        const bool unseen = g_r.shaders.find(C().vsHash) == g_r.shaders.end() || g_r.shaders.find(C().psHash) == g_r.shaders.end();
        static auto lastScan = std::chrono::steady_clock::time_point{};
        static LiveSwitch noScan("REGPU_NO_SHADER_SCAN");
        if (unseen && !noScan)
        {
            if (g_scanKnown.count(C().vsHash) || g_scanKnown.count(C().psHash))
                g_scanPrefound++;
            else
                g_scanMissed++;
            if (!g_compiler.park.load() && std::chrono::steady_clock::now() - lastScan > std::chrono::milliseconds(300))
            {
                const auto t0 = std::chrono::steady_clock::now();
                ShaderScanNow(std::max(2u, std::thread::hardware_concurrency() / 2));
                lastScan = std::chrono::steady_clock::now();
                DrainScan(0);
                if (getenv("REGPU_PERF"))
                    Log("perf: shader scan took %.0f ms, %llu shaders found in total", std::chrono::duration<double, std::milli>(lastScan - t0).count(),
                        (unsigned long long)ShaderScanTotal());
            }
        }
    }
    // A live edit of this pixel shader (PsEditAlias): this draw uses the alias, the context keeps the real hash.
    struct PsAlias
    {
        uint64_t real, realVs;
        PsAlias() : real(C().psHash), realVs(C().vsHash) { C().psHash = PsEditAlias(real); C().vsHash = PsEditAlias(realVs); }
        ~PsAlias() { C().psHash = real; C().vsHash = realVs; }
    } psAlias;
    const CompiledShader& vs = GetShader(C().vsHash, C().vsCode, false);
    if (g_drawStats.load(std::memory_order_relaxed) == 2) // the debug server's drawstats: every draw of a frame
    {
        std::lock_guard lock(g_drawStatsMutex);
        g_drawStatsCounts[{ psAlias.realVs, psAlias.real }]++;
    }
    const CompiledShader& ps = GetShader(C().psHash, C().psCode, true);
    Section(1);
    // The shaders were translated from this draw's own microcode; wait for them to compile.
    g_waitWhat = "shader";
    g_waitBlend = gpureg::Get(RB_BLENDCONTROL0);
    static LiveSwitch skipBuilding("REGPU_SKIP_BUILDING_DRAWS"); // see GetPipeline
    if (skipBuilding && GameSettled() &&
        (vs.state.load(std::memory_order_acquire) == CompiledShader::Pending || ps.state.load(std::memory_order_acquire) == CompiledShader::Pending))
    {
        TraceSkip("compiling (skipped, REGPU_SKIP_BUILDING_DRAWS)", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    WaitHoldingImpl([&] {
        return vs.state.load(std::memory_order_acquire) != CompiledShader::Pending && ps.state.load(std::memory_order_acquire) != CompiledShader::Pending;
    });
    if (!vs.IsReady() || !ps.IsReady())
    {
        TraceSkip(vs.state == CompiledShader::Failed || ps.state == CompiledShader::Failed ? "shader" : "compiling", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    if (alias.on && (ps.info.usedTextureSlots || vs.info.usedTextureSlots))
    {
        dropForeignDepth("textured");
        return;
    }
    // Triangle lists and restart-free strips read their indices on the GPU;
    // everything else is expanded here.
    const uint32_t prim = initiator & 0x3F, indexSource = (initiator >> 6) & 3, indexCount = initiator >> 16;
    const bool restartOn = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    static LiveSwitch noDirectIndex("REGPU_NO_DIRECT_INDEX");
    const bool directIndex = g_r.physical && !noDirectIndex && (indexSource == 0 || indexSource == 2) &&
        (prim == 4 || prim == 13 || (prim == 6 && !restartOn));
    const uint32_t directVertices = prim == 4 ? indexCount / 3 * 3 : prim == 13 ? indexCount / 4 * 6 : (indexCount >= 3 ? (indexCount - 2) * 3 : 0);
    IndexListEntry* indexEntry = directIndex ? nullptr : CachedIndexList(initiator, dmaBase, dmaSize);
    // REGPU_TRACE_INDEX_SIZE=<n>: log the first n index-buffer draws (count against the DMA size
    // field, restart and index-clamp state, the highest index an expanded list reaches);
    // REGPU_TRACE_INDEX_VS=<hash>[,<hash>...] limits it to those vertex shaders. Read at start.
    {
        static const int traceIndexSize = getenv("REGPU_TRACE_INDEX_SIZE") ? atoi(getenv("REGPU_TRACE_INDEX_SIZE")) : 0;
        static const std::vector<uint64_t> traceIndexVs = [] {
            std::vector<uint64_t> v;
            if (const char* s = getenv("REGPU_TRACE_INDEX_VS"); s && *s)
                for (char* end = nullptr;; s = end + 1)
                {
                    v.push_back(strtoull(s, &end, 16));
                    if (*end != ',')
                        break;
                }
            return v;
        }();
        // With a filter, the first 100 16-bit draws of any shader are logged too (the DMA size unit),
        // and a listed shader's draw is logged only when its state differs from the last one logged
        // for the same index buffer, so a long run stays readable.
        static std::atomic<int> traced{ 0 }, traced16{ 0 };
        const bool listed = traceIndexVs.empty() || std::find(traceIndexVs.begin(), traceIndexVs.end(), C().vsHash) != traceIndexVs.end();
        bool changed = true;
        if (indexSource == 0 && traceIndexSize > 0 && listed && !traceIndexVs.empty())
        {
            static std::mutex m;
            static std::unordered_map<uint32_t, uint64_t> last;
            uint64_t key = (uint64_t(initiator) << 32 | (dmaSize & 0xFFFFFF)) ^ (uint64_t(gpureg::Get(PA_SU_SC_MODE_CNTL)) * 0x9E3779B97F4A7C15ull);
            key ^= (uint64_t(gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX)) << 17) ^ (uint64_t(gpureg::Get(0x2100)) << 29) ^ (uint64_t(gpureg::Get(0x2101)) << 41);
            key ^= uint64_t(indexEntry ? indexEntry->maxIndex : 0u) * 0xC2B2AE3D27D4EB4Full ^ (directIndex ? 1 : 2);
            std::lock_guard lock(m);
            auto [it, inserted] = last.try_emplace(dmaBase, key);
            changed = inserted || it->second != key;
            it->second = key;
        }
        if (indexSource == 0 && traceIndexSize > 0 &&
            (listed ? changed && traced.fetch_add(1) < traceIndexSize : !((initiator >> 11) & 1) && traced16.fetch_add(1) < 100))
            Log("index draw: vs %016llx prim %u %s count %u dma size %u (endian %u) dma %08X | mode %08X restart %d reset %08X min %08X max %08X offset %u | %s, highest index %u",
                (unsigned long long)C().vsHash, prim, (initiator >> 11) & 1 ? "32-bit" : "16-bit", indexCount, dmaSize & 0xFFFFFF, dmaSize >> 30, dmaBase,
                gpureg::Get(PA_SU_SC_MODE_CNTL), int(restartOn), gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX), gpureg::Get(0x2101), gpureg::Get(0x2100),
                gpureg::Get(VGT_INDX_OFFSET), directIndex ? "direct" : "expanded", indexEntry ? indexEntry->maxIndex : 0u);
    }
    static const std::vector<uint32_t> noList;
    const std::vector<uint32_t>& list = indexEntry ? indexEntry->list : noList;
    if (directIndex ? directVertices == 0 : (!indexEntry || list.empty()))
    {
        TraceSkip("primitive", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    Section(2);
    {
        // Debug bisection: REGPU_SKIP_DRAWS=a-b drops draws a..b-1 of the REGPU_DUMP_FRAME frame.
        static uint32_t skipFrom = 0, skipTo = 0;
        static const bool parsed = [] { if (const char* s = getenv("REGPU_SKIP_DRAWS")) sscanf(s, "%u-%u", &skipFrom, &skipTo); return true; }();
        static const uint64_t envDumpFrame = getenv("REGPU_DUMP_FRAME") ? strtoull(getenv("REGPU_DUMP_FRAME"), nullptr, 10) : 0;
        const uint64_t dumpFrame = g_hotkeyFrame.load(std::memory_order_relaxed) ? g_hotkeyFrame.load(std::memory_order_relaxed) : envDumpFrame; // (F12)
        (void)parsed;
        // REGPU_SKIP_VS=h1,h2,...: drop those vertex shaders in the frame after the dump
        // (written to frame2.png), for an A/B pair under the same camera.
        static const std::vector<uint64_t> skipVs = [] {
            std::vector<uint64_t> v;
            for (const char* s = getenv("REGPU_SKIP_VS"); s && *s;)
            {
                char* end;
                v.push_back(strtoull(s, &end, 16));
                s = *end == ',' ? end + 1 : end;
                if (end == s && *end != ',') break;
            }
            return v;
        }();
        const bool skipThis = g_r.frame == dumpFrame && std::find(skipVs.begin(), skipVs.end(), C().vsHash) != skipVs.end();
        // REGPU_SWEEP="start,step,count" (with REGPU_DUMP_FRAME or F12 as the first frame): frame
        // dumpFrame+i drops draws [start+i*step, start+(i+1)*step) and is written to
        // out/sweep/<i>.png, so one run compares many skips on near-identical frames.
        static uint32_t sweepStart = 0, sweepStep = 0, sweepCount = 0;
        static const bool sweepParsed = [] { if (const char* e = getenv("REGPU_SWEEP")) sscanf(e, "%u,%u,%u", &sweepStart, &sweepStep, &sweepCount); return true; }();
        (void)sweepParsed;
        bool sweepSkip = false;
        if (sweepCount && dumpFrame && g_r.frame + 1 >= dumpFrame && g_r.frame + 1 < dumpFrame + sweepCount)
        {
            const uint32_t i = uint32_t(g_r.frame + 1 - dumpFrame);
            sweepSkip = g_r.drawsThisFrame >= sweepStart + i * sweepStep && g_r.drawsThisFrame < sweepStart + (i + 1) * sweepStep;
            // REGPU_SWEEP_ONLY=1: the opposite: drop every draw in the whole swept range except this frame's chunk.
            if (getenv("REGPU_SWEEP_ONLY"))
                sweepSkip = g_r.drawsThisFrame >= sweepStart && g_r.drawsThisFrame < sweepStart + sweepCount * sweepStep && !sweepSkip;
        }
        if (skipThis || sweepSkip || (skipTo && g_r.frame + 1 == dumpFrame && g_r.drawsThisFrame >= skipFrom && g_r.drawsThisFrame < skipTo))
        {
            g_r.drawsThisFrame++;
            return;
        }
    }
    BeginFrameIfNeeded();
    SelectSurfaces(false);
    if (g_edramTargetsOn)
        UpdateEdramTargets(ps);
    // Temporal upscaling: the main view's draws in the scene also write motion vectors (their
    // shaders' motion variants, once compiled; until then the draw has no motion).
    const bool cullable = (initiator & 0x3F) != 8 && (initiator & 0x3F) != 1;
    const bool wantMotion = g_temporal && g_scenePhase && edramMode == 4 && WidePass();
    if (g_temporal && !g_scenePhase && edramMode == 4 && WidePass())
        g_upscale.postDraws++;
    if (g_temporal && g_scenePhase)
    {
        g_upscale.sceneDraws++;
        g_tstat.draws++;
        if (edramMode == 4)
            g_tstat.edram4++;
        if (WidePass())
            g_tstat.widePass++;
        if (wantMotion)
            g_tstat.wantMotion++;
    }
    const CompiledShader* mvs = wantMotion ? MotionShader(vs) : nullptr;
    const CompiledShader* mps = wantMotion ? MotionShader(ps) : nullptr;
    bool motion = mvs && mps && mvs->IsReady() && mps->IsReady();
    if (wantMotion && !(mvs && mps))
        g_tstat.shaderMissing++;
    else if (wantMotion && !motion)
        g_tstat.shaderNotReady++;
    D3D12_GPU_VIRTUAL_ADDRESS motionPrevious = 0; // last frame's vertex constants of this draw (b2)
    ID3D12PipelineState* pso = motion ? GetPipeline(*mvs, *mps, false, cullable, true) : nullptr;
    if (wantMotion && motion && !pso)
        g_tstat.noPipeline++;
    if (!pso)
    {
        motion = false;
        pso = GetPipeline(vs, ps, edramMode == 5, cullable);
    }
    {
        DrawRecord& d = g_drawRing[g_drawRingNext.fetch_add(1, std::memory_order_relaxed) % kDrawRing];
        d = { g_r.frame, C().vsHash, C().psHash, gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF, gpureg::Get(RB_COLOR_INFO) & 0xFFF, motion };
    }
    if (g_temporal && g_scenePhase)
    {
        MotionDrawStat& s = g_motionDrawStats[C().vsHash];
        s.draws++;
        if (motion)
            s.motion++;
        else
            s.why = edramMode != 4 ? "not a colour pass (edram mode)" : !WidePass() ? "target pitch is not the main view's" : "motion shader not ready";
        s.pitch = gpureg::Get(0x2000 /* RB_SURFACE_INFO */) & 0x3FFF;
        s.colorBase = gpureg::Get(RB_COLOR_INFO) & 0xFFF;
        s.psHash = C().psHash;
        s.vsConstants = vs.info.constantsUsed;
        s.vfetch = uint32_t(vs.info.vertexFetchConstants.size());
    }
    if (!pso)
    {
        // A shader that failed to translate or compile: the draw is lost (logged once per pair).
        if (vs.state.load() == CompiledShader::Failed || ps.state.load() == CompiledShader::Failed)
        {
            static std::mutex failedMutex;
            static std::set<std::pair<uint64_t, uint64_t>> failedSeen;
            std::lock_guard failedLock(failedMutex);
            if (failedSeen.insert({ vs.hash, ps.hash }).second)
                Log("renderer: draw dropped, vs %016llx%s ps %016llx%s", (unsigned long long)vs.hash, vs.state.load() == CompiledShader::Failed ? " (failed)" : "",
                    (unsigned long long)ps.hash, ps.state.load() == CompiledShader::Failed ? " (failed)" : "");
        }
        TraceSkip("pipeline", initiator);
        g_skippedPipeline++;
        return;
    }
    FrameResources& f = Frame();

    Section(3);
    SubSection(0);
    // Index list.
    UINT64 listOffset = 0;
    if (!directIndex)
    {
        if (indexEntry->frame != g_r.frame)
        {
            indexEntry->offset = RingAlloc(list.size() * 4, 16, "index list");
            if (indexEntry->offset != ~0ull)
            {
                memcpy(f.ringCpu + indexEntry->offset, list.data(), list.size() * 4);
                indexEntry->frame = g_r.frame;
            }
        }
        listOffset = indexEntry->offset;
    }
    if (listOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }

    SubSection(1);
    // Vertex constants: c[256], bools, loop constants, fetch table, info.
    struct VSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t l[32];
        uint32_t vf[96][4];
        uint32_t info[4];
        float point[4];
        float vscale[4], voffset[4];
        uint32_t vte[4];
        uint32_t vfl[32][4]; // vertex fetch layout of the bound shader
        float vuser[4][4];   // the game's per-draw constants (Host::onDraw; hlsl.cpp vuser), zero by default
        float vpoly[4];      // x: polygon offset (window-space depth), y: pixel centre shift (hlsl.cpp)
        float vwide[4];      // x: Hor+ factor (see hlsl.cpp)
        float vjitter[4];     // motion-vector variants: this frame's jitter (NDC)
    };
    struct PSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t l[32];
        uint32_t info[4];
        float info2[4];
        uint32_t tinfo[32][4];
        float tbias[32][4];
    };
    const UINT64 vsOffset = RingAlloc(sizeof(VSConstants)), psOffset = RingAlloc(sizeof(PSConstants));
    if (vsOffset == ~0ull || psOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }
    auto* vc = reinterpret_cast<VSConstants*>(f.ringCpu + vsOffset);
    auto* pc = reinterpret_cast<PSConstants*>(f.ringCpu + psOffset);
    {
        // Byte-swapped block copy of both constant banks (1024 dwords each).
        const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        // Only the constants each shader reads (the rest of the block is never read).
        const auto* src = reinterpret_cast<const __m128i*>(gpureg::File() + gpureg::AluConstants * 4);
        auto* dv = reinterpret_cast<__m128i*>(vc->c);
        auto* dp = reinterpret_cast<__m128i*>(pc->c);
        static LiveSwitch allConstants("REGPU_ALL_CONSTANTS"); // A/B
        const uint32_t nv = allConstants ? 256 : std::min(vs.info.constantsUsed, 256u), np = allConstants ? 256 : std::min(ps.info.constantsUsed, 256u);
        for (uint32_t i = 0; i < nv; i++)
            _mm_storeu_si128(dv + i, _mm_shuffle_epi8(_mm_loadu_si128(src + i), swap));
        for (uint32_t i = 0; i < np; i++)
            _mm_storeu_si128(dp + i, _mm_shuffle_epi8(_mm_loadu_si128(src + 256 + i), swap));
        if (motion)
        {
            // This draw's constants for the next frame, and last frame's for its motion. The
            // draw is known by its shaders, vertex and index data and how often it came up.
            uint64_t key = C().vsHash * 0x9E3779B97F4A7C15ull ^ C().psHash ^ (uint64_t(dmaBase) << 20) ^ initiator;
            for (uint32_t k : vs.info.vertexFetchConstants)
            {
                key = (key ^ gpureg::Get(gpureg::FetchConstants + k * 2)) * 0xFF51AFD7ED558CCDull;
                break; // the first vertex buffer is enough to tell draws apart
            }
            key ^= uint64_t(g_upscale.occurrences[key]++) * 0xC2B2AE3D27D4EB4Full;
            // Last frame's constants are still in last frame's ring (it is reused only after
            // kFrames): the draw reads them there (b2).
            auto& entry = g_upscale.constants[key];
            const uint32_t parity = g_upscale.constantsFrame & 1;
            const D3D12_GPU_VIRTUAL_ADDRESS now = f.ring->GetGPUVirtualAddress() + vsOffset;
            const bool known = entry.frame + 1 == g_upscale.constantsFrame && !g_upscale.reset;
            g_tstat.motion++;
            if (known)
            {
                g_tstat.known++;
                MotionDrawStat& s = g_motionDrawStats[C().vsHash];
                s.known++;
                if (const float* previous = g_motionSampleThisFrame ? PreviousRingFloats(entry.address[parity ^ 1]) : nullptr)
                {
                    uint32_t changedRegs = 0, first = 0;
                    for (uint32_t r = 0; r < nv; r++)
                        if (memcmp(&vc->c[r][0], previous + r * 4, 16) != 0)
                        {
                            if (!changedRegs)
                                first = r;
                            changedRegs++;
                        }
                    if (changedRegs)
                    {
                        s.changed++;
                        s.changedRegs += changedRegs;
                        s.firstChanged = first;
                    }
                }
            }
            motionPrevious = known ? entry.address[parity ^ 1] : now;
            entry.address[parity] = now;
            entry.frame = g_upscale.constantsFrame;
            vc->vjitter[0] = g_upscale.jitterNdc[0];
            vc->vjitter[1] = g_upscale.jitterNdc[1];
            vc->vjitter[2] = vc->vjitter[3] = 0;
        }
    }
    for (uint32_t i = 0; i < 8; i++)
        vc->b[i] = pc->b[i] = gpureg::Get(gpureg::BoolConstants + i);
    // Loop constants (count, aL start and step), shared by both stages like the bools.
    for (uint32_t i = 0; i < 32; i++)
        vc->l[i] = pc->l[i] = gpureg::Get(gpureg::LoopConstants + i);
    // Debug: REGPU_FORCE_BOOL=<index>:<0|1> forces one boolean constant for every draw.
    if (static const char* force = getenv("REGPU_FORCE_BOOL"); force)
    {
        static uint32_t index = 0, value = 0;
        static const bool parsed = sscanf(force, "%u:%u", &index, &value) == 2 && index < 256;
        if (parsed)
        {
            const uint32_t bit = 1u << (index & 31);
            vc->b[index >> 5] = pc->b[index >> 5] = value ? (pc->b[index >> 5] | bit) : (pc->b[index >> 5] & ~bit);
        }
    }
    SubSection(2);
    // Only the entries the shader fetches are set, each in full below: zeroing all 96 was 1.5 KB of
    // stores a draw. REGPU_ZERO_ALL_FETCH=1: zero all 96 first, as before (A/B).
    if (static LiveSwitch zeroAllFetch("REGPU_ZERO_ALL_FETCH"); zeroAllFetch)
        memset(vc->vf, 0, sizeof(vc->vf));
    // The highest vertex index the draw can fetch with: auto-indexed draws count up from
    // VGT_INDX_OFFSET; expanded lists know theirs; indices read on the GPU from an index buffer
    // are not known here (~0u).
    const uint32_t drawMaxIndex = indexEntry ? indexEntry->maxIndex
        : indexSource == 2 && indexCount ? indexCount - 1 + gpureg::Get(VGT_INDX_OFFSET) : ~0u;
    for (uint32_t k : vs.info.vertexFetchConstants)
    {
        // Index 31 with select 3 names slot 96, past the 32 x 3 vertex fetch constants. No
        // register backs it; the shader's LoadGuest returns zero for it and the draw goes ahead.
        if (k >= 96)
            continue;
        vc->vf[k][0] = vc->vf[k][1] = vc->vf[k][2] = vc->vf[k][3] = 0;
        const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
        const uint32_t address = d0 & 0xFFFFFFFC, bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
        // The shader's fetches read zero for elements at or past the constant's size, as the Xenos does, for
        // every index source (GPU-read index buffers and computed indices included).
        // REGPU_FETCH_NO_SIZE_BOUND=1: no bound, reads continue into the memory after it (A/B).
        static LiveSwitch noSizeBound("REGPU_FETCH_NO_SIZE_BOUND");
        vc->vf[k][3] = noSizeBound ? ~0u : bytes;
        // The bound above already makes fetches past the constant's size read zero, for every index
        // source, so the copy holds only the constant's own bytes. (Copies used to be zero-padded up
        // to the highest vertex the draw indexes, up to 4 MB each, which only spent upload ring.)
        // REGPU_FETCH_CPU_ZERO_PAD=1: pad the copies as before (A/B).
        static LiveSwitch cpuZeroPad("REGPU_FETCH_CPU_ZERO_PAD");
        uint32_t zeroTo = 0;
        if (cpuZeroPad && drawMaxIndex != ~0u)
            for (const auto& l : vs.info.vertexFetchLayout)
                if ((l[0] & 0x7Fu) == k)
                {
                    const int64_t end = (int64_t(drawMaxIndex) * l[1] + int32_t(l[2]) + 4) * 4;
                    if (end > int64_t(std::max(bytes, zeroTo)) && end <= int64_t(kMaxFetchZeroBytes))
                        zeroTo = uint32_t(end);
                }
        // Large buffers (static geometry) are read in place; small ones may be
        // rewritten before the GPU runs this frame, so they are copied.
        static LiveSwitch noDirect("REGPU_NO_DIRECT_FETCH");
        static const uint32_t directBytes = getenv("REGPU_DIRECT_FETCH_KB") ? uint32_t(atoi(getenv("REGPU_DIRECT_FETCH_KB"))) * 1024u : kDirectFetchBytes;
        if (g_r.physical && !noDirect && !zeroTo && bytes >= directBytes && !DynamicBuffer(address, bytes))
        {
            vc->vf[k][0] = address & 0x1FFFFFFF;
            vc->vf[k][1] = d1 & 3;
            vc->vf[k][2] = 1;
            continue;
        }
        // REGPU_TRACE_MIDSIZE=1: log copied buffers of 16-64 KB that change within a frame (they must stay copied)
        static LiveSwitch traceMid("REGPU_TRACE_MIDSIZE");
        static const uint32_t traceMinBytes = traceMid ? uint32_t(atoi(getenv("REGPU_TRACE_MIDSIZE"))) * 1024u : 0;
        if (traceMid && bytes >= traceMinBytes)
        {
            static std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> seen; // address -> frame, hash
            static uint32_t changed = 0, total = 0;
            const uint64_t h = ContentHash(GuestPtr<uint8_t>(GpuPhysToGuest(address)), bytes);
            auto& e = seen[address];
            total++;
            if (e.first == g_r.frame && e.second != h && changed++ < 40)
                Log("midsize: %08X %u bytes changed within frame %llu (vs %016llx)", address, bytes, (unsigned long long)g_r.frame, (unsigned long long)C().vsHash);
            static uint32_t across = 0;
            if (e.first != g_r.frame && e.first + 4 > g_r.frame && e.second != h && across++ < 60)
                Log("midsize: %08X %u bytes changed since frame %llu (vs %016llx ps %016llx)", address, bytes, (unsigned long long)e.first, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
            e = { g_r.frame, h };
        }
        const UINT64 dataOffset = UploadGuest(GpuPhysToGuest(address), bytes, zeroTo);
        if (dataOffset == ~0ull)
        {
            TraceSkip("ring full", initiator);
            return;
        }
        vc->vf[k][0] = uint32_t(dataOffset);
        vc->vf[k][1] = d1 & 3;
    }
    SubSection(3);
    // Perspective geometry in a wide pass (see ViewExtent): the game's camera already projects
    // at the wider aspect once hooks/camera_aspect.cpp widened it, so only the vertical zoom
    // for REGPU_WIDE_FOV < 100 remains; otherwise the view is narrowed here (Hor+ without the
    // game's culling knowing).
    {
        const float wide = WidePass() ? OutputWide() : 1.0f;
        const float extent = WidePass() ? ViewExtent() : 1.0f;
        const bool gameWidened = g_gameWidescreen.load(std::memory_order_relaxed);
        vc->vwide[0] = gameWidened ? 1.0f : 1.0f / extent;
        vc->vwide[1] = wide / extent;
        static LiveSwitch noSqueeze("REGPU_NO_HUD_SQUEEZE"); // A/B: 2D stretched with the view
        vc->vwide[2] = noSqueeze ? 1.0f : 1.0f / wide;
        vc->vwide[3] = 1; // set after the textures are bound (below)
    }
    if (!vs.info.vertexFetchLayout.empty())
        memcpy(vc->vfl, vs.info.vertexFetchLayout.data(), vs.info.vertexFetchLayout.size() * sizeof(vc->vfl[0]));
    vc->info[0] = uint32_t(listOffset);
    vc->info[1] = (initiator & 0x3F) == 1 ? 2 : 0;
    // Viewport transform in the vertex shader (see hlsl.cpp).
    vc->vscale[0] = RegFloat(PA_CL_VPORT_XSCALE);
    vc->vscale[1] = RegFloat(PA_CL_VPORT_YSCALE);
    vc->vscale[2] = RegFloat(PA_CL_VPORT_ZSCALE);
    vc->vscale[3] = 0;
    vc->voffset[3] = 0;
    // The title clears its (non-MSAA) shadow depth maps through views of the same EDRAM
    // with MSAA on: a pixel of a 4x surface covers 2x2 samples of the linear layout we
    // render in, a pixel of a 2x surface 1x2. Scale those clear rectangles to match.
    static LiveSwitch noMsaaScale("REGPU_NO_MSAA_SCALE");
    if (!noMsaaScale && (initiator & 0x3F) == 8 && (gpureg::Get(RB_COLOR_MASK) & 0xF) == 0 && (gpureg::Get(RB_SURFACE_INFO) & 0x3FFF) < 1000)
    {
        const uint32_t msaa = (gpureg::Get(RB_SURFACE_INFO) >> 16) & 3;
        if (msaa == 2)
            vc->vscale[3] = vc->voffset[3] = 2.0f;
        else if (msaa == 1)
        {
            vc->vscale[3] = 1.0f;
            vc->voffset[3] = 2.0f;
        }
    }
    // The same trick between two MSAA views (a title can clear a 2x composite
    // through a 4x view, 1000 pixels of it plus a 48-pixel 2x strip): texture-less rectangles
    // are scaled from their view's layout to the surface's (see g_nativeMsaa).
    else if (!noMsaaScale)
    {
        const uint32_t surface = gpureg::Get(RB_SURFACE_INFO), msaa = (surface >> 16) & 3;
        if ((initiator & 0x3F) != 8 || ps.info.usedTextureSlots || vs.info.usedTextureSlots)
        {
            SetNativeMsaa(LayoutKey(surface), msaa);
        }
        else if (TallView())
        {
            const uint32_t native = (g_tallInfo >> 16) & 3;
            vc->vscale[3] = float(MsaaAcross(msaa)) / float(MsaaAcross(native));
            vc->voffset[3] = float(MsaaDown(msaa)) / float(MsaaDown(native));
        }
        else if (const int native = ForeignNativeMsaa(); native >= 0)
        {
            vc->vscale[3] = float(MsaaAcross(msaa)) / float(MsaaAcross(uint32_t(native)));
            vc->voffset[3] = float(MsaaDown(msaa)) / float(MsaaDown(uint32_t(native)));
        }
    }
    // Through a foreign EDRAM layout: view pixels to host pixels by the two MSAA layouts (the
    // tile runs' offsets are the viewports, below).
    if (alias.on)
    {
        const uint32_t viewMsaa = (alias.viewSurface >> 16) & 3, hostMsaa = (alias.hostSurface >> 16) & 3;
        vc->vscale[3] = float(MsaaAcross(viewMsaa)) / float(MsaaAcross(hostMsaa));
        vc->voffset[3] = float(MsaaDown(viewMsaa)) / float(MsaaDown(hostMsaa));
    }
    vc->voffset[0] = RegFloat(PA_CL_VPORT_XOFFSET);
    vc->voffset[1] = RegFloat(PA_CL_VPORT_YOFFSET);
    vc->voffset[2] = RegFloat(PA_CL_VPORT_ZOFFSET);
    vc->vte[0] = gpureg::Get(PA_CL_VTE_CNTL);
    vc->vte[1] = vc->vte[2] = vc->vte[3] = 0;
    // Polygon offset (PA_SU_SC_MODE_CNTL bits 11 front, 12 back, 13 points and lines): the constant
    // term added to window-space depth, halved for 20e4 float depth; the slope term is the pipeline's
    // slope-scaled bias (MakePipelineKey). Decals drawn over a surface with a depth test against it
    // need it, or they fight the surface below. REGPU_NO_POLY_OFFSET=1 leaves both out (A/B, live).
    {
        static LiveSwitch noPolyOffset("REGPU_NO_POLY_OFFSET");
        const uint32_t sc = gpureg::Get(PA_SU_SC_MODE_CNTL);
        const bool tri = prim == 4 || prim == 5 || prim == 6 || prim == 8 || prim == 13;
        const bool enabled = tri ? (sc & (3u << 11)) != 0 : ((sc >> 13) & 1) != 0;
        float offset = 0;
        if (enabled && !noPolyOffset)
        {
            offset = (sc >> 11) & 1 ? RegFloat(0x2381) : RegFloat(0x2383); // PA_SU_POLY_OFFSET_FRONT/BACK_OFFSET
            if ((gpureg::Get(0x2002) >> 16) & 1) // RB_DEPTH_INFO: 20e4 float depth
                offset *= 0.5f;
        }
        vc->vpoly[0] = offset;
        // Direct3D 9 pixel centres (PA_SU_VTX_CNTL bit 0 clear) sit half a pixel off the host's.
        // REGPU_HALF_PIXEL_CENTRES=1 shifts every draw by it (test, off at start, live).
        static LiveSwitch halfPixel("REGPU_HALF_PIXEL_CENTRES");
        vc->vpoly[1] = halfPixel && (gpureg::Get(0x2302) & 1) == 0 ? 0.5f : 0.0f;
        vc->vpoly[2] = vc->vpoly[3] = 0;
    }
    // Triangle lists and restart-free strips from an index buffer: a real indexed draw.
    static LiveSwitch noIndexed("REGPU_NO_INDEXED");
    D3D12_INDEX_BUFFER_VIEW indexView{};
    uint32_t indexedCount = 0;
    if (directIndex && !noIndexed && indexSource == 0 && (prim == 4 || prim == 6))
    {
        const bool is32 = (initiator >> 11) & 1;
        const uint32_t available = dmaSize & 0xFFFFFF;
        const uint32_t count = std::min(indexCount, available);
        indexedCount = prim == 4 ? count / 3 * 3 : count;
        if (indexedCount < 3 || !GetIndexBuffer(dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u), count, is32, dmaSize >> 30, indexView))
            indexedCount = 0;
    }
    if (indexedCount)
    {
        vc->vte[1] = 1u | 8u; // the vertex ID is the index
        vc->vte[3] = gpureg::Get(VGT_INDX_OFFSET);
    }
    else if (directIndex)
    {
        const bool is32 = (initiator >> 11) & 1;
        vc->vte[1] = 1u | (prim == 6 ? 2u : 0u) | (prim == 13 ? 64u : 0u) | (is32 ? 4u : 0u) | (indexSource == 2 ? 8u : 0u) | ((dmaSize >> 30) << 4);
        vc->vte[2] = dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u);
        vc->vte[3] = gpureg::Get(VGT_INDX_OFFSET);
    }
    // The window scissor marks a tall pass (PassHeight); this draw may be its first, so check before the
    // target size is taken (else the vertices are placed for 720 rows in a taller viewport: a one-off 1024-row
    // downsample came out stretched 1.42x).
    (void)WindowScissor();
    vc->info[2] = PassWidth();
    vc->info[3] = PassHeight();
    // Point sizes are 12.4 fixed-point radii.
    const uint32_t pointSize = gpureg::Get(PA_SU_POINT_SIZE), pointMinMax = gpureg::Get(PA_SU_POINT_MINMAX);
    vc->point[0] = float(pointSize >> 16) / 8.0f;
    vc->point[1] = float(pointSize & 0xFFFF) / 8.0f;
    vc->point[2] = float(pointMinMax & 0xFFFF) / 8.0f;
    vc->point[3] = float(pointMinMax >> 16) / 8.0f;
    // The game's per-draw constants for shaders it rewrote (regpu.h Host::onDraw).
    memset(vc->vuser, 0, sizeof(vc->vuser));
    if (const auto onDraw = regpu::GetHost().onDraw; onDraw && (vs.tag || ps.tag))
        onDraw(C().vsHash, C().psHash, vs.tag, ps.tag, vc->c, vc->vuser);
    pc->info[1] = (gpureg::Get(SQ_PROGRAM_CNTL) >> 18) & 1 ? (gpureg::Get(SQ_CONTEXT_MISC) >> 8) & 0xFF : 0xFF;

    const uint32_t colorControl = gpureg::Get(RB_COLORCONTROL);
    // Depth-only passes (EDRAM mode 5) don't run the colour export, so there is no alpha test
    // (it tests the exported colour's alpha): a title can reset depth between UI layers
    // with a rectangle whose colour has alpha 0 while the alpha test (GREATER 0) is still on
    // from the layer before; testing it killed the reset, and every later layer failed its
    // depth test. REGPU_DEPTH_ALPHATEST=1 tests alpha in mode 5 too (the old behaviour, A/B).
    static LiveSwitch depthAlphaTest("REGPU_DEPTH_ALPHATEST");
    pc->info[0] = (colorControl >> 3) & 1 && (edramMode != 5 || depthAlphaTest) ? (colorControl & 7) : 7;
    pc->info2[0] = RegFloat(RB_ALPHA_REF);
    pc->info2[1] = 1.0f / ScaleX();
    pc->info2[2] = 1.0f / ScaleY();
    // Our colour target is half float: a 7e3 surface (formats 3 and 12, the title's HDR scene)
    // saturates at 31.875, so its output is clamped to that, as the hardware does (the bloom
    // reads those values). A fixed-point surface (8888, 8888_GAMMA, 2_10_10_10 and its
    // 10_10_10_10 view: formats 0, 1, 2, 10) holds [0, 1]: what the shader outputs above 1 is
    // stored as 1 (a scene drawn into a 2_10_10_10 target kept its unbounded lights, and the
    // highlights bloomed into white blobs). REGPU_NO_UNORM_CLAMP leaves them unbounded.
    const uint32_t colorFormat = (gpureg::Get(RB_COLOR_INFO) >> 16) & 0xF;
    static LiveSwitch noUnormClamp("REGPU_NO_UNORM_CLAMP");
    const bool unormTarget = colorFormat == 0 || colorFormat == 1 || colorFormat == 2 || colorFormat == 10;
    pc->info2[3] = colorFormat == 3 || colorFormat == 12 ? 31.875f : unormTarget && !noUnormClamp ? 1.0f : 0.0f;
    // Colour exponent bias (RB_COLOR_INFO bits 20-25, signed): the hardware scales the colour
    // output by 2^bias before it is stored and blended, in every format. A
    // title draws a 16_16 shadow map with bias 5, filling the target's -32..32 fixed range, and
    // reads it back as integers through a resolve (copy bias 10) and a fetch exponent of -15;
    // unscaled, every value arrived 32x too small and the whole shadowed area went dark.
    // The 16_16 and 16_16_16_16 targets (formats 4, 5) hold -32..32. REGPU_NO_COLOR_EXP_BIAS: unscaled.
    static LiveSwitch noColorExpBias("REGPU_NO_COLOR_EXP_BIAS");
    const int colorExpBias = noColorExpBias ? 0 : int32_t(gpureg::Get(RB_COLOR_INFO) << 6) >> 26;
    const float colorScale = std::ldexp(1.0f, colorExpBias);
    memcpy(&pc->info[2], &colorScale, sizeof(colorScale));
    pc->info[3] = colorFormat == 4 || colorFormat == 5 ? 1 : 0;

    SubSection(4);
    Section(4);
    // Textures are bindless: each one has a permanent descriptor in the heap's
    // bindless region, and the draw passes its index in the constants.
    memset(pc->tinfo, 0, sizeof(pc->tinfo));
    memset(pc->tbias, 0, sizeof(pc->tbias));
    const uint32_t usedSlots = ps.info.usedTextureSlots | vs.info.usedTextureSlots;
    if (g_bindingsFrame != g_r.frame || g_bindingsEpoch != g_resolveEpoch)
    {
        if (!g_bindings)
            g_bindings = std::make_unique<SlotBinding[]>(kBindingSlots); // (entries carry their frame and epoch)
        g_bindingsFrame = g_r.frame;
        g_bindingsEpoch = g_resolveEpoch;
    }
    for (uint32_t m = usedSlots; m; m &= m - 1)
    {
        const uint32_t slot = uint32_t(__builtin_ctz(m));
        const TextureFetch fetch = ReadTextureFetch(slot);
        uint64_t bindingKey = 0xCBF29CE484222325ull;
        for (uint32_t d : fetch.dwords)
            bindingKey = (bindingKey ^ d) * 0x100000001B3ull;
        SlotBinding& memo = g_bindings[(bindingKey ^ bindingKey >> 29) & (kBindingSlots - 1)];
        static const bool noSlotMemo = getenv("REGPU_NO_SLOT_MEMO") != nullptr || getenv("REGPU_DUMP_PS_TEXTURES") != nullptr;
        if (!noSlotMemo && memo.frame == g_r.frame && memo.epoch == g_resolveEpoch && memcmp(memo.fetch, fetch.dwords, sizeof(memo.fetch)) == 0)
        {
            memcpy(pc->tinfo[slot], memo.tinfo, sizeof(memo.tinfo));
            memcpy(pc->tbias[slot], memo.tbias, sizeof(memo.tbias));
            g_drawSamplesResolve |= memo.samplesResolve;
            continue;
        }
        const bool samplesBefore = g_drawSamplesResolve;
        g_drawSamplesResolve = false;
        const bool cube = fetch.Dimension() == 3;
        const bool volume = fetch.Dimension() == 2;
        static const uint64_t dumpPs = getenv("REGPU_DUMP_PS_TEXTURES") ? strtoull(getenv("REGPU_DUMP_PS_TEXTURES"), nullptr, 16) : 0;
        if (dumpPs && C().psHash == dumpPs && !cube)
        {
            static std::set<uint32_t> dumped;
            std::vector<uint8_t> rgba;
            if (dumped.insert(fetch.Address()).second && DecodeTexture(fetch, rgba))
            {
                char name[80];
                std::filesystem::create_directories("out/tex");
                snprintf(name, sizeof(name), "out/tex/t%u_%08X_%s_%ux%u_sw%03X.png", slot, fetch.Address(), TextureFormatName(fetch.Format()), fetch.Width(),
                    fetch.Height(), fetch.Swizzle());
                WritePng(name, fetch.Width(), fetch.Height(), rgba);
            }
        }
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool native = false;
        g_textureFailure[0] = 0;
        ID3D12Resource* texture = cube ? GetCubeTexture(fetch, format) : volume ? GetVolumeTexture(fetch, format) : GetTexture(fetch, format, native);
        // Debug: REGPU_NO_LIVE_CUBE=1 binds the fallback for cubes the title rendered (live reflections).
        if (static LiveSwitch noLiveCube("REGPU_NO_LIVE_CUBE"); noLiveCube && cube && ResolvedFor(fetch))
            texture = nullptr;
        const uint32_t index = texture ? BindlessIndex(texture, format, cube, volume) : (volume ? 2u : cube ? 1u : 0u);
        if (RendererTracing())
            CaptureTexture(fetch, slot, cube, texture != nullptr, index, native, g_textureFailure);
        // w: dimension (1 2D, 2 3D, 3 cube); bit 31 + bits 8-19: the fetch swizzle, applied in the
        // shader for textures that live on the GPU (resolves). Decoded ones are swizzled already.
        static LiveSwitch noGpuSwizzle("REGPU_NO_GPU_SWIZZLE");
        const ResolvedTexture* resolvedPtr = ResolvedFor(fetch);
        TraceResolvedBind(fetch, slot, resolvedPtr);
        const bool onGpu = (!noGpuSwizzle && fetch.Type() == 2 && resolvedPtr != nullptr) || native;
        // Our resolved textures keep red and blue in place; when the title
        // resolved with the swap bit, its fetch swizzle expects them exchanged.
        uint32_t swizzle = fetch.Swizzle();
        if (onGpu && resolvedPtr && resolvedPtr->swapped)
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = (swizzle >> (3 * c)) & 7;
                if (sel == 0 || sel == 2)
                    swizzle ^= (sel ^ (2 - sel)) << (3 * c);
            }
        pc->tinfo[slot][3] = (cube ? 3 : volume ? 2 : 1) | (onGpu ? (1u << 31) | (swizzle << 8) : 0);
        // Clamp X and Y (dword 0 bits 10-12, 13-15): 0 wrap, 1 mirrored repeat, 2 and up clamp, the odd
        // ones (3, 5, 7) mirrored once first. Filter: dword 3 bits 19-20 (mag): 0 point. With only one
        // axis clamped (kerbs: clamped across, repeating along) the wrapping sampler is used and the
        // shader clamps that axis (bits 18, 19). The shader mirrors first (tinfo.z bits 20, 21 repeat;
        // 22, 23 once): a mode taken as a plain clamp smeared the edge texels across the mirrored half
        // of a symmetric image. A mirrored axis is still clamped after the fold: wrapped, the filter
        // blends the opposite edge in. REGPU_NO_TEXTURE_MIRROR=1: mirrored modes clamped only, as before.
        const uint32_t clampX = (fetch.dwords[0] >> 10) & 7, clampY = (fetch.dwords[0] >> 13) & 7;
        static LiveSwitch oldClamp("REGPU_OLD_CLAMP"); // A/B: X's mode for both axes (before the kerb fix)
        const bool clamp = oldClamp ? clampX != 0 : clampX != 0 && clampY != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc->tinfo[slot][0] = (point ? (clamp ? 2 : 3) : (clamp ? 1 : 0)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        if (!clamp && !oldClamp)
            pc->tinfo[slot][0] |= (clampX ? 1u << 18 : 0u) | (clampY ? 1u << 19 : 0u);
        // Clamp to border on both axes (modes 6, 7): the shader returns the border colour
        // outside the image (dword 5 bits 0-1: 1 white, else black).
        if (clampX >= 6 && clampY >= 6)
            pc->tinfo[slot][0] |= (1u << 16) | ((fetch.dwords[5] & 3) == 1 ? 1u << 17 : 0u);
        // Sign modes (dword 0 bits 2-9, per texture component: gamma, biased; see TSign), in
        // bits 20-27. The shader applies them before the fetch swizzle, so a texture swizzled
        // on the CPU gets them moved to where its components went. REGPU_NO_TEXTURE_SIGN=1
        // ignores them (A/B: textures read in gamma space, as before).
        static LiveSwitch noTextureSign("REGPU_NO_TEXTURE_SIGN");
        if (const uint32_t signs = (fetch.dwords[0] >> 2) & 0xFF; signs && !noTextureSign)
        {
            uint32_t applied = signs;
            if (!onGpu)
            {
                applied = 0;
                for (uint32_t c = 0; c < 4; c++)
                    if (const uint32_t sel = (fetch.Swizzle() >> (3 * c)) & 7; sel < 4)
                        applied |= ((signs >> (2 * sel)) & 3) << (2 * c);
            }
            pc->tinfo[slot][0] |= applied << 20;
        }
        // Below output resolution the upscaler wants sharper mips (log2 of the scale, e.g. -0.58 at 67%).
        static LiveSwitch noLodBias("REGPU_NO_LOD_BIAS");
        pc->tbias[slot][0] = fetch.LodBias() + (g_sceneScale < 1.0f && !noLodBias ? std::log2(g_sceneScale) : 0.0f);
        memcpy(&pc->tbias[slot][1], &index, 4);
        // A resolved texture is as wide as the pitch it was resolved with; the fetch
        // constant may declare a narrower image inside it (e.g. 163 of 192 columns):
        // the shader scales UVs so 0..1 spans the declared image, not the padding.
        pc->tbias[slot][2] = pc->tbias[slot][3] = 1.0f;
        if (onGpu && resolvedPtr && !cube && resolvedPtr->width && resolvedPtr->height && resolvedPtr->resource)
        {
            pc->tbias[slot][2] = std::min(1.0f, float(fetch.Width()) / float(resolvedPtr->width));
            pc->tbias[slot][3] = std::min(1.0f, float(fetch.Height()) / float(resolvedPtr->height));
        }
        pc->tinfo[slot][1] = fetch.Width() | (volume ? fetch.Depth() << 16 : 0); // 3D: the depth in bits 16-31
        pc->tinfo[slot][2] = fetch.Height();
        static LiveSwitch noMirror("REGPU_NO_TEXTURE_MIRROR");
        if (!oldClamp && !noMirror)
            pc->tinfo[slot][2] |= (clampX == 1 ? 1u << 20 : 0u) | (clampY == 1 ? 1u << 21 : 0u) | (clampX >= 3 && (clampX & 1) ? 1u << 22 : 0u) |
                (clampY >= 3 && (clampY & 1) ? 1u << 23 : 0u);
        {
            // Soft-shadow cascade tests. A depth copy read as an 8-bit colour texture gets the
            // console's packed depth bytes instead of float depth (REGPU_DEPTH_AS_8888, with variants for the
            // byte layout). Slots in REGPU_CASCADE_SLOTS can read one colour channel as lit.
            static LiveSwitch depthAs8888("REGPU_DEPTH_AS_8888"), depthRgb("REGPU_DEPTH_AS_8888_RGB"), depthRev("REGPU_DEPTH_AS_8888_REVERSED"),
                depthFloat("REGPU_DEPTH_AS_8888_FLOAT20E4"), depthStencil("REGPU_DEPTH_AS_8888_STENCIL0"), litR("REGPU_CASCADE_R_LIT"), litG("REGPU_CASCADE_G_LIT"), litB("REGPU_CASCADE_B_LIT");
            static const uint32_t cascadeSlots = [] {
                uint32_t m = 0;
                for (const char* p = getenv("REGPU_CASCADE_SLOTS"); p && *p;)
                {
                    char* end = nullptr;
                    const unsigned long v = strtoul(p, &end, 10);
                    if (end == p)
                        break;
                    if (v < 32)
                        m |= 1u << v;
                    p = *end == ',' ? end + 1 : end;
                }
                return m;
            }();
            if (depthAs8888 && onGpu && resolvedPtr && resolvedPtr->format == DXGI_FORMAT_R32_FLOAT && fetch.Format() == 6)
                pc->tinfo[slot][0] |= (1u << 28) | (depthRgb ? 1u << 29 : 0u) | (depthRev ? 1u << 30 : 0u) | (depthFloat ? 1u << 31 : 0u);
            if (depthAs8888 && depthStencil && (pc->tinfo[slot][0] >> 28 & 1))
                pc->tinfo[slot][2] |= 1u << 19;
            if ((cascadeSlots >> slot) & 1)
                pc->tinfo[slot][2] |= ((litR ? 1u : 0u) | (litG ? 2u : 0u) | (litB ? 4u : 0u)) << 16;
        }
        memcpy(memo.fetch, fetch.dwords, sizeof(memo.fetch));
        memo.frame = g_r.frame;
        memo.epoch = g_resolveEpoch;
        memcpy(memo.tinfo, pc->tinfo[slot], sizeof(memo.tinfo));
        memcpy(memo.tbias, pc->tbias[slot], sizeof(memo.tbias));
        memo.samplesResolve = g_drawSamplesResolve;
        g_drawSamplesResolve |= samplesBefore;
    }
    // HUD and menus sample ordinary textures; full-screen layers sample resolved targets
    // and must keep spanning the width (squeezing them warped the image outside 16:9).
    vc->vwide[3] = g_drawSamplesResolve ? 0.0f : 1.0f;

    Section(5);
    // Viewport. The viewport transform happens in the vertex shader; D3D maps 1:1.
    // Tiled passes shift each draw into EDRAM by the window offset. We render in screen
    // space, so tiled draws stay unshifted. The title's rectangle draws (depth resets)
    // reset the window offset to zero and use tile-relative coordinates instead:
    // those land in the current tile, so shift them by the tile's origin.
    D3D12_RECT scissor = WindowScissor();
    float shiftX = 0, shiftY = 0;
    {
        const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
        const bool vertexOffset = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1;
        const int32_t wx = vertexOffset && TileColumns() ? int32_t(offsetReg << 17) >> 17 : 0;
        const int32_t wy = vertexOffset ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
        const bool full = scissor.left <= 0 && scissor.top <= 0 && scissor.right >= LONG(PassWidth()) && scissor.bottom >= LONG(PassHeight());
        if (!full)
        {
            g_tile.originX = -wx;
            g_tile.originY = -wy;
            g_tile.rect = scissor;
            g_tile.surface = gpureg::Get(RB_SURFACE_INFO);
        }
        // Only draws on the tiled pass's own surface are tile-relative: a photo or replay mode can draw its
        // depth-of-field composite once, full-screen, into another view of the same EDRAM after
        // the 4x scene's three tiles, and clipped to the last tile it covered only the bottom
        // third (the rest of the picture was black).
        // Another MSAA view of the tiled surface itself (stencil clears of a composite) is still
        // tile-relative.
        else if ((g_tile.originX != 0 || g_tile.originY != 0) && (g_tile.surface == ~0u || g_tile.surface == gpureg::Get(RB_SURFACE_INFO) || (!g_noForeignTile && ForeignNativeMsaa() >= 0)))
        {
            shiftX = float(g_tile.originX + wx);
            shiftY = float(g_tile.originY + wy);
            scissor.left = std::max(scissor.left, g_tile.rect.left);
            scissor.top = std::max(scissor.top, g_tile.rect.top);
            scissor.right = std::min(scissor.right, g_tile.rect.right);
            scissor.bottom = std::min(scissor.bottom, g_tile.rect.bottom);
        }
    }
    const D3D12_VIEWPORT vp{ shiftX * ScaleX(), shiftY * ScaleY(), float(PassWidth()) * ScaleX(), float(PassHeight()) * ScaleY(), 0, 1 };
    if (alias.on && (shiftX != 0 || shiftY != 0))
    {
        dropForeignDepth("tile-relative (predicated tiling)");
        return;
    }
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
    {
        TraceSkip("empty scissor", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    D3D12_RECT hostScissor = ToHost(scissor);
    // A scissor narrower than the screen in a wide pass clips 2D content (a minimap),
    // which the vertex shader keeps in the centred 16:9 area: map it the same way. Not for
    // screen layers (post-processing done in halves would leave a seam).
    if (WidePass() && ScaleWide() != ScaleY() && !g_drawSamplesResolve && (scissor.left > 0 || scissor.right < LONG(kWidth)))
    {
        const double centre = kWidth * ScaleWide() / 2.0, s = ScaleY();
        hostScissor.left = LONG(std::lround(centre + (scissor.left - double(kWidth) / 2) * s));
        hostScissor.right = LONG(std::lround(centre + (scissor.right - double(kWidth) / 2) * s));
    }
    DrawState& ds = g_drawState;
    if (memcmp(&ds.viewport, &vp, sizeof(vp)) != 0)
    {
        ds.viewport = vp;
        C().list->RSSetViewports(1, &vp);
    }
    if (memcmp(&ds.scissor, &hostScissor, sizeof(hostScissor)) != 0)
    {
        ds.scissor = hostScissor;
        C().list->RSSetScissorRects(1, &hostScissor);
    }
    if (const uint32_t ref = gpureg::Get(RB_STENCILREFMASK) & 0xFF; ref != ds.stencilRef)
    {
        ds.stencilRef = ref;
        C().list->OMSetStencilRef(ref);
    }
    {
        // RB_BLEND_RED..ALPHA: the constant colour for blend factors 12-15.
        const float factor[4] = { RegFloat(0x2105), RegFloat(0x2106), RegFloat(0x2107), RegFloat(0x2108) };
        if (memcmp(ds.blendFactor, factor, sizeof(factor)) != 0)
        {
            memcpy(ds.blendFactor, factor, sizeof(factor));
            C().list->OMSetBlendFactor(factor);
        }
    }
    if (motion != ds.motionRoot)
    {
        // Motion pipelines have their own root signature; switching clears the root arguments.
        ds.motionRoot = motion;
        C().list->SetGraphicsRootSignature(motion ? g_r.motionRootSignature.Get() : g_r.rootSignature.Get());
        ds.table2 = ds.table3 = D3D12_GPU_DESCRIPTOR_HANDLE{};
    }
    if (motion != ds.motionTargets)
    {
        ds.motionTargets = motion;
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = { C().rtv, g_upscale.motionRtv };
        C().list->OMSetRenderTargets(motion ? 2 : 1, targets, FALSE, &C().dsv);
    }
    if (pso != ds.pso)
    {
        ds.pso = pso;
        const auto t0 = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        C().list->SetPipelineState(pso);
        if (g_perfTiming)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 1.0)
                Log("perf: SetPipelineState took %.1f ms (vs %016llx ps %016llx)", ms, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
        }
    }
    // REGPU_REPEAT_STATS=1 (measurement, live): how much of each draw's setup repeats, from the previous draw
    // (what skipping unchanged stages would save) and from the same draw a frame before (what a whole-draw
    // cache would save). Per second: "repeat: ..." with the share of draws whose part matched.
    if (static LiveSwitch repeatStats("REGPU_REPEAT_STATS"); repeatStats)
    {
        struct Parts
        {
            uint64_t pipeline = 0, textures = 0, vertices = 0, constants = 0;
        };
        Parts p;
        p.pipeline = (psAlias.realVs * 0x9E3779B97F4A7C15ull) ^ psAlias.real ^ (uint64_t(gpureg::Get(RB_BLENDCONTROL0)) << 32 | gpureg::Get(RB_DEPTHCONTROL)) * 31 ^
            (uint64_t(gpureg::Get(RB_COLOR_MASK) | (gpureg::Get(PA_SU_SC_MODE_CNTL) & 0xFFFF) << 8) << 32 | gpureg::Get(RB_SURFACE_INFO)) * 131 ^
            (uint64_t(gpureg::Get(RB_COLOR_INFO)) << 32 | gpureg::Get(RB_DEPTH_INFO)) * 1031;
        const uint32_t slots = ps.info.usedTextureSlots | vs.info.usedTextureSlots;
        for (uint32_t slot = 0; slot < 32; slot++)
            if ((slots >> slot) & 1)
            {
                const TextureFetch t = ReadTextureFetch(slot);
                p.textures = p.textures * 0x100000001B3ull ^ ContentHash(reinterpret_cast<const uint8_t*>(t.dwords), sizeof(t.dwords), 4) ^ slot;
            }
        for (uint32_t k : vs.info.vertexFetchConstants)
            p.vertices = p.vertices * 0x100000001B3ull ^ gpureg::Get(gpureg::FetchConstants + k * 2) ^ uint64_t(gpureg::Get(gpureg::FetchConstants + k * 2 + 1)) << 32;
        p.constants = ContentHash(reinterpret_cast<const uint8_t*>(vc), sizeof(*vc), 4) ^ ContentHash(reinterpret_cast<const uint8_t*>(pc), sizeof(*pc), 4) * 3;
        struct Stats
        {
            std::mutex m;
            Parts prev;
            std::vector<Parts> last, now;
            uint64_t frame = ~0ull;
            uint64_t draws = 0, prevPipe = 0, prevTex = 0, prevVert = 0, prevAll = 0, framePipe = 0, frameTex = 0, frameVert = 0, frameConst = 0, frameAll = 0;
            std::chrono::steady_clock::time_point logged = std::chrono::steady_clock::now();
        };
        static Stats s;
        std::lock_guard lock(s.m);
        if (s.frame != g_r.frame)
        {
            s.frame = g_r.frame;
            s.last.swap(s.now);
            s.now.clear();
            s.prev = {};
        }
        const size_t index = s.now.size();
        s.now.push_back(p);
        s.draws++;
        s.prevPipe += p.pipeline == s.prev.pipeline;
        s.prevTex += p.textures == s.prev.textures;
        s.prevVert += p.vertices == s.prev.vertices;
        s.prevAll += p.pipeline == s.prev.pipeline && p.textures == s.prev.textures && p.vertices == s.prev.vertices;
        s.prev = p;
        if (index < s.last.size())
        {
            const Parts& q = s.last[index];
            s.framePipe += p.pipeline == q.pipeline;
            s.frameTex += p.textures == q.textures;
            s.frameVert += p.vertices == q.vertices;
            s.frameConst += p.constants == q.constants;
            s.frameAll += p.pipeline == q.pipeline && p.textures == q.textures && p.vertices == q.vertices && p.constants == q.constants;
        }
        if (const auto t = std::chrono::steady_clock::now(); t - s.logged > std::chrono::seconds(1) && s.draws)
        {
            const double n = double(s.draws) / 100.0;
            Log("repeat: %llu draws | vs previous draw: pipeline %.0f%% textures %.0f%% vertices %.0f%% all three %.0f%% | vs same draw last frame: "
                "pipeline %.0f%% textures %.0f%% vertices %.0f%% constants %.0f%% everything %.0f%%",
                (unsigned long long)s.draws, s.prevPipe / n, s.prevTex / n, s.prevVert / n, s.prevAll / n, s.framePipe / n, s.frameTex / n, s.frameVert / n,
                s.frameConst / n, s.frameAll / n);
            s.logged = t;
            s.draws = s.prevPipe = s.prevTex = s.prevVert = s.prevAll = s.framePipe = s.frameTex = s.frameVert = s.frameConst = s.frameAll = 0;
        }
    }
    C().list->SetGraphicsRootConstantBufferView(0, f.ring->GetGPUVirtualAddress() + vsOffset);
    C().list->SetGraphicsRootConstantBufferView(1, f.ring->GetGPUVirtualAddress() + psOffset);
    if (motion)
        C().list->SetGraphicsRootConstantBufferView(4, motionPrevious);
    if (const D3D12_GPU_DESCRIPTOR_HANDLE t2 = Gpu(g_r.srvHeap.Get(), f.descriptorBase, g_r.srvStride); t2.ptr != ds.table2.ptr)
    {
        ds.table2 = t2;
        C().list->SetGraphicsRootDescriptorTable(2, t2);
    }
    if (const D3D12_GPU_DESCRIPTOR_HANDLE t3 = Gpu(g_r.srvHeap.Get(), kBindlessBase, g_r.srvStride); t3.ptr != ds.table3.ptr)
    {
        ds.table3 = t3;
        C().list->SetGraphicsRootDescriptorTable(3, t3);
    }
    // REGPU_CAPTURE_COMPOSITE=<n>: trace (as F12) the n-th frame that draws into the 2048-wide
    // surface (pitch field 0x820: a 2048-wide composite), from its first such draw on.
    if (static const int compositeN = getenv("REGPU_CAPTURE_COMPOSITE") ? atoi(getenv("REGPU_CAPTURE_COMPOSITE")) : 0;
        compositeN && (gpureg::Get(RB_SURFACE_INFO) & 0x3FFF) == 0x820)
    {
        static uint64_t lastFrame = ~0ull;
        static int seen = 0;
        if (g_r.frame != lastFrame && ++seen == compositeN)
        {
            g_hotkeyFrame = g_r.frame + 1;
            Log("capture: composite pass %d, tracing frame %llu", seen, (unsigned long long)(g_r.frame + 1));
        }
        lastFrame = g_r.frame;
    }
    const uint64_t traceFrame = TraceFrame();
    if (traceFrame && g_r.frame + 1 == traceFrame)
    {
        std::string tex;
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            if (((ps.info.usedTextureSlots >> slot) & 1) == 0)
                continue;
            const TextureFetch t = ReadTextureFetch(slot);
            char e[160];
            snprintf(e, sizeof(e), " t%u=%s %ux%u@%08X sw%03X sg%02X d%u [%08X %08X %08X %08X %08X %08X]", slot, TextureFormatName(t.Format()), t.Width(), t.Height(), t.Address(), t.Swizzle(),
                (t.dwords[0] >> 2) & 0xFF, t.Dimension(), t.dwords[0], t.dwords[1], t.dwords[2], t.dwords[3], t.dwords[4], t.dwords[5]);
            tex += e;
        }
        Log("    indices (initiator %08X dma %08X offset %u): %u %u %u %u %u %u ... %u", initiator, dmaBase, gpureg::Get(VGT_INDX_OFFSET),
            list.size() > 0 ? list[0] : 0, list.size() > 1 ? list[1] : 0, list.size() > 2 ? list[2] : 0, list.size() > 3 ? list[3] : 0,
            list.size() > 4 ? list[4] : 0, list.size() > 5 ? list[5] : 0, list.empty() ? 0 : list.back());
        Log("draw #%u vs %016llx ps %016llx prim %u n %zu blend %08X mask %X scissor %ld,%ld-%ld,%ld vte %03X wide %d layer %d pitch %u%s", g_r.drawsThisFrame.load(),
            (unsigned long long)C().vsHash, (unsigned long long)C().psHash, initiator & 0x3F, list.size(), gpureg::Get(RB_BLENDCONTROL0),
            gpureg::Get(RB_COLOR_MASK) & 15, scissor.left, scissor.top, scissor.right, scissor.bottom, gpureg::Get(PA_CL_VTE_CNTL) & 0x3FF,
            int(WidePass()), int(g_drawSamplesResolve), gpureg::Get(RB_SURFACE_INFO) & 0x3FFF, tex.c_str());
        uint32_t bools[8];
        for (int i = 0; i < 8; i++)
            bools[i] = gpureg::Get(gpureg::BoolConstants + i);
        Log("    bools %08X %08X %08X %08X %08X %08X %08X %08X", bools[0], bools[1], bools[2], bools[3], bools[4], bools[5], bools[6], bools[7]);
        if (getenv("REGPU_TRACE_VC"))
            for (uint32_t i = 0; i < 256; i++)
            {
                const float* c = vc->c[i];
                if (c[0] != 0 || c[1] != 0 || c[2] != 0 || c[3] != 0)
                    Log("    vc%u = %g %g %g %g", i, c[0], c[1], c[2], c[3]);
            }
        for (uint32_t k : vs.info.vertexFetchConstants)
        {
            const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
            const uint32_t* v = GuestPtr<uint32_t>(GpuPhysToGuest(d0 & 0xFFFFFFFC));
            Log("    vf%u: %08X size %u endian %u: %08X %08X %08X %08X %08X %08X %08X %08X", k, d0 & 0xFFFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4, d1 & 3,
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            if ((initiator & 0x3F) == 8)
            {
                // Rectangle lists: print the corners as floats (big-endian vertex data).
                std::string corners;
                for (uint32_t i = 0; i < 3 && (i * 7 + 1) * 4 < ((d1 >> 2) & 0xFFFFFF) * 4; i++)
                {
                    uint32_t xb = __builtin_bswap32(v[i * 7]), yb = __builtin_bswap32(v[i * 7 + 1]);
                    float x, y;
                    memcpy(&x, &xb, 4);
                    memcpy(&y, &yb, 4);
                    char t[48];
                    snprintf(t, sizeof(t), " (%g,%g)", x, y);
                    corners += t;
                }
                Log("    rect corners:%s", corners.c_str());
            }
            std::string nz;
            for (uint32_t i = 0; i * 8 + 5 < ((d1 >> 2) & 0xFFFFFF) && i < 24; i++)
            {
                char t[16];
                snprintf(t, sizeof(t), " %08X", v[i * 8 + 5]);
                nz += t;
            }
            Log("    vf%u normal z per vertex:%s", k, nz.c_str());
            // REGPU_TRACE_VSTAT=<dword>,<stride>: min/max/mean of that big-endian float over the
            // vertices this draw indexes (e.g. 3,15: one vertex stream of a model).
            if (const char* e = getenv("REGPU_TRACE_VSTAT"))
            {
                uint32_t off = 0, stride = 0;
                const uint32_t dwordsInBuffer = (d1 >> 2) & 0xFFFFFF;
                if (sscanf(e, "%u,%u", &off, &stride) == 2 && stride && off < stride && !list.empty())
                {
                    double lo = 1e30, hi = -1e30, sum = 0;
                    uint32_t n = 0, above = 0;
                    for (uint32_t index : list)
                    {
                        const uint64_t at = uint64_t(index) * stride + off;
                        if (at >= dwordsInBuffer)
                            continue;
                        const uint32_t b = __builtin_bswap32(v[at]);
                        float f;
                        memcpy(&f, &b, 4);
                        lo = std::min<double>(lo, f);
                        hi = std::max<double>(hi, f);
                        sum += f;
                        n++;
                        above += f > 0.5f;
                    }
                    if (n)
                        Log("    vf%u dword %u of %u: min %g max %g mean %g, %u of %u above 0.5", k, off, stride, lo, hi, sum / n, above, n);
                }
            }
        }
        Log("    scissors: generic %08X-%08X screen %08X-%08X window %08X-%08X offset %08X vport %g,%g +%g,%g", gpureg::Get(0x2090), gpureg::Get(0x2091),
            gpureg::Get(0x200E), gpureg::Get(0x200F), gpureg::Get(PA_SC_WINDOW_SCISSOR_TL), gpureg::Get(PA_SC_WINDOW_SCISSOR_BR), gpureg::Get(PA_SC_WINDOW_OFFSET),
            RegFloat(PA_CL_VPORT_XOFFSET), RegFloat(PA_CL_VPORT_YOFFSET), RegFloat(PA_CL_VPORT_XSCALE), RegFloat(PA_CL_VPORT_YSCALE));
        Log("    vport z scale %g offset %g, vte %08X, clip %08X, depth clamp/zrange regs %08X %08X", RegFloat(PA_CL_VPORT_ZSCALE), RegFloat(PA_CL_VPORT_ZOFFSET), gpureg::Get(0x2206), gpureg::Get(0x2204), gpureg::Get(0x2280), gpureg::Get(0x2281));
        Log("    alpha ref %g (pinfo.x %u) edram mode %u colormask %X", RegFloat(RB_ALPHA_REF), pc->info[0], edramMode, gpureg::Get(RB_COLOR_MASK));
        Log("    surface %08X color %08X depth %08X depthctl %08X stencil %08X colorctl %08X mode %X", gpureg::Get(0x2000), gpureg::Get(0x2001), gpureg::Get(0x2002),
            gpureg::Get(RB_DEPTHCONTROL), gpureg::Get(RB_STENCILREFMASK), gpureg::Get(RB_COLORCONTROL), gpureg::Get(PA_SU_SC_MODE_CNTL));
        for (uint32_t c : { 0u, 1u, 5u, 6u, 7u, 14u, 22u, 254u, 255u })
            Log("    pc%u = %g %g %g %g", c, pc->c[c][0], pc->c[c][1], pc->c[c][2], pc->c[c][3]);
    }
    {
        const auto t0 = g_perfTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const D3D_PRIMITIVE_TOPOLOGY topology = indexedCount && prim == 6 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (topology != g_drawState.topology)
        {
            g_drawState.topology = topology;
            C().list->IASetPrimitiveTopology(topology);
        }
        if (indexedCount && memcmp(&indexView, &g_drawState.indexBuffer, sizeof(indexView)) != 0)
        {
            g_drawState.indexBuffer = indexView;
            C().list->IASetIndexBuffer(&indexView);
        }
        const auto issue = [&] {
            const UINT vertices = indexedCount ? indexedCount : directIndex ? directVertices : UINT(list.size());
            if (indexedCount)
                C().list->DrawIndexedInstanced(indexedCount, 1, 0, 0, 0);
            else
                C().list->DrawInstanced(vertices, 1, 0, 0);
        };
        if (!alias.on)
        {
            issue();
        }
        else
        {
            // Through a foreign EDRAM layout: each run of tiles along a view tile row that also
            // stays on one row of the host layout is drawn once, its viewport moved by where the
            // run's tiles lie on the host surface and its scissor cut to them (in samples: view
            // pixels x the view's MSAA, host pixels / the host's).
            const uint32_t viewMsaa = (alias.viewSurface >> 16) & 3, hostMsaa = (alias.hostSurface >> 16) & 3;
            const int32_t va = int32_t(MsaaAcross(viewMsaa)), vd = int32_t(MsaaDown(viewMsaa));
            const int32_t ha = int32_t(MsaaAcross(hostMsaa)), hd = int32_t(MsaaDown(hostMsaa));
            const int32_t viewTiles = int32_t((alias.viewSurface & 0x3FFF) * va / kEdramTileW);
            const int32_t hostTiles = int32_t((alias.hostSurface & 0x3FFF) * ha / kEdramTileW);
            const int32_t tw = int32_t(kEdramTileW), th = int32_t(kEdramTileH);
            const int32_t left = scissor.left * va, top = scissor.top * vd, right = scissor.right * va, bottom = scissor.bottom * vd;
            const float scale = ScaleY();
            for (int32_t row = top / th; row * th < bottom; row++)
                for (int32_t col = left / tw; col * tw < right && col < viewTiles;)
                {
                    const int32_t tile = row * viewTiles + col, hostRow = tile / hostTiles, hostCol = tile % hostTiles;
                    const int32_t run = std::min(viewTiles - col, hostTiles - hostCol); // tiles that stay together
                    const int32_t dx = (hostCol - col) * tw, dy = (hostRow - row) * th; // view samples to host samples
                    const int32_t x0 = std::max(left, col * tw), x1 = std::min(right, (col + run) * tw);
                    const int32_t y0 = std::max(top, row * th), y1 = std::min(bottom, (row + 1) * th);
                    col += run;
                    if (x1 <= x0 || y1 <= y0)
                        continue;
                    const D3D12_VIEWPORT runVp{ float(dx) / float(ha) * scale, float(dy) / float(hd) * scale, vp.Width, vp.Height, 0, 1 };
                    const D3D12_RECT runScissor{ LONG(std::lround(std::floor(float(x0 + dx) / float(ha)) * scale)),
                        LONG(std::lround(std::floor(float(y0 + dy) / float(hd)) * scale)), LONG(std::lround(std::ceil(float(x1 + dx) / float(ha)) * scale)),
                        LONG(std::lround(std::ceil(float(y1 + dy) / float(hd)) * scale)) };
                    C().list->RSSetViewports(1, &runVp);
                    C().list->RSSetScissorRects(1, &runScissor);
                    issue();
                    g_drawState.viewport = runVp;
                    g_drawState.scissor = runScissor;
                }
        }
        if (g_perfTiming)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 1.0)
                Log("perf: DrawInstanced took %.1f ms (vs %016llx ps %016llx)", ms, (unsigned long long)C().vsHash, (unsigned long long)C().psHash);
        }
    }
    Section(6);
    g_r.drawsThisFrame++;
}

}

// ---- Benchmark (benchmark.cmd) ------------------------------------------------------
// Frame times while recording (a timed run), and the report written at the end.
std::atomic<bool> g_benchRecording{ false };
std::mutex g_benchMutex;
std::vector<float> g_benchFrames; // ms

void BenchmarkRecord(bool on)
{
    g_benchRecording.store(on, std::memory_order_relaxed);
}

void BenchmarkWriteReport(const char* path)
{
    std::vector<float> frames;
    {
        std::lock_guard lock(g_benchMutex);
        frames = g_benchFrames;
    }
    std::string cpu = "?", gpu = "?";
    {
        char name[256];
        DWORD size = sizeof(name);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr,
                name, &size) == ERROR_SUCCESS)
            cpu = name;
        while (!cpu.empty() && cpu.back() == ' ')
            cpu.pop_back();
    }
    uint32_t cores = 0;
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes))
            for (DWORD at = 0; at < bytes; at += reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at)->Size)
                cores++;
    }
    MEMORYSTATUSEX memory{ sizeof(memory) };
    GlobalMemoryStatusEx(&memory);
    if (g_r.device)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(g_r.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            gpu = std::string(name) + " (" + std::to_string(desc.DedicatedVideoMemory >> 20) + " MB)";
        }
    }
    const Settings& st = GetSettings();
    static const char* upscalers[] = { "off", "FSR 1", "FSR 3", "DLSS", "FSR 4" };
    static const char* qualities[] = { "native (AA)", "quality", "balanced", "performance", "ultra performance" };
    std::string upscaler = upscalers[std::min<uint32_t>(uint32_t(st.upscaler), 4)];
    if (st.upscaler != Settings::Upscaler::Off)
        upscaler += std::string(", ") + qualities[std::min<uint32_t>(st.quality, 4)];

    FILE* out = fopen(path, "w");
    if (!out)
        return;
    fprintf(out, "ReGPU benchmark\n\n");
    fprintf(out, "CPU:        %s (%u cores, %u threads)\n", cpu.c_str(), cores, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    fprintf(out, "GPU:        %s\n", gpu.c_str());
    fprintf(out, "RAM:        %.0f GB\n", memory.ullTotalPhys / 1073741824.0);
    fprintf(out, "Resolution: %ux%u%s\n", OutputWidth(), OutputHeight(), st.fullscreen ? " fullscreen" : " windowed");
    fprintf(out, "Upscaler:   %s\n\n", upscaler.c_str());
    if (frames.size() < 100)
    {
        fprintf(out, "Not enough frames recorded (%zu): the run ended too early. Start the game once first\n"
                     "(the first start compiles shaders), then run the benchmark again.\n", frames.size());
        fclose(out);
        return;
    }
    double total = 0;
    for (float f : frames)
        total += f;
    std::vector<float> sorted = frames;
    std::sort(sorted.begin(), sorted.end());
    // Lows: the average frame rate over the slowest 1% / 0.1% of frames.
    auto low = [&](double fraction) {
        const size_t n = std::max<size_t>(1, size_t(sorted.size() * fraction));
        double sum = 0;
        for (size_t i = sorted.size() - n; i < sorted.size(); i++)
            sum += sorted[i];
        return 1000.0 * double(n) / sum;
    };
    fprintf(out, "Average:    %.1f fps (%zu frames in %.1f s)\n", 1000.0 * double(frames.size()) / total, frames.size(), total / 1000.0);
    fprintf(out, "1%% low:     %.1f fps\n", low(0.01));
    fprintf(out, "0.1%% low:   %.1f fps\n", low(0.001));
    fprintf(out, "Slowest:    %.1f ms\n", double(sorted.back()));
    // Pacing: how much consecutive frames differ (0 for perfectly even frames).
    double jitter = 0;
    for (size_t i = 1; i < frames.size(); i++)
        jitter += std::fabs(double(frames[i]) - double(frames[i - 1]));
    fprintf(out, "Pacing:     %.2f ms average change between frames, median frame %.2f ms\n", jitter / double(frames.size() - 1),
        double(sorted[sorted.size() / 2]));
    // Timeline: average and slowest frame per 5 s (does the first stretch run slower?).
    fprintf(out, "Timeline:  ");
    double windowMs = 0, windowMax = 0;
    size_t windowFrames = 0;
    for (float f : frames)
    {
        windowMs += f;
        windowMax = std::max(windowMax, double(f));
        windowFrames++;
        if (windowMs >= 5000.0)
        {
            fprintf(out, " %.0f fps (max %.1f ms) |", 1000.0 * double(windowFrames) / windowMs, windowMax);
            windowMs = windowMax = 0;
            windowFrames = 0;
        }
    }
    fprintf(out, "\n");
    fclose(out);
    Log("benchmark: report written to %s", path);
}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer);
void RendererOnSwap(uint64_t frameNumber, uint32_t frontBuffer)
{
    const auto t0 = std::chrono::steady_clock::now();
    RendererOnSwapImpl(frameNumber, frontBuffer);
    const auto now = std::chrono::steady_clock::now();
    g_perf.swapSeconds += std::chrono::duration<double>(now - t0).count();
    g_perf.frames++;
    static LiveSwitch perf("REGPU_PERF");
    static auto lastSwap = now;
    static double frameDraw = 0, lastDrawTotal = 0;
    // REGPU_STALL_MS=<ms>: a watcher logs every thread's host stack (Host::logThreadStacks) while no frame
    // has been presented for that long, once per stall, 20 at most, from 30 s after the first frame (not
    // during loading). Finds what a thread is blocked in while the stall is still on.
    {
        static std::atomic<int64_t> lastSwapMs{ 0 };
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        lastSwapMs = nowMs;
        static const int stallMs = getenv("REGPU_STALL_MS") ? atoi(getenv("REGPU_STALL_MS")) : 0;
        static const bool watcher = [nowMs] {
            if (stallMs <= 0)
                return false;
            std::thread([armAt = nowMs + 30000] {
                int dumps = 0;
                int64_t dumpedFor = 0;
                while (dumps < 20)
                {
                    Sleep(20);
                    const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    const int64_t last = lastSwapMs.load();
                    if (t < armAt || t - last < stallMs || dumpedFor == last)
                        continue;
                    dumpedFor = last;
                    dumps++;
                    Log("renderer: no frame presented for %lld ms (stall %d)", (long long)(t - last), dumps);
                    if (regpu::GetHost().logThreadStacks)
                        regpu::GetHost().logThreadStacks("no frame presented (REGPU_STALL_MS)");
                }
            }).detach();
            return true;
        }();
        (void)watcher;
    }
    {
        const double dt = std::chrono::duration<double>(now - lastSwap).count() * 1000.0;
        lastSwap = now;
        if (g_benchRecording.load(std::memory_order_relaxed))
        {
            std::lock_guard lock(g_benchMutex);
            g_benchFrames.push_back(float(dt));
        }
        g_perf.maxFrame = std::max(g_perf.maxFrame, dt);
        if (dt > 20.0)
            g_perf.slowFrames++;
        const double drawNow = g_perf.drawSeconds * 1000.0;
        static double drawBefore = 0;
        const double frameDrawMs = drawNow >= drawBefore ? drawNow - drawBefore : drawNow;
        drawBefore = drawNow;
        static const double slowMs = getenv("REGPU_SLOW_MS") ? atof(getenv("REGPU_SLOW_MS")) : 25.0; // slow-frame log threshold
        if (perf && dt > slowMs)
        {
            if (regpu::GetHost().slowFrame)
                regpu::GetHost().slowFrame(dt + 2.0);
        }
        if (perf && dt > slowMs)
            Log("perf: slow frame %llu: %.1f ms | draw %.1f ms (worst draw %.1f; shaders %.1f indices %.1f pipeline %.1f constants %.1f "
                "textures %.1f submit %.1f), waited %.1f ms on %u builds, %u pipelines started, %u resources created",
                (unsigned long long)frameNumber, dt, frameDrawMs, g_frameEvents.worstDrawMs, g_frameEvents.section[0], g_frameEvents.section[1],
                g_frameEvents.section[2], g_frameEvents.section[3], g_frameEvents.section[4], g_frameEvents.section[5], g_frameEvents.waitMs,
                g_frameEvents.waits, g_frameEvents.pipelines, g_frameEvents.created);
        g_frameEvents = {};
    }
    const double window = std::chrono::duration<double>(now - g_perf.windowStart).count();
    if (window >= 1.0)
    {
        if (perf && g_perf.frames)
            Log("perf: %.1f fps, %.2f ms/frame | renderer draw %.2f ms, swap %.2f ms per frame | %.0f draws/frame", g_perf.frames / window,
                window * 1000.0 / g_perf.frames, g_perf.drawSeconds * 1000.0 / g_perf.frames, g_perf.swapSeconds * 1000.0 / g_perf.frames,
                double(g_perf.draws) / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   shaders %.2f | indices %.2f | pipeline %.2f | constants %.2f | textures %.2f | submit %.2f ms/frame",
                g_perf.section[0] * 1000.0 / g_perf.frames, g_perf.section[1] * 1000.0 / g_perf.frames, g_perf.section[2] * 1000.0 / g_perf.frames,
                g_perf.section[3] * 1000.0 / g_perf.frames, g_perf.section[4] * 1000.0 / g_perf.frames, g_perf.section[5] * 1000.0 / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   constants part: index list %.2f | constant packing %.2f | vertex data %.2f | the rest %.2f ms/frame; texture checks %.2f ms/frame (%.0f a frame)",
                g_perf.sub[0] * 1000.0 / g_perf.frames, g_perf.sub[1] * 1000.0 / g_perf.frames, g_perf.sub[2] * 1000.0 / g_perf.frames,
                g_perf.sub[3] * 1000.0 / g_perf.frames, g_textureHashSeconds * 1000.0 / g_perf.frames, double(g_textureChecks) / g_perf.frames);
        if (perf)
            Log("perf:   cpu%s | slowest frame %.1f ms, %llu over 20 ms", GuestThreadCpuUsage(window).c_str(), g_perf.maxFrame,
                (unsigned long long)g_perf.slowFrames);
        if (perf && g_perf.frames)
            Log("perf:   waiting on host GPU %.2f ms/frame | GPU time %.2f ms/frame (worst %.1f)", g_gpuWaitSeconds * 1000.0 / g_perf.frames,
                g_gpuFrames ? g_gpuFrameMs / g_gpuFrames : 0.0, g_gpuFrameMaxMs);
        g_gpuFrameMs = g_gpuFrameMaxMs = 0;
        g_gpuFrames = 0;
        if (perf && g_perf.frames)
            Log("perf:   descriptor tables built %.0f, reused %.0f per frame", double(g_perf.tablesBuilt) / g_perf.frames, double(g_perf.tablesReused) / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   uploads: %.1f MB hashed, %.1f MB copied (%.0f copies, %.0f reused) per frame",
                double(g_uploadStats.hashed.exchange(0)) / g_perf.frames / 1048576.0, double(g_uploadStats.copied.exchange(0)) / g_perf.frames / 1048576.0,
                double(g_uploadStats.copies.exchange(0)) / g_perf.frames, double(g_uploadStats.hits.exchange(0)) / g_perf.frames);
        if (perf && !g_created.empty())
        {
            std::string s;
            for (auto& [what, n] : g_created)
                s += " " + what + ":" + std::to_string(n);
            Log("perf:   resources created this second:%s", s.c_str());
        }
        if (perf && g_pipelinesCreated)
            Log("perf:   pipelines started %u", g_pipelinesCreated);
        if (perf && (g_scanPrefound || g_scanMissed))
            Log("perf:   new shaders at draw time: %u already found by scan, %u not", g_scanPrefound, g_scanMissed);
        g_scanPrefound = g_scanMissed = 0;
        if (g_edramTargetsOn)
        {
            const edram::TargetCache::Stats es = g_edramTargets.TakeStats();
            if (es.targetsCreated || es.transfers || es.skipped)
                Log("edram: %llu targets created, %llu transfers (%llu tiles), %llu skipped", (unsigned long long)es.targetsCreated,
                    (unsigned long long)es.transfers, (unsigned long long)es.transferTiles, (unsigned long long)es.skipped);
        }
        if (perf && g_skippedPipeline)
            Log("perf:   draws skipped waiting for pipelines: %u", g_skippedPipeline);
        g_skippedPipeline = 0;
        g_pipelineMs = 0;
        g_pipelinesCreated = 0;
        g_pipelineLibrary.MaybeSave();
        g_created.clear();
        g_gpuWaitSeconds = 0;
        g_perf = Perf{};
        g_textureHashSeconds = 0;
        g_textureChecks = 0;
    }
}

// ---- Precompile (test build): before the game goes on, compile every stored shader and
// build every stored pipeline, with the overlay up. The stored set is whatever earlier
// runs recorded (shadercache/*.ucode and pipelines.manifest), so the more the game has
// been played, the more is covered. REGPU_NO_PRECOMPILE=1 turns it off.
bool g_precompileDone = false;

void PrecompileAll()
{
    if (g_precompileDone)
        return;
    g_precompileDone = true;
    // From here on the game runs: speculative finds wait for a draw that needs them.
    struct ParkAfter
    {
        ~ParkAfter() { g_compiler.park = true; }
    } parkAfter;
    if (getenv("REGPU_NO_PRECOMPILE"))
        return;
    const auto start = std::chrono::steady_clock::now();
    // Only the shaders stored pipelines use: older caches also hold scan finds that never drew.
    std::vector<PipelineKey> keys = g_manifest.pending;
    std::vector<std::pair<bool, uint64_t>> shaders;
    {
        std::set<std::pair<bool, uint64_t>> used;
        for (const PipelineKey& k : keys)
            for (const auto s : { std::pair{ false, k.vs }, std::pair{ true, k.ps } })
                if (used.insert(s).second)
                    shaders.push_back(s);
    }
    g_stats.warmTotal = uint32_t(keys.size());
    g_stats.warmDone = 0;
    Log("precompile: %zu shaders and %zu pipelines stored", shaders.size(), keys.size());
    size_t nextShader = 0, nextKey = 0;
    size_t shadersInstalled = 0, pipelinesStarted = 0;
    auto lastPresent = std::chrono::steady_clock::time_point{};
    for (;;)
    {
        // Phase 1: load and translate shaders a batch at a time (compiles run on worker threads).
        for (int batch = 0; batch < 64 && nextShader < shaders.size(); batch++, nextShader++)
        {
            const auto [pixel, hash] = shaders[nextShader];
            if (g_r.shaders.count(hash))
                continue;
            std::vector<uint32_t> code;
            if (Manifest::LoadShader(pixel, hash, code))
            {
                GetShader(hash, code, pixel);
                shadersInstalled++;
            }
        }
        // Phase 2: start pipelines whose shaders are ready.
        for (int batch = 0; batch < 256 && nextKey < keys.size(); batch++)
        {
            const PipelineKey key = keys[nextKey];
            const CompiledShader* vs = nullptr;
            const CompiledShader* ps = nullptr;
            if (auto f = g_r.shaders.find(key.vs); f != g_r.shaders.end())
                vs = f->second.get();
            if (auto f = g_r.shaders.find(key.ps); f != g_r.shaders.end())
                ps = f->second.get();
            if (!vs || !ps)
            {
                // Shader not loaded yet: load it now (translation queues the compile).
                std::vector<uint32_t> code;
                if (!vs && Manifest::LoadShader(false, key.vs, code))
                    vs = &GetShader(key.vs, code, false);
                code.clear();
                if (!ps && Manifest::LoadShader(true, key.ps, code))
                    ps = &GetShader(key.ps, code, true);
            }
            // A shader not stored (a static one from Host::staticShaders, installed after this): skip the
            // pipeline. Waiting for it, as before, waited for good (FH1 on the FX: a black screen).
            if (vs && ps && (vs->state.load() == CompiledShader::Pending || ps->state.load() == CompiledShader::Pending))
                break; // wait for this one to finish compiling before going on
            if (vs && ps && vs->state.load() == CompiledShader::Ready && ps->state.load() == CompiledShader::Ready)
            {
                if (!g_r.pipelines.count(key))
                {
                    EnsurePipeline(*vs, *ps, key);
                    pipelinesStarted++;
                }
            }
            nextKey++;
            g_stats.warmDone++;
        }
        // Done when every shader is loaded and every pipeline is built.
        size_t building = 0;
        for (auto& [k, e] : g_r.pipelines)
            if (e->state.load() == 0)
                building++;
        const bool allSent = nextShader >= shaders.size() && nextKey >= keys.size();
        if (allSent && g_compiler.jobs.load() == 0 && building == 0)
            break;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPresent > std::chrono::milliseconds(33))
        {
            lastPresent = now;
            g_hold.active = true;
            HoldPresent();
        }
        Sleep(1);
    }
    g_hold.active = false;
    // A warm start never has a long loading hold, so without this settled skipping
    // never arms and each first-time effect stalls for its compile.
    if (!keys.empty())
        g_lastLongHold = std::chrono::steady_clock::now();
    Log("precompile: done in %.1f s (%zu shaders loaded, %zu pipelines started)",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), shadersInstalled, pipelinesStarted);

    // Every shader in the game data (Host::staticShaders) goes to the compile queue as well. Compiles
    // run on the background workers and the game doesn't wait for them; cached bytecode loads
    // straight away, so only the first boot pays (REGPU_NO_STATIC_SHADERS=1 skips this).
    const regpu::Host& host = regpu::GetHost();
    if (getenv("REGPU_NO_STATIC_SHADERS") || !host.staticShaders)
        return;
    auto BootScreenCompose = [&](const wchar_t* title, uint32_t done, uint32_t total, std::vector<uint8_t>& rgba) {
        return host.bootScreen && host.bootScreen(title, done, total, rgba);
    };
    const auto staticStart = std::chrono::steady_clock::now();
    size_t queued = 0;
    std::vector<const CompiledShader*> compiling;
    // The first boot's search of the game files takes a few seconds: say so on screen.
    std::vector<uint8_t> bootFrame;
    if (!(host.staticShadersSaved && host.staticShadersSaved()) && BootScreenCompose(L"PREPARING SHADERS", 0, 0, bootFrame))
        BootPresent(bootFrame);
    for (const regpu::ScannedShader& s : host.staticShaders())
    {
        const uint64_t hash = Fnv(s.code.data(), s.code.size() * 4) ^ (s.pixel ? 1 : 0);
        if (g_r.shaders.count(hash))
            continue;
        g_scanKnown.insert(hash);
        const CompiledShader& cs = GetShader(hash, s.code, s.pixel, true);
        if (cs.state.load() == CompiledShader::Pending)
            compiling.push_back(&cs);
        queued++;
    }
    // Compiling them while the game runs halves the frame rate for minutes, so the first
    // boot waits here with the overlay up; later boots find the bytecode cached and
    // queue nothing.
    const int toCompile = int(compiling.size());
    Log("static shaders: %zu new, %d to compile (prepared in %.1f s)", queued, toCompile,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - staticStart).count());
    if (!toCompile)
        return;
    g_stats.staticTotal = uint32_t(toCompile);
    g_stats.staticDone = 0;
    const auto compileStart = std::chrono::steady_clock::now();
    auto lastShown = std::chrono::steady_clock::time_point{}, lastScan = std::chrono::steady_clock::now();
    for (size_t next = 0;;)
    {
        while (next < compiling.size() && compiling[next]->state.load() != CompiledShader::Pending)
            next++;
        if (next == compiling.size())
            break;
        const auto now = std::chrono::steady_clock::now();
        // The game keeps running meanwhile and builds the shaders it makes at runtime (the
        // title's among them): compile those in the same wait.
        if (now - lastScan > std::chrono::milliseconds(1500))
        {
            lastScan = now;
            ShaderScanNow(2);
            const size_t before = compiling.size();
            DrainScan(0, &compiling);
            g_stats.staticTotal += uint32_t(compiling.size() - before);
        }
        if (now - lastShown > std::chrono::milliseconds(100))
        {
            lastShown = now;
            g_stats.staticDone = uint32_t(std::count_if(compiling.begin(), compiling.end(),
                [](const CompiledShader* cs) { return cs->state.load() != CompiledShader::Pending; }));
            if (BootScreenCompose(L"COMPILING SHADERS", g_stats.staticDone.load(), g_stats.staticTotal.load(), bootFrame))
                BootPresent(bootFrame);
            else
            {
                g_hold.active = true;
                HoldPresent();
            }
        }
        Sleep(5);
    }
    g_hold.active = false;
    g_stats.staticTotal = 0;
    g_lastLongHold = std::chrono::steady_clock::now();
    Log("static shaders: %zu compiled in %.1f s (%zu found in memory meanwhile)", compiling.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count(), compiling.size() - size_t(toCompile));
}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer)
{
    if (regpu::GetHost().onSwap)
        regpu::GetHost().onSwap(g_r.frame);
    if (!g_r.ok)
        return;
    PrecompileAll();
    WarmupStep();
    BeginFrameIfNeeded();
    // A frame that rendered small but never resolved a scene (a menu): upscale what it shows
    // and stay at full size for a while (menus are drawn sharp at full size).
    if (g_scenePhase && !g_upscale.switchedThisFrame)
    {
        if (g_upscale.rearms)
        {
            g_upscale.rearmFailures++;
            g_upscale.rearmBlockedUntil = g_r.frame + 120ull * (1u << std::min(g_upscale.rearmFailures, 5u));
            Log("temporal: the scene was re-armed but no main view resolved this frame; re-arming is off until frame %llu",
                (unsigned long long)g_upscale.rearmBlockedUntil);
        }
        UpscaleResolved(frontBuffer);
        g_sceneScale = 1.0f;
        g_scenePhase = false;
        g_upscale.retryFrame = g_r.frame + 120;
        g_upscale.reset = true; // the next scene has no usable history
    }
    // Show the front buffer the title resolved to; fall back to the render
    // target if it was never resolved.
    ID3D12Resource* image = C().color.Get();
    D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto it = g_r.resolved.find(frontBuffer);
    // Any colour format: 2_10_10_10 front buffers are common (GradeFrontBuffer makes them 8-bit).
    const bool fromResolve = it != g_r.resolved.end() && it->second.width == kWidth && it->second.height == kHeight &&
        it->second.format != DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* graded = nullptr; // the front buffer through the display's gamma table
    if (fromResolve)
    {
        image = it->second.resource.Get();
        graded = GradeFrontBuffer(image, it->second.guestFormat);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        C().list->ResourceBarrier(1, &b);
    }
    // The swap chain takes 8-bit colour: a front buffer in another format is shown through its graded
    // copy, and without one (no descriptors left) not at all this frame.
    const bool showable = fromResolve && (graded || it->second.format == DXGI_FORMAT_R8G8B8A8_UNORM);
    // The held image is re-shown as it is, so only an 8888 one can be held.
    if (fromResolve && it->second.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        g_hold.image = it->second.resource;
    ID3D12Resource* overlay = showable ? PrepareOverlay() : nullptr;
    ID3D12Resource* shown = graded ? graded : image;
    if (showable)
        if (ID3D12Resource* smoothed = SmaaFrontBuffer(shown))
            shown = smoothed;
    if (showable)
        if (ID3D12Resource* enhanced = EnhanceFrontBuffer(shown))
            shown = enhanced;
    if (showable)
        if (ID3D12Resource* view = ViewFrontBuffer(shown))
            shown = view;
    const bool presenting = showable && PresenterRecordCopy(C().list->Real(), shown, overlay, kOverlayW, kOverlayH);
    {
        const uint64_t traceFrame = TraceFrame();
        if (traceFrame && g_r.frame + 1 == traceFrame)
            Log("swap: front buffer %08X, %s", frontBuffer, fromResolve ? "resolved" : "not resolved: showing the render target");
    }

    static uint64_t dumpFrame = getenv("REGPU_DUMP_FRAME") ? strtoull(getenv("REGPU_DUMP_FRAME"), nullptr, 10) : 0;
    // A capture asked for some other way (regpuctl capture: RendererRequestCapture) dumps that frame too.
    if (const uint64_t hot = g_hotkeyFrame.load(std::memory_order_relaxed); hot > dumpFrame && hot > g_r.frame)
        dumpFrame = hot;
    {
        // F12 in the game window: capture the next frame (frame.png, the resolves in out\res,
        // and the frame's draws in out/play.log) to report rendering bugs.
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (down && !wasDown && pid == GetCurrentProcessId())
        {
            g_hotkeyFrame = g_r.frame + 2;
            dumpFrame = g_r.frame + 2;
            Log("capture: F12 pressed, tracing frame %llu", (unsigned long long)dumpFrame);
        }
        wasDown = down;
    }
    // REGPU_CAPTURE_TALL=1: the same capture as F12, once a pass taller than the screen was drawn.
    {
        static LiveSwitch captureTall("REGPU_CAPTURE_TALL");
        static bool capturedTall = false;
        if (captureTall && !capturedTall && g_tallSeen)
        {
            capturedTall = true;
            dumpFrame = g_r.frame + 1; // the frame the tall pass was drawn in (already being traced)
            Log("capture: tall pass seen, dumping frame %llu", (unsigned long long)dumpFrame);
        }
    }
    // REGPU_CAPTURE_AT=<seconds>: the same capture as F12, that long after the start (for replays).
    {
        static const double captureAt = getenv("REGPU_CAPTURE_AT") ? atof(getenv("REGPU_CAPTURE_AT")) : 0;
        static const auto captureStart = std::chrono::steady_clock::now();
        static bool captured = false;
        if (captureAt > 0 && !captured && std::chrono::duration<double>(std::chrono::steady_clock::now() - captureStart).count() >= captureAt)
        {
            captured = true;
            g_hotkeyFrame = g_r.frame + 2;
            dumpFrame = g_r.frame + 2;
            Log("capture: REGPU_CAPTURE_AT reached, tracing frame %llu", (unsigned long long)dumpFrame);
        }
    }
    ++g_r.frame;
    DebugOnSwap(g_r.frame);
    if (int st = g_drawStats.load(); st) // drawstats: arm at one swap, stop at the next
        g_drawStats = st == 1 ? 2 : 0;
    // The debug server's "shot": this frame (when it has a picture), to the file it names.
    std::string shotPath;
    const bool shot = showable && DebugShotPending(shotPath);
    if (shot)
        dumpFrame = g_r.frame;
    // REGPU_DUMP_AT=<seconds>: dump the first resolved frame after that long (comparable
    // screenshots whatever the frame rate).
    {
        static const double dumpAt = getenv("REGPU_DUMP_AT") ? atof(getenv("REGPU_DUMP_AT")) : 0;
        static const auto start = std::chrono::steady_clock::now();
        if (dumpAt > 0 && !dumpFrame && showable && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= dumpAt)
            dumpFrame = g_r.frame;
    }
    g_tile = TileState{};
    static LiveSwitch dumpSecond("REGPU_SKIP_VS");
    static const uint64_t dumpEvery = getenv("REGPU_DUMP_EVERY") ? strtoull(getenv("REGPU_DUMP_EVERY"), nullptr, 10) : 0;
    static uint32_t sweepCountForDump = [] { uint32_t a = 0, b = 0, c = 0; if (const char* e = getenv("REGPU_SWEEP")) sscanf(e, "%u,%u,%u", &a, &b, &c); return c; }();
    const bool sweepDump = showable && sweepCountForDump && dumpFrame && g_r.frame >= dumpFrame && g_r.frame < dumpFrame + sweepCountForDump;
    const bool dump = sweepDump || (showable && ((dumpFrame && (g_r.frame == dumpFrame || (dumpSecond && g_r.frame == dumpFrame + 1))) || (dumpEvery && g_r.frame % dumpEvery == 0)));
    if (dump)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_r.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, OutputWidth(), OutputHeight(), 1, (OutputWidth() * 4 + 255) & ~255u };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = shown;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    // REGPU_DUMP_RESOLVES=1 (with REGPU_DUMP_FRAME): also write every resolved texture to out/res.
    static std::vector<ResolveDump> resolveDumps;
    resolveDumps.clear();
    if (dump && (getenv("REGPU_DUMP_RESOLVES") || (g_hotkeyFrame.load() && g_r.frame == dumpFrame)))
    {
        auto add = [&](uint32_t base, ResolvedTexture& r, int variant) {
            if (!r.resource)
                return;
            ResolveDump d;
            d.base = base;
            d.variant = variant;
            d.width = r.hostWidth; // the texture's own size
            d.height = r.hostHeight;
            d.format = r.format;
            const UINT bpp = HostBpp(r.format);
            d.pitch = (d.width * bpp + 255) & ~255u;
            d.buffer = CreateBuffer(UINT64(d.pitch) * d.height, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER bar{};
            bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar.Transition.pResource = r.resource.Get();
            bar.Transition.Subresource = 0;
            bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            C().list->ResourceBarrier(1, &bar);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = d.buffer.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { r.format, d.width, d.height, 1, d.pitch };
            src.pResource = r.resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            C().list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
            C().list->ResourceBarrier(1, &bar);
            resolveDumps.push_back(std::move(d));
        };
        for (auto& [base, r] : g_r.resolved)
        {
            add(base, r, 0);
            int v = 1;
            for (ResolvedTexture& spare : r.spares)
                add(base, spare, v++);
        }
    }
    if (fromResolve)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        C().list->ResourceBarrier(1, &b);
        // (The graded copy stays in COPY_SOURCE: GradeFrontBuffer moves it to UAV around its dispatch.)
    }

    FlushIndexCopies(); // this frame's index uploads into the arena, after its draws
    if (g_gpuPasses && g_passQueries && !g_passMarks[UINT(&Frame() - g_r.frames)].empty())
    {
        const UINT frameIndex = UINT(&Frame() - g_r.frames);
        PassMark("frame end %u%u", 0, 0);
        const UINT n = UINT(g_passMarks[frameIndex].size());
        C().list->ResolveQueryData(g_passQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * kPassMarks, n, g_passReadback.Get(),
            UINT64(frameIndex) * kPassMarks * 8);
    }
    if (g_timestamps)
    {
        const UINT frameIndex = UINT(&Frame() - g_r.frames);
        C().list->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2 + 1);
        C().list->ResolveQueryData(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2, 2, g_timestampReadback.Get(), UINT64(frameIndex) * 16);
        Frame().timed = true;
    }
    // A query must end in the list it began in (a segment the title left open: no block to keep it for).
    if (g_occlusionOpen)
    {
        g_occlusionKey = 0;
        EndOcclusionQuery();
    }
    for (const UINT part : g_occlusionSplit)
        g_occlusionPending[g_r.frameIndex].push_back({ 0u, part });
    g_occlusionSplit.clear();
    C().list->Close();
    ID3D12CommandList* lists[] = { C().list->Real() };
    g_r.queue->ExecuteCommandLists(1, lists);
    if (presenting)
        PresenterPresent();
    Frame().fence = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), Frame().fence);
    g_r.listOpen = false;

    if (dump)
    {
        WaitFenceValue(Frame().fence, INFINITE);
        for (ResolveDump& d : resolveDumps)
        {
            uint8_t* mapped = nullptr;
            d.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            // REGPU_DUMP_RESOLVES_RAW=1: also the texels as stored (rows of width x bytes-per-texel, no padding),
            // out/res/<base>_<w>x<h>_v<n>_<bpp>bpp.raw: the PNG clamps to [0, 1], which hides HDR and integer values.
            if (getenv("REGPU_DUMP_RESOLVES_RAW"))
            {
                const UINT bpp = d.format == DXGI_FORMAT_R32_FLOAT ? 4 : HostBpp(d.format);
                std::filesystem::create_directories("out/res");
                char rawName[112];
                snprintf(rawName, sizeof(rawName), "out/res/%08X_%ux%u_v%d_%ubpp.raw", d.base, d.width, d.height, d.variant, bpp);
                if (FILE* f = fopen(rawName, "wb"))
                {
                    for (UINT y = 0; y < d.height; y++)
                        fwrite(mapped + size_t(y) * d.pitch, 1, size_t(d.width) * bpp, f);
                    fclose(f);
                }
            }
            std::vector<uint8_t> out(size_t(d.width) * d.height * 4);
            for (UINT y = 0; y < d.height; y++)
                for (UINT x = 0; x < d.width; x++)
                {
                    uint8_t* o = &out[(size_t(y) * d.width + x) * 4];
                    if (d.format == DXGI_FORMAT_R32G32B32A32_FLOAT)
                    {
                        const float* f = reinterpret_cast<const float*>(mapped + size_t(y) * d.pitch) + x * 4;
                        for (int c = 0; c < 4; c++)
                            o[c] = uint8_t(std::clamp(f[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    else if (d.format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    {
                        const uint16_t* h = reinterpret_cast<const uint16_t*>(mapped + size_t(y) * d.pitch) + x * 4;
                        for (int c = 0; c < 4; c++)
                        {
                            const uint32_t e = (h[c] >> 10) & 31, m = h[c] & 1023;
                            float f = e == 0 ? m / 1024.0f * 6.1035e-5f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, int(e) - 15);
                            if (h[c] & 0x8000)
                                f = -f;
                            o[c] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                    }
                    else if (d.format == DXGI_FORMAT_R32_FLOAT)
                    {
                        const float f = reinterpret_cast<const float*>(mapped + size_t(y) * d.pitch)[x];
                        if (x == 0 && y == 0)
                            d.minDepth = d.maxDepth = f;
                        d.minDepth = std::min(d.minDepth, f);
                        d.maxDepth = std::max(d.maxDepth, f);
                        if (f > 0.0f && f < 1.0f)
                            d.midCount++;
                        o[0] = o[1] = o[2] = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                        o[3] = 255;
                    }
                    else
                        memcpy(o, mapped + size_t(y) * d.pitch + x * 4, 4);
                    o[3] = 255;
                }
            d.buffer->Unmap(0, nullptr);
            if (d.format == DXGI_FORMAT_R32_FLOAT)
                Log("resolve dump: depth %08X %ux%u min %g max %g strictly-inside %llu", d.base, d.width, d.height, d.minDepth, d.maxDepth,
                    (unsigned long long)d.midCount);
            std::filesystem::create_directories("out/res");
            char name[96];
            snprintf(name, sizeof(name), "out/res/%08X_%ux%u_v%d.png", d.base, d.width, d.height, d.variant);
            WritePng(name, d.width, d.height, out);
        }
        resolveDumps.clear();
        uint8_t* data = nullptr;
        g_r.readback->Map(0, nullptr, reinterpret_cast<void**>(&data));
        // Written at the render size; the region probes below work on a 1280x720 copy.
        const uint32_t dw = OutputWidth(), dh = OutputHeight(), dpitch = (dw * 4 + 255) & ~255u;
        std::vector<uint8_t> full(size_t(dw) * dh * 4);
        for (uint32_t y = 0; y < dh; y++)
            memcpy(&full[size_t(y) * dw * 4], data + size_t(y) * dpitch, size_t(dw) * 4);
        g_r.readback->Unmap(0, nullptr);
        for (size_t i = 3; i < full.size(); i += 4)
            full[i] = 255;
        std::vector<uint8_t> rgba(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++)
                memcpy(&rgba[(size_t(y) * kWidth + x) * 4], &full[(size_t(y) * dh / kHeight * dw + size_t(x) * dw / kWidth) * 4], 4);
        static const uint64_t every = getenv("REGPU_DUMP_EVERY") ? strtoull(getenv("REGPU_DUMP_EVERY"), nullptr, 10) : 0;
        // REGPU_REGION_HASH="x,y,w,h" (with REGPU_DUMP_EVERY): instead of a PNG, log a hash of the bright
        // pixels in that part of the output, to see how often it changes (text, animations).
        static uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
        static const bool regionHash = [] {
            const char* e = getenv("REGPU_REGION_HASH");
            return e && sscanf(e, "%u,%u,%u,%u", &rx, &ry, &rw, &rh) == 4;
        }();
        if (regionHash && every && g_r.frame % every == 0)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t y = ry; y < ry + rh && y < dh; y++)
                for (uint32_t x = rx; x < rx + rw && x < dw; x++)
                {
                    const uint8_t* px = &full[(size_t(y) * dw + x) * 4];
                    h = (h ^ (std::max({ px[0], px[1], px[2] }) > 90 ? 1u : 0u)) * 0x100000001B3ull;
                }
            Log("region: frame %llu hash %08X", (unsigned long long)g_r.frame, uint32_t(h ^ (h >> 32)));
        }
        else if (every && g_r.frame % every == 0 && g_r.frame != dumpFrame)
        {
            char name[64];
            std::filesystem::create_directories("out/frames");
            snprintf(name, sizeof(name), "out/frames/%05llu.png", (unsigned long long)g_r.frame);
            WritePng(name, dw, dh, full);
        }
        else if (sweepDump && getenv("REGPU_SWEEP_STAT"))
        {
            // REGPU_SWEEP_STAT="x,y,w,h": log the region's mean colour instead of writing a PNG.
            uint32_t x0 = 0, y0 = 0, w = 1, h = 1;
            sscanf(getenv("REGPU_SWEEP_STAT"), "%u,%u,%u,%u", &x0, &y0, &w, &h);
            uint64_t sum[3] = {};
            for (uint32_t y = y0; y < y0 + h && y < kHeight; y++)
                for (uint32_t x = x0; x < x0 + w && x < kWidth; x++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += rgba[(size_t(y) * kWidth + x) * 4 + c];
            Log("sweep %llu: %llu %llu %llu", (unsigned long long)(g_r.frame - dumpFrame), (unsigned long long)(sum[0] / (w * h)),
                (unsigned long long)(sum[1] / (w * h)), (unsigned long long)(sum[2] / (w * h)));
        }
        else if (sweepDump)
        {
            std::filesystem::create_directories("out/sweep");
            char name[64];
            snprintf(name, sizeof(name), "out/sweep/%03llu.png", (unsigned long long)(g_r.frame - dumpFrame));
            WritePng(name, dw, dh, full);
        }
        else if (shot && g_r.frame == dumpFrame)
        {
            std::error_code ec;
            if (const auto dir = std::filesystem::path(shotPath).parent_path(); !dir.empty())
                std::filesystem::create_directories(dir, ec);
            WritePng(shotPath.c_str(), dw, dh, full);
            DebugShotDone(shotPath);
        }
        else
            WritePng(g_r.frame == dumpFrame ? "frame.png" : "frame2.png", dw, dh, full);
        Log("renderer: wrote frame.png (frame %llu: %u draws, %u skipped)", (unsigned long long)frameNumber, g_r.drawsThisFrame.load(),
            g_r.skippedThisFrame.load());
    }
    if (frameNumber % 600 == 0)
        Log("renderer: frame %llu: %u draws, %u skipped, %zu shaders, %zu pipelines, %zu textures", (unsigned long long)frameNumber,
            g_r.drawsThisFrame.load(), g_r.skippedThisFrame.load(), g_r.shaders.size(), g_r.pipelines.size(), g_r.textures.size());
    g_r.drawsThisFrame = g_r.skippedThisFrame = 0;
    g_r.frameIndex = (g_r.frameIndex + 1) % kFrames;
}

bool RendererTracing()
{
    const uint64_t traceFrame = TraceFrame();
    return traceFrame && g_r.frame + 1 == traceFrame;
}

// The camera aspect the game is given (hooks/camera_aspect.cpp): 16/9 widened by the view
// extent, so its projection and frustum culling cover the wider view.
float GameAspect()
{
    return 16.0f / 9.0f * ViewExtent();
}

// The debug server's "capture": what F12 does.
// Like F12: the next whole frame. (frame + 1 is the frame being drawn now, so a request arriving partway
// through it traced only its last draws, often none.)
void RendererRequestCapture() { g_hotkeyFrame = g_r.frame + 2; }

// The debug server's "ps HASH [EXPR]": an empty expression drops the edit. Returns the alias (0 if dropped).
uint64_t RendererEditPixelShader(uint64_t hash, const std::string& expr)
{
    static std::atomic<uint64_t> serial{ 0 };
    std::lock_guard lock(g_psEditMutex);
    if (auto it = g_psEditAlias.find(hash); it != g_psEditAlias.end())
    {
        g_psEditExpr.erase(it->second);
        g_psEditAlias.erase(it);
    }
    uint64_t alias = 0;
    if (!expr.empty())
    {
        alias = hash ^ (0x9E3779B97F4A7C15ull * ++serial) ^ 0xD3B6000000000000ull;
        g_psEditAlias[hash] = alias;
        g_psEditExpr[alias] = expr;
        g_psEditEver.insert(alias);
    }
    g_psEdits = !g_psEditAlias.empty();
    return alias;
}

// The debug server's "hlsl HASH": the shader's translated source ("" if it hasn't been seen).
std::string RendererShaderHlsl(uint64_t hash)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_r.shaders.find(hash);
    if (it == g_r.shaders.end())
        return {};
    const CompiledShader* cs = it->second.get();
    return cs->hlslSource ? cs->hlslSource->info.hlsl : cs->info.hlsl;
}

// Whether a live edit's alias is drawing: its shader compiled and a pipeline with it built.
bool RendererPsEditReady(uint64_t alias)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_r.shaders.find(alias);
    if (it == g_r.shaders.end() || !it->second->IsReady())
        return false;
    for (const auto& [key, entry] : g_r.pipelines)
        if ((key.ps == alias || key.vs == alias) && entry->state.load(std::memory_order_acquire) == 1)
            return true;
    return false;
}

// drawstats: counts the next whole frame's draws by shader pair (blocks until it's done).
std::string RendererDrawStats()
{
    {
        std::lock_guard lock(g_drawStatsMutex);
        g_drawStatsCounts.clear();
    }
    g_drawStats = 1;
    for (int i = 0; i < 400 && g_drawStats.load() != 0; i++)
        Sleep(5);
    std::lock_guard lock(g_drawStatsMutex);
    std::string out;
    char buf[96];
    for (const auto& [k, n] : g_drawStatsCounts)
    {
        snprintf(buf, sizeof(buf), "%s{\"vs\":\"%016llx\",\"ps\":\"%016llx\",\"draws\":%u}", out.empty() ? "" : ",", (unsigned long long)k.first,
            (unsigned long long)k.second, n);
        out += buf;
    }
    return out;
}

void SetGameWidescreen(bool on) { g_gameWidescreen.store(on, std::memory_order_relaxed); }
} // namespace regpu
