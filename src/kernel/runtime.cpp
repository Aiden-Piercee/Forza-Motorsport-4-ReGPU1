#include "kernel/runtime.h"

#include <chrono>
#include <thread>

#include <intrin.h>
#include <windows.h>

#include "kernel/imports.h"
#include "loader.h"

namespace
{
std::filesystem::path g_gameDir;
std::filesystem::path g_userDir;
uint32_t g_tscFrequency = 0;

// KeTimeStampBundle: InterruptTime (100 ns since boot), SystemTime
// (FILETIME), TickCount (ms). The title polls it for timing.
void TimeStampThread()
{
    SetThreadDescription(GetCurrentThread(), L"timestamp bundle");
    const auto start = Clock::now();
    for (;;)
    {
        const uint32_t bundle = g_title.keTimeStampBundle;
        if (bundle)
        {
            const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            StoreBE64(bundle + 0, uint64_t(since / 100));
            StoreBE64(bundle + 8, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
            StoreBE32(bundle + 16, uint32_t(since / 1000000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint32_t MeasureTsc()
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t t0 = __rdtsc();
    Sleep(50);
    const uint64_t t1 = __rdtsc();
    QueryPerformanceCounter(&b);
    const double seconds = double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
    // KeQueryPerformanceFrequency returns 32 bits on the console; a
    // multi-GHz TSC overflows it, so titles must see a scaled timebase.
    return uint32_t(std::min((t1 - t0) / seconds, 4294967295.0));
}
}

void GpuInit();
void AudioInit();

void RuntimeInit(const std::filesystem::path& gameDir)
{
    g_gameDir = std::filesystem::absolute(gameDir);
    char buffer[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", buffer, MAX_PATH))
        g_userDir = std::filesystem::path(buffer) / "Documents" / "fm2";
    else
        g_userDir = g_gameDir.parent_path() / "user";
    std::filesystem::create_directories(g_userDir / "cache");
    std::filesystem::create_directories(g_userDir / "content");

    g_tscFrequency = MeasureTsc();
    Log("runtime: TSC %.3f MHz, user data in %s", g_tscFrequency / 1e6, g_userDir.string().c_str());

    std::thread(TimeStampThread).detach();
    GpuInit();
    AudioInit();
}

const std::filesystem::path& GameDir() { return g_gameDir; }
const std::filesystem::path& UserDir() { return g_userDir; }
uint32_t TscFrequency() { return g_tscFrequency; }

void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
{
    if (!overlapped)
        return;
    StoreBE32(overlapped + 0x00, result);
    StoreBE32(overlapped + 0x04, length);
    StoreBE32(overlapped + 0x18, extendedError);
    if (const uint32_t event = LoadBE32(overlapped + 0x0C))
    {
        if (auto object = GetHandle(event); object && object->kind == KernelObject::Kind::Event)
        {
            {
                std::lock_guard lock(g_objectMutex);
                static_cast<EventObject&>(*object).signaled = true;
                SyncGuestSignalState(*object);
            }
            SignalObjectsChanged();
        }
    }
    if (LoadBE32(overlapped + 0x10))
        LOG_ONCE("XOVERLAPPED completion routines are not called yet");
}
