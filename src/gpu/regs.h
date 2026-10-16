#pragma once
#include <cstdint>

#include "memory.h"

// The Xenos register file, memory-mapped at 0x7FC80000 in guest space and
// written by the command processor. Constants live in it too:
// ALU 0x4000+, fetch 0x4800+, bool 0x4900+, loop 0x4908+.
namespace gpureg
{
constexpr uint32_t kBase = 0x7FC80000;
constexpr uint32_t AluConstants = 0x4000;
constexpr uint32_t FetchConstants = 0x4800;
constexpr uint32_t BoolConstants = 0x4900;
constexpr uint32_t LoopConstants = 0x4908;

inline uint32_t Get(uint32_t index) { return LoadBE32(kBase + index * 4); }
inline void Set(uint32_t index, uint32_t value) { StoreBE32(kBase + index * 4, value); }
}

// Physical addresses as the GPU sees them, in our 0xA0000000 view.
inline uint32_t GpuPhysToGuest(uint32_t phys) { return 0xA0000000 + (phys & 0x1FFFFFFF); }

// Frame capture (src/gpu/frame.cpp), called on the command processor thread.
enum class ShaderType
{
    Vertex = 0,
    Pixel = 1,
};
void FrameOnShaderLoad(ShaderType type, const uint32_t* microcodeBE, uint32_t dwords, uint32_t guestAddress);
void FrameOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
void FrameOnSwap(uint64_t frame, uint32_t frontBuffer);
