// The title's Lua 5.1 runtime: log script errors as they are thrown.
// luaD_throw(L, errcode) has the message on the stack top for runtime
// errors. Layout (32-bit big-endian): L->top at +8, TValue 16 bytes with
// the type tag at +8, TString characters at +16.
#include <cstring>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_824B91F0);

// luaD_throw
PPC_FUNC(sub_824B91F0)
{
    const uint32_t L = ctx.r3.u32, errcode = ctx.r4.u32;
    const uint32_t top = __builtin_bswap32(*GuestPtr<uint32_t>(L + 8));
    const uint32_t tag = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16 + 8));
    if (tag == 4) // LUA_TSTRING
    {
        const uint32_t str = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16));
        const uint32_t len = __builtin_bswap32(*GuestPtr<uint32_t>(str + 12));
        Log("lua: error %u: %.*s", errcode, int(len < 1024 ? len : 1024), GuestPtr<const char>(str + 16));
    }
    else
        Log("lua: error %u (value type %u)", errcode, tag);
    __imp__sub_824B91F0(ctx, base);
}
