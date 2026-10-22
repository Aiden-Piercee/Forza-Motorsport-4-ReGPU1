// Debug scan (FM2_SCAN_FLOAT=<hex bits>,<frame>): at that swap, logs every committed guest
// address holding the big-endian 32-bit value, with the floats around it, to find game
// state (the camera's aspect ratio for the ultrawide frustum).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "log.h"
#include "memory.h"

void MemScanOnSwap(uint64_t frame)
{
    static uint32_t bits = 0;
    static uint64_t at = 0;
    static bool parsed = [] {
        if (const char* v = getenv("FM2_SCAN_FLOAT"))
            return sscanf(v, "%x,%llu", &bits, &at) == 2;
        return false;
    }();
    if (!parsed || frame != at)
        return;
    const uint32_t want = __builtin_bswap32(bits);
    static const int kMax = getenv("FM2_SCAN_MAX") ? atoi(getenv("FM2_SCAN_MAX")) : 200;
    int found = 0;
    for (uint64_t a = 0x10000; a < 0x100000000ull && found < kMax;)
    {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(g_base + a, &mi, sizeof(mi)))
            break;
        const uint64_t start = uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - g_base), end = start + mi.RegionSize;
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD))
        {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + start);
            for (uint64_t i = 0; i < (end - start) / 4 && found < kMax; i++)
                if (p[i] == want)
                {
                    const uint32_t addr = uint32_t(start + i * 4);
                    char around[256] = {};
                    int n = 0;
                    for (int k = -4; k <= 4; k++)
                    {
                        if (addr + k * 4 < start || addr + k * 4 + 4 > end)
                            continue;
                        const uint32_t v = __builtin_bswap32(p[i + k]);
                        float f;
                        memcpy(&f, &v, 4);
                        n += snprintf(around + n, sizeof(around) - n, "%s%g", k == -4 ? "" : " ", f);
                    }
                    Log("scan: %08X: %s", addr, around);
                    found++;
                }
        }
        a = end;
    }
    Log("scan: %d hits for %08X at frame %llu", found, bits, (unsigned long long)frame);
}
