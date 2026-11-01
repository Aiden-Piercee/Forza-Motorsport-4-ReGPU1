#include "gpu/edram_targets.h"

#include <algorithm>
#include <string>

#include <d3dcompiler.h>

#include "gpu/edram_transfer_hlsl.h"
#include "gpu/recorded_list.h"
#include "log.h"

namespace regpu
{
using Microsoft::WRL::ComPtr;

namespace edram
{
namespace
{
constexpr uint32_t kMaxTargets = 256; // per kind (colour, depth); a frame uses a handful
constexpr uint32_t kMaxHostSide = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
constexpr DXGI_FORMAT kDepthResource = DXGI_FORMAT_R32G8X24_TYPELESS, kDepthView = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
uint32_t Samples(const SurfaceKey& k) { return 1u << k.msaa; }
} // namespace

HostFormat HostColorFormat(uint32_t f)
{
    switch (f)
    {
    case k8888:
    case k8888Gamma: // (gamma is applied by the draw's shader; the stored bits are the guest's)
        return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT };
    case k2101010:
        return { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT };
    case k2101010Float: // 7e3 fits in half floats exactly
        return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT };
    case k1616: // fixed point -32..32: snorm, scaled by the draw's shader
        return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT };
    case k16161616:
        return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT };
    case k1616Float:
        return { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UINT };
    case k16161616Float:
        return { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT };
    case k32Float:
        return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT };
    case k3232Float:
        return { DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_UINT };
    default: // 8, 9, 11, 13: not formats the hardware has; stored as 8_8_8_8
        return { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT };
    }
}

bool TargetCache::Init(ID3D12Device* device, uint32_t scaleX, uint32_t scaleY)
{
    std::lock_guard lock(mutex_);
    scaleX_ = std::clamp<uint32_t>(scaleX, 1, 4);
    scaleY_ = std::clamp<uint32_t>(scaleY, 1, 4);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    stencilRef_ = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) && options.PSSpecifiedStencilRefSupported;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
    {
        Log("edram: transfer root signature failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kMaxTargets * 2; // draw and raw views
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_))))
        return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kMaxTargets;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_))))
        return false;
    rtvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device_ = device;
    Log("edram: render target cache at %ux%u, stencil %s", scaleX_, scaleY_, stencilRef_ ? "in one pass (SV_StencilRef)" : "by bit (8 passes)");
    return true;
}

HostTarget* TargetCache::GetOrCreate(const SurfaceKey& key, const RecordContext& rc)
{
    if (auto it = targets_.find(key.Packed()); it != targets_.end())
        return &it->second;
    if (key.depth ? dsvUsed_ >= kMaxTargets : rtvUsed_ + 2 > kMaxTargets * 2)
    {
        stats_.skipped++;
        return nullptr;
    }
    HostTarget t;
    t.key = key;
    t.width = std::min(key.WidthPixels() * scaleX_, kMaxHostSide);
    t.height = std::min(key.HeightPixels() * scaleY_, kMaxHostSide);
    t.format = key.depth ? HostFormat{ kDepthResource, kDepthView, kDepthView } : HostColorFormat(key.format);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = t.width;
    td.Height = t.height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = Samples(key);
    td.Format = t.format.resource;
    td.Flags = key.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    t.state = key.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, nullptr, IID_PPV_ARGS(&t.resource))))
    {
        Log("edram: could not create a %ux%u %s target (base %u, pitch %u tiles, msaa %u, format %u)", t.width, t.height,
            key.depth ? "depth" : "colour", key.baseTiles, key.pitchTiles32, key.msaa, key.format);
        stats_.skipped++;
        return nullptr;
    }
    // EDRAM holds whatever was there; the host starts from zero (render targets must be
    // initialised before use).
    if (key.depth)
    {
        t.drawView = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        t.drawView.ptr += SIZE_T(dsvUsed_++) * dsvStride_;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthView;
        dv.ViewDimension = td.SampleDesc.Count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(t.resource.Get(), &dv, t.drawView);
        rc.list->ClearDepthStencilView(t.drawView, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    }
    else
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE start = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        t.drawView.ptr = start.ptr + SIZE_T(rtvUsed_++) * rtvStride_;
        t.rawView.ptr = start.ptr + SIZE_T(rtvUsed_++) * rtvStride_;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.ViewDimension = td.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Format = t.format.draw;
        device_->CreateRenderTargetView(t.resource.Get(), &rv, t.drawView);
        rv.Format = t.format.raw;
        device_->CreateRenderTargetView(t.resource.Get(), &rv, t.rawView);
        const FLOAT zero[4] = {};
        rc.list->ClearRenderTargetView(t.rawView, zero, 0, nullptr);
    }
    stats_.targetsCreated++;
    Log("edram: %s target base %u pitch %u tiles msaa %u format %u: %ux%u host pixels", key.depth ? "depth" : "colour", key.baseTiles,
        key.pitchTiles32, key.msaa, key.format, t.width, t.height);
    return &targets_.emplace(key.Packed(), std::move(t)).first->second;
}

void TargetCache::Transition(HostTarget& t, D3D12_RESOURCE_STATES to, const RecordContext& rc)
{
    if (t.state == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = to;
    rc.list->ResourceBarrier(1, &b);
    t.state = to;
}

std::vector<D3D12_RECT> TargetCache::TileRects(const SurfaceKey& dest, uint32_t startTiles, uint32_t endTiles) const
{
    std::vector<D3D12_RECT> rects;
    const uint32_t pitch = dest.PitchTiles();
    if (!pitch || endTiles <= startTiles)
        return rects;
    const uint32_t rel0 = (startTiles + kTileCount - dest.baseTiles) & (kTileCount - 1), rel1 = rel0 + (endTiles - startTiles);
    // A tile in host pixels.
    const LONG tw = LONG(((kTileWidthSamples >> uint32_t(dest.Is64bpp())) >> SamplesXLog2(dest.msaa)) * scaleX_);
    const LONG th = LONG((kTileHeightSamples >> SamplesYLog2(dest.msaa)) * scaleY_);
    const LONG maxX = LONG(std::min(dest.WidthPixels() * scaleX_, kMaxHostSide)), maxY = LONG(std::min(dest.HeightPixels() * scaleY_, kMaxHostSide));
    auto add = [&](uint32_t row0, uint32_t row1, uint32_t col0, uint32_t col1) {
        D3D12_RECT r{ LONG(col0) * tw, LONG(row0) * th, std::min(LONG(col1) * tw, maxX), std::min(LONG(row1) * th, maxY) };
        if (r.right > r.left && r.bottom > r.top)
            rects.push_back(r);
    };
    const uint32_t r0 = rel0 / pitch, c0 = rel0 % pitch, r1 = (rel1 - 1) / pitch, c1 = (rel1 - 1) % pitch + 1;
    if (r0 == r1)
        add(r0, r0 + 1, c0, c1);
    else
    {
        add(r0, r0 + 1, c0, pitch);
        if (r1 > r0 + 1)
            add(r0 + 1, r1, 0, pitch);
        add(r1, r1 + 1, 0, c1);
    }
    return rects;
}

ID3D12PipelineState* TargetCache::TransferPipeline(const HostTarget& dest, const HostTarget& source, int stencilBit)
{
    // stencilBit: -1 one pass (with SV_StencilRef when supported; otherwise stencil cleared to
    // 0), 0..7 the pass that sets that stencil bit.
    const uint32_t stencilMode = !dest.key.depth ? 0 : stencilBit >= 0 ? 2 : stencilRef_ ? 1 : 0;
    const uint32_t shaderKey = uint32_t(source.key.depth) | Samples(source.key) << 1 | uint32_t(dest.key.depth) << 4 | Samples(dest.key) << 5 | stencilMode << 8;
    const uint64_t key = uint64_t(shaderKey) | uint64_t(dest.format.raw) << 16 | uint64_t(stencilBit + 1) << 32;
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second.Get();

    auto compile = [&](const char* entry, const char* target, uint32_t k) -> ID3DBlob* {
        auto& blob = shaders_[k];
        if (blob)
            return blob.Get();
        const std::string defines[5] = { std::to_string(k & 1), std::to_string((k >> 1) & 7), std::to_string((k >> 4) & 1),
            std::to_string((k >> 5) & 7), std::to_string((k >> 8) & 3) };
        const D3D_SHADER_MACRO macros[] = { { "SRC_DEPTH", defines[0].c_str() }, { "SRC_SAMPLES", defines[1].c_str() },
            { "DEST_DEPTH", defines[2].c_str() }, { "DEST_SAMPLES", defines[3].c_str() }, { "STENCIL", defines[4].c_str() }, { nullptr, nullptr } };
        ComPtr<ID3DBlob> error;
        if (FAILED(D3DCompile(kEdramTransferHlsl, sizeof(kEdramTransferHlsl) - 1, "edram_transfer", macros, nullptr, entry, target, 0, 0, &blob, &error)))
        {
            Log("edram: transfer shader %s %03X failed: %s", entry, k, error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            blob.Reset();
            return nullptr;
        }
        return blob.Get();
    };
    ID3DBlob* vs = compile("vs", "vs_5_1", 0x80000000u);
    ID3DBlob* ps = compile("ps", "ps_5_1", shaderKey);
    if (!vs || !ps)
    {
        pipelines_[key] = nullptr;
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = Samples(dest.key);
    if (dest.key.depth)
    {
        pd.DSVFormat = kDepthView;
        auto& ds = pd.DepthStencilState;
        ds.DepthEnable = TRUE;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ds.DepthWriteMask = stencilBit >= 0 ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = 0xFF;
        ds.StencilWriteMask = stencilBit >= 0 ? UINT8(1u << stencilBit) : 0xFF;
        ds.FrontFace = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
        ds.BackFace = ds.FrontFace;
    }
    else
    {
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = dest.format.raw;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        Log("edram: transfer pipeline %03X (dest format %u) failed", shaderKey, unsigned(dest.format.raw));
    return (pipelines_[key] = pso).Get();
}

bool TargetCache::RecordTransfer(HostTarget& dest, HostTarget& source, uint32_t startTiles, uint32_t endTiles, const RecordContext& rc)
{
    const std::vector<D3D12_RECT> rects = TileRects(dest.key, startTiles, endTiles);
    if (rects.empty())
        return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    ID3D12PipelineState* pso = TransferPipeline(dest, source, -1);
    if (!pso || !rc.descriptors || !rc.descriptors(2, cpu, gpu))
    {
        stats_.skipped++;
        return false;
    }
    const UINT srvStride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const bool ms = Samples(source.key) > 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE second{ cpu.ptr + srvStride };
    if (source.key.depth)
    {
        srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        device_->CreateShaderResourceView(source.resource.Get(), &srv, cpu);
        srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if (!ms)
            srv.Texture2D.PlaneSlice = 1;
        device_->CreateShaderResourceView(source.resource.Get(), &srv, second);
    }
    else
    {
        srv.Format = source.format.raw;
        device_->CreateShaderResourceView(source.resource.Get(), &srv, cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC none = srv;
        none.Format = DXGI_FORMAT_R32G32_UINT;
        none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        none.Texture2D = {};
        none.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(nullptr, &none, second); // (t1 unused by colour sources)
    }

    Transition(source, source.key.depth ? D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, rc);
    Transition(dest, dest.key.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, rc);
    RecordedList& list = *rc.list;
    if (dest.key.depth)
        list.OMSetRenderTargets(0, nullptr, FALSE, &dest.drawView);
    else
        list.OMSetRenderTargets(1, &dest.rawView, FALSE, nullptr);
    list.SetGraphicsRootSignature(root_.Get());
    const uint32_t constants[4] = { TransferInfo(dest.key), TransferInfo(source.key), scaleX_ | scaleY_ << 4, 0 };
    list.SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    list.SetGraphicsRootDescriptorTable(1, gpu);
    const D3D12_VIEWPORT vp{ 0, 0, float(dest.width), float(dest.height), 0, 1 };
    list.RSSetViewports(1, &vp);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto drawRects = [&](ID3D12PipelineState* p) {
        list.SetPipelineState(p);
        for (const D3D12_RECT& r : rects)
        {
            list.RSSetScissorRects(1, &r);
            list.DrawInstanced(3, 1, 0, 0);
        }
    };
    if (dest.key.depth)
        list.OMSetStencilRef(0); // (without SV_StencilRef the first pass clears stencil to 0)
    drawRects(pso);
    if (dest.key.depth && !stencilRef_)
    {
        // Each stencil bit: its pass replaces the bit (reference 0xFF) where the guest's is set.
        list.OMSetStencilRef(0xFF);
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t withBit[4] = { constants[0], constants[1], constants[2] | uint32_t(bit) << 8, 0 };
            list.SetGraphicsRoot32BitConstants(0, 4, withBit, 0);
            if (ID3D12PipelineState* p = TransferPipeline(dest, source, bit))
                drawRects(p);
        }
    }
    stats_.transfers++;
    stats_.transferTiles += endTiles - startTiles;
    return true;
}

bool TargetCache::Update(const Pass& pass, const RecordContext& rc, HostTarget* out[5])
{
    std::lock_guard lock(mutex_);
    for (int i = 0; i < 5; i++)
        out[i] = nullptr;
    if (!device_ || !rc.list)
        return false;
    bool recorded = false;
    for (int i = 0; i < 5; i++)
    {
        if (!((pass.used >> i) & 1))
            continue;
        const SurfaceKey& key = pass.targets[i];
        const bool existed = targets_.count(key.Packed()) != 0;
        HostTarget* t = GetOrCreate(key, rc);
        recorded |= t && !existed;
        if (!t)
            continue;
        // Only tiles another layout wrote last are copied: a layout drawing over its own tiles,
        // or over tiles never written, transfers nothing.
        std::vector<Transfer> transfers;
        ownership_.Claim(key, 0, pass.lengthTiles[i], &transfers);
        for (const Transfer& tr : transfers)
        {
            auto source = targets_.find(tr.source.Packed());
            if (source != targets_.end())
                recorded |= RecordTransfer(*t, source->second, tr.startTiles, tr.endTiles, rc);
        }
        out[i] = t;
    }
    return recorded;
}

TargetCache::Stats TargetCache::TakeStats()
{
    std::lock_guard lock(mutex_);
    Stats s = stats_;
    stats_ = {};
    return s;
}
} // namespace edram
} // namespace regpu
