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
