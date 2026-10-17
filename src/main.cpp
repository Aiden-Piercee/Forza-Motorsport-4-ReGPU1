#include <filesystem>
#include <thread>

#include <windows.h>

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "loader.h"
#include "log.h"
#include "memory.h"

void CrashHandlerInit();
void GpuLogState();
void D3DDebugLog();

int main(int argc, char** argv)
{
    CrashHandlerInit();
    std::filesystem::path gameDir = argc > 1 ? argv[1] : "game";
    Log("fm2: game folder %s", std::filesystem::absolute(gameDir).string().c_str());

    MemoryInit();
    LoadTitle(gameDir / "default.xex");
    RuntimeInit(gameDir);
    ThreadSystemInit(g_title.tlsTemplate, g_title.tlsDataSize, g_title.tlsSlotCount);

    void StartWatch();
    StartWatch();
    auto main = CreateGuestThread(g_title.entryPoint, 0, 0, g_title.stackSize, false, "main");
    char watchdogMs[16] = {};
    const DWORD watchdog = GetEnvironmentVariableA("FM2_WATCHDOG", watchdogMs, 16) ? (atoi(watchdogMs) > 1 ? std::max(50, atoi(watchdogMs)) : 3000) : INFINITE;
    if (getenv("FM2_WATCHW"))
    {
        std::thread([] {
            void ArmWatchpointAll();
            for (;;)
            {
                Sleep(10);
                ArmWatchpointAll();
            }
        }).detach();
    }
    while (WaitForSingleObject(main->thread->hostHandle, watchdog) == WAIT_TIMEOUT)
    {
        Log("watchdog:");
        LogGuestThreads();
        GpuLogState();
        D3DDebugLog();
        void LogFileStats();
        LogFileStats();
    }
    Log("fm2: main thread exited with %08X", main->exitCode);
    return 0;
}
