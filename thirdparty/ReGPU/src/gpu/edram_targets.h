#pragma once
// Host render targets for the EDRAM model (gpu/edram.h): one D3D12 render target per EDRAM
// layout, and the transfers that copy tiles between layouts when one takes over tiles another
// wrote. Every copy goes through the guest's EDRAM bits (the 32 or 64 bits per sample the
// hardware stores), so a reinterpretation between formats, MSAA modes or colour and depth
// gives what the console would read back, and a transfer between the same formats is exact.
//
// Steps 1 and 2 of the render target plan (docs/FM4-RENDERER-PORT.md). Draws do not use these
// targets yet: with REGPU_EDRAM_TARGETS=1 the renderer keeps them and their ownership up to
// date alongside its own surfaces, to bring up target creation and the transfer shaders.
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "gpu/edram.h"

namespace regpu
{
class RecordedList;

namespace edram
{
// The host format of each guest format. Resources are typeless; transfers use the _UINT
// views (the raw bits), draws the typed views.
struct HostFormat
{
    DXGI_FORMAT resource, draw, raw;
};
HostFormat HostColorFormat(uint32_t storageFormat);


struct HostTarget
{
    SurfaceKey key;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE drawView{}; // RTV of the draw format, or the DSV
    D3D12_CPU_DESCRIPTOR_HANDLE rawView{};  // colour: RTV of the _UINT format (transfers)
    HostFormat format{};
    uint32_t width = 0, height = 0;          // host pixels
};

// What the renderer lends for a recording: its list and this frame's shader-visible descriptors.
struct RecordContext
{
    RecordedList* list = nullptr;
    // Reserves `count` consecutive descriptors in the bound CBV/SRV/UAV heap; false when the
    // frame has run out (the transfer is skipped and counted).
    std::function<bool(uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)> descriptors;
};

class TargetCache
{
public:
    // scaleX/Y: host pixels per guest pixel (1..4).
    bool Init(ID3D12Device* device, uint32_t scaleX, uint32_t scaleY);
    bool Ready() const { return device_ != nullptr; }

    // Makes the pass's render targets current: creates the ones not seen before, claims the
    // tiles the draw can reach, and records the transfers for tiles another layout wrote last.
    // out[0] is depth, out[1..4] colour 0..3 (null where the pass uses none). Returns whether
    // anything was recorded (the caller then rebinds its own state).
    bool Update(const Pass& pass, const RecordContext& rc, HostTarget* out[5]);

    struct Stats
    {
        uint64_t targetsCreated = 0, transfers = 0, transferTiles = 0, skipped = 0;
    };
    Stats TakeStats();

private:
    HostTarget* GetOrCreate(const SurfaceKey& key, const RecordContext& rc);
    void Transition(HostTarget& t, D3D12_RESOURCE_STATES to, const RecordContext& rc);
    bool RecordTransfer(HostTarget& dest, HostTarget& source, uint32_t startTiles, uint32_t endTiles, const RecordContext& rc);
    ID3D12PipelineState* TransferPipeline(const HostTarget& dest, const HostTarget& source, int stencilBit);
    // Destination rectangles (host pixels) covering tiles [start, end) of `dest`.
    std::vector<D3D12_RECT> TileRects(const SurfaceKey& dest, uint32_t startTiles, uint32_t endTiles) const;

    ID3D12Device* device_ = nullptr;
    uint32_t scaleX_ = 1, scaleY_ = 1;
    bool stencilRef_ = false; // SV_StencilRef: depth and stencil in one pass
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_;
    uint32_t rtvUsed_ = 0, dsvUsed_ = 0, rtvStride_ = 0, dsvStride_ = 0;
    std::map<uint64_t, HostTarget> targets_; // SurfaceKey::Packed()
    std::map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
    std::map<uint32_t, Microsoft::WRL::ComPtr<ID3DBlob>> shaders_;
    Ownership ownership_;
    Stats stats_;
    std::mutex mutex_;
};
} // namespace edram
} // namespace regpu
