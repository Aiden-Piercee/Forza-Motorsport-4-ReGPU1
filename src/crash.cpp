// Unhandled-exception report: host address, symbol (sub_XXXXXXXX is the
// guest function), and the guest address of a bad memory access.
#include <windows.h>
#include <dbghelp.h>

#include <string>

#include "kernel/thread.h"
#include "log.h"
#include "memory.h"

namespace
{
void Describe(DWORD64 address, char* out, size_t size)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
        snprintf(out, size, "%s+%llX", symbol->Name, displacement);
    else
        snprintf(out, size, "%016llX", address);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;
    char where[300];
    Describe(DWORD64(record->ExceptionAddress), where, sizeof(where));
    GuestThread* thread = CurrentGuestThread();
    Log("CRASH: exception %08lX at %s on guest thread %u", record->ExceptionCode, where, thread ? thread->id : 0);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const uint64_t fault = record->ExceptionInformation[1];
        const uint64_t guest = fault - uint64_t(g_base);
        Log("CRASH: %s host %016llX = guest %llX", record->ExceptionInformation[0] ? "write to" : "read from", fault, guest);
    }
    if (thread && thread->ctx)
        Log("CRASH: guest lr %08llX r1 %08X r3 %08X", thread->ctx->lr, thread->ctx->r1.u32, thread->ctx->r3.u32);

    // Host call stack.
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        Describe(frame.AddrPC.Offset, where, sizeof(where));
        Log("CRASH:   #%d %s", i, where);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
}

// Host stack of another thread (suspended while walking); names of
// recompiled frames are the guest functions.
std::string HostStack(void* threadHandle, int maxFrames)
{
    std::string out;
    if (SuspendThread(threadHandle) == DWORD(-1))
        return out;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(threadHandle, &context))
    {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        char where[300];
        for (int i = 0; i < maxFrames; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), threadHandle, &frame, &context, nullptr,
                    SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                break;
            Describe(frame.AddrPC.Offset, where, sizeof(where));
            char* name = where;
            if (!strncmp(name, "__imp__", 7))
                name += 7;
            if (char* plus = strchr(name, '+'))
                *plus = 0;
            if (!out.empty())
                out += " < ";
            out += name;
        }
    }
    ResumeThread(threadHandle);
    return out;
}

void CrashHandlerInit()
{
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(OnCrash);
}
