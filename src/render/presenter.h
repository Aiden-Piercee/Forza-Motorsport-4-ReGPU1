#pragma once
#include <cstdint>

// Window + D3D12 swap chain (FM2_HEADLESS=1 disables it).
void PresenterInit();
// Called from VdSwap: the guest finished a frame.
void PresenterSubmitFrame(uint64_t frame);
// Keyboard input goes to the game only while the window has focus.
bool PresenterHasFocus();
