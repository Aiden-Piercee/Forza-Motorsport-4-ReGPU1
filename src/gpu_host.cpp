// The GPU is the ReGPU library (thirdparty/ReGPU, include/regpu/regpu.h). This file hands it the
// player's display settings and the runtime pieces it needs: guest memory, guest threads, guest
// calls and the interrupt lock. ReGPU provides the Vd* imports itself.
#include <string>

#include "regpu/regpu.h"

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

std::string CurrentHostStack(int maxFrames); // crash.cpp
void LogAllHostThreadStacks(const char* why); // hang.cpp
void SpikeDump(double ms);                    // kernel/thread.cpp

namespace
{
void ApplySettings(regpu::Config& c)
{
    const Settings& s = GetSettings();
    c.width = s.width;
    c.height = s.height;
    c.aspectW = s.aspectW;
    c.aspectH = s.aspectH;
    c.fullscreen = s.fullscreen;
    c.vsync = s.vsync;
    c.maxFps = s.maxFps;
    c.enhance = s.enhance;
    c.msaa = s.msaa;
    c.upscaler = regpu::Config::Upscaler(uint32_t(s.upscaler));
    c.quality = s.quality;
    c.sharpness = s.sharpness;
}
} // namespace

void GpuStart()
{
    regpu::Config& config = regpu::GetConfig();
    ApplySettings(config);
    config.userDir = UserDir(); // shader and pipeline caches, in the same place as before
    config.windowTitle = L"Forza Motorsport 4";

    regpu::Host& host = regpu::GetHost();
    host.base = g_base;
    host.physicalSection = PhysicalSection();
    host.alloc = [](bool physical, uint32_t size, uint32_t align) {
        return GuestAlloc(physical ? Heap::Physical : Heap::Virtual, size, align);
    };
    host.createThread = [](const char* name, void (*body)(PPCContext&)) { CreateHostGuestThread(name, body); };
    host.currentPcr = [] { return CurrentGuestThread()->pcr; };
    host.guestCall = [](PPCContext& ctx, uint32_t address) { GuestCall(ctx, address); };
    host.interruptLockAcquire = [](int cpu) { InterruptLockAcquire(cpu); };
    host.interruptLockRelease = [] { InterruptLockRelease(); };
    host.cpuUsage = [](double wallSeconds) { return GuestThreadCpuUsage(wallSeconds); };
    host.logThreadStacks = [](const char* why) { LogAllHostThreadStacks(why); };
    host.currentStack = [](int maxFrames) { return CurrentHostStack(maxFrames); };
    host.slowFrame = [](double ms) { SpikeDump(ms); };

    Log("gpu: ReGPU at %ux%u, msaa %u", config.width, config.height, config.msaa);
    regpu::Start();
}
