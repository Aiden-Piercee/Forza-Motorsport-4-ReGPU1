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
constexpr UINT kDescriptorsPerFrame = 34 * 8192; // 8192 draws
constexpr UINT kTableSize = 34; // t0 upload ring, t1-t32 textures, t33 guest physical memory
constexpr uint32_t kDirectFetchBytes = 64 * 1024;

// Registers.
constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_ALPHA_REF = 0x210E, RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, VGT_INDX_OFFSET = 0x2102;
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
};

// A resolve destination backed by a host texture: fetches of this guest
// address read the copy of the render target instead of guest memory.
struct ResolvedTexture
{
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // R32_FLOAT for depth resolves
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
    ComPtr<ID3D12Resource> color, depth, readback;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> pipelines;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> textures;
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
    Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource(buffer)");
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
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: blit root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.blitRootSignature)),
        "CreateRootSignature(blit)");

    static const char kSource[] = R"(
Texture2D<float> depthTex : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target
{
    return depthTex.Load(int3(int2(pos.xy), 0));
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
}

void Init()
{
    g_r.device = RenderDevice();
    g_r.queue = RenderQueue();
    auto* d = g_r.device;

    // Root signature: b0, b1 root CBVs; one table t0-t32; four static samplers.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTableSize;
    D3D12_ROOT_PARAMETER params[3]{};
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
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 4;
    rs.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        Fatal("renderer: root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    Check(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_r.rootSignature)), "CreateRootSignature");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescriptorsPerFrame * kFrames;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.srvHeap)), "CreateDescriptorHeap(srv)");
    g_r.srvStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.stagingHeap)), "CreateDescriptorHeap(staging)");
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(nullptr, &nullSrv, Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride));

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

    // Colour and depth targets.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_r.color)),
        "CreateCommittedResource(color)");
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; // typeless: depth resolves read it
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.0f;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&g_r.depth)),
        "CreateCommittedResource(depth)");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.rtvHeap)), "CreateDescriptorHeap(rtv)");
    d->CreateRenderTargetView(g_r.color.Get(), nullptr, g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.dsvHeap)), "CreateDescriptorHeap(dsv)");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d->CreateDepthStencilView(g_r.depth.Get(), &dsvDesc, g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart());
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

FrameResources& Frame() { return g_r.frames[g_r.frameIndex]; }

void BeginFrameIfNeeded()
{
    if (g_r.listOpen)
        return;
    FrameResources& f = Frame();
    if (g_r.fence->GetCompletedValue() < f.fence)
    {
        g_r.fence->SetEventOnCompletion(f.fence, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, INFINITE);
    }
    f.garbage.clear();
    f.allocator->Reset();
    g_r.list->Reset(f.allocator.Get(), nullptr);
    f.ringUsed = 0;
    f.uploads.clear();
    f.descriptorsUsed = 0;
    ID3D12DescriptorHeap* heaps[] = { g_r.srvHeap.Get() };
    g_r.list->SetDescriptorHeaps(1, heaps);
    g_r.list->SetGraphicsRootSignature(g_r.rootSignature.Get());
    if (g_r.colorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_r.color.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = g_r.colorState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_r.list->ResourceBarrier(1, &b);
        g_r.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    const auto rtv = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_r.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
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
    uint64_t sample = bytes;
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
            cs.pixel ? "ps_5_1" : "vs_5_1", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &cs.code, &errors);
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
        Log("renderer: %s %016llx:\n%s", pixel ? "ps" : "vs", (unsigned long long)hash, cs.info.hlsl.c_str());
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
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
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

ID3D12PipelineState* GetPipeline(const CompiledShader& vs, const CompiledShader& ps, bool depthOnly)
{
    const uint32_t blend = gpureg::Get(RB_BLENDCONTROL0), depth = gpureg::Get(RB_DEPTHCONTROL);
    const uint32_t mask = depthOnly ? 0 : gpureg::Get(RB_COLOR_MASK) & 0xF;
    char key[96];
    snprintf(key, sizeof(key), "%016llx%016llx%08x%08x%x", (unsigned long long)g_r.vsHash, (unsigned long long)g_r.psHash, blend, depth, mask);
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
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = (depth >> 1) & 1;
    pd.DepthStencilState.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g_r.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        Log("renderer: pipeline %s failed", key);
        g_r.pipelines[key] = nullptr;
        return nullptr;
    }
    g_r.pipelines[key] = pso;
    return pso.Get();
}

// Decodes (if changed) and uploads a texture; returns its resource.
ID3D12Resource* GetTexture(const TextureFetch& fetch, DXGI_FORMAT& format)
{
    format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (auto it = g_r.resolved.find(fetch.Address()); it != g_r.resolved.end())
    {
        format = it->second.format;
        return it->second.resource.Get();
    }
    if (fetch.Type() != 2 || fetch.Dimension() != 1 || !TextureFormatSupported(fetch.Format()))
        return nullptr;
    const uint32_t w = fetch.Width(), h = fetch.Height();
    auto& cached = g_r.textures[{ fetch.Address(), fetch.Format(), w, h }];
    if (cached.resource && cached.checkedFrame == g_r.frame)
        return cached.resource.Get();
    cached.checkedFrame = g_r.frame;
    const uint8_t* source = GuestPtr<uint8_t>(GpuPhysToGuest(fetch.Address()));
    const size_t sourceBytes = std::max<size_t>(size_t(std::max(fetch.Pitch(), w)) * h / 2, 64); // conservative: >= 4 bpp
    // Only what changes the decoded image: sampler state (clamp, filter) varies per draw.
    const uint64_t hash = ContentHash(source, sourceBytes) ^ (uint64_t(fetch.Swizzle()) << 8) ^ (uint64_t(fetch.Pitch()) << 24) ^
        (uint64_t(fetch.Tiled()) << 40) ^ (uint64_t(fetch.Endian()) << 44);
    if (cached.resource && cached.contentHash == hash)
        return cached.resource.Get();

    std::vector<uint8_t> rgba;
    if (!DecodeTexture(fetch, rgba))
        return nullptr;
    if (RendererTracing())
        Log("texture upload: %08X %s %ux%u (%s)", fetch.Address(), TextureFormatName(fetch.Format()), w, h, cached.resource ? "changed" : "new");
    static const uint32_t dumpAddress = getenv("FM2_DUMP_TEXTURE") ? uint32_t(strtoul(getenv("FM2_DUMP_TEXTURE"), nullptr, 16)) : 0;
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
            "CreateCommittedResource(texture)");
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
    if (source == 0) // DMA
    {
        const uint32_t base = GpuPhysToGuest(dmaBase & ~(is32 ? 3u : 1u));
        const uint32_t endian = dmaSize >> 30;
        const uint32_t available = (dmaSize & 0xFFFFFF) * (is32 ? 1 : 2);
        for (uint32_t i = 0; i < count && i < available; i++)
            g[i] = ReadIndex(base, i, is32, endian) + offset;
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
    case 6: // triangle strip
        for (uint32_t i = 0; i + 2 < count; i++)
        {
            if (i & 1)
                out.insert(out.end(), { g[i + 1], g[i], g[i + 2] });
            else
                out.insert(out.end(), { g[i], g[i + 1], g[i + 2] });
        }
        return true;
    case 5: // triangle fan
        for (uint32_t i = 1; i + 1 < count; i++)
            out.insert(out.end(), { g[0], g[i], g[i + 1] });
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

// EDRAM resolve: copy a rectangle of the render target to the destination
// (as a host texture), then optionally clear colour and depth.
void Resolve()
{
    if (!g_r.ok)
        Init();
    BeginFrameIfNeeded();
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
    if (command != 3 && sourceSelect == 4) // depth copy (shadow maps)
    {
        const uint32_t pitchReg = gpureg::Get(RB_COPY_DEST_PITCH);
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t base = ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) - uint32_t(rect.top) * pitch * 4) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, kWidth);
        const uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + uint32_t(rect.top), 1, kHeight);
        ResolvedTexture& dest = g_r.resolved[base];
        if (!dest.resource || dest.width != width || dest.height != height || dest.format != DXGI_FORMAT_R32_FLOAT)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_R32_FLOAT;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (dest.resource)
                Frame().garbage.push_back(std::move(dest.resource));
            Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&dest.resource)),
                "CreateCommittedResource(depth resolve)");
            dest.width = width;
            dest.height = height;
            dest.format = DXGI_FORMAT_R32_FLOAT;
        }
        FrameResources& f = Frame();
        if (f.descriptorsUsed < kDescriptorsPerFrame)
        {
            const UINT slot = f.descriptorBase + f.descriptorsUsed++;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_r.device->CreateShaderResourceView(g_r.depth.Get(), &srv, Cpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
            const auto rtv = g_r.blitRtvHeap->GetCPUDescriptorHandleForHeapStart();
            g_r.device->CreateRenderTargetView(dest.resource.Get(), nullptr, rtv);

            D3D12_RESOURCE_BARRIER b[2]{};
            b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[0].Transition.pResource = g_r.depth.Get();
            b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b[1].Transition.pResource = dest.resource.Get();
            b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_r.list->ResourceBarrier(2, b);
            g_r.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            g_r.list->SetGraphicsRootSignature(g_r.blitRootSignature.Get());
            g_r.list->SetPipelineState(g_r.depthBlit.Get());
            g_r.list->SetGraphicsRootDescriptorTable(0, Gpu(g_r.srvHeap.Get(), slot, g_r.srvStride));
            const D3D12_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
            const D3D12_RECT sr{ rect.left, rect.top, std::min<LONG>(rect.right, width), std::min<LONG>(rect.bottom, height) };
            g_r.list->RSSetViewports(1, &vp);
            g_r.list->RSSetScissorRects(1, &sr);
            g_r.list->DrawInstanced(3, 1, 0, 0);
            std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
            std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
            g_r.list->ResourceBarrier(2, b);
            // Back to the main targets and root signature.
            const auto mainRtv = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            const auto dsv = g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_r.list->OMSetRenderTargets(1, &mainRtv, FALSE, &dsv);
            g_r.list->SetGraphicsRootSignature(g_r.rootSignature.Get());
        }
    }
    if (command != 3 && sourceSelect < 4) // colour copy
    {
        // Each tile resolves to the destination row it starts at; key the
        // host texture by the image's own base so the tiles land together.
        const uint32_t pitchReg = gpureg::Get(RB_COPY_DEST_PITCH);
        const uint32_t pitch = pitchReg & 0x3FFF;
        const uint32_t base = ((gpureg::Get(RB_COPY_DEST_BASE) & 0x1FFFFFFF) - uint32_t(rect.top) * pitch * 4) & 0x1FFFFFFF;
        const uint32_t width = std::clamp<uint32_t>(pitch, 1, kWidth);
        const uint32_t height = std::clamp<uint32_t>(((pitchReg >> 16) & 0x3FFF) + uint32_t(rect.top), 1, kHeight);
        ResolvedTexture& dest = g_r.resolved[base];
        if (!dest.resource || dest.width != width || dest.height != height || dest.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (dest.resource)
                Frame().garbage.push_back(std::move(dest.resource));
            Check(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&dest.resource)),
                "CreateCommittedResource(resolve)");
            dest.width = width;
            dest.height = height;
            dest.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        const D3D12_BOX box{ UINT(rect.left), UINT(rect.top), 0, UINT(std::min<LONG>(rect.right, width)), UINT(std::min<LONG>(rect.bottom, height)), 1 };
        if (box.right > box.left && box.bottom > box.top)
        {
            TransitionColor(D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = dest.resource.Get();
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_r.list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = dest.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = g_r.color.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g_r.list->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            g_r.list->ResourceBarrier(1, &b);
            TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    if ((control >> 8) & 1) // colour clear (ARGB)
    {
        const uint32_t c = gpureg::Get(RB_COLOR_CLEAR);
        const float color[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        TransitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_r.list->ClearRenderTargetView(g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart(), color, 1, &rect);
    }
    if ((control >> 9) & 1) // depth/stencil clear: depth 24 bits | stencil 8
    {
        const uint32_t v = gpureg::Get(RB_DEPTH_CLEAR);
        g_r.list->ClearDepthStencilView(g_r.dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            float(v >> 8) / 16777215.0f, UINT8(v & 0xFF), 1, &rect);
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

void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
{
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
    if (!vs.IsReady() || !ps.IsReady())
    {
        TraceSkip(vs.state == CompiledShader::Failed || ps.state == CompiledShader::Failed ? "shader" : "compiling", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    std::vector<uint32_t> list;
    if (!BuildIndexList(initiator, dmaBase, dmaSize, list) || list.empty())
    {
        TraceSkip("primitive", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    BeginFrameIfNeeded();
    ID3D12PipelineState* pso = GetPipeline(vs, ps, edramMode == 5);
    if (!pso)
    {
        TraceSkip("pipeline", initiator);
        return;
    }
    FrameResources& f = Frame();

    // Index list.
    const UINT64 listOffset = RingAlloc(list.size() * 4, 16);
    if (listOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }
    memcpy(f.ringCpu + listOffset, list.data(), list.size() * 4);

    // Vertex constants: c[256], bools, fetch table, info.
    struct VSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t vf[96][4];
        uint32_t info[4];
        float point[4];
    };
    struct PSConstants
    {
        float c[256][4];
        uint32_t b[8];
        uint32_t info[4];
        float info2[4];
        uint32_t tinfo[32][4];
    };
    const UINT64 vsOffset = RingAlloc(sizeof(VSConstants)), psOffset = RingAlloc(sizeof(PSConstants));
    if (vsOffset == ~0ull || psOffset == ~0ull)
    {
        TraceSkip("ring full", initiator);
        return;
    }
    auto* vc = reinterpret_cast<VSConstants*>(f.ringCpu + vsOffset);
    auto* pc = reinterpret_cast<PSConstants*>(f.ringCpu + psOffset);
    for (uint32_t i = 0; i < 256 * 4; i++)
    {
        const uint32_t v = gpureg::Get(gpureg::AluConstants + i), p = gpureg::Get(gpureg::AluConstants + 1024 + i);
        memcpy(&vc->c[i / 4][i % 4], &v, 4);
        memcpy(&pc->c[i / 4][i % 4], &p, 4);
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
        if (g_r.physical && bytes >= kDirectFetchBytes)
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
    const bool clipSpace = gpureg::Get(PA_CL_VTE_CNTL) & 1;
    vc->info[0] = uint32_t(listOffset);
    vc->info[1] = (clipSpace ? 0 : 1) | ((initiator & 0x3F) == 1 ? 2 : 0);
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

    // Descriptor table: t0 = upload ring (raw), t1+ = textures.
    if (f.descriptorsUsed + kTableSize > kDescriptorsPerFrame)
    {
        TraceSkip("descriptors", initiator);
        return;
    }
    const UINT table = f.descriptorBase + f.descriptorsUsed;
    f.descriptorsUsed += kTableSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    raw.Buffer.NumElements = UINT(kRingSize / 4);
    raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g_r.device->CreateShaderResourceView(f.ring.Get(), &raw, Cpu(g_r.srvHeap.Get(), table, g_r.srvStride));
    if (g_r.physical)
    {
        raw.Buffer.NumElements = UINT(g_r.physical->GetDesc().Width / 4);
        g_r.device->CreateShaderResourceView(g_r.physical.Get(), &raw, Cpu(g_r.srvHeap.Get(), table + 33, g_r.srvStride));
    }
    else
        g_r.device->CopyDescriptorsSimple(1, Cpu(g_r.srvHeap.Get(), table + 33, g_r.srvStride), Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    memset(pc->tinfo, 0, sizeof(pc->tinfo));
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        const auto dest = Cpu(g_r.srvHeap.Get(), table + 1 + slot, g_r.srvStride);
        ID3D12Resource* texture = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        TextureFetch fetch{};
        if (((ps.info.usedTextureSlots | vs.info.usedTextureSlots) >> slot) & 1)
        {
            fetch = ReadTextureFetch(slot);
            texture = GetTexture(fetch, format);
        }
        if (!texture)
        {
            g_r.device->CopyDescriptorsSimple(1, dest, Cpu(g_r.stagingHeap.Get(), 0, g_r.srvStride), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            continue;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_r.device->CreateShaderResourceView(texture, &srv, dest);
        // Clamp X (dword 0 bits 10-12): 0 wrap, else clamp. Filter: dword 3 bits 19-20 (mag): 0 point.
        const bool clamp = ((fetch.dwords[0] >> 10) & 7) != 0;
        const bool point = ((fetch.dwords[3] >> 19) & 3) == 0;
        pc->tinfo[slot][0] = point ? (clamp ? 2 : 3) : (clamp ? 1 : 0);
        pc->tinfo[slot][1] = fetch.Width();
        pc->tinfo[slot][2] = fetch.Height();
    }

    // Viewport.
    D3D12_VIEWPORT vp{ 0, 0, float(kWidth), float(kHeight), 0, 1 };
    if (clipSpace)
    {
        const float xs = std::fabs(RegFloat(PA_CL_VPORT_XSCALE)), ys = std::fabs(RegFloat(PA_CL_VPORT_YSCALE));
        if (xs > 0 && ys > 0)
            vp = { RegFloat(PA_CL_VPORT_XOFFSET) - xs, RegFloat(PA_CL_VPORT_YOFFSET) - ys, xs * 2, ys * 2, 0, 1 };
        const float zs = RegFloat(PA_CL_VPORT_ZSCALE), zo = RegFloat(PA_CL_VPORT_ZOFFSET);
        if (zs != 0)
        {
            vp.MinDepth = std::clamp(zo, 0.0f, 1.0f);
            vp.MaxDepth = std::clamp(zo + zs, 0.0f, 1.0f);
        }
    }
    const D3D12_RECT scissor = WindowScissor();
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
    {
        TraceSkip("empty scissor", initiator);
        g_r.skippedThisFrame++;
        return;
    }
    g_r.list->RSSetViewports(1, &vp);
    g_r.list->RSSetScissorRects(1, &scissor);
    g_r.list->SetPipelineState(pso);
    g_r.list->SetGraphicsRootConstantBufferView(0, f.ring->GetGPUVirtualAddress() + vsOffset);
    g_r.list->SetGraphicsRootConstantBufferView(1, f.ring->GetGPUVirtualAddress() + psOffset);
    g_r.list->SetGraphicsRootDescriptorTable(2, Gpu(g_r.srvHeap.Get(), table, g_r.srvStride));
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
            snprintf(e, sizeof(e), " t%u=%s %ux%u@%08X sw%03X", slot, TextureFormatName(t.Format()), t.Width(), t.Height(), t.Address(), t.Swizzle());
            tex += e;
        }
        Log("draw vs %016llx ps %016llx prim %u n %zu blend %08X mask %X scissor %ld,%ld-%ld,%ld%s", (unsigned long long)g_r.vsHash,
            (unsigned long long)g_r.psHash, initiator & 0x3F, list.size(), gpureg::Get(RB_BLENDCONTROL0), gpureg::Get(RB_COLOR_MASK) & 15,
            scissor.left, scissor.top, scissor.right, scissor.bottom, tex.c_str());
        uint32_t bools[8];
        for (int i = 0; i < 8; i++)
            bools[i] = gpureg::Get(gpureg::BoolConstants + i);
        Log("    bools %08X %08X %08X %08X", bools[0], bools[1], bools[2], bools[3]);
        for (uint32_t c : { 5u, 6u, 7u, 14u, 22u, 254u, 255u })
            Log("    pc%u = %g %g %g %g", c, pc->c[c][0], pc->c[c][1], pc->c[c][2], pc->c[c][3]);
    }
    g_r.list->DrawInstanced(UINT(list.size()), 1, 0, 0);
    g_r.drawsThisFrame++;
}

void RendererOnSwap(uint64_t frameNumber, uint32_t frontBuffer)
{
    if (!g_r.ok)
        return;
    BeginFrameIfNeeded();
    // Show the front buffer the title resolved to; fall back to the render
    // target if it was never resolved.
    ID3D12Resource* image = g_r.color.Get();
    D3D12_RESOURCE_STATES imageState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto it = g_r.resolved.find(frontBuffer);
    const bool fromResolve = it != g_r.resolved.end() && it->second.width == kWidth && it->second.height == kHeight;
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
    else
        TransitionColor(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const bool presenting = PresenterRecordCopy(g_r.list.Get(), image);
    {
        static const uint64_t traceFrame = getenv("FM2_TRACE_DRAWS") ? strtoull(getenv("FM2_TRACE_DRAWS"), nullptr, 10) : 0;
        if (traceFrame && g_r.frame + 1 == traceFrame)
            Log("swap: front buffer %08X, %s", frontBuffer, fromResolve ? "resolved" : "not resolved: showing the render target");
    }

    static const uint64_t dumpFrame = getenv("FM2_DUMP_FRAME") ? strtoull(getenv("FM2_DUMP_FRAME"), nullptr, 10) : 0;
    ++g_r.frame;
    const bool dump = dumpFrame && g_r.frame == dumpFrame;
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
        WritePng("frame.png", kWidth, kHeight, rgba);
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
