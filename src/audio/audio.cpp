// Phase 1 audio: the render-driver clock the title's audio engine runs on.
// The title registers a callback the console calls every 256 samples at
// 48 kHz (5.33 ms); we call it on schedule and drop the frames it submits.
// Phase 4 adds output (XAudio2/WASAPI) and XMA decoding.
#include <atomic>
#include <chrono>
#include <thread>

#include <windows.h>
#include <timeapi.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

namespace
{
std::atomic<uint32_t> g_callback{ 0 };
std::atomic<uint32_t> g_callbackArg{ 0 };
std::shared_ptr<ThreadObject> g_audioThread;

void AudioLoop(PPCContext& ctx)
{
    timeBeginPeriod(1);
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);
        if (const uint32_t cb = g_callback)
        {
            ctx.r3.u64 = g_callbackArg;
            GuestCall(ctx, cb);
        }
    }
}
}

void AudioInit() {}

GUEST_EXPORT(XAudioRegisterRenderDriverClient)
{
    // (callback struct { u32 fn, u32 arg }*, driver*)
    const uint32_t callback = ctx.r3.u32;
    g_callback = LoadBE32(callback);
    g_callbackArg = LoadBE32(callback + 4);
    StoreIfNonNull(ctx.r4.u32, 0x41550000);
    if (!g_audioThread)
        g_audioThread = CreateHostGuestThread("audio driver", AudioLoop);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioSubmitRenderDriverFrame)
{
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    StoreIfNonNull(ctx.r4.u32, 0);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XAudioGetVoiceCategoryVolume)
{
    const float one = 1.0f;
    uint32_t bits;
    memcpy(&bits, &one, 4);
    StoreIfNonNull(ctx.r4.u32, bits);
    Return(ctx, X_STATUS_SUCCESS);
}
