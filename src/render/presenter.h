#pragma once
#include <cstdint>
#include <vector>

// Window + D3D12 swap chain (FM2_HEADLESS=1 disables it).
void PresenterInit();
// Called from VdSwap: the guest finished a frame.
void PresenterSubmitFrame(uint64_t frame);
// Shows a CPU-side RGBA8 image (scaled 1:1 into the 1280x720 back buffer).
void PresenterSubmitImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
// Keyboard input goes to the game only while the window has focus.
bool PresenterHasFocus();

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// Shared D3D12 device and queue (created on first use, also when headless).
ID3D12Device* RenderDevice();
ID3D12CommandQueue* RenderQueue();
// Records copying src (COPY_SOURCE, 1280x720 RGBA8) into the current back
// buffer; returns false when there is no window. Call PresenterPresent()
// after executing the list.
bool PresenterRecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src);
void PresenterPresent();
// Called by the renderer: the presenter stops drawing its own frames.
void PresenterUseRenderer();
