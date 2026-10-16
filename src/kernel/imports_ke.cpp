// Threads, synchronisation, handles, TLS, time.
#include <atomic>
#include <thread>
#include <unordered_map>

#include <windows.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

namespace
{
constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;

std::mutex g_namedMutex;
std::unordered_map<std::string, std::weak_ptr<KernelObject>> g_named;

// Named objects (OBJECT_ATTRIBUTES with a name) are shared by name.
template <typename Make>
uint32_t CreateNamed(uint32_t handleOut, uint32_t attributes, Make make)
{
    const std::string name = ObjectAttributesName(attributes);
    std::shared_ptr<KernelObject> object;
    uint32_t status = X_STATUS_SUCCESS;
    if (!name.empty())
    {
        std::lock_guard lock(g_namedMutex);
        if (auto existing = g_named[name].lock())
        {
            object = existing;
            status = X_STATUS_OBJECT_NAME_EXISTS;
        }
        else
        {
            object = make();
            g_named[name] = object;
        }
    }
    else
        object = make();
    if (!object->createdBy)
        object->createdBy = uint32_t(CurrentGuestThread() && CurrentGuestThread()->ctx ? CurrentGuestThread()->ctx->lr : 0);
    StoreBE32(handleOut, CreateHandle(object));
    return status;
}

std::shared_ptr<ThreadObject> ThreadObjectFromKthread(uint32_t kthreadAddr)
{
    return FindThreadByKthread(kthreadAddr);
}

std::shared_ptr<KernelObject> ObjectFromHandleOrPseudo(uint32_t handle)
{
    if (handle == kCurrentThreadHandle)
    {
        if (GuestThread* t = CurrentGuestThread())
            return t->object.lock();
    }
    return GetHandle(handle);
}

// Objects referenced by guest pointer: threads are found by KTHREAD,
// everything else by its dispatcher header.
std::shared_ptr<KernelObject> ObjectFromPointer(uint32_t pointer)
{
    if (auto t = ThreadObjectFromKthread(pointer))
        return t;
    return GetDispatcherObject(pointer);
}

void SleepUntil(std::optional<Clock::time_point> deadline)
{
    if (!deadline)
    {
        Sleep(INFINITE);
        return;
    }
    const auto now = Clock::now();
    if (*deadline <= now)
    {
        SwitchToThread();
        return;
    }
    std::this_thread::sleep_until(*deadline);
}
}

// Threads ------------------------------------------------------------------

GUEST_EXPORT(ExCreateThread)
{
    // (handle*, stackSize, threadId*, xapiStartup, startAddress, startContext, creationFlags)
    const uint32_t handleOut = ctx.r3.u32, stackSize = ctx.r4.u32, idOut = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32, start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;
    auto t = CreateGuestThread(start, param, xapiStartup, stackSize, flags & 1, nullptr);
    StoreIfNonNull(handleOut, t->handle);
    StoreIfNonNull(idOut, t->thread->id);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(ExTerminateThread)
{
    ExitCurrentGuestThread(ctx.r3.u32);
}

GUEST_EXPORT(NtResumeThread)
{
    auto object = ObjectFromHandleOrPseudo(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Thread)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    auto& t = *static_cast<ThreadObject&>(*object).thread;
    StoreIfNonNull(ctx.r4.u32, uint32_t(t.suspendCount));
    ResumeGuestThread(t);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(KeResumeThread)
{
    auto object = ThreadObjectFromKthread(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, 0);
        return;
    }
    const int previous = object->thread->suspendCount;
    ResumeGuestThread(*object->thread);
    Return(ctx, uint32_t(previous));
}

GUEST_EXPORT(NtSuspendThread)
{
    auto object = ObjectFromHandleOrPseudo(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Thread)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    StoreIfNonNull(ctx.r4.u32, uint32_t(SuspendGuestThread(*static_cast<ThreadObject&>(*object).thread)));
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(KeSetAffinityThread)
{
    // (thread, affinity, previous*) - host scheduling decides.
    StoreIfNonNull(ctx.r5.u32, 0x3F);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(KeQueryBasePriorityThread)
{
    Return(ctx, 0);
}

GUEST_EXPORT(KeSetBasePriorityThread)
{
    Return(ctx, 0);
}

GUEST_EXPORT(KeDelayExecutionThread)
{
    // (mode, alertable, interval*)
    SleepUntil(TimeoutFromGuest(ctx.r5.u32));
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtYieldExecution)
{
    SwitchToThread();
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(KeGetCurrentProcessType)
{
    Return(ctx, 1); // title process
}

GUEST_EXPORT(KeEnterCriticalRegion) {}
GUEST_EXPORT(KeLeaveCriticalRegion) {}
GUEST_EXPORT(KeLockL2) { Return(ctx, 0); }
GUEST_EXPORT(KeUnlockL2) {}
GUEST_EXPORT(KiApcNormalRoutineNop) {}

GUEST_EXPORT(KeSetCurrentStackPointers)
{
    // (stackPointer, thread, stackAllocBase, stackBase, stackLimit). Only
    // XAPI fibers call this; they are replaced by host fibers.
    LOG_ONCE("KeSetCurrentStackPointers called (lr %08llX)", ctx.lr);
}

// IRQL and spinlocks --------------------------------------------------------

namespace
{
void AcquireSpin(uint32_t lock)
{
    auto& word = *reinterpret_cast<std::atomic<uint32_t>*>(GuestPtr(lock));
    const uint32_t me = __builtin_bswap32(CurrentGuestThread() ? CurrentGuestThread()->kthread : 1);
    for (int spin = 0;; spin++)
    {
        uint32_t expected = 0;
        if (word.compare_exchange_weak(expected, me, std::memory_order_acquire))
            return;
        if (spin < 128)
            _mm_pause();
        else
            SwitchToThread();
    }
}

void ReleaseSpin(uint32_t lock)
{
    reinterpret_cast<std::atomic<uint32_t>*>(GuestPtr(lock))->store(0, std::memory_order_release);
}
}

GUEST_EXPORT(KfAcquireSpinLock)
{
    AcquireSpin(ctx.r3.u32);
    Return(ctx, 0); // old IRQL
}

GUEST_EXPORT(KfReleaseSpinLock)
{
    ReleaseSpin(ctx.r3.u32);
}

GUEST_EXPORT(KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpin(ctx.r3.u32);
}

GUEST_EXPORT(KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseSpin(ctx.r3.u32);
}

GUEST_EXPORT(KeRaiseIrqlToDpcLevel)
{
    Return(ctx, 0);
}

GUEST_EXPORT(KfLowerIrql) {}

// Interlocked SLists (SLIST_HEADER: Next u32, Depth u16, Sequence u16) ------

namespace
{
std::mutex g_slistMutex;
}

GUEST_EXPORT(InterlockedPopEntrySList)
{
    std::lock_guard lock(g_slistMutex);
    const uint32_t head = ctx.r3.u32;
    const uint32_t first = LoadBE32(head);
    if (first)
    {
        StoreBE32(head, LoadBE32(first));
        auto* depth = GuestPtr<uint16_t>(head + 4);
        *depth = __builtin_bswap16(__builtin_bswap16(*depth) - 1);
    }
    Return(ctx, first);
}

GUEST_EXPORT(InterlockedFlushSList)
{
    std::lock_guard lock(g_slistMutex);
    const uint32_t head = ctx.r3.u32;
    const uint32_t first = LoadBE32(head);
    StoreBE32(head, 0);
    *GuestPtr<uint16_t>(head + 4) = 0;
    Return(ctx, first);
}

// TLS ------------------------------------------------------------------------

GUEST_EXPORT(KeTlsAlloc)
{
    Return(ctx, TlsAllocSlot());
}

GUEST_EXPORT(KeTlsFree)
{
    TlsFreeSlot(ctx.r3.u32);
    Return(ctx, 1);
}

GUEST_EXPORT(KeTlsGetValue)
{
    GuestThread* t = CurrentGuestThread();
    const uint32_t slot = ctx.r3.u32;
    Return(ctx, t && slot < t->tlsSlots.size() ? t->tlsSlots[slot] : 0);
}

GUEST_EXPORT(KeTlsSetValue)
{
    GuestThread* t = CurrentGuestThread();
    const uint32_t slot = ctx.r3.u32;
    if (!t || slot >= t->tlsSlots.size())
    {
        Return(ctx, 0);
        return;
    }
    t->tlsSlots[slot] = ctx.r4.u32;
    Return(ctx, 1);
}

// Time -----------------------------------------------------------------------

GUEST_EXPORT(KeQueryPerformanceFrequency)
{
    Return(ctx, TscFrequency());
}

GUEST_EXPORT(KeQuerySystemTime)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    StoreBE64(ctx.r3.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// Dispatcher objects by guest pointer ----------------------------------------

GUEST_EXPORT(KeInitializeSemaphore)
{
    // (semaphore, count, limit)
    const uint32_t s = ctx.r3.u32;
    memset(GuestPtr(s), 0, 0x14);
    *GuestPtr<uint8_t>(s) = 5;
    *GuestPtr<uint8_t>(s + 2) = 5;
    StoreBE32(s + 4, ctx.r4.u32);
    StoreBE32(s + 0x10, ctx.r5.u32);
    GetDispatcherObject(s);
}

GUEST_EXPORT(KeReleaseSemaphore)
{
    // (semaphore, increment, adjustment, wait)
    auto object = GetDispatcherObject(ctx.r3.u32);
    int32_t previous = 0;
    {
        std::lock_guard lock(g_objectMutex);
        if (object->kind == KernelObject::Kind::Semaphore)
        {
            auto& s = static_cast<SemaphoreObject&>(*object);
            previous = s.count;
            s.count = std::min(s.count + int32_t(ctx.r5.u32), s.maximum > 0 ? s.maximum : INT32_MAX);
            SyncGuestSignalState(s);
        }
    }
    SignalObjectsChanged();
    Return(ctx, uint32_t(previous));
}

namespace
{
const uint32_t g_traceObject = [] {
    const char* v = getenv("FM2_TRACE_OBJ");
    return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
}();

void TraceObject(const char* what, uint32_t address, PPCContext& ctx)
{
    if (g_traceObject && address == g_traceObject)
        Log("obj %08X: %s by thread %u (lr %08llX)", address, what, CurrentGuestThreadId(), ctx.lr);
}

uint32_t SetEvent(const std::shared_ptr<KernelObject>& object, bool state)
{
    uint32_t previous = 0;
    {
        std::lock_guard lock(g_objectMutex);
        if (object->kind == KernelObject::Kind::Event)
        {
            auto& e = static_cast<EventObject&>(*object);
            previous = e.signaled;
            e.signaled = state;
            SyncGuestSignalState(e);
        }
    }
    if (state)
        SignalObjectsChanged();
    return previous;
}
}

GUEST_EXPORT(KeSetEvent)
{
    TraceObject("KeSetEvent", ctx.r3.u32, ctx);
    Return(ctx, SetEvent(GetDispatcherObject(ctx.r3.u32), true));
}

GUEST_EXPORT(KeResetEvent)
{
    TraceObject("KeResetEvent", ctx.r3.u32, ctx);
    Return(ctx, SetEvent(GetDispatcherObject(ctx.r3.u32), false));
}

GUEST_EXPORT(KeWaitForSingleObject)
{
    // (object, reason, mode, alertable, timeout*)
    const uint32_t address = ctx.r3.u32;
    auto object = ObjectFromPointer(address);
    TraceObject("KeWaitForSingleObject", address, ctx);
    Return(ctx, WaitForObjects({ object }, false, TimeoutFromGuest(ctx.r7.u32)));
    if (g_traceObject && g_traceObject == address && ctx.r3.u32 != X_STATUS_TIMEOUT)
        Log("obj %08X: wait returned %X", address, ctx.r3.u32);
}

GUEST_EXPORT(KeWaitForMultipleObjects)
{
    // (count, objects*, waitType, reason, mode, alertable, timeout*, waitBlocks*)
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objects.push_back(ObjectFromPointer(LoadBE32(ctx.r4.u32 + i * 4)));
    Return(ctx, WaitForObjects(objects, ctx.r5.u32 == 0, TimeoutFromGuest(ctx.r9.u32)));
}

// Handles --------------------------------------------------------------------

GUEST_EXPORT(NtClose)
{
    Return(ctx, CloseHandle(ctx.r3.u32) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE);
}

GUEST_EXPORT(NtDuplicateObject)
{
    // (handle, newHandle*, options)
    auto object = ObjectFromHandleOrPseudo(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    StoreIfNonNull(ctx.r4.u32, CreateHandle(object));
    if (ctx.r5.u32 & 1) // DUPLICATE_CLOSE_SOURCE
        CloseHandle(ctx.r3.u32);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(ObReferenceObjectByHandle)
{
    // (handle, objectType, object*)
    auto object = ObjectFromHandleOrPseudo(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t guest;
    if (object->kind == KernelObject::Kind::Thread)
    {
        guest = std::static_pointer_cast<ThreadObject>(object)->thread->kthread;
    }
    else
        guest = GetGuestObject(object);
    StoreIfNonNull(ctx.r5.u32, guest);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(ObDereferenceObject) {}

// Events, semaphores, mutants, timers by handle --------------------------------

GUEST_EXPORT(NtCreateEvent)
{
    // (handle*, attributes*, type (0 notification, 1 synchronization), initialState)
    const bool manual = ctx.r5.u32 == 0;
    const bool initial = ctx.r6.u32 != 0;
    Return(ctx, CreateNamed(ctx.r3.u32, ctx.r4.u32, [&] { return std::make_shared<EventObject>(manual, initial); }));
}

GUEST_EXPORT(NtSetEvent)
{
    auto object = GetHandle(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    StoreIfNonNull(ctx.r4.u32, SetEvent(object, true));
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtClearEvent)
{
    auto object = GetHandle(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    SetEvent(object, false);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtCreateSemaphore)
{
    // (handle*, attributes*, initial, maximum)
    const int32_t initial = int32_t(ctx.r5.u32), maximum = int32_t(ctx.r6.u32);
    Return(ctx, CreateNamed(ctx.r3.u32, ctx.r4.u32, [&] { return std::make_shared<SemaphoreObject>(initial, maximum); }));
}

GUEST_EXPORT(NtReleaseSemaphore)
{
    // (handle, count, previous*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Semaphore)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t status = X_STATUS_SUCCESS;
    {
        std::lock_guard lock(g_objectMutex);
        auto& s = static_cast<SemaphoreObject&>(*object);
        StoreIfNonNull(ctx.r5.u32, uint32_t(s.count));
        if (s.maximum > 0 && s.count + int32_t(ctx.r4.u32) > s.maximum)
            status = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        else
            s.count += int32_t(ctx.r4.u32);
        SyncGuestSignalState(s);
    }
    SignalObjectsChanged();
    Return(ctx, status);
}

GUEST_EXPORT(NtCreateMutant)
{
    // (handle*, attributes*, initialOwner)
    const uint32_t owner = ctx.r5.u32 ? CurrentGuestThreadId() : 0;
    Return(ctx, CreateNamed(ctx.r3.u32, ctx.r4.u32, [&] { return std::make_shared<MutantObject>(owner); }));
}

GUEST_EXPORT(NtReleaseMutant)
{
    // (handle, previous*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Mutant)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t status = X_STATUS_SUCCESS;
    {
        std::lock_guard lock(g_objectMutex);
        auto& m = static_cast<MutantObject&>(*object);
        StoreIfNonNull(ctx.r4.u32, uint32_t(m.recursion));
        if (m.owner != CurrentGuestThreadId())
            status = X_STATUS_MUTANT_NOT_OWNED;
        else if (--m.recursion == 0)
            m.owner = 0;
    }
    SignalObjectsChanged();
    Return(ctx, status);
}

GUEST_EXPORT(NtCreateTimer)
{
    // (handle*, attributes*, type (0 notification, 1 synchronization))
    const bool manual = ctx.r5.u32 == 0;
    Return(ctx, CreateNamed(ctx.r3.u32, ctx.r4.u32, [&] { return std::make_shared<TimerObject>(manual); }));
}

GUEST_EXPORT(NtSetTimerEx)
{
    // (handle, dueTime*, apcRoutine, apcMode, apcContext, resume, period (ms), previousState*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Timer)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    if (ctx.r5.u32)
        LOG_ONCE("NtSetTimerEx with an APC routine (%08X) is not supported", ctx.r5.u32);
    {
        std::lock_guard lock(g_objectMutex);
        auto& t = static_cast<TimerObject&>(*object);
        StoreIfNonNull(ctx.r10.u32, t.IsSignaled(0));
        t.due = TimeoutFromGuestValue(int64_t(LoadBE64(ctx.r4.u32)));
        t.period = std::chrono::milliseconds(ctx.r9.u32);
    }
    SignalObjectsChanged();
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(NtCancelTimer)
{
    auto object = GetHandle(ctx.r3.u32);
    if (!object || object->kind != KernelObject::Kind::Timer)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    std::lock_guard lock(g_objectMutex);
    auto& t = static_cast<TimerObject&>(*object);
    StoreIfNonNull(ctx.r4.u32, t.IsSignaled(0));
    t.due.reset();
    Return(ctx, X_STATUS_SUCCESS);
}

// Waits by handle ---------------------------------------------------------------

GUEST_EXPORT(NtWaitForSingleObjectEx)
{
    // (handle, mode, alertable, timeout*)
    auto object = ObjectFromHandleOrPseudo(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    Return(ctx, WaitForObjects({ object }, false, TimeoutFromGuest(ctx.r6.u32)));
}

GUEST_EXPORT(NtWaitForMultipleObjectsEx)
{
    // (count, handles*, waitType, mode, alertable, timeout*)
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto object = ObjectFromHandleOrPseudo(LoadBE32(ctx.r4.u32 + i * 4));
        if (!object)
        {
            Return(ctx, X_STATUS_INVALID_HANDLE);
            return;
        }
        objects.push_back(object);
    }
    Return(ctx, WaitForObjects(objects, ctx.r5.u32 == 0, TimeoutFromGuest(ctx.r8.u32)));
}

GUEST_EXPORT(NtSignalAndWaitForSingleObjectEx)
{
    // (signal, wait, mode, alertable, timeout*)
    auto signal = GetHandle(ctx.r3.u32);
    auto wait = ObjectFromHandleOrPseudo(ctx.r4.u32);
    if (!signal || !wait)
    {
        Return(ctx, X_STATUS_INVALID_HANDLE);
        return;
    }
    {
        std::lock_guard lock(g_objectMutex);
        switch (signal->kind)
        {
        case KernelObject::Kind::Event:
            static_cast<EventObject&>(*signal).signaled = true;
            break;
        case KernelObject::Kind::Semaphore:
            static_cast<SemaphoreObject&>(*signal).count++;
            break;
        case KernelObject::Kind::Mutant:
        {
            auto& m = static_cast<MutantObject&>(*signal);
            if (m.owner == CurrentGuestThreadId() && --m.recursion == 0)
                m.owner = 0;
            break;
        }
        default:
            break;
        }
        SyncGuestSignalState(*signal);
    }
    SignalObjectsChanged();
    Return(ctx, WaitForObjects({ wait }, false, TimeoutFromGuest(ctx.r7.u32)));
}

// Pools ----------------------------------------------------------------------

GUEST_EXPORT(ExAllocatePool)
{
    Return(ctx, GuestAlloc(Heap::Virtual, ctx.r3.u32));
}

GUEST_EXPORT(ExFreePool)
{
    GuestFree(ctx.r3.u32);
}

GUEST_EXPORT(ExRegisterTitleTerminateNotification) {}

GUEST_EXPORT(ExGetXConfigSetting)
{
    // (category, setting, buffer, bufferSize, requiredSize*)
    const uint16_t category = uint16_t(ctx.r3.u32), setting = uint16_t(ctx.r4.u32);
    const uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32;
    uint32_t value = 0, valueSize = 4;
    switch ((category << 16) | setting)
    {
    case 0x00030009: value = 1; break;          // user: language (English)
    case 0x0003000A: value = 0x00040000; break; // user: video flags (widescreen)
    case 0x00020002: value = 0x00800000; break; // console: AV region
    case 0x00030007: value = 0; break;          // user: retail flags
    default:
        LOG_ONCE("ExGetXConfigSetting: unknown setting %04X/%04X, returning 0", category, setting);
        break;
    }
    if (buffer && size >= valueSize)
        StoreBE32(buffer, value);
    StoreIfNonNull(ctx.r7.u32, valueSize);
    Return(ctx, X_STATUS_SUCCESS);
}
