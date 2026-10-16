#pragma once
#include <cstdint>
#include <vector>

// A Xenos texture fetch constant (6 dwords), the fields the decoder needs.
struct TextureFetch
{
    uint32_t dwords[6];

    uint32_t Type() const { return dwords[0] & 3; } // 2 = texture
    bool Tiled() const { return dwords[0] >> 31; }
    uint32_t Pitch() const { return ((dwords[0] >> 22) & 0x1FF) << 5; } // texels
    uint32_t Format() const { return dwords[1] & 0x3F; }
    uint32_t Endian() const { return (dwords[1] >> 6) & 3; }
    uint32_t Address() const { return dwords[1] & 0xFFFFF000; } // physical
    uint32_t Width() const { return (dwords[2] & 0x1FFF) + 1; }
    uint32_t Height() const { return ((dwords[2] >> 13) & 0x1FFF) + 1; }
    uint32_t Swizzle() const { return (dwords[3] >> 1) & 0xFFF; }
    uint32_t Dimension() const { return (dwords[5] >> 9) & 3; } // 1 = 2D
};

TextureFetch ReadTextureFetch(uint32_t slot);
bool TextureFormatSupported(uint32_t format);
const char* TextureFormatName(uint32_t format);

// Decodes mip 0 of a 2D texture to RGBA8 (row-major, width*height*4).
bool DecodeTexture(const TextureFetch& fetch, std::vector<uint8_t>& rgba);
