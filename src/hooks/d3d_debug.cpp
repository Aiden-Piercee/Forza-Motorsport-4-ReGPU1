// Temporary instrumentation of D3D internals while bringing up phase 1.
#include <atomic>
#include <thread>

#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>

#include "kernel/imports.h"
#include "kernel/thread.h"

extern "C" PPC_FUNC(__imp__sub_82371D60);

namespace
{
std::atomic<uint32_t> g_waitPosition{ 0 }, g_waitGeneration{ 0 }, g_waitDevice{ 0 }, g_waiting{ 0 };
}

// D3D: wait until the GPU retires command position r4 (generation r5).
PPC_FUNC(sub_82371D60)
{
    g_waitDevice = ctx.r3.u32;
    g_waitPosition = ctx.r4.u32;
    g_waitGeneration = ctx.r5.u32;
    g_waiting++;
    __imp__sub_82371D60(ctx, base);
    g_waiting--;
}

void D3DDebugLogFlips(uint32_t dev)
{
    std::string q;
    for (int i = 0; i < 8; i++)
    {
        char e[32];
        snprintf(e, sizeof(e), " [%08X@%u]", LoadBE32(dev + 16196 + i * 8), LoadBE32(dev + 16200 + i * 8));
        q += e;
    }
    Log("  d3d: vblanks %u flips done %u queue %u..%u%s", LoadBE32(dev + 16172), LoadBE32(dev + 16192), LoadBE32(dev + 16324),
        LoadBE32(dev + 16328), q.c_str());
}

void D3DDebugLog()
{
    if (const uint32_t dev = LoadBE32(LoadBE32(0x820009CC)))
        D3DDebugLogFlips(dev);
    if (!g_waiting || !g_waitDevice)
        return;
    const uint32_t block = LoadBE32(g_waitDevice + 0x2A10);
    void D3DDebugLogKicks();
    D3DDebugLogKicks();
    Log("  d3d: waiting for retire of %08X gen %u; retired %08X", g_waitPosition.load(), g_waitGeneration.load(), LoadBE32(block + 4));
    const uint32_t dev = g_waitDevice;
    const uint32_t sblock = LoadBE32(dev + 0x2A14);
    Log("  d3d: interrupt block %08X: ack %08X callback %08X arg %08X", sblock, LoadBE32(sblock), LoadBE32(sblock + 0x10), LoadBE32(sblock + 0x14));
    Log("  d3d: flags 2A3D %02X 2A3E %02X; global device %08X", *GuestPtr<uint8_t>(dev + 0x2A3D), *GuestPtr<uint8_t>(dev + 0x2A3E),
        LoadBE32(LoadBE32(0x820009CC)));
}

extern "C" PPC_FUNC(__imp__sub_823724C8);

namespace
{
std::mutex g_kickMutex;
std::vector<std::string> g_kicks;
}

// D3D: submit r5 indirect buffers listed at r4 ({address, size} pairs) to the ring.
PPC_FUNC(sub_823724C8)
{
    {
        char line[256];
        int n = snprintf(line, sizeof(line), "thread %u kicks %u:", CurrentGuestThreadId(), ctx.r5.u32);
        for (uint32_t i = 0; i < ctx.r5.u32 && i < 4; i++)
            n += snprintf(line + n, sizeof(line) - n, " %08X+%X", LoadBE32(ctx.r4.u32 + i * 8 + 4), LoadBE32(ctx.r4.u32 + i * 8));
        std::lock_guard lock(g_kickMutex);
        g_kicks.push_back(line);
        if (g_kicks.size() > 8)
            g_kicks.erase(g_kicks.begin());
    }
    __imp__sub_823724C8(ctx, base);
}

void D3DDebugLogKicks()
{
    std::lock_guard lock(g_kickMutex);
    for (auto& k : g_kicks)
        Log("  d3d: %s", k.c_str());
}

// Polls a guest word (FM2_WATCH=hex) and logs changes with the main
// thread's position.
void StartWatch()
{
    const char* v = getenv("FM2_WATCH");
    if (!v)
        return;
    const uint32_t address = uint32_t(strtoul(v, nullptr, 16));
    std::thread([address] {
        uint32_t last = LoadBE32(address);
        Log("watch %08X = %08X", address, last);
        for (;;)
        {
            const uint32_t now = LoadBE32(address);
            if (now != last)
            {
                Log("watch %08X: %08X -> %08X", address, last, now);
                last = now;
            }
            _mm_pause();
        }
    }).detach();
}

extern "C" PPC_FUNC(__imp__sub_8236CB28);
extern "C" PPC_FUNC(__imp__sub_825ADE70);

// D3D swap (calls VdSwap).
PPC_FUNC(sub_8236CB28)
{
    static std::atomic<int> n{ 0 };
    if (n++ < 200 && getenv("FM2_TRACE_SWAP"))
        Log("d3d swap #%d by thread %u (lr %08llX) flags 2A3E %02X", n.load(), CurrentGuestThreadId(), ctx.lr,
            *GuestPtr<uint8_t>(ctx.r3.u32 + 0x2A3E));
    __imp__sub_8236CB28(ctx, base);
}

PPC_FUNC(sub_825ADE70)
{
    static std::atomic<int> n{ 0 };
    if (n++ < 200 && getenv("FM2_TRACE_SWAP"))
        Log("game present #%d by thread %u (lr %08llX)", n.load(), CurrentGuestThreadId(), ctx.lr);
    __imp__sub_825ADE70(ctx, base);
}
