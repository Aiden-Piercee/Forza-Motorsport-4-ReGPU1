#pragma once
#include <cstdint>
#include <vector>

// The first-boot compile screen as 1280x720 RGBA8 (the swap chain size). False when the
// game's UI textures can't be read; the plain overlay is used then.
// total 0: no progress yet (searching).
bool BootScreenCompose(const wchar_t* title, uint32_t done, uint32_t total, std::vector<uint8_t>& rgba);
// FM2_BOOTSCREEN_PREVIEW=1: writes the candidate textures and a composed screen to
// out/bootscreen/*.png.
void BootScreenPreview();
