// stackdump <pid> [frames]: prints every thread's call stack of a running process (fm2.exe), with
// symbols from its PDB, without stopping it for more than a moment per thread. For hangs: the
// recompiled guest functions show as sub_8XXXXXXX, so a stuck game shows what each thread waits on.
//   clang-cl /O2 tools/stackdump.cpp dbghelp.lib /Fe:build/stackdump.exe   (or see CMakeLists.txt)
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "stackdump <pid> [frames]\n");
        return 2;
    }
    const DWORD pid = DWORD(strtoul(argv[1], nullptr, 10));
    const int maxFrames = argc > 2 ? atoi(argv[2]) : 24;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        fprintf(stderr, "can't open process %lu (%lu)\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(process, nullptr, TRUE))
    {
        fprintf(stderr, "SymInitialize failed (%lu)\n", GetLastError());
        return 1;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    std::vector<DWORD> threads;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid)
            threads.push_back(te.th32ThreadID);
    CloseHandle(snap);
    for (DWORD tid : threads)
    {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread)
            continue;
        std::wstring desc;
        if (PWSTR d = nullptr; SUCCEEDED(GetThreadDescription(thread, &d)) && d)
        {
            desc = d;
            LocalFree(d);
        }
        SuspendThread(thread);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        std::string out;
        if (GetThreadContext(thread, &c))
        {
            STACKFRAME64 f{};
            f.AddrPC.Offset = c.Rip;
            f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = c.Rbp;
            f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = c.Rsp;
            f.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < maxFrames && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &f, &c, nullptr, SymFunctionTableAccess64,
                                                          SymGetModuleBase64, nullptr);
                 i++)
            {
                char buf[sizeof(SYMBOL_INFO) + 256]{};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                char line[300];
                if (SymFromAddr(process, f.AddrPC.Offset, &disp, sym))
                    snprintf(line, sizeof(line), "    %s+%llX\n", sym->Name, (unsigned long long)disp);
                else
                    snprintf(line, sizeof(line), "    %016llX\n", (unsigned long long)f.AddrPC.Offset);
                out += line;
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
        printf("thread %lu %ls\n%s", tid, desc.c_str(), out.c_str());
    }
    return 0;
}
