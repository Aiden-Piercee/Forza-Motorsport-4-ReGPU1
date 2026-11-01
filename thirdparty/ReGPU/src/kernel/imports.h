#pragma once
// Implementing guest imports (the Vd* video exports): PPC_FUNC(__imp__Name) receives the caller's
// registers. Arguments are r3-r10, then 8-byte stack slots.
#include <ppc_config.h> // the game's (REGPU_PPC_DIR)
#include <ppc_context.h>

#include "log.h"
#include "memory.h"

// Used inside namespace regpu: the export itself is global (the game links it by name), its body
// is regpu's.
#define GUEST_EXPORT(name)                                                 \
    static void Impl_##name(PPCContext& __restrict ctx, uint8_t* base);   \
    }                                                                      \
    PPC_FUNC(__imp__##name) { regpu::Impl_##name(ctx, base); }             \
    namespace regpu                                                        \
    {                                                                      \
    static void Impl_##name(PPCContext& __restrict ctx, uint8_t* base)

namespace regpu
{
inline uint32_t StackArg(PPCContext& ctx, int index) // index 8 = 9th argument
{
    return LoadBE32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
}

inline void Return(PPCContext& ctx, uint64_t value)
{
    ctx.r3.u64 = value;
}

inline void StoreIfNonNull(uint32_t address, uint32_t value)
{
    if (address)
        StoreBE32(address, value);
}
} // namespace regpu
