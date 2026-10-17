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
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/hlsl.h"
#include "gpu/regs.h"
#include "gpu/texture.h"
#include "gpu/ucode.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "log.h"
#include "png.h"
#include "render/presenter.h"
#include "gpu/renderer.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kWidth = 1280, kHeight = 720;
constexpr UINT kFrames = 3;
constexpr UINT64 kRingSize = 64ull << 20;
constexpr UINT kDescriptorsPerFrame = 8192; // per frame: ring + guest memory views, then resolve-blit views
constexpr UINT kBindless = 1u << 16;      // permanent texture descriptors (0 null 2D, 1 null cube)
constexpr UINT kBindlessBase = kDescriptorsPerFrame * 3;
// The EDRAM colour target: half floats hold the title's HDR (7e3) range.
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT kMaxSurfaces = 64; // EDRAM bases with host surfaces
constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
constexpr uint32_t kDirectFetchBytes = 64 * 1024;

// Registers.
constexpr uint32_t RB_STENCILREFMASK = 0x210D;
constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_ALPHA_REF = 0x210E, RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111, PA_CL_VPORT_YOFFSET = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
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
    ComPtr<ID3DBlob> code;
    TranslatedShader info;
    bool IsReady() const { return state.load(std::memory_order_acquire) == Ready; }
};

struct CachedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint64_t contentHash = 0;
    uint64_t checkedFrame = ~0ull; // content already validated in this frame
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // cubes: matches their faces
};

// A resolve destination backed by a host texture: fetches of this guest
// address read the copy of the render target instead of guest memory.
struct ResolvedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // R32_FLOAT for depth resolves
    bool swapped = false; // resolved with the copy swap bit: guest memory has red and blue exchanged
    // The title resolves differently sized images (bloom chain) to one address within a
    // frame: keep earlier sizes here and swap them in rather than recreating them.
    std::vector<ResolvedTexture> spares;
};

struct FrameResources
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> ring;
    uint8_t* ringCpu = nullptr;
    UINT64 ringUsed = 0;
    // Guest ranges already uploaded this frame: (address << 32 | bytes) -> (offset, sample hash).
    std::unordered_map<uint64_t, std::pair<UINT64, uint64_t>> uploads;
    UINT descriptorBase = 0;
    UINT descriptorsUsed = 0;
    UINT64 fence = 0;
    std::vector<ComPtr<ID3D12Resource>> garbage; // released once this frame's work completes
};

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

struct Renderer
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12GraphicsCommandList> list;
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
    ComPtr<ID3D12PipelineState> depthBlit;                // depth buffer -> R32_FLOAT
    ComPtr<ID3D12PipelineState> colorBlit8, colorBlit16;  // colour -> 8888 / RGBA16F, scaled
    // EDRAM surfaces: one host colour and one host depth target per EDRAM base
    // (RB_COLOR_INFO / RB_DEPTH_INFO bits 0-11), so passes that the title
    // places at different EDRAM offsets do not overwrite each other.
    struct Surface
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_CPU_DESCRIPTOR_HANDLE view{};
    };
    std::unordered_map<uint32_t, Surface> colorSurfaces, depthSurfaces;
    uint32_t colorBase = ~0u, depthBase = ~0u;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
    // The bound surfaces.
    ComPtr<ID3D12Resource> color, depth, readback;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash> pipelines;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> textures;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> cubes;
    std::unordered_map<uint32_t, CachedTexture> mipChains; // resolved textures with their mips, by base
    // Bindless texture descriptors.
    struct BindlessEntry
    {
        UINT index;
        ComPtr<ID3D12Resource> resource; // kept alive while the descriptor may be used
        uint64_t lastFrame;
    };
    std::unordered_map<uint64_t, BindlessEntry> bindless; // by resource pointer, format, cube
    std::vector<UINT> bindlessFree;
    UINT bindlessNext = 2;
    std::unordered_map<uint32_t, ResolvedTexture> resolved; // by physical address

    // Active shaders: the last microcode loaded per stage.
    std::vector<uint32_t> vsCode, psCode;
    uint64_t vsHash = 0, psHash = 0;

    uint64_t frame = 0;
    uint32_t drawsThisFrame = 0, skippedThisFrame = 0;
    bool ok = false;
};

Renderer g_r;

void LogDebugMessages()
{
    if (g_r.device)
        Log("renderer: device removed reason %08lX", g_r.device->GetDeviceRemovedReason());
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
std::map<std::string, uint64_t> g_created; // FM2_PERF: committed resources created, by kind
void CountCreate(const char* what) { g_created[what]++; }

const bool g_dumpHlsl = getenv("FM2_DUMP_HLSL") != nullptr;

uint64_t Fnv(const void* data, size_t bytes, uint64_t h = 0xCBF29CE484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Sampled hash for large texture sources: every 64th dword plus the ends.
uint64_t ContentHash(const uint8_t* data, size_t bytes)
{
    uint64_t h = Fnv(&bytes, sizeof(bytes));
    for (size_t i = 0; i + 4 <= bytes; i += 256)
        h = Fnv(data + i, 4, h);
    if (bytes >= 64)
        h = Fnv(data + bytes - 64, 64, h);
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
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2;
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
cbuffer Blit : register(b0) { float scale; uint swapRB; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthTex.Load(int3(int2(pos.xy), 0));
}
float4 psColor(float4 pos : SV_Position) : SV_Target
{
    float4 c = colorTex.Load(int3(int2(pos.xy), 0)) * scale;
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
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.colorBlit16)), "CreateGraphicsPipelineState(colour blit 16)");
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
    D3D12_DESCRIPTOR_RANGE bindless[2]{};
    for (UINT i = 0; i < 2; i++)
    {
        // 2D textures in space1, cubes in space2, over the same descriptors.
        bindless[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindless[i].NumDescriptors = kBindless;
        bindless[i].RegisterSpace = 1 + i;
        bindless[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[4]{};
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
    params[3].DescriptorTable.NumDescriptorRanges = 2;
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
    g_r.readback = CreateBuffer(UINT64(kWidth) * kHeight * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    for (UINT i = 0; i < kFrames; i++)
    {
        FrameResources& f = g_r.frames[i];
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)), "CreateCommandAllocator");
        f.ring = CreateBuffer(kRingSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        f.ring->Map(0, nullptr, reinterpret_cast<void**>(&f.ringCpu));
        f.descriptorBase = i * kDescriptorsPerFrame;
    }
    Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_r.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g_r.list)),
        "CreateCommandList");
    g_r.list->Close();
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
Renderer::Surface& GetSurface(bool depth, uint32_t base)
{
    auto& map = depth ? g_r.depthSurfaces : g_r.colorSurfaces;
    if (auto it = map.find(base); it != map.end())
        return it->second;
    if (map.size() >= kMaxSurfaces)
        return map.begin()->second; // out of views: share one (should not happen)
    Renderer::Surface& s = map[base];
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
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
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        g_r.device->CreateDepthStencilView(s.resource.Get(), &dsvDesc, s.view);
    }
    else
    {
        td.Format = kColorFormat;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                  IID_PPV_ARGS(&s.resource)),
            "CreateCommittedResource(colour surface)"); CountCreate("colour surface");
        s.view = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        s.view.ptr += SIZE_T(index) * g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_r.device->CreateRenderTargetView(s.resource.Get(), nullptr, s.view);
    }
    Log("renderer: EDRAM %s surface at base %03X", depth ? "depth" : "colour", base);
    return s;
}

// Binds the surfaces for the current RB_COLOR_INFO / RB_DEPTH_INFO bases
// (the list must be open).
void SelectSurfaces(bool force)
{
    const uint32_t colorBase = gpureg::Get(RB_COLOR_INFO) & 0xFFF, depthBase = gpureg::Get(RB_DEPTH_INFO) & 0xFFF;
    if (!force && colorBase == g_r.colorBase && depthBase == g_r.depthBase)
        return;
    if (g_r.colorBase != ~0u)
        g_r.colorSurfaces[g_r.colorBase].state = g_r.colorState;
    Renderer::Surface& c = GetSurface(false, colorBase);
    Renderer::Surface& z = GetSurface(true, depthBase);
    g_r.color = c.resource;
    g_r.colorState = c.state;
    g_r.rtv = c.view;
    g_r.depth = z.resource;
    g_r.dsv = z.view;
    g_r.colorBase = colorBase;
    g_r.depthBase = depthBase;
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_r.list->OMSetRenderTargets(1, &g_r.rtv, FALSE, &g_r.dsv);
}

// Bindless texture descriptors: one per (resource, format, dimension), kept
// while the texture is in use. Entries unused for a few frames (replaced or
// dropped textures) go back to the free list; their resources are released
// once the GPU is done with the frame that last used them.
UINT BindlessIndex(ID3D12Resource* resource, DXGI_FORMAT format, bool cube)
{
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource)) << 8 ^ uint64_t(format) << 1 ^ uint64_t(cube);
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
        return cube ? 1u : 0u; // full: draw with a null texture
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (cube)
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
        if (it->second.lastFrame + kFrames + 2 < g_r.frame)
        {
            Frame().garbage.push_back(std::move(it->second.resource));
            g_r.bindlessFree.push_back(it->second.index);
            it = g_r.bindless.erase(it);
        }
        else
            ++it;
    }
}

void BeginFrameIfNeeded()
{
    if (g_r.listOpen)
        return;
    FrameResources& f = Frame();
    if (g_r.fence->GetCompletedValue() < f.fence)
    {
        const auto w0 = std::chrono::steady_clock::now();
        g_r.fence->SetEventOnCompletion(f.fence, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, INFINITE);
        g_gpuWaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    }
    f.garbage.clear();
    f.allocator->Reset();
    g_r.list->Reset(f.allocator.Get(), nullptr);
    f.ringUsed = 0;
    f.uploads.clear();
    // Per-frame table: t0 this frame's ring, t1 guest physical memory.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.NumElements = UINT(kRingSize / 4);
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
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    g_r.list->SetDescriptorHeaps(1, heaps);
    g_r.list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    SelectSurfaces(true);
    g_r.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_r.listOpen = true;
}

// Allocates from the frame's upload ring; returns the offset (or ~0).
UINT64 RingAlloc(UINT64 bytes, UINT64 alignment = 256)
{
    FrameResources& f = Frame();
    const UINT64 offset = (f.ringUsed + alignment - 1) & ~(alignment - 1);
    if (offset + bytes > kRingSize)
    {
        static UINT64 lastFull = 0;
        if (RendererTracing() && lastFull != g_r.frame)
        {
            lastFull = g_r.frame;
            Log("ring full: %llu bytes requested, %llu used, %llu uploads cached", (unsigned long long)bytes, (unsigned long long)f.ringUsed, (unsigned long long)f.uploads.size());
        }
        return ~0ull;
    }
    f.ringUsed = offset + bytes;
    return offset;
}

// Copies a guest range into the ring once per frame; draws sharing a vertex
// buffer reuse the copy. A sampled hash catches ranges rewritten mid-frame.
UINT64 UploadGuest(uint32_t guest, uint32_t bytes)
{
    FrameResources& f = Frame();
    const uint8_t* src = GuestPtr<uint8_t>(guest);
    // Small buffers are often rewritten between draws (UI quads), so hash all
    // of them; large ones (static geometry) are sampled.
    uint64_t sample = bytes;
    if (bytes <= 16384)
        sample = ContentHash(src, bytes);
    else
        for (uint32_t i = 0; i < 16 && bytes >= 8; i++)
        {
            uint64_t v;
            memcpy(&v, src + (uint64_t(bytes - 8) * i / 15), 8);
            sample = (sample ^ v) * 0x100000001B3ull;
        }
    const uint64_t key = uint64_t(guest) << 32 | bytes;
    if (auto it = f.uploads.find(key); it != f.uploads.end() && it->second.second == sample)
        return it->second.first;
    if (RendererTracing() && bytes > (16u << 10))
        Log("upload: %08X %u bytes (ring %llu)", guest, bytes, (unsigned long long)f.ringUsed);
    const UINT64 offset = RingAlloc(std::max<uint32_t>(bytes, 16) + 16, 16);
    if (offset == ~0ull)
        return offset;
    memcpy(f.ringCpu + offset, src, bytes);
    f.uploads[key] = { offset, sample };
    return offset;
}

// Shader compilation runs on worker threads so the command processor never
// stalls on fxc: a draw whose shaders are still compiling is skipped. The
// bytecode is cached on disk by HLSL hash, so later runs start warm.
std::filesystem::path ShaderCachePath(uint64_t key)
{
    char name[32];
    snprintf(name, sizeof(name), "%016llx.dxbc", (unsigned long long)key);
    return UserDir() / "shadercache" / name;
}

struct ShaderCompiler
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<CompiledShader*> queue;
    std::vector<std::thread> workers;

    void Start()
    {
        std::filesystem::create_directories(UserDir() / "shadercache");
        const unsigned n = std::max(2u, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < n; i++)
            workers.emplace_back([this] { Run(); });
        for (auto& w : workers)
            w.detach();
    }

    void Push(CompiledShader* cs)
    {
        {
            std::lock_guard lock(mutex);
            queue.push_back(cs);
        }
        cv.notify_one();
    }

    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            CompiledShader* cs;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !queue.empty(); });
                cs = queue.front();
                queue.pop_front();
            }
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
        if (ms > 200)
            Log("renderer: %s %016llx compiled in %.0f ms (background)", cs.pixel ? "ps" : "vs", (unsigned long long)cs.hash, ms);
        std::ofstream(ShaderCachePath(cs.cacheKey), std::ios::binary)
            .write(static_cast<const char*>(cs.code->GetBufferPointer()), std::streamsize(cs.code->GetBufferSize()));
        cs.state.store(CompiledShader::Ready, std::memory_order_release);
    }
} g_compiler;

const CompiledShader& GetShader(uint64_t hash, const std::vector<uint32_t>& codeBE, bool pixel)
{
    auto it = g_r.shaders.find(hash);
    if (it != g_r.shaders.end())
        return *it->second;
    static std::once_flag started;
    std::call_once(started, [] { g_compiler.Start(); });
    auto& slot = g_r.shaders[hash];
    slot = std::make_unique<CompiledShader>();
    CompiledShader& cs = *slot;
    cs.hash = hash;
    cs.pixel = pixel;
    const ucode::Shader parsed = ucode::Parse(codeBE.data(), uint32_t(codeBE.size()), pixel);
    cs.info = TranslateShader(parsed);
    if (!cs.info.ok)
    {
        Log("renderer: %s %016llx not translated: %s", pixel ? "ps" : "vs", (unsigned long long)hash, cs.info.error.c_str());
        cs.state = CompiledShader::Failed;
        return cs;
    }
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
    // Debug: FM2_DEBUG_VS=<hash>:<statement> appends a statement to that vertex
    // shader's guest program (it can read r[] and write o[]).
    if (const char* dbg = getenv("FM2_DEBUG_VS"); dbg && !pixel && strtoull(dbg, nullptr, 16) == hash)
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
    // Debug: FM2_TINT_PS=h1,h2,... paints those pixel shaders in distinct flat
    // colours (red, green, blue, yellow, magenta, cyan, white, orange, ...).
    if (const char* list = getenv("FM2_TINT_PS"); list && pixel)
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
    // Debug: FM2_DEBUG_PS=<hash>:<expr> replaces that pixel shader's output, e.g.
    // "a62c809452fcc71b:float4(input.o[4].xyz, 1)".
    // FM2_DEBUG_PS_ALL=<expr> does the same for every pixel shader.
    if (const char* all = getenv("FM2_DEBUG_PS_ALL"); all && pixel)
        if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
            cs.info.hlsl.replace(at, 13, std::string("return ") + all + ";");
    if (const char* dbg = getenv("FM2_DEBUG_PS"); dbg && pixel && strtoull(dbg, nullptr, 16) == hash)
        if (const char* expr = strchr(dbg, ':'))
            if (size_t at = cs.info.hlsl.rfind("return oC[0];"); at != std::string::npos)
                cs.info.hlsl.replace(at, 13, std::string("return ") + (expr + 1) + ";");
    cs.cacheKey = Fnv(cs.info.hlsl.data(), cs.info.hlsl.size(), pixel ? 1 : 2);
    if (std::ifstream in{ ShaderCachePath(cs.cacheKey), std::ios::binary | std::ios::ate })
    {
        const auto size = size_t(in.tellg());
        if (size && SUCCEEDED(D3DCreateBlob(size, &cs.code)))
        {
            in.seekg(0);
            in.read(static_cast<char*>(cs.code->GetBufferPointer()), std::streamsize(size));
            cs.state = CompiledShader::Ready;
            return cs;
        }
    }
    g_compiler.Push(&cs);
    return cs;
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

ID3D12PipelineState* GetPipeline(const CompiledShader& vs, const CompiledShader& ps, bool depthOnly, bool cullable)
{
    const uint32_t blend = gpureg::Get(RB_BLENDCONTROL0);
    uint32_t depth = gpureg::Get(RB_DEPTHCONTROL);
    // Debug: FM2_NO_ZTEST=a-b turns the depth test off for draws a..b-1 of the FM2_DUMP_FRAME frame.
    static uint32_t noZFrom = 0, noZTo = 0;
    static const bool noZParsed = [] { if (const char* s = getenv("FM2_NO_ZTEST")) sscanf(s, "%u-%u", &noZFrom, &noZTo); return true; }();
    static const uint64_t noZFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
    (void)noZParsed;
    if (noZTo && g_r.frame + 1 == noZFrame && g_r.drawsThisFrame >= noZFrom && g_r.drawsThisFrame < noZTo)
        depth &= ~(getenv("FM2_NO_ZWRITE") ? 4u : 2u);
    const uint32_t mask = depthOnly ? 0 : gpureg::Get(RB_COLOR_MASK) & 0xF;
    // Stencil masks (not the reference, which is dynamic) and culling are pipeline state.
    const uint32_t stencil = gpureg::Get(RB_STENCILREFMASK) & 0xFFFF00, mode = gpureg::Get(PA_SU_SC_MODE_CNTL) & 7;
    // Alpha-to-mask (RB_COLORCONTROL bit 4): cut-out foliage and fences.
    const bool alphaToMask = (gpureg::Get(RB_COLORCONTROL) >> 4) & 1;
    PipelineKey key{ g_r.vsHash, g_r.psHash, blend, depth, stencil | (mask << 24) | (mode << 28),
        uint32_t(alphaToMask) | (uint32_t(cullable) << 1) };
    auto it = g_r.pipelines.find(key);
    if (it != g_r.pipelines.end())
        return it->second.Get();

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
    pd.DepthStencilState.DepthEnable = (depth >> 1) & 1;
    pd.DepthStencilState.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
    // Stencil: Xenos ops 0 keep, 1 zero, 2 replace, 3 incr wrap, 4 decr wrap, 5 invert,
    // 6 incr clamp, 7 decr clamp. Back faces use their own ops when enabled.
    static const D3D12_STENCIL_OP kOps[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR,
        D3D12_STENCIL_OP_DECR, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_DECR_SAT };
    static const bool noStencil = getenv("FM2_NO_STENCIL") != nullptr;
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
        ds.BackFace = (depth >> 7) & 1 ? faceOps(20) : ds.FrontFace;
    }
    // Culling: bit 0 front, bit 1 back; bit 2 set means clockwise faces are front.
    static const bool flip = getenv("FM2_CULL_FLIP") != nullptr;
    pd.RasterizerState.FrontCounterClockwise = ((mode & 4) == 0) != flip;
    // Only real triangle primitives keep the guest winding; generated geometry
    // (rectangle corners, point sprites) is never culled.
    pd.RasterizerState.CullMode = !cullable ? D3D12_CULL_MODE_NONE
        : (mode & 3) == 1 ? D3D12_CULL_MODE_FRONT : (mode & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kColorFormat;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g_r.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        Log("renderer: pipeline %016llx/%016llx failed", (unsigned long long)key.vs, (unsigned long long)key.ps);
        g_r.pipelines[key] = nullptr;
        return nullptr;
    }
    g_r.pipelines[key] = pso;
    return pso.Get();
}

// Cube textures: six faces, each a padded 2D slice. Faces the title rendered
// and resolved (dynamic reflections) are copied from their host textures every
// frame; static faces are decoded once and re-decoded when their data changes.
ID3D12Resource* GetCubeTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    const uint32_t w = fetch.Width(), h = fetch.Height();
    if (w != h || w > 2048)
        return nullptr;
    const uint32_t stride = TextureFaceStride(fetch);
    CachedTexture& cached = g_r.cubes[{ fetch.Address(), fetch.Format(), w, h }];
    if (cached.resource && cached.checkedFrame == g_r.frame)
    {
        format = cached.format;
        return cached.resource.Get();
    }
    cached.checkedFrame = g_r.frame;

    // Faces the title resolved into?
    const ResolvedTexture* faces[6]{};
    bool resolved = false;
    for (uint32_t i = 0; i < 6; i++)
    {
        auto it = g_r.resolved.find((fetch.Address() + i * stride) & 0x1FFFFFFF);
        if (it != g_r.resolved.end() && it->second.format != DXGI_FORMAT_R32_FLOAT && it->second.width >= w && it->second.height >= h)
        {
            faces[i] = &it->second;
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
    if (!cached.resource || cached.width != w || cached.height != h || cached.format != want)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 6;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = want;
        if (cached.resource)
            Frame().garbage.push_back(std::move(cached.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                  IID_PPV_ARGS(&cached.resource)),
            "CreateCommittedResource(cube)"); CountCreate("cube");
        cached.width = w;
        cached.height = h;
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
        g_r.list->ResourceBarrier(1, &b);
        if (n)
            g_r.list->ResourceBarrier(n, fb);
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
            const D3D12_BOX box{ 0, 0, 0, w, h, 1 };
            g_r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        for (UINT k = 0; k < n; k++)
            std::swap(fb[k].Transition.StateBefore, fb[k].Transition.StateAfter);
        if (n)
            g_r.list->ResourceBarrier(n, fb);
    }
    else
    {
        // Static: hash all six faces, decode on change.
        const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
        const uint64_t hash = ContentHash(source, size_t(stride) * 6) ^ (uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Endian()) << 44);
        if (cached.contentHash == hash)
            return cached.resource.Get();
        const UINT rowPitch = (w * 4 + 255) & ~255u;
        const UINT64 faceBytes = (UINT64(rowPitch) * h + 511) & ~511ull;
        const UINT64 offset = RingAlloc(faceBytes * 6, 512);
        if (offset == ~0ull)
            return nullptr;
        g_r.list->ResourceBarrier(1, &b);
        for (uint32_t i = 0; i < 6; i++)
        {
            TextureFetch face = fetch;
            face.dwords[1] = (face.dwords[1] & 0xFFF) | ((fetch.Address() + i * stride) & 0xFFFFF000);
            face.dwords[5] = (face.dwords[5] & ~(3u << 9)) | (1u << 9); // decode as 2D
            std::vector<uint8_t> rgba;
            if (!DecodeTexture(face, rgba))
                rgba.assign(size_t(w) * h * 4, 0);
            if (getenv("FM2_DUMP_CUBES"))
            {
                char name[80];
                std::filesystem::create_directories("out/cube");
                snprintf(name, sizeof(name), "out/cube/%08X_face%u_stride%X.png", fetch.Address(), i, stride);
                WritePng(name, w, h, rgba);
            }
            for (uint32_t y = 0; y < h; y++)
                memcpy(Frame().ringCpu + offset + faceBytes * i + UINT64(y) * rowPitch, &rgba[size_t(y) * w * 4], size_t(w) * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = cached.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            src.pResource = Frame().ring.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = offset + faceBytes * i;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, rowPitch };
            g_r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        cached.contentHash = hash;
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_r.list->ResourceBarrier(1, &b);
    return cached.resource.Get();
}

// A render target the title resolved together with its mip chain (it
// downsamples and resolves each level itself): levels 1+ follow each other
// from the fetch constant's mip address, each padded to 32x32 texels and
// 4 KB. Assembled into one mipmapped host texture every frame it is used.
ID3D12Resource* ResolvedMipChain(const TextureFetch& fetch, const ResolvedTexture& level0)
{
    const uint32_t w = std::min(fetch.Width(), level0.width), h = std::min(fetch.Height(), level0.height);
    const UINT bpp = level0.format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
    std::vector<const ResolvedTexture*> levels{ &level0 };
    uint32_t address = fetch.MipAddress();
    for (uint32_t i = 1; i <= fetch.MipMaxLevel() && (w >> i || h >> i); i++)
    {
        const uint32_t lw = std::max(w >> i, 1u), lh = std::max(h >> i, 1u);
        auto it = g_r.resolved.find(address & 0x1FFFFFFF);
        if (it == g_r.resolved.end() || it->second.format != level0.format || it->second.width < lw || it->second.height < lh)
            break;
        levels.push_back(&it->second);
        const uint32_t bytes = ((lw + 31) & ~31u) * ((lh + 31) & ~31u) * bpp;
        address += std::max((bytes + 4095) & ~4095u, 4096u);
    }
    if (levels.size() < 2)
        return nullptr;

    auto& chain = g_r.mipChains[fetch.Address()];
    if (chain.resource && chain.checkedFrame == g_r.frame)
        return chain.resource.Get();
    chain.checkedFrame = g_r.frame;
    const UINT count = UINT(levels.size());
    if (!chain.resource || chain.width != w || chain.height != h || chain.format != level0.format ||
        chain.resource->GetDesc().MipLevels != count)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
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
        chain.width = w;
        chain.height = h;
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
    g_r.list->ResourceBarrier(UINT(b.size()), b.data());
    for (UINT i = 0; i < count; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = chain.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        src.pResource = levels[i]->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{ 0, 0, 0, std::max(w >> i, 1u), std::max(h >> i, 1u), 1 };
        g_r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    for (auto& x : b)
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    g_r.list->ResourceBarrier(UINT(b.size()), b.data());
    return chain.resource.Get();
}

// Decodes (if changed) and uploads a texture; returns its resource.
ID3D12Resource* GetTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (auto it = g_r.resolved.find(fetch.Address()); it != g_r.resolved.end())
    {
        format = it->second.format;
        if (fetch.MipMaxLevel() > 0 && fetch.MipAddress())
            if (ID3D12Resource* chain = ResolvedMipChain(fetch, it->second))
                return chain;
        return it->second.resource.Get();
    }
    if (fetch.Type() != 2 || fetch.Dimension() != 1 || !TextureFormatSupported(fetch.Format()))
        return nullptr;
    const uint32_t w = fetch.Width(), h = fetch.Height();
    auto& cached = g_r.textures[{ fetch.Address(), fetch.Format(), w, h }];
    static const bool dumpAll = getenv("FM2_DUMP_TEXTURES") != nullptr;
    const bool forceDecode = dumpAll && RendererTracing();
    if (cached.resource && cached.checkedFrame == g_r.frame && !forceDecode)
        return cached.resource.Get();
    cached.checkedFrame = g_r.frame;
    const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
    const size_t sourceBytes = std::max<size_t>(size_t(std::max(fetch.Pitch(), w)) * h / 2, 64); // conservative: >= 4 bpp
    // Only what changes the decoded image: sampler state (clamp, filter) varies per draw.
    const uint64_t hash = ContentHash(source, sourceBytes) ^ (uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Pitch()) << 24) ^
        (uint64_t(fetch.Tiled()) << 40) ^ (uint64_t(fetch.Endian()) << 44);
    if (cached.resource && cached.contentHash == hash && !forceDecode)
        return cached.resource.Get();

    std::vector<uint8_t> rgba;
    if (!DecodeTexture(fetch, rgba))
        return nullptr;
    if (RendererTracing())
        Log("texture upload: %08X %s %ux%u (%s)", fetch.Address(), TextureFormatName(fetch.Format()), w, h, cached.resource ? "changed" : "new");
    static const uint32_t dumpAddress = getenv("FM2_DUMP_TEXTURE") ? uint32_t(strtoul(getenv("FM2_DUMP_TEXTURE"), nullptr, 16)) : 0;
    if (RendererTracing() && getenv("FM2_DUMP_TEXTURES"))
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
    if (!cached.resource || cached.width != w || cached.height != h)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (cached.resource)
            Frame().garbage.push_back(std::move(cached.resource));
        Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                  IID_PPV_ARGS(&cached.resource)),
            "CreateCommittedResource(texture)"); CountCreate("texture");
        cached.width = w;
        cached.height = h;
    }
    else
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = cached.resource.Get();
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_r.list->ResourceBarrier(1, &b);
    }
    const UINT rowPitch = (w * 4 + 255) & ~255u;
    const UINT64 offset = RingAlloc(UINT64(rowPitch) * h, 512);
    if (offset == ~0ull)
        return nullptr;
    for (uint32_t y = 0; y < h; y++)
        memcpy(Frame().ringCpu + offset + UINT64(y) * rowPitch, &rgba[size_t(y) * w * 4], size_t(w) * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = cached.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = Frame().ring.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, rowPitch };
    g_r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cached.resource.Get();
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g_r.list->ResourceBarrier(1, &b);
    cached.contentHash = hash;
    return cached.resource.Get();
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
    std::vector<uint32_t> g(count);
    // Primitive restart: a raw index equal to the reset value ends the strip/fan.
    const bool restart = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    const uint32_t resetIndex = gpureg::Get(VGT_MULTI_PRIM_IB_RESET_INDX) & (is32 ? 0xFFFFFF : 0xFFFF);
    std::vector<bool> cut;
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
                cut[i] = true;
            }
            g[i] = raw + offset;
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
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t corner : { 0u, 1u, 2u, 2u, 1u, 3u })
                out.push_back((corner << 29) | (g[i] & 0x1FFFFFFF));
        return true;
    case 4: // triangle list
        out.assign(g.begin(), g.begin() + (count / 3) * 3);
        return true;
    case 6: // triangle strip(s), split at restart indices
    case 5: // triangle fan(s)
        for (uint32_t start = 0; start < count;)
        {
            uint32_t end = start;
            while (end < count && !(end < cut.size() && cut[end]))
                end++;
            for (uint32_t i = start; i + 2 < end; i++)
            {
                const uint32_t k = i - start;
                if (prim == 5)
                    out.insert(out.end(), { g[start], g[i + 1], g[i + 2] });
                else if (k & 1)
                    out.insert(out.end(), { g[i + 1], g[i], g[i + 2] });
                else
                    out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
            }
            start = end + 1;
        }
        return true;
    case 13: // quad list
        for (uint32_t i = 0; i + 3 < count; i += 4)
            out.insert(out.end(), { g[i], g[i + 1], g[i + 2], g[i], g[i + 2], g[i + 3] });
        return true;
    case 8: // rectangle list: first triangle as given, second from the
            // hypotenuse ends and the computed fourth corner
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t pos = uint32_t(out.size());
            out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
            for (uint32_t role : { 1u, 2u, 0u })
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
// Origin (screen rows) and screen rectangle of the tile pass being replayed.
struct TileState
{
    int originY = 0;
    D3D12_RECT rect{ 0, 0, 1 << 14, 1 << 14 };
} g_tile;

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
    return { std::clamp<LONG>(x0, 0, kWidth), std::clamp<LONG>(y0, 0, kHeight), std::clamp<LONG>(x1, 0, kWidth), std::clamp<LONG>(y1, 0, kHeight) };
}

void TransitionColor(D3D12_RESOURCE_STATES to)
{
    if (g_r.colorState == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_r.color.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = g_r.colorState;
    b.Transition.StateAfter = to;
    g_r.list->ResourceBarrier(1, &b);
    g_r.colorState = to;
}

// Draws the resolve rectangle of the colour (or depth) buffer into the
// destination's host texture at the same pixels, converting the format and
// scaling colour by the resolve's exponent bias.
void BlitResolve(bool depth, uint32_t base, uint32_t width, uint32_t height, DXGI_FORMAT format, const D3D12_RECT& rect, float scale, bool swapRB = false)
{
    ResolvedTexture& dest = g_r.resolved[base];
    auto swapVariant = [](ResolvedTexture& a, ResolvedTexture& b) {
        std::swap(a.resource, b.resource);
        std::swap(a.width, b.width);
        std::swap(a.height, b.height);
        std::swap(a.format, b.format);
        std::swap(a.swapped, b.swapped);
    };
    if (!dest.resource || dest.width != width || dest.height != height || dest.format != format)
    {
        bool found = false;
        for (ResolvedTexture& spare : dest.spares)
            if (spare.resource && spare.width == width && spare.height == height && spare.format == format)
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
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = format;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&dest.resource)),
                "CreateCommittedResource(resolve)");
            CountCreate("resolve");
            dest.width = width;
            dest.height = height;
            dest.format = format;
            dest.swapped = false;
        }
    }
    const D3D12_RECT sr{ rect.left, rect.top, std::min<LONG>(rect.right, width), std::min<LONG>(rect.bottom, height) };
    FrameResources& f = Frame();
    if (sr.right <= sr.left || sr.bottom <= sr.top || f.descriptorsUsed >= kDescriptorsPerFrame)
        return;

    const UINT slot = f.descriptorBase + f.descriptorsUsed++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_r.device->CreateShaderResourceView(depth ? g_r.depth.Get() : g_r.color.Get(), &srv, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    const auto rtv = g_r.blitRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_r.device->CreateRenderTargetView(dest.resource.Get(), nullptr, rtv);

    D3D12_RESOURCE_BARRIER b[2]{};
    UINT barriers = 1;
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = dest.resource.Get();
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (depth)
    {
        b[1].Transition.pResource = g_r.depth.Get();
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers = 2;
    }
    else
        TransitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_r.list->ResourceBarrier(barriers, b);

    ID3D12PipelineState* pso = depth ? g_r.depthBlit.Get()
        : format == DXGI_FORMAT_R8G8B8A8_UNORM ? g_r.colorBlit8.Get() : g_r.colorBlit16.Get();
    g_r.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_r.list->SetGraphicsRootSignature(g_r.blitRootSignature.Get());
    g_r.list->SetPipelineState(pso);
    g_r.list->SetGraphicsRootDescriptorTable(0, Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
    const uint32_t blitConstants[2] = { std::bit_cast<uint32_t>(scale), uint32_t(swapRB) };
    g_r.list->SetGraphicsRoot32BitConstants(1, 2, blitConstants, 0);
    const D3D12_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
    g_r.list->RSSetViewports(1, &vp);
    g_r.list->RSSetScissorRects(1, &sr);
    g_r.list->DrawInstanced(3, 1, 0, 0);

    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    g_r.list->ResourceBarrier(barriers, b);
    TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Back to the main targets and root signature.
    g_r.list->OMSetRenderTargets(1, &g_r.rtv, FALSE, &g_r.dsv);
    g_r.list->SetGraphicsRootSignature(g_r.rootSignature.Get());
}

// EDRAM resolve: copy a rectangle of the render target to the destination
// (as a host texture), then optionally clear colour and depth.
void Resolve()
{
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
    SelectSurfaces(false);
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
    static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
    if (traceFrame && g_r.frame + 1 == traceFrame)
        Log("resolve: control %08X dest %08X pitch %08X info %08X rect %ld,%ld-%ld,%ld clear %08X", control, gpureg::Get(RB_COPY_DEST_BASE),
            gpureg::Get(RB_COPY_DEST_PITCH), gpureg::Get(RB_COPY_DEST_INFO), rect.left, rect.top, rect.right, rect.bottom, gpureg::Get(RB_COLOR_CLEAR));
    if (command != 3 && sourceSelect <= 4)
    {
        // Each tile resolves to the destination row it starts at; key the
        // host texture by the image's own base so the tiles land together.
        const uint32_t pitchReg = gpureg::Get(RB_COPY_DEST_PITCH);
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t base = ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) - uint32_t(rect.top) * pitch * 4) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, kWidth);
        const uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + uint32_t(rect.top), 1, kHeight);
        const uint32_t info = gpureg::Get(RB_COPY_DEST_INFO);
        const bool depth = sourceSelect == 4;
        // Destination: 8888 stays 8-bit (clamped, as the hardware does); other
        // formats keep the HDR range. The exponent bias scales colour copies.
        const uint32_t destFormat = (info >> 7) & 0x3F;
        const DXGI_FORMAT format = depth ? DXGI_FORMAT_R32_FLOAT
            : destFormat == 6 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const int bias = int32_t(info << 10) >> 26; // bits 16-21, signed
        // Bit 24 (copy swap): the destination stores red and blue exchanged; the
        // fetch swizzle swaps them back when the title samples it.
        const bool swapRB = !depth && ((info >> 24) & 1);
        BlitResolve(depth, base, width, height, format, rect, depth ? 1.0f : std::ldexp(1.0f, bias));
        g_r.resolved[base].swapped = swapRB;
    }
    // Clears after a tile's resolve prepare the EDRAM for the next tile, which
    // we draw lower on the same host surface: a full-width clear covers every
    // row from the tile down.
    D3D12_RECT clearRect = rect;
    const uint32_t surfacePitch = gpureg::Get(RB_SURFACE_INFO) & 0x3FFF;
    static const bool noClearExtend = getenv("FM2_NO_CLEAR_EXTEND") != nullptr;
    if (!noClearExtend && surfacePitch && uint32_t(rect.right - rect.left) >= std::min<uint32_t>(surfacePitch, kWidth) && rect.left == 0)
        clearRect.bottom = kHeight;
    if ((control >> 8) & 1) // colour clear (ARGB)
    {
        const uint32_t c = gpureg::Get(RB_COLOR_CLEAR);
        const float color[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_r.list->ClearRenderTargetView(g_r.rtv, color, 1, &clearRect);
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
        g_r.list->ClearDepthStencilView(g_r.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, UINT8(v & 0xFF), 1, &clearRect);
    }
}
}

void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords)
{
    std::vector<uint32_t>& code = type == 0 ? g_r.vsCode : g_r.psCode;
    code.assign(microcodeBE, microcodeBE + dwords);
    (type == 0 ? g_r.vsHash : g_r.psHash) = Fnv(code.data(), code.size() * 4) ^ type;
}

namespace
{
void TraceSkip(const char* why, uint32_t initiator)
{
    static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
    if (traceFrame && g_r.frame + 1 == traceFrame)
        Log("skip draw (%s): prim %u count %u mode %u vs %016llx ps %016llx", why, initiator & 0x3F, initiator >> 16,
            gpureg::Get(RB_MODECONTROL) & 7, (unsigned long long)g_r.vsHash, (unsigned long long)g_r.psHash);
}
}

namespace
{
// Performance counters (FM2_PERF=1 logs them once a second).
struct Perf
{
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    double drawSeconds = 0, swapSeconds = 0;
    double section[8] = {};
    uint64_t tablesBuilt = 0, tablesReused = 0;
    std::chrono::steady_clock::time_point mark;
    uint64_t frames = 0, draws = 0, slowFrames = 0;
    double maxFrame = 0;
} g_perf;
void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
}

void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    const auto t0 = std::chrono::steady_clock::now();
    DrawImpl(initiator, dmaBase, dmaSize);
    g_perf.drawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_perf.draws++;
}

namespace
{
void Section(int i)
{
    const auto now = std::chrono::steady_clock::now();
    if (i > 0)
        g_perf.section[i - 1] += std::chrono::duration<double>(now - g_perf.mark).count();
    g_perf.mark = now;
}

void DrawImpl(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
    Section(0);
    static const bool noRender = getenv("FM2_NO_RENDER") != nullptr;
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
    if ((edramMode != 4 && edramMode != 5) || g_r.vsCode.empty() || g_r.psCode.empty())
    {
        TraceSkip("edram mode", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    const CompiledShader& vs = GetShader(g_r.vsHash, g_r.vsCode, false);
    const CompiledShader& ps = GetShader(g_r.psHash, g_r.psCode, true);
    Section(1);
    if (!vs.IsReady() || !ps.IsReady())
    {
        TraceSkip(vs.state == CompiledShader::Failed || ps.state == CompiledShader::Failed ? "shader" : "compiling", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    // Triangle lists and restart-free strips read their indices on the GPU;
    // everything else is expanded here.
    const uint32_t prim = initiator & 0x3F, indexSource = (initiator >> 6) & 3, indexCount = initiator >> 16;
    const bool restartOn = (gpureg::Get(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    static const bool noDirectIndex = getenv("FM2_NO_DIRECT_INDEX") != nullptr;
    const bool directIndex = g_r.physical && !noDirectIndex && (indexSource == 0 || indexSource == 2) &&
        (prim == 4 || prim == 13 || (prim == 6 && !restartOn));
    const uint32_t directVertices = prim == 4 ? indexCount / 3 * 3 : prim == 13 ? indexCount / 4 * 6 : (indexCount >= 3 ? (indexCount - 2) * 3 : 0);
    std::vector<uint32_t> list;
    if (directIndex ? directVertices == 0 : (!BuildIndexList(initiator, dmaBase, dmaSize, list) || list.empty()))
    {
        TraceSkip("primitive", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    Section(2);
    {
        // Debug bisection: FM2_SKIP_DRAWS=a-b drops draws a..b-1 of the FM2_DUMP_FRAME frame.
        static uint32_t skipFrom = 0, skipTo = 0;
        static const bool parsed = [] { if (const char* s = getenv("FM2_SKIP_DRAWS")) sscanf(s, "%u-%u", &skipFrom, &skipTo); return true; }();
        static const uint64_t dumpFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
        (void)parsed;
        // FM2_SKIP_VS=h1,h2,...: drop those vertex shaders in the frame after the dump
        // (written to frame2.png), for an A/B pair under the same camera.
        static const std::vector<uint64_t> skipVs = [] {
            std::vector<uint64_t> v;
            for (const char* s = getenv("FM2_SKIP_VS"); s && *s;)
            {
                char* end;
                v.push_back(strtoull(s, &end, 16));
                s = *end == ',' ? end + 1 : end;
                if (end == s && *end != ',') break;
            }
            return v;
        }();
        const bool skipThis = g_r.frame == dumpFrame && std::find(skipVs.begin(), skipVs.end(), g_r.vsHash) != skipVs.end();
        if (skipThis || (skipTo && g_r.frame + 1 == dumpFrame && g_r.drawsThisFrame >= skipFrom && g_r.drawsThisFrame < skipTo))
        {
            g_r.drawsThisFrame++;
            return;
        }
    }
    BeginFrameIfNeeded();
    SelectSurfaces(false);
    ID3D12PipelineState* pso = GetPipeline(vs, ps, edramMode == 5, (initiator & 0x3F) != 8 && (initiator & 0x3F) != 1);
    if (!pso)
    {
        TraceSkip("pipeline", initiator);
        return;
    }
    FrameResources& f = Frame();

    Section(3);
    // Index list.
    const UINT64 listOffset = directIndex ? 0 : RingAlloc(list.size() * 4, 16);
    if (listOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }
    if (!directIndex)
        memcpy(f.ringCpu + listOffset, list.data(), list.size() * 4);

    // Vertex constants: c[256], bools, fetch table, info.
    struct VSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t vf[96][4];
        uint32_t info[4];
        float point[4];
        float vscale[4], voffset[4];
        uint32_t vte[4];
    };
    struct PSConstants
    {
        float c[256][4];
        uint32_t b[8];
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
        const auto* src = GuestPtr<const __m128i>(gpureg::kBase + gpureg::AluConstants * 4);
        auto* dv = reinterpret_cast<__m128i*>(vc->c);
        auto* dp = reinterpret_cast<__m128i*>(pc->c);
        for (uint32_t i = 0; i < 256; i++)
        {
            _mm_storeu_si128(dv + i, _mm_shuffle_epi8(_mm_loadu_si128(src + i), swap));
            _mm_storeu_si128(dp + i, _mm_shuffle_epi8(_mm_loadu_si128(src + 256 + i), swap));
        }
    }
    for (uint32_t i = 0; i < 8; i++)
        vc->b[i] = pc->b[i] = gpureg::Get(gpureg::BoolConstants + i);
    memset(vc->vf, 0, sizeof(vc->vf));
    for (uint32_t k : vs.info.vertexFetchConstants)
    {
        const uint32_t d0 = gpureg::Get(gpureg::FetchConstants + k * 2), d1 = gpureg::Get(gpureg::FetchConstants + k * 2 + 1);
        const uint32_t address = d0 & 0xFFFFFFFC, bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
        // Large buffers (static geometry) are read in place; small ones may be
        // rewritten before the GPU runs this frame, so they are copied.
        static const bool noDirect = getenv("FM2_NO_DIRECT_FETCH") != nullptr;
        if (g_r.physical && !noDirect && bytes >= kDirectFetchBytes)
        {
            vc->vf[k][0] = address & 0x1FFFFFFF;
            vc->vf[k][1] = d1 & 3;
            vc->vf[k][2] = 1;
            continue;
        }
        const UINT64 dataOffset = UploadGuest(GpuPhysToGuest(address), bytes);
        if (dataOffset == ~0ull)
        {
            TraceSkip("ring full", initiator);
            return;
        }
        vc->vf[k][0] = uint32_t(dataOffset);
        vc->vf[k][1] = d1 & 3;
    }
    vc->info[0] = uint32_t(listOffset);
    vc->info[1] = (initiator & 0x3F) == 1 ? 2 : 0;
    // Viewport transform in the vertex shader (see hlsl.cpp).
    vc->vscale[0] = RegFloat(PA_CL_VPORT_XSCALE);
    vc->vscale[1] = RegFloat(PA_CL_VPORT_YSCALE);
    vc->vscale[2] = RegFloat(PA_CL_VPORT_ZSCALE);
    vc->vscale[3] = 0;
    vc->voffset[0] = RegFloat(PA_CL_VPORT_XOFFSET);
    vc->voffset[1] = RegFloat(PA_CL_VPORT_YOFFSET);
    vc->voffset[2] = RegFloat(PA_CL_VPORT_ZOFFSET);
    vc->voffset[3] = 0;
    vc->vte[0] = gpureg::Get(PA_CL_VTE_CNTL);
    vc->vte[1] = vc->vte[2] = vc->vte[3] = 0;
    if (directIndex)
    {
        const bool is32 = (initiator >> 11) & 1;
        vc->vte[1] = 1u | (prim == 6 ? 2u : 0u) | (prim == 13 ? 64u : 0u) | (is32 ? 4u : 0u) | (indexSource == 2 ? 8u : 0u) | ((dmaSize >> 30) << 4);
        vc->vte[2] = dmaBase & 0x1FFFFFFF & ~(is32 ? 3u : 1u);
        vc->vte[3] = gpureg::Get(VGT_INDX_OFFSET);
    }
    vc->info[2] = kWidth;
    vc->info[3] = kHeight;
    // Point sizes are 12.4 fixed-point radii.
    const uint32_t pointSize = gpureg::Get(PA_SU_POINT_SIZE), pointMinMax = gpureg::Get(PA_SU_POINT_MINMAX);
    vc->point[0] = float(pointSize >> 16) / 8.0f;
    vc->point[1] = float(pointSize & 0xFFFF) / 8.0f;
    vc->point[2] = float(pointMinMax & 0xFFFF) / 8.0f;
    vc->point[3] = float(pointMinMax >> 16) / 8.0f;
    pc->info[1] = (gpureg::Get(SQ_PROGRAM_CNTL) >> 18) & 1 ? (gpureg::Get(SQ_CONTEXT_MISC) >> 8) & 0xFF : 0xFF;

    const uint32_t colorControl = gpureg::Get(RB_COLORCONTROL);
    pc->info[0] = (colorControl >> 3) & 1 ? (colorControl & 7) : 7;
    pc->info2[0] = RegFloat(RB_ALPHA_REF);

    Section(4);
    // Textures are bindless: each one has a permanent descriptor in the heap's
    // bindless region, and the draw passes its index in the constants.
    memset(pc->tinfo, 0, sizeof(pc->tinfo));
    memset(pc->tbias, 0, sizeof(pc->tbias));
    const uint32_t usedSlots = ps.info.usedTextureSlots | vs.info.usedTextureSlots;
    for (uint32_t m = usedSlots; m; m &= m - 1)
    {
        const uint32_t slot = uint32_t(__builtin_ctz(m));
        const TextureFetch fetch = ReadTextureFetch(slot);
        const bool cube = fetch.Dimension() == 3;
        static const uint64_t dumpPs = getenv("FM2_DUMP_PS_TEXTURES") ? strtoull(getenv("FM2_DUMP_PS_TEXTURES"), nullptr, 16) : 0;
        if (dumpPs && g_r.psHash == dumpPs && !cube)
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
        ID3D12Resource* texture = cube ? GetCubeTexture(fetch, format) : GetTexture(fetch, format);
        const uint32_t index = texture ? BindlessIndex(texture, format, cube) : (cube ? 1u : 0u);
        // w: dimension (1 2D, 3 cube); bit 31 + bits 8-19: the fetch swizzle, applied in the
        // shader for textures that live on the GPU (resolves). Decoded ones are swizzled already.
        static const bool noGpuSwizzle = getenv("FM2_NO_GPU_SWIZZLE") != nullptr;
        auto resolvedIt = g_r.resolved.find(fetch.Address() & 0x1FFFFFFF);
        const bool onGpu = !noGpuSwizzle && fetch.Type() == 2 && resolvedIt != g_r.resolved.end();
        // Our resolved textures keep red and blue in place; when the title
        // resolved with the swap bit, its fetch swizzle expects them exchanged.
        uint32_t swizzle = fetch.Swizzle();
        if (onGpu && resolvedIt->second.swapped)
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = (swizzle >> (3 * c)) & 7;
                if (sel == 0 || sel == 2)
                    swizzle ^= (sel ^ (2 - sel)) << (3 * c);
            }
        pc->tinfo[slot][3] = (cube ? 3 : 1) | (onGpu ? (1u << 31) | (swizzle << 8) : 0);
        // Clamp X (dword 0 bits 10-12): 0 wrap, else clamp. Filter: dword 3 bits 19-20 (mag): 0 point.
        const bool clamp = ((fetch.dwords[0] >> 10) & 7) != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc->tinfo[slot][0] = (point ? (clamp ? 2 : 3) : (clamp ? 1 : 0)) | ((uint32_t(fetch.ExpAdjust()) & 0x3F) << 8);
        pc->tbias[slot][0] = fetch.LodBias();
        memcpy(&pc->tbias[slot][1], &index, 4);
        pc->tinfo[slot][1] = fetch.Width();
        pc->tinfo[slot][2] = fetch.Height();
    }

    Section(5);
    // Viewport. The viewport transform happens in the vertex shader; D3D maps 1:1.
    // Tiled passes shift each draw into EDRAM by the window offset. We render in screen
    // space, so tiled draws stay unshifted. The title's rectangle draws (depth resets)
    // reset the window offset to zero and use tile-relative coordinates instead:
    // those land in the current tile, so shift them by the tile's origin.
    D3D12_RECT scissor = WindowScissor();
    float shiftY = 0;
    {
        const uint32_t offsetReg = gpureg::Get(PA_SC_WINDOW_OFFSET);
        const int32_t wy = ((gpureg::Get(PA_SU_SC_MODE_CNTL) >> 16) & 1) ? int32_t((offsetReg >> 16) << 17) >> 17 : 0;
        const bool full = scissor.left <= 0 && scissor.top <= 0 && scissor.right >= LONG(kWidth) && scissor.bottom >= LONG(kHeight);
        if (!full)
        {
            g_tile.originY = -wy;
            g_tile.rect = scissor;
        }
        else if (g_tile.originY != 0)
        {
            shiftY = float(g_tile.originY + wy);
            scissor.left = std::max(scissor.left, g_tile.rect.left);
            scissor.top = std::max(scissor.top, g_tile.rect.top);
            scissor.right = std::min(scissor.right, g_tile.rect.right);
            scissor.bottom = std::min(scissor.bottom, g_tile.rect.bottom);
        }
    }
    const D3D12_VIEWPORT vp{ 0, shiftY, float(kWidth), float(kHeight), 0, 1 };
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
    {
        TraceSkip("empty scissor", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    g_r.list->RSSetViewports(1, &vp);
    g_r.list->RSSetScissorRects(1, &scissor);
    g_r.list->OMSetStencilRef(gpureg::Get(RB_STENCILREFMASK) & 0xFF);
    {
        // RB_BLEND_RED..ALPHA: the constant colour for blend factors 12-15.
        const float factor[4] = { RegFloat(0x2105), RegFloat(0x2106), RegFloat(0x2107), RegFloat(0x2108) };
        g_r.list->OMSetBlendFactor(factor);
    }
    g_r.list->SetPipelineState(pso);
    g_r.list->SetGraphicsRootConstantBufferView(0, f.ring->GetGPUVirtualAddress() + vsOffset);
    g_r.list->SetGraphicsRootConstantBufferView(1, f.ring->GetGPUVirtualAddress() + psOffset);
    g_r.list->SetGraphicsRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), f.descriptorBase, g_r.srvStride));
    g_r.list->SetGraphicsRootDescriptorTable(3, Gpu(g_r.srvHeap.Get(), kBindlessBase, g_r.srvStride));
    static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
    if (traceFrame && g_r.frame + 1 == traceFrame)
    {
        std::string tex;
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            if (((ps.info.usedTextureSlots >> slot) & 1) == 0)
                continue;
            const TextureFetch t = ReadTextureFetch(slot);
            char e[96];
            snprintf(e, sizeof(e), " t%u=%s %ux%u@%08X sw%03X sg%02X d%u", slot, TextureFormatName(t.Format()), t.Width(), t.Height(), t.Address(), t.Swizzle(),
                (t.dwords[0] >> 2) & 0xFF, t.Dimension());
            tex += e;
        }
        Log("    indices (initiator %08X dma %08X offset %u): %u %u %u %u %u %u ... %u", initiator, dmaBase, gpureg::Get(VGT_INDX_OFFSET),
            list.size() > 0 ? list[0] : 0, list.size() > 1 ? list[1] : 0, list.size() > 2 ? list[2] : 0, list.size() > 3 ? list[3] : 0,
            list.size() > 4 ? list[4] : 0, list.size() > 5 ? list[5] : 0, list.empty() ? 0 : list.back());
        Log("draw vs %016llx ps %016llx prim %u n %zu blend %08X mask %X scissor %ld,%ld-%ld,%ld%s", (unsigned long long)g_r.vsHash,
            (unsigned long long)g_r.psHash, initiator & 0x3F, list.size(), gpureg::Get(RB_BLENDCONTROL0), gpureg::Get(RB_COLOR_MASK) & 15,
            scissor.left, scissor.top, scissor.right, scissor.bottom, tex.c_str());
        uint32_t bools[8];
        for (int i = 0; i < 8; i++)
            bools[i] = gpureg::Get(gpureg::BoolConstants + i);
        Log("    bools %08X %08X %08X %08X %08X %08X %08X %08X", bools[0], bools[1], bools[2], bools[3], bools[4], bools[5], bools[6], bools[7]);
        if (getenv("FM2_TRACE_VC"))
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
            std::string nz;
            for (uint32_t i = 0; i * 8 + 5 < ((d1 >> 2) & 0xFFFFFF) && i < 24; i++)
            {
                char t[16];
                snprintf(t, sizeof(t), " %08X", v[i * 8 + 5]);
                nz += t;
            }
            Log("    vf%u normal z per vertex:%s", k, nz.c_str());
        }
        Log("    scissors: generic %08X-%08X screen %08X-%08X window %08X-%08X offset %08X vport %g,%g +%g,%g", gpureg::Get(0x2090), gpureg::Get(0x2091),
            gpureg::Get(0x200E), gpureg::Get(0x200F), gpureg::Get(PA_SC_WINDOW_SCISSOR_TL), gpureg::Get(PA_SC_WINDOW_SCISSOR_BR), gpureg::Get(PA_SC_WINDOW_OFFSET),
            RegFloat(PA_CL_VPORT_XOFFSET), RegFloat(PA_CL_VPORT_YOFFSET), RegFloat(PA_CL_VPORT_XSCALE), RegFloat(PA_CL_VPORT_YSCALE));
        Log("    vport z scale %g offset %g, vte %08X, clip %08X, depth clamp/zrange regs %08X %08X", RegFloat(PA_CL_VPORT_ZSCALE), RegFloat(PA_CL_VPORT_ZOFFSET), gpureg::Get(0x2206), gpureg::Get(0x2204), gpureg::Get(0x2280), gpureg::Get(0x2281));
        Log("    surface %08X color %08X depth %08X depthctl %08X colorctl %08X mode %X", gpureg::Get(0x2000), gpureg::Get(0x2001), gpureg::Get(0x2002),
            gpureg::Get(RB_DEPTHCONTROL), gpureg::Get(RB_COLORCONTROL), gpureg::Get(PA_SU_SC_MODE_CNTL));
        for (uint32_t c : { 0u, 1u, 5u, 6u, 7u, 14u, 22u, 254u, 255u })
            Log("    pc%u = %g %g %g %g", c, pc->c[c][0], pc->c[c][1], pc->c[c][2], pc->c[c][3]);
    }
    g_r.list->DrawInstanced(directIndex ? directVertices : UINT(list.size()), 1, 0, 0);
    Section(6);
    g_r.drawsThisFrame++;
}

}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer);
void RendererOnSwap(uint64_t frameNumber, uint32_t frontBuffer)
{
    const auto t0 = std::chrono::steady_clock::now();
    RendererOnSwapImpl(frameNumber, frontBuffer);
    const auto now = std::chrono::steady_clock::now();
    g_perf.swapSeconds += std::chrono::duration<double>(now - t0).count();
    g_perf.frames++;
    static const bool perf = getenv("FM2_PERF") != nullptr;
    static auto lastSwap = now;
    static double frameDraw = 0, lastDrawTotal = 0;
    {
        const double dt = std::chrono::duration<double>(now - lastSwap).count() * 1000.0;
        lastSwap = now;
        g_perf.maxFrame = std::max(g_perf.maxFrame, dt);
        if (dt > 20.0)
            g_perf.slowFrames++;
        if (perf && dt > 30.0 && dt < 400.0)
            Log("perf: slow frame %llu: %.1f ms (draw work this window %.1f ms over %llu frames)", (unsigned long long)frameNumber, dt,
                g_perf.drawSeconds * 1000.0, (unsigned long long)g_perf.frames);
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
        if (perf)
            Log("perf:   cpu%s | slowest frame %.1f ms, %llu over 20 ms", GuestThreadCpuUsage(window).c_str(), g_perf.maxFrame,
                (unsigned long long)g_perf.slowFrames);
        if (perf && g_perf.frames)
            Log("perf:   waiting on host GPU %.2f ms/frame", g_gpuWaitSeconds * 1000.0 / g_perf.frames);
        if (perf && g_perf.frames)
            Log("perf:   descriptor tables built %.0f, reused %.0f per frame", double(g_perf.tablesBuilt) / g_perf.frames, double(g_perf.tablesReused) / g_perf.frames);
        if (perf && !g_created.empty())
        {
            std::string s;
            for (auto& [what, n] : g_created)
                s += " " + what + ":" + std::to_string(n);
            Log("perf:   resources created this second:%s", s.c_str());
        }
        g_created.clear();
        g_gpuWaitSeconds = 0;
        g_perf = Perf{};
    }
}

void RendererOnSwapImpl(uint64_t frameNumber, uint32_t frontBuffer)
{
    if (!g_r.ok)
        return;
    BeginFrameIfNeeded();
    // Show the front buffer the title resolved to; fall back to the render
    // target if it was never resolved.
    ID3D12Resource* image = g_r.color.Get();
    D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto it = g_r.resolved.find(frontBuffer);
    const bool fromResolve = it != g_r.resolved.end() && it->second.width == kWidth && it->second.height == kHeight &&
        it->second.format == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (fromResolve)
    {
        image = it->second.resource.Get();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_r.list->ResourceBarrier(1, &b);
    }
    // The render target is half float; without an 8-bit resolve there is
    // nothing presentable this frame.
    const bool presenting = fromResolve && PresenterRecordCopy(g_r.list.Get(), image);
    {
        static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
        if (traceFrame && g_r.frame + 1 == traceFrame)
            Log("swap: front buffer %08X, %s", frontBuffer, fromResolve ? "resolved" : "not resolved: showing the render target");
    }

    static const uint64_t dumpFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
    ++g_r.frame;
    g_tile = TileState{};
    static const bool dumpSecond = getenv("FM2_SKIP_VS") != nullptr;
    static const uint64_t dumpEvery = getenv("FM2_DUMP_EVERY") ? strtoull(getenv("FM2_DUMP_EVERY"), nullptr, 10) : 0;
    const bool dump = fromResolve && ((dumpFrame && (g_r.frame == dumpFrame || (dumpSecond && g_r.frame == dumpFrame + 1))) || (dumpEvery && g_r.frame % dumpEvery == 0));
    if (dump)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_r.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, kWidth * 4 };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = image;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (fromResolve)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = image;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g_r.list->ResourceBarrier(1, &b);
    }

    g_r.list->Close();
    ID3D12CommandList* lists[] = { g_r.list.Get() };
    g_r.queue->ExecuteCommandLists(1, lists);
    if (presenting)
        PresenterPresent();
    Frame().fence = g_r.nextFence++;
    g_r.queue->Signal(g_r.fence.Get(), Frame().fence);
    g_r.listOpen = false;

    if (dump)
    {
        g_r.fence->SetEventOnCompletion(Frame().fence, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, INFINITE);
        uint8_t* data = nullptr;
        g_r.readback->Map(0, nullptr, reinterpret_cast<void**>(&data));
        std::vector<uint8_t> rgba(data, data + size_t(kWidth) * kHeight * 4);
        g_r.readback->Unmap(0, nullptr);
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
        static const uint64_t every = getenv("FM2_DUMP_EVERY") ? strtoull(getenv("FM2_DUMP_EVERY"), nullptr, 10) : 0;
        if (every && g_r.frame % every == 0 && g_r.frame != dumpFrame)
        {
            char name[64];
            std::filesystem::create_directories("out/frames");
            snprintf(name, sizeof(name), "out/frames/%05llu.png", (unsigned long long)g_r.frame);
            WritePng(name, kWidth, kHeight, rgba);
        }
        else
            WritePng(g_r.frame == dumpFrame ? "frame.png" : "frame2.png", kWidth, kHeight, rgba);
        Log("renderer: wrote frame.png (frame %llu: %u draws, %u skipped)", (unsigned long long)frameNumber, g_r.drawsThisFrame,
            g_r.skippedThisFrame);
    }
    if (frameNumber % 600 == 0)
        Log("renderer: frame %llu: %u draws, %u skipped, %zu shaders, %zu pipelines, %zu textures", (unsigned long long)frameNumber,
            g_r.drawsThisFrame, g_r.skippedThisFrame, g_r.shaders.size(), g_r.pipelines.size(), g_r.textures.size());
    g_r.drawsThisFrame = g_r.skippedThisFrame = 0;
    g_r.frameIndex = (g_r.frameIndex + 1) % kFrames;
}

bool RendererTracing()
{
    static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
    return traceFrame && g_r.frame + 1 == traceFrame;
}
