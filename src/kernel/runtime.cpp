#include "kernel/runtime.h"

#include <chrono>
#include <thread>

#include <intrin.h>
#include <windows.h>

#include <unordered_map>

#include "kernel/imports.h"
#include "kernel/thread.h"
#include "loader.h"
#include "render/presenter.h"
#include "settings.h"

namespace
{
std::filesystem::path g_gameDir;
std::filesystem::path g_userDir;
std::filesystem::path g_contentDir;
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

    g_tscFrequency = MeasureTsc();
    Log("runtime: TSC %.3f MHz, user data in %s", g_tscFrequency / 1e6, g_userDir.string().c_str());

    std::thread(TimeStampThread).detach();
    // The test launcher (launcher.cpp): settings before the window, GPU and audio start.
    bool RunLauncher();
    if (!RunLauncher())
    {
        Log("runtime: quit from the launcher");
        std::_Exit(0);
    }
    // Saves: one content folder per profile (profiles\<name>\content). Saves from before
    // profiles existed (the old shared content folder) become the default "Player" profile's.
    std::string folder;
    for (char c : GetSettings().profile)
        folder += strchr("<>:\"/\\|?*", c) || c < 32 ? '_' : c;
    g_contentDir = g_userDir / "profiles" / folder / "content";
    std::error_code ec;
    const auto legacy = g_userDir / "content";
    if (GetSettings().profile == "Player" && !std::filesystem::exists(g_contentDir, ec) && std::filesystem::exists(legacy, ec))
    {
        std::filesystem::create_directories(g_contentDir.parent_path(), ec);
        std::filesystem::rename(legacy, g_contentDir, ec);
        Log("runtime: moved saves to the Player profile (%s)", ec ? ec.message().c_str() : "ok");
    }
    std::filesystem::create_directories(g_contentDir, ec);
    std::filesystem::create_directories(DlcDir(), ec);
    Log("runtime: profile %s, saves in %s", GetSettings().profile.c_str(), g_contentDir.string().c_str());
    PresenterInit();
    GpuInit();
    AudioInit();
}

const std::filesystem::path& GameDir() { return g_gameDir; }
const std::filesystem::path& UserDir() { return g_userDir; }
const std::filesystem::path& ContentDir() { return g_contentDir; }
const std::filesystem::path& DlcDir()
{
    static const std::filesystem::path dir = g_userDir / "dlc";
    return dir;
}
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

void TraceImport(const char* name, PPCContext& ctx)
{
    static const double after = getenv("FM2_TRACE_IMPORTS") ? atof(getenv("FM2_TRACE_IMPORTS")) : -1;
    if (after < 0)
        return;
    static const auto start = Clock::now();
    if (std::chrono::duration<double>(Clock::now() - start).count() < after)
        return;
    static std::mutex mutex;
    static std::unordered_map<std::string, int> counts;
    std::lock_guard lock(mutex);
    static const int limit = getenv("FM2_TRACE_LIMIT") ? atoi(getenv("FM2_TRACE_LIMIT")) : 5;
    if (counts[name]++ < limit)
        Log("import %s (thread %u, lr %08llX, r3 %08X r4 %08X r5 %08X r9 %08X)", name, CurrentGuestThreadId(), ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r9.u32);
}
