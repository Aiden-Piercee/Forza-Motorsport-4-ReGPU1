// Differential check of the title's Bink read_runs (sub_829560A0) against a
// plain C restatement of the same machine code (FM2_CHECK_RUNS=1). Logs the
// first call whose output or bit-reader state differs, and any call whose
// run overshoots its count (which the shipped code never stops on).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "kernel/imports.h"
#include "kernel/objects.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

extern uint32_t g_binkRingBase; // kernel/imports_io.cpp
uint32_t g_binkPlane[3];

namespace
{
uint32_t Load32(uint32_t a) { return __builtin_bswap32(*GuestPtr<uint32_t>(a)); }
uint8_t Load8(uint32_t a) { return *GuestPtr<uint8_t>(a); }
// PowerPC shifts: amounts of 32 or more give 0.
uint32_t Shl(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v << (n & 31); }
uint32_t Shr(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v >> (n & 31); }

struct Reader
{
    uint32_t buf, ptr, avail;
};

struct Result
{
    std::vector<uint8_t> out;
    Reader reader{};
    uint32_t count = 0;
    bool overshoot = false;
    int path = 0; // 0 empty, 1 fill, 2 huffman
};

Result Reference(uint32_t bundle, Reader r)
{
    Result res;
    const uint32_t len = Load32(bundle + 40), start = Load32(bundle + 44);
    uint32_t t;
    if (r.avail < len)
    {
        const uint32_t w = Load32(r.ptr);
        r.ptr += 4;
        t = (Shl(w, r.avail) | r.buf) & Shr(0xFFFFFFFF, 32 - len);
        r.buf = Shr(w, len - r.avail);
        r.avail = r.avail - len + 32;
    }
    else
    {
        t = r.buf & Shr(0xFFFFFFFF, 32 - len);
        r.buf = Shr(r.buf, len);
        r.avail -= len;
    }
    res.count = t;
    if (!t)
    {
        res.reader = r;
        return res;
    }
    uint32_t bit;
    if (!r.avail)
    {
        const uint32_t w = Load32(r.ptr);
        r.ptr += 4;
        r.avail = 31;
        bit = w & 1;
        r.buf = w >> 1;
    }
    else
    {
        bit = r.buf & 1;
        r.buf >>= 1;
        r.avail--;
    }
    if (bit)
    {
        res.path = 1;
        uint32_t v;
        if (r.avail < 4)
        {
            const uint32_t w = Load32(r.ptr);
            r.ptr += 4;
            v = (Shl(w, r.avail) | r.buf) & 15;
            r.buf = Shr(w, 4 - r.avail);
            r.avail += 28;
        }
        else
        {
            v = r.buf & 15;
            r.buf >>= 4;
            r.avail -= 4;
        }
        res.out.assign(t, uint8_t(v));
        res.reader = r;
        return res;
    }
    res.path = 2;
    const uint32_t maxBits = Load32(bundle + 32), table = Load32(bundle + 36);
    const uint32_t mask = Shr(0xFFFFFFFF, 32 - maxBits);
    uint8_t last = 0;
    int64_t remaining = t;
    while (remaining > 0 && res.out.size() < 0x100000)
    {
        uint32_t e;
        if (r.avail >= maxBits)
        {
            e = Load8(table + (r.buf & mask));
            const uint32_t l = e >> 4;
            r.avail -= l;
            r.buf = Shr(r.buf, l);
        }
        else
        {
            const uint32_t w = Load32(r.ptr);
            e = Load8(table + ((Shl(w, r.avail) | r.buf) & mask));
            const uint32_t l = e >> 4;
            if (r.avail >= l)
            {
                r.avail -= l;
                r.buf = Shr(r.buf, l);
            }
            else
            {
                r.buf = Shr(w, l - r.avail);
                r.avail = r.avail - l + 32;
                r.ptr += 4;
            }
        }
        const uint8_t sym = Load8(bundle + 16 + (e & 15));
        if (sym < 12)
        {
            res.out.push_back(sym);
            last = sym;
            remaining--;
        }
        else
        {
            const uint8_t run = Load8(0x821C63F0 + sym - 12);
            res.out.insert(res.out.end(), run, last);
            remaining -= run;
        }
    }
    res.overshoot = remaining < 0;
    res.reader = r;
    return res;
}
}

PPC_FUNC_IMPL(__imp__sub_829560A0);

PPC_FUNC(sub_829560A0)
{
    static LiveSwitch check("FM2_CHECK_RUNS");
    if (!check)
    {
        __imp__sub_829560A0(ctx, base);
        return;
    }
    const uint32_t bundle = ctx.r3.u32, readerAddr = ctx.r4.u32;
    {
        static std::mutex lock;
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        std::lock_guard<std::mutex> guard(lock);
        if (seen.insert({ CurrentGuestThreadId(), bundle & ~0xFFFu }).second)
            Log("bink: read_runs on thread %u with bundles at %08X", CurrentGuestThreadId(), bundle & ~0xFFFu);
    }
    const bool active = Load32(bundle + 0) == Load32(bundle + 4);
    const Reader in{ Load32(readerAddr), Load32(readerAddr + 4), Load32(readerAddr + 8) };
    Result ref;
    if (active)
        ref = Reference(bundle, in);
    const uint32_t start = Load32(bundle + 44);
    // Past the 2 MB read ring the decoder should see a copy of its start.
    const uint32_t ring = g_binkRingBase;
    if (ring && in.ptr - (ring + 0x200000) < 0x40000)
    {
        static uint32_t lastBad = 0;
        const uint32_t off = in.ptr - ring - 0x200000;
        if (memcmp(GuestPtr(in.ptr), GuestPtr(ring + off), 16) != 0 && lastBad != (in.ptr & ~0xFFFu))
        {
            lastBad = in.ptr & ~0xFFFu;
            Log("bink: reader %08X past ring %08X end differs from ring start (+%X): %08X vs %08X", in.ptr, ring, off, Load32(in.ptr), Load32(ring + off));
        }
    }
    // The shipped code never stops on an overshoot, so report before running it.
    if (ref.overshoot)
    {
        Log("bink: read_runs will overshoot: bundle %08X count %u produced %zu, path %d, maxbits %u table %08X len %u data %08X (reader %08X/%08X/%u)",
            bundle, ref.count, ref.out.size(), ref.path, Load32(bundle + 32), Load32(bundle + 36), Load32(bundle + 40), start, in.buf, in.ptr,
            in.avail);
        Log("bink:   syms %08X %08X %08X %08X", Load32(bundle + 16), Load32(bundle + 20), Load32(bundle + 24), Load32(bundle + 28));
        if (FILE* f = fopen("out/plane.bin", "wb"))
        {
            fwrite(GuestPtr(g_binkPlane[0]), 1, g_binkPlane[1] * 224, f);
            fclose(f);
            Log("bink: plane %08X pitch %u width %u dumped", g_binkPlane[0], g_binkPlane[1], g_binkPlane[2]);
        }
        if (FILE* f = fopen("out/frame.bin", "wb"))
        {
            fwrite(GuestPtr(in.ptr - 0x10000), 1, 0x20000, f);
            fclose(f);
        }
    }

    __imp__sub_829560A0(ctx, base);

    if (!active)
        return;
    static int reports = 0;
    const Reader out{ Load32(readerAddr), Load32(readerAddr + 4), Load32(readerAddr + 8) };
    bool same = out.buf == ref.reader.buf && out.ptr == ref.reader.ptr && out.avail == ref.reader.avail;
    const size_t n = std::min<size_t>(ref.out.size(), 0x400);
    if (same && n)
        same = memcmp(GuestPtr(start), ref.out.data(), n) == 0;
    if (ref.overshoot && reports < 20)
    {
        reports++;
        Log("bink: read_runs overshoot: bundle %08X count %u produced %zu (reader in %08X/%08X/%u)", bundle, ref.count, ref.out.size(), in.buf, in.ptr,
            in.avail);
    }
    // FM2_DUMP_RING="start,end": dump that guest range at the first count larger
    // than a 752-wide row of blocks.
    // FM2_DUMP_FRAME=1: on a count larger than a 752-wide block row, write the
    // 256 KB from the reader's position back to the frame buffer after the ring.
    if (ref.count > 94 && getenv("FM2_DUMP_FRAME") && ring && in.ptr - (ring + 0x200000) < 0x40000)
        if (FILE* f = fopen("out/frame.bin", "wb"))
        {
            fwrite(GuestPtr(ring + 0x200040), 1, 0x40000, f);
            fclose(f);
            Log("bink: frame buffer dumped at count %u, reader +%X", ref.count, in.ptr - ring - 0x200040);
        }
    if (ref.count > 94 && getenv("FM2_DUMP_RING"))
    {
        static bool dumped = false;
        const char* ring = getenv("FM2_DUMP_RING");
        const uint32_t a = uint32_t(strtoul(ring, nullptr, 16)), b = uint32_t(strtoul(strchr(ring, ',') + 1, nullptr, 16));
        if (!dumped)
            if (FILE* f = fopen("out/ring.bin", "wb"))
            {
                dumped = true;
                fwrite(GuestPtr(a), 1, b - a, f);
                fclose(f);
                Log("bink: ring dumped at count %u", ref.count);
            }
    }
    if (!same && reports < 20)
    {
        reports++;
        Log("bink: read_runs MISMATCH path %d bundle %08X count %u: reader ref %08X/%08X/%u ours %08X/%08X/%u (in %08X/%08X/%u)", ref.path, bundle,
            ref.count, ref.reader.buf, ref.reader.ptr, ref.reader.avail, out.buf, out.ptr, out.avail, in.buf, in.ptr, in.avail);
    }
}
