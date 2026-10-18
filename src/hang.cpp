// Hang diagnostics: the host call stack of every thread in the process, logged when a
// renderer wait runs far too long, so a lockup in the field says what each worker is
// stuck in.
#include <string>

#include <windows.h>
#include <tlhelp32.h>

#include "log.h"

std::string HostStack(void* threadHandle, int maxFrames);

void LogAllHostThreadStacks(const char* why)
{
    Log("hang: %s; host thread stacks:", why);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        const std::string stack = HostStack(thread, 14);
        Log("hang:   thread %lu (priority %d): %s", entry.th32ThreadID, GetThreadPriority(thread), stack.c_str());
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}
