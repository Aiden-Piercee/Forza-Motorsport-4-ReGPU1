// Title-independent diagnostics the runtime calls into, and the FM2 easter-egg entry points
// as no-ops. FM2's versions (src/hooks/fm2) are keyed to FM2 code addresses and shaders and
// are not part of an FM4 build.
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <intrin.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

// FM2 logged D3D device internals at fixed addresses; FM4's D3D wrapper has its own layout.
void D3DDebugLog() {}

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
                // FM2_WATCH_STACKS=1: where every thread is at that moment (the writer is one of them).
                static int stacks = 0;
                if (getenv("FM2_WATCH_STACKS") && stacks++ < 6)
                    LogGuestThreads();
            }
            _mm_pause();
        }
    }).detach();
}

// FM2's typed-code easter egg (src/hooks/fm2/dealer.cpp). Not carried over to FM4.
void DealerOnChar(wchar_t) {}
void DealerCarDraw(uint64_t, const float[256][4], float[4]) {}
bool DealerMutesMusic() { return false; }

#include <atomic>

// Shared state of address-bound FM2 hooks that the renderer reads. Without those hooks they
// keep their idle values; an FM4 hook that sets them replaces these definitions.
std::atomic<uint64_t> g_patchConfig{ 0 };          // shader-patcher input hash (FM2_RECORD_CONFIGS)
std::atomic<bool> g_cameraAspectWidened{ false };  // the game's camera was given the wider aspect
