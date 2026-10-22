// FM2_TRACE_EXIT=1: log how the process ends (debug). Covers exit() (atexit), console
// control events, abort(), and ExitProcess from anywhere (a TLS callback runs in every
// process detach), each with the exit code and the calling thread's host stack. A process
// killed from outside (TerminateProcess, taskkill /F) runs none of these: no line at all.
#include <windows.h>

#include <csignal>
#include <cstdlib>
#include <string>

#include "log.h"

std::string CurrentHostStack(int maxFrames); // crash.cpp

namespace
{
bool g_trace = false;

void Report(const char* how)
{
    DWORD code = 0;
    GetExitCodeProcess(GetCurrentProcess(), &code);
    Log("exit: %s, exit code %lu (%08lX), thread %lu: %s", how, code, code, GetCurrentThreadId(), CurrentHostStack(24).c_str());
}

void OnAtExit()
{
    Report("exit()");
}

BOOL WINAPI OnConsoleEvent(DWORD type)
{
    char how[48];
    snprintf(how, sizeof(how), "console event %lu", type);
    Report(how);
    return FALSE;
}

void OnAbort(int)
{
    Report("abort()");
}

void NTAPI OnTls(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_PROCESS_DETACH && g_trace)
        Report("process detach (ExitProcess)");
}

struct Init
{
    Init()
    {
        if (!getenv("FM2_TRACE_EXIT"))
            return;
        g_trace = true;
        atexit(OnAtExit);
        SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
        signal(SIGABRT, OnAbort);
    }
} g_init;
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_fm2ExitTls")
#pragma section(".CRT$XLF", long, read)
extern "C" __declspec(allocate(".CRT$XLF")) const PIMAGE_TLS_CALLBACK g_fm2ExitTls = OnTls;
