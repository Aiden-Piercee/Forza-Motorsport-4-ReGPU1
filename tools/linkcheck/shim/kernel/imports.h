#pragma once
// Stand-in for src/kernel/imports.h in the link check: tools/genstubs.py's stubs
// need only these two names, and the check never runs guest code.
#include <ppc_context.h>

#define LOG_ONCE(...) ((void)0)

inline void Return(PPCContext& ctx, uint64_t value)
{
    ctx.r3.u64 = value;
}
