#include "kernel/thread.h"

#include <unordered_map>

#include <windows.h>

#include "log.h"

namespace
{
uint32_t g_tlsTemplate = 0;
uint32_t g_tlsDataSize = 0;
uint32_t g_tlsSlotCount = 64;
std::atomic<uint32_t> g_nextThreadId{ 1 };
thread_local GuestThread* t_thread = nullptr;

std::mutex g_threadsMutex;
std::unordered_map<uint32_t, std::weak_ptr<ThreadObject>> g_threadsByKthread;

std::mutex g_tlsMutex;
std::vector<bool> g_tlsUsed;

// Host stack for recompiled code: frames are big and call chains deep.
constexpr size_t kHostStackSize = 16 * 1024 * 1024;

struct ExitThreadSignal
{
    uint32_t code;
};

std::shared_ptr<GuestThread> SetUpGuestThread(uint32_t stackSize, const char* name)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->name = name ? name : "";

    stackSize = std::max<uint32_t>((stackSize + 0xFFF) & ~0xFFFu, 0x40000);
    thread->stackLow = GuestAlloc(Heap::Virtual, stackSize, 0x1000);
    thread->stackHigh = thread->stackLow + stackSize;

    // 64 KB aligned: titles disable interrupts with "mtmsrd r13,1", relying
    // on the PCR address having MSR[EE] (0x8000) clear.
    thread->pcr = GuestAlloc(Heap::Virtual, kpcr::Size, 0x10000);
    thread->kthread = GuestAlloc(Heap::Virtual, kthread::Size, 0x100);
    // Static TLS: the .tls template, then the dynamic slots.
    thread->tls = GuestAlloc(Heap::Virtual, g_tlsDataSize + g_tlsSlotCount * 4 + 16);
    if (g_tlsDataSize)
        memcpy(GuestPtr(thread->tls), GuestPtr(g_tlsTemplate), g_tlsDataSize);

    StoreBE32(thread->pcr + kpcr::TlsPtr, thread->tls);
    StoreBE32(thread->pcr + kpcr::PcrPtr, thread->pcr);
    StoreBE32(thread->pcr + kpcr::StackBase, thread->stackHigh);
    StoreBE32(thread->pcr + kpcr::StackEnd, thread->stackLow);
    StoreBE32(thread->pcr + kpcr::CurrentThread, thread->kthread);
    *GuestPtr<uint8_t>(thread->pcr + kpcr::CurrentCpu) = uint8_t(thread->id % 6);

    *GuestPtr<uint8_t>(thread->kthread) = 6; // dispatcher type: thread
    StoreBE32(thread->kthread + kthread::StackBase, thread->stackHigh);
    StoreBE32(thread->kthread + kthread::StackLimit, thread->stackLow);
    StoreBE32(thread->kthread + kthread::TlsAddress, thread->tls);
    StoreBE32(thread->kthread + kthread::ThreadId, thread->id);
    thread->tlsSlots.assign(g_tlsSlotCount, 0);
    return thread;
}

DWORD WINAPI HostThreadMain(void* param)
{
    auto* object = static_cast<ThreadObject*>(param);
    std::shared_ptr<GuestThread> thread = object->thread;
    // Keep the object alive until the thread is done with it.
    std::shared_ptr<ThreadObject> self = std::static_pointer_cast<ThreadObject>(GetHandle(object->handle));
    t_thread = thread.get();

    {
        std::unique_lock lock(thread->suspendMutex);
        thread->suspendCv.wait(lock, [&] { return thread->suspendCount <= 0; });
    }

    uint32_t exitCode = 0;
    try
    {
        PPCContext ctx{};
        InitGuestContext(ctx, *thread);
        if (thread->hostEntry)
            thread->hostEntry(ctx);
        else if (thread->xapiStartup)
        {
            ctx.r3.u64 = thread->entry;
            ctx.r4.u64 = thread->param;
            GuestCall(ctx, thread->xapiStartup);
        }
        else
        {
            ctx.r3.u64 = thread->param;
            GuestCall(ctx, thread->entry);
        }
        exitCode = ctx.r3.u32;
    }
    catch (const ExitThreadSignal& exit)
    {
        exitCode = exit.code;
    }

    {
        std::lock_guard lock(g_objectMutex);
        self->exited = true;
        self->exitCode = exitCode;
        if (self->guestObject)
            StoreBE32(self->guestObject + 4, 1);
    }
    SignalObjectsChanged();
    thread->ctx = nullptr;
    return 0;
}
}

void ThreadSystemInit(uint32_t tlsTemplate, uint32_t tlsDataSize, uint32_t tlsSlotCount)
{
    g_tlsTemplate = tlsTemplate;
    g_tlsDataSize = tlsDataSize;
    g_tlsSlotCount = tlsSlotCount ? tlsSlotCount : 64;
    g_tlsUsed.assign(g_tlsSlotCount, false);
}

void InitGuestContext(PPCContext& ctx, GuestThread& thread)
{
    ctx.r1.u64 = thread.stackHigh - 0x100;
    ctx.r13.u64 = thread.pcr;
    ctx.msr = 0x9030; // EE | ME | IR | DR
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    ctx.fpscr.disableFlushModeUnconditional();
    thread.ctx = &ctx;
}

namespace
{
std::shared_ptr<ThreadObject> StartThread(uint32_t entry, uint32_t param, uint32_t xapiStartup, uint32_t stackSize,
    bool suspended, const char* name, std::function<void(PPCContext&)> hostEntry)
{
    auto object = std::make_shared<ThreadObject>();
    object->thread = SetUpGuestThread(stackSize, name);
    object->thread->hostEntry = std::move(hostEntry);
    object->thread->entry = entry;
    object->thread->param = param;
    object->thread->xapiStartup = xapiStartup;
    object->thread->object = object;
    object->thread->suspendCount = suspended ? 1 : 0;
    StoreBE32(object->thread->kthread + kthread::StartAddress, entry);
    object->guestObject = object->thread->kthread;
    CreateHandle(object);
    {
        std::lock_guard lock(g_threadsMutex);
        g_threadsByKthread[object->thread->kthread] = object;
    }

    DWORD hostId = 0;
    HANDLE handle = CreateThread(nullptr, kHostStackSize, HostThreadMain, object.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);
    if (!handle)
        Fatal("CreateThread failed (%lu)", GetLastError());
    object->thread->hostHandle = handle;

    wchar_t wname[64];
    swprintf(wname, 64, L"guest %u %S", object->thread->id, name ? name : "");
    SetThreadDescription(handle, wname);
    Log("thread %u created: %s %08X param %08X%s", object->thread->id, name ? name : "entry", entry, param, suspended ? " (suspended)" : "");
    return object;
}
}

std::shared_ptr<ThreadObject> CreateGuestThread(uint32_t entry, uint32_t param, uint32_t xapiStartup,
    uint32_t stackSize, bool suspended, const char* name)
{
    return StartThread(entry, param, xapiStartup, stackSize, suspended, name, nullptr);
}

std::shared_ptr<ThreadObject> CreateHostGuestThread(const char* name, std::function<void(PPCContext&)> body)
{
    return StartThread(0, 0, 0, 0x10000, false, name, std::move(body));
}

void ResumeGuestThread(GuestThread& thread)
{
    {
        std::lock_guard lock(thread.suspendMutex);
        --thread.suspendCount;
    }
    thread.suspendCv.notify_all();
}

int SuspendGuestThread(GuestThread& thread)
{
    // Only threads that have not started yet can be suspended for real;
    // the title only does this at creation time.
    std::lock_guard lock(thread.suspendMutex);
    LOG_ONCE("SuspendGuestThread on a running thread is not implemented");
    return thread.suspendCount++;
}

void ExitCurrentGuestThread(uint32_t code)
{
    throw ExitThreadSignal{ code };
}

uint32_t InterruptLockOwner();
std::string HostStack(void* threadHandle, int maxFrames);

void LogGuestThreads()
{
    Log("  interrupt lock owner: thread %u", InterruptLockOwner());
    std::lock_guard lock(g_threadsMutex);
    for (auto& [kthread, weak] : g_threadsByKthread)
    {
        auto object = weak.lock();
        if (!object || object->exited)
            continue;
        GuestThread& t = *object->thread;
        if (t.ctx)
        {
            Log("  thread %u %-16s %s", t.id, t.name.c_str(), HostStack(t.hostHandle, 10).c_str());
            if (!t.waitingOn.empty())
                Log("      waits on %s", t.waitingOn.c_str());
        }
        else
            Log("  thread %u %-16s not started (suspend %d)", t.id, t.name.c_str(), t.suspendCount);
    }
}

std::shared_ptr<ThreadObject> FindThreadByKthread(uint32_t kthread)
{
    std::lock_guard lock(g_threadsMutex);
    auto it = g_threadsByKthread.find(kthread);
    return it == g_threadsByKthread.end() ? nullptr : it->second.lock();
}

GuestThread* CurrentGuestThread()
{
    return t_thread;
}

uint32_t CurrentGuestThreadId()
{
    return t_thread ? t_thread->id : 0;
}

void GuestCall(PPCContext& ctx, uint32_t address)
{
    PPCFunc* func = PPC_LOOKUP_FUNC(g_base, address);
    if (!func)
        Fatal("call to unmapped guest function %08X (lr %08llX)", address, ctx.lr);
    func(ctx, g_base);
}

namespace
{
std::recursive_mutex g_interruptLock;
std::atomic<uint32_t> g_interruptLockOwner{ 0 };
thread_local int t_interruptDepth = 0;

void Lock()
{
    g_interruptLock.lock();
    if (t_interruptDepth++ == 0)
        g_interruptLockOwner = CurrentGuestThreadId();
}

void Unlock()
{
    if (--t_interruptDepth == 0)
        g_interruptLockOwner = 0;
    g_interruptLock.unlock();
}
}

void InterruptLockAcquire() { Lock(); }
void InterruptLockRelease() { Unlock(); }
uint32_t InterruptLockOwner() { return g_interruptLockOwner; }

void PPCMsrChanged(PPCContext& ctx, uint64_t oldMsr)
{
    if (oldMsr & 0x8000)
        Lock();   // interrupts now disabled
    else if (t_interruptDepth > 0)
        Unlock(); // re-enabled
}

uint32_t TlsAllocSlot()
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < g_tlsUsed.size(); i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            return i;
        }
    }
    return 0xFFFFFFFF;
}

void TlsFreeSlot(uint32_t slot)
{
    std::lock_guard lock(g_tlsMutex);
    if (slot < g_tlsUsed.size())
        g_tlsUsed[slot] = false;
}
