// Debug probe (FM2_PROBE_WAIT=1): logs what the title's GPU wait loop
// (sub_82369340) polls, once per distinct address.
#include <cstdlib>
#include <atomic>
#include <set>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_82369340);

PPC_FUNC(sub_82369340)
{
    static const bool probe = getenv("FM2_PROBE_WAIT") != nullptr;
    if (probe)
    {
        static std::set<uint32_t> seen;
        const uint32_t device = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32));
        const uint32_t pointer = __builtin_bswap32(*GuestPtr<uint32_t>(device + 10768));
        const uint32_t target = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32 + 8));
        if (seen.size() < 8 && seen.insert(pointer).second)
            Log("probe: wait loop polls %08X (now %08X, waiting for change from %08X), lr %08llX", pointer,
                __builtin_bswap32(*GuestPtr<uint32_t>(pointer)), target, ctx.lr);
    }
    __imp__sub_82369340(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82373E38);

// The wait loop's timeout path: count how often it fires.
PPC_FUNC(sub_82373E38)
{
    static const bool probe = getenv("FM2_PROBE_WAIT") != nullptr;
    static std::atomic<uint64_t> calls{ 0 };
    if (probe && (++calls % 200) == 1)
        Log("probe: wait-loop timeout path taken %llu times", (unsigned long long)calls.load());
    __imp__sub_82373E38(ctx, base);
}
