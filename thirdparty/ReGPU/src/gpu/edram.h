#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace regpu
{
// The Xenos EDRAM as the render-target model sees it: 2048 tiles of 80x16 samples at 32 bits
// per sample. A render target is a base tile, a row pitch in tiles, an MSAA mode and a
// format; its pixels are placed row of tiles after row of tiles from the base, wrapping at
// the end of the EDRAM. Different render targets (another pitch, MSAA mode or format, colour
// or depth) can cover the same tiles, and a title relies on reading what another layout
// wrote there (resolves, clears through another view, predicated tiles). Each host render
// target stands for one such layout; the ownership map records which layout last wrote each
// tile, so the renderer copies (transfers) the data when another layout takes the tiles over.
//
// Nothing here is title specific: everything comes from RB_SURFACE_INFO, RB_COLOR*_INFO,
// RB_DEPTH_INFO, RB_MODECONTROL, the masks and the scissor and viewport registers.
// The layout rules are the hardware's: 80x16-sample tiles of 32 bits, 2x and 4x MSAA stored as
// 1x2 and 2x2 samples per pixel, rows of RB_SURFACE_INFO's pitch, 64-bit formats twice as wide.
namespace edram
{
constexpr uint32_t kTileWidthSamples = 80, kTileHeightSamples = 16, kTileCount = 2048;
constexpr uint32_t kMaxHeight = 8192; // the largest guest render target or texture side

// RB_SURFACE_INFO msaa_samples.
enum Msaa : uint32_t { k1x = 0, k2x = 1, k4x = 2 };
// Samples per pixel along each axis: 2x is 1x2, 4x is 2x2.
constexpr uint32_t SamplesXLog2(uint32_t msaa) { return msaa >= k4x ? 1 : 0; }
constexpr uint32_t SamplesYLog2(uint32_t msaa) { return msaa >= k2x ? 1 : 0; }

// RB_COLOR_INFO color_format.
enum ColorFormat : uint32_t
{
    k8888 = 0, k8888Gamma = 1, k2101010 = 2, k2101010Float = 3, k1616 = 4, k16161616 = 5, k1616Float = 6,
    k16161616Float = 7, k2101010As10101010 = 10, k2101010FloatAs16161616 = 12, k32Float = 14, k3232Float = 15,
};
// RB_DEPTH_INFO depth_format.
enum DepthFormat : uint32_t { kD24S8 = 0, kD24FS8 = 1 };

constexpr bool Is64bpp(uint32_t colorFormat) { return colorFormat == k16161616 || colorFormat == k16161616Float || colorFormat == k3232Float; }
// The format the data is stored in: the _AS_ variants only change how blending works.
constexpr uint32_t StorageFormat(uint32_t colorFormat)
{
    return colorFormat == k2101010As10101010 ? k2101010 : colorFormat == k2101010FloatAs16161616 ? k2101010Float : colorFormat;
}

// Row pitch in tiles of a 32 bpp render target (64 bpp ones take twice as many).
constexpr uint32_t PitchTiles32(uint32_t pitchPixels, uint32_t msaa)
{
    return ((pitchPixels << SamplesXLog2(msaa)) + kTileWidthSamples - 1) / kTileWidthSamples;
}
// Height in pixels of a render target with this pitch: the rows from its base up to the same
// base in the next EDRAM addressing period, at most kMaxHeight.
uint32_t HeightPixels(uint32_t pitchTiles32, uint32_t msaa);

// One render target layout: a host render target stands for each.
struct SurfaceKey
{
    uint32_t baseTiles = 0;    // 0..2047
    uint32_t pitchTiles32 = 0; // 0: no surface
    uint32_t msaa = 0;
    bool depth = false;
    uint32_t format = 0;       // StorageFormat for colour, DepthFormat for depth

    bool Empty() const { return pitchTiles32 == 0; }
    bool Is64bpp() const { return !depth && edram::Is64bpp(format); }
    uint32_t PitchTiles() const { return pitchTiles32 << uint32_t(Is64bpp()); }
    // Size in pixels (a host render target is this times the resolution scale).
    uint32_t WidthPixels() const { return pitchTiles32 * kTileWidthSamples >> SamplesXLog2(msaa); }
    uint32_t HeightPixels() const { return edram::HeightPixels(pitchTiles32, msaa); }
    uint64_t Packed() const
    {
        return uint64_t(baseTiles) | uint64_t(pitchTiles32) << 11 | uint64_t(msaa) << 21 | uint64_t(depth) << 23 | uint64_t(format) << 24;
    }
    bool operator==(const SurfaceKey& o) const { return Packed() == o.Packed(); }
    bool operator!=(const SurfaceKey& o) const { return !(*this == o); }
};

// A layout packed for the transfer shader's constants (gpu/edram_transfer_hlsl.h).
inline uint32_t TransferInfo(const SurfaceKey& k)
{
    return k.baseTiles | k.PitchTiles() << 11 | k.msaa << 22 | uint32_t(k.Is64bpp()) << 24 | (k.format & 15) << 26;
}

struct Rect
{
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool Empty() const { return right <= left || bottom <= top; }
};

// The registers a pass is decoded from (as the command processor holds them).
struct PassRegisters
{
    uint32_t surfaceInfo = 0;  // RB_SURFACE_INFO
    uint32_t colorInfo[4]{};   // RB_COLOR_INFO, RB_COLOR1..3_INFO
    uint32_t depthInfo = 0;    // RB_DEPTH_INFO
    uint32_t modeControl = 0;  // RB_MODECONTROL
    uint32_t colorMask = 0;    // RB_COLOR_MASK
    uint32_t depthControl = 0; // RB_DEPTHCONTROL
    uint32_t windowOffset = 0; // PA_SC_WINDOW_OFFSET
    uint32_t scissorTL = 0, scissorBR = 0; // PA_SC_WINDOW_SCISSOR_TL/BR
    uint32_t screenScissorTL = 0, screenScissorBR = 0x20002000; // PA_SC_SCREEN_SCISSOR_TL/BR
    uint32_t modeCntl = 0;     // PA_SU_SC_MODE_CNTL
    uint32_t vteCntl = 0;      // PA_CL_VTE_CNTL
    uint32_t vtxCntl = 0;      // PA_SU_VTX_CNTL (pixel centre)
    float viewportYScale = 0, viewportYOffset = 0; // PA_CL_VPORT_YSCALE/YOFFSET
    uint32_t psColorTargets = 0xF; // colour targets the pixel shader writes (bit per target)
};

// A draw's render targets and the EDRAM it covers.
struct Pass
{
    uint32_t pitchPixels = 0, msaa = 0, pitchTiles32 = 0;
    Rect scissor;            // in render target pixels (window offset applied), clamped to the pitch
    uint32_t heightUsed = 0; // rows this draw can reach (scissor, viewport), at most the targets' height
    // [0] depth/stencil, [1..4] colour targets 0..3; a target is used if its bit is in `used`.
    SurfaceKey targets[5];
    uint32_t lengthTiles[5]{}; // tiles from the target's base that the draw can write
    uint32_t used = 0;
};

// Window scissor in render target pixels: the window offset applies unless the scissor
// disables it, and the screen scissor and pitch bound it.
Rect Scissor(const PassRegisters& r);
// Decodes the render targets a draw uses (edram mode 4 colour and depth, 5 depth only)
// and how far into the EDRAM each can reach. Targets sharing a base keep the one earlier in
// importance order (colour 0..3, then depth), as only one can be bound there.
Pass DescribePass(const PassRegisters& r);

// A range of tiles another layout wrote last, to copy into the new owner before it draws.
struct Transfer
{
    uint32_t startTiles, endTiles; // absolute tiles, end exclusive, within one addressing period
    SurfaceKey source;
};

// Which layout last wrote each tile.
class Ownership
{
public:
    Ownership(); // no tile written yet
    // `dest` takes over `lengthTiles` tiles from its base plus `startRelative` (wrapping at
    // the end of the EDRAM). Ranges another layout owned are appended to `transfers`.
    void Claim(const SurfaceKey& dest, uint32_t startRelative, uint32_t lengthTiles, std::vector<Transfer>* transfers);
    SurfaceKey OwnerAt(uint32_t tile) const;
    // Owned ranges in tile order, merged where the owner is the same.
    std::vector<Transfer> Ranges() const;
    void Reset() { *this = Ownership(); }

private:
    struct Range
    {
        uint32_t end;
        SurfaceKey owner;
    };
    void ClaimExtent(const SurfaceKey& dest, uint32_t start, uint32_t end, std::vector<Transfer>* transfers);
    std::map<uint32_t, Range> ranges_; // start tile -> range, covering 0..kTileCount (empty owner: never written)
};
} // namespace edram
} // namespace regpu
