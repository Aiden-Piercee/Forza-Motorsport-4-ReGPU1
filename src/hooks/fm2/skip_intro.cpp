// Skip the intro (launcher checkbox, or FM2_SKIP_INTRO=1). sub_8220EAC8 builds the boot
// sequence, five timed entries (the disclaimer 3 s, Turn 10 10 s, Microsoft 10 s, Dolby 5 s,
// the Forza intro 79 s), and hands it to the sequencer's setter, sub_822892C0. The
// sequencer moves on by time, not at the end of the video, so the hook shortens those
// entries to a few frames; the title screen comes up as soon as the game is loaded.
// (Not queueing the sequence at all, or hiding the videos, leaves it waiting forever.)
#include <cstring>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

namespace
{
constexpr uint32_t kEntrySize = 224, kDurationOffset = 160; // entry stride, double seconds
constexpr double kSplash[] = { 3, 10, 10, 5, 79 };

double LoadDouble(uint32_t a)
{
    uint64_t raw;
    memcpy(&raw, GuestPtr<uint8_t>(a), 8);
    raw = __builtin_bswap64(raw);
    double d;
    memcpy(&d, &raw, 8);
    return d;
}

void StoreDouble(uint32_t a, double d)
{
    uint64_t raw;
    memcpy(&raw, &d, 8);
    raw = __builtin_bswap64(raw);
    memcpy(GuestPtr<uint8_t>(a), &raw, 8);
}
}

PPC_FUNC_IMPL(__imp__sub_822892C0);
PPC_FUNC(sub_822892C0)
{
    // The setter also takes other sequences: only touch the boot one, recognised by its timings.
    static bool done = false;
    if (!done && GetSettings().skipIntro)
    {
        bool boot = true;
        for (uint32_t i = 0; i < std::size(kSplash); i++)
            boot = boot && LoadDouble(ctx.r4.u32 + i * kEntrySize + kDurationOffset) == kSplash[i];
        if (boot)
        {
            done = true;
            for (uint32_t i = 0; i < std::size(kSplash); i++)
                StoreDouble(ctx.r4.u32 + i * kEntrySize + kDurationOffset, 0.05);
            Log("intro: skipped (legal text and splash videos)");
        }
    }
    __imp__sub_822892C0(ctx, base);
}
