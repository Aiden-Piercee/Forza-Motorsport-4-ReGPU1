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

PPC_FUNC_IMPL(__imp__sub_82953E38);
uint32_t GuestAllocationSize(uint32_t address, uint32_t* start);

// Timer callback list walk: validate nodes (callback at +16, next at +0)
// before the title calls through them (FM2_PROBE_LIST=1).
PPC_FUNC(sub_82953E38)
{
    static const bool probe = getenv("FM2_PROBE_LIST") != nullptr;
    if (probe)
    {
        static std::set<uint32_t> known;
        uint32_t node = __builtin_bswap32(*GuestPtr<uint32_t>(ctx.r3.u32 + 24));
        for (int n = 0; node && n < 256; n++)
        {
            const uint32_t callback = __builtin_bswap32(*GuestPtr<uint32_t>(node + 16));
            if (callback < 0x82000000 || callback >= 0x83000000)
            {
                uint32_t start = 0;
                const uint32_t size = GuestAllocationSize(node, &start);
                Log("probe: bad timer node %08X (callback %08X) in allocation %08X+%X; words: %08X %08X %08X %08X %08X %08X", node, callback, start,
                    size, __builtin_bswap32(GuestPtr<uint32_t>(node)[0]), __builtin_bswap32(GuestPtr<uint32_t>(node)[1]),
                    __builtin_bswap32(GuestPtr<uint32_t>(node)[2]), __builtin_bswap32(GuestPtr<uint32_t>(node)[3]),
                    __builtin_bswap32(GuestPtr<uint32_t>(node)[4]), __builtin_bswap32(GuestPtr<uint32_t>(node)[5]));
                break;
            }
            if (known.insert(node).second)
                Log("probe: timer node %08X callback %08X", node, callback);
            node = __builtin_bswap32(*GuestPtr<uint32_t>(node));
        }
    }
    __imp__sub_82953E38(ctx, base);
}
