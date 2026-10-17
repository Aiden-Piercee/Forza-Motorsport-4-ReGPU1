// Unhandled-exception report: host address, symbol (sub_XXXXXXXX is the
// guest function), and the guest address of a bad memory access.
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <string>

#include "kernel/imports.h"
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
        Log("CRASH: guest lr %08llX ctr %08llX r1 %08X r3 %08X", thread->ctx->lr, thread->ctx->ctr.u64, thread->ctx->r1.u32, thread->ctx->r3.u32);

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
    Log("CRASH: other threads:");
    LogGuestThreads();
    return EXCEPTION_EXECUTE_HANDLER;
}
}

// Host stack of another thread (suspended while walking); names of
// recompiled frames are the guest functions.
std::string HostStack(void* threadHandle, int maxFrames)
{
    std::string out;
    if (GetThreadId(threadHandle) == GetCurrentThreadId())
        return "(this thread)";
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

// Hardware write watchpoint on a guest byte (FM2_WATCHW=addr or
// FM2_WATCHW=ptr,offset to watch [ptr]+offset). Logs the writing thread's
// host stack, i.e. the guest functions, for the first writes.
namespace
{
std::atomic<int> g_watchHits{ 0 };

LONG WINAPI OnWatchHit(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 7))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_watchHits++ < 40)
    {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        std::string out;
        char where[300];
        for (int i = 0; i < 6 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++)
        {
            Describe(frame.AddrPC.Offset, where, sizeof(where));
            out += out.empty() ? "" : " < ";
            out += where;
        }
        GuestThread* t = CurrentGuestThread();
        Log("watchpoint: thread %u wrote %02X (dr6 %llX): %s", t ? t->id : 0, *reinterpret_cast<uint8_t*>(info->ContextRecord->Dr0),
            info->ContextRecord->Dr6, out.c_str());
        if (t && t->ctx)
        {
            const PPCContext& g = *t->ctx;
            Log("watchpoint:   r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X", g.r3.u32, g.r4.u32, g.r5.u32,
                g.r6.u32, g.r7.u32, g.r8.u32, g.r9.u32, g.r10.u32, g.r11.u32, g.r12.u32);
            Log("watchpoint:   r24 %08X r25 %08X r26 %08X r27 %08X r28 %08X r29 %08X r30 %08X r31 %08X lr %08llX", g.r24.u32, g.r25.u32, g.r26.u32,
                g.r27.u32, g.r28.u32, g.r29.u32, g.r30.u32, g.r31.u32, g.lr);
        }
    }
    info->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

void ArmWatchpoint(void* threadHandle)
{
    static uint64_t address = [] {
        const char* v = getenv("FM2_WATCHW");
        if (!v)
            return uint64_t(0);
        AddVectoredExceptionHandler(1, OnWatchHit);
        uint32_t a = uint32_t(strtoul(v, nullptr, 16));
        if (const char* comma = strchr(v, ','))
            a = LoadBE32(a) + uint32_t(strtoul(comma + 1, nullptr, 16));
        Log("watchpoint on guest %08X", a);
        return uint64_t(g_base + a);
    }();
    if (!address)
        return;
    if (SuspendThread(threadHandle) == DWORD(-1))
        return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(threadHandle, &c);
    c.Dr0 = address;
    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16); // L0, write, 1 byte
    const uint32_t guest = uint32_t(address - uint64_t(g_base));
    if (guest >= 0xA0000000)
    {
        // Physical memory is mapped three times; watch every alias.
        const uint32_t offset = guest & 0x1FFFFFFF;
        uint64_t* regs[] = { &c.Dr1, &c.Dr2 };
        int n = 0;
        for (uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
        {
            if (view + offset == guest || n == 2)
                continue;
            *regs[n] = uint64_t(g_base + view + offset);
            c.Dr7 |= (1ull << (2 + n * 2)) | (1ull << (20 + n * 4));
            n++;
        }
    }
    SetThreadContext(threadHandle, &c);
    ResumeThread(threadHandle);
}
