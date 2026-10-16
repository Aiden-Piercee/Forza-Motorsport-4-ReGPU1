#pragma once
// Force-included into the recompiled code (see CMakeLists.txt).
#include <cstdint>

struct PPCContext;

// MSR[EE] cleared -> take the global interrupt lock; set -> release it.
void PPCMsrChanged(PPCContext& ctx, uint64_t oldMsr);
#define PPC_MSR_CHANGED(ctx, oldMsr)                       \
    do                                                     \
    {                                                      \
        if (((oldMsr) ^ (ctx).msr) & 0x8000)               \
            PPCMsrChanged((ctx), (oldMsr));                \
    } while (0)
