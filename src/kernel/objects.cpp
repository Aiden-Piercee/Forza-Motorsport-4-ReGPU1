#include "kernel/objects.h"

#include <algorithm>
#include <unordered_map>

#include <windows.h>

#include "kernel/thread.h"
#include "loader.h"
#include "log.h"

std::mutex g_objectMutex;
std::condition_variable g_objectCv;

namespace
{
// Threads blocked in WaitForObjects. A signal wakes only those whose wait
// it can satisfy, instead of every waiting thread.
struct Waiter
{
    std::condition_variable cv;
    const std::vector<std::shared_ptr<KernelObject>>* objects;
    bool waitAll;
    uint32_t tid;
};
std::vector<Waiter*> g_waiters;

bool Satisfiable(const Waiter& w)
{
    if (w.waitAll)
    {
        for (auto& o : *w.objects)
            if (!o->IsSignaled(w.tid))
                return false;
        return true;
    }
    for (auto& o : *w.objects)
        if (o->IsSignaled(w.tid))
            return true;
    return false;
}
}

namespace
{
std::mutex g_handleMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_dispatchers;
uint32_t g_nextHandle = 0xF8000004;

// Written into DISPATCHER_HEADER.WaitListHead.Flink once we own the header,
// with the handle in Blink. Re-initialising the object in guest code
// overwrites it, which makes us create a fresh object.
constexpr uint32_t kHeaderMagic = 0x464D324B; // "FM2K"

enum DispatcherType : uint8_t
{
    NotificationEvent = 0,
    SynchronizationEvent = 1,
    MutantType = 2,
    SemaphoreType = 5,
    ThreadType = 6,
    NotificationTimer = 8,
    SynchronizationTimer = 9,
};
}

void TimerObject::OnWaitSatisfied(uint32_t)
{
    if (manualReset)
        return;
    if (period.count() > 0 && due)
        due = *due + period;
    else
        due.reset();
}

uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
{
    std::lock_guard lock(g_handleMutex);
    const uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    if (!object->handle)
        object->handle = handle;
    g_handles[handle] = std::move(object);
    return handle;
}

std::shared_ptr<KernelObject> GetHandle(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseHandle(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    return g_handles.erase(handle) != 0;
}

uint32_t DuplicateHandle(uint32_t handle)
{
    auto object = GetHandle(handle);
    return object ? CreateHandle(object) : 0;
}

std::shared_ptr<KernelObject> GetDispatcherObject(uint32_t header)
{
    std::lock_guard lock(g_handleMutex);
    if (LoadBE32(header + 8) == kHeaderMagic)
    {
        auto it = g_dispatchers.find(header);
        if (it != g_dispatchers.end())
            return it->second;
    }
    else if (auto it = g_dispatchers.find(header); it != g_dispatchers.end())
    {
        // Our body of an object, its marker overwritten by the title (it initialised the dispatcher header in
        // place). Making a new object here split one event in two: signalled through the handle, waited on
        // through the pointer (the waiter sat out its full timeout, a 1 s hitch in races). The same object is
        // kept and the marker restored. FM4_SPLIT_DISPATCHERS=1: a new object, as before (A/B).
        static const bool split = getenv("FM4_SPLIT_DISPATCHERS") != nullptr;
        static int logged = 0;
        if (logged++ < 50)
            Log("objects: dispatcher at %08X (handle %08X) lost its marker (type %u, state %d)%s", header, it->second->handle,
                *GuestPtr<uint8_t>(header), int32_t(LoadBE32(header + 4)), split ? ": new object" : ": kept");
        if (!split)
        {
            StoreBE32(header + 8, kHeaderMagic);
            StoreBE32(header + 12, it->second->handle);
            return it->second;
        }
    }

    const uint8_t type = *GuestPtr<uint8_t>(header);
    const int32_t signalState = int32_t(LoadBE32(header + 4));
    std::shared_ptr<KernelObject> object;
    switch (type)
    {
    case NotificationEvent:
    case SynchronizationEvent:
        object = std::make_shared<EventObject>(type == NotificationEvent, signalState != 0);
        break;
    case SemaphoreType:
        object = std::make_shared<SemaphoreObject>(signalState, int32_t(LoadBE32(header + 0x10)));
        break;
    case MutantType:
        object = std::make_shared<MutantObject>(0);
        break;
    case NotificationTimer:
    case SynchronizationTimer:
        object = std::make_shared<TimerObject>(type == NotificationTimer);
        break;
    default:
        Log("GetDispatcherObject: unsupported dispatcher type %u at %08X", type, header);
        object = std::make_shared<EventObject>(true, false);
        break;
    }
    object->guestObject = header;
    g_dispatchers[header] = object;
    StoreBE32(header + 8, kHeaderMagic);
    StoreBE32(header + 12, 0);
    return object;
}

uint32_t GetGuestObject(const std::shared_ptr<KernelObject>& object)
{
    std::lock_guard lock(g_handleMutex);
    if (!object->guestObject)
    {
        // A zeroed body big enough for any kernel object the title pokes at, after the
        // OBJECT_HEADER the kernel keeps in front of every object: PointerCount, HandleCount,
        // Type, flags. Code that waits on a referenced object finds the dispatcher header through
        // Type->DefaultObject (an offset into the body), so Type must be real: with none, a wait
        // on an object from ObReferenceObjectByHandle read a type at 0 and waited on garbage.
        uint32_t type = 0;
        switch (object->kind)
        {
        case KernelObject::Kind::Event: type = KernelObjectType(0x00E); break;
        case KernelObject::Kind::Mutant: type = KernelObjectType(0x012); break;
        case KernelObject::Kind::Semaphore: type = KernelObjectType(0x017); break;
        case KernelObject::Kind::Timer: type = KernelObjectType(0x01C); break;
        default:
        {
            // Other objects get a type of their own whose DefaultObject is also offset 0.
            static const uint32_t generic = GuestAlloc(Heap::Virtual, 0x40);
            type = generic;
            break;
        }
        }
        // FM4_NO_OBJECT_HEADERS=1 (A/B): the bare body with no header in front, as before.
        static const bool noHeaders = getenv("FM4_NO_OBJECT_HEADERS") != nullptr;
        if (noHeaders)
            object->guestObject = GuestAlloc(Heap::Virtual, 0x200);
        else
        {
            const uint32_t header = GuestAlloc(Heap::Virtual, 0x10 + 0x200);
            StoreBE32(header + 0, 1); // PointerCount
            StoreBE32(header + 4, 1); // HandleCount
            StoreBE32(header + 8, type);
            object->guestObject = header + 0x10;
        }
        StoreBE32(object->guestObject + 8, kHeaderMagic);
        StoreBE32(object->guestObject + 12, object->handle);
        g_dispatchers[object->guestObject] = object;
    }
    return object->guestObject;
}

std::optional<Clock::time_point> TimeoutFromGuestValue(int64_t value)
{
    if (value < 0)
        return Clock::now() + std::chrono::nanoseconds(-value * 100);
    // Absolute FILETIME: compare against the host's system time.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const int64_t now = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return Clock::now() + std::chrono::nanoseconds(std::max<int64_t>(0, value - now) * 100);
}

std::optional<Clock::time_point> TimeoutFromGuest(uint32_t timeoutPtr)
{
    if (!timeoutPtr)
        return std::nullopt;
    return TimeoutFromGuestValue(int64_t(LoadBE64(timeoutPtr)));
}

uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
    std::optional<Clock::time_point> deadline)
{
    const uint32_t tid = CurrentGuestThreadId();
    if (GuestThread* t = CurrentGuestThread())
    {
        static const char* kinds[] = { "event", "semaphore", "mutant", "timer", "thread", "file", "other" };
        char text[128];
        snprintf(text, sizeof(text), "%s handle %08X guest %08X created at %08X%s", kinds[int(objects[0]->kind)], objects[0]->handle,
            objects[0]->guestObject, objects[0]->createdBy, objects.size() > 1 ? " (+more)" : "");
        t->waitingOn = text;
    }
    // FM4_LONG_WAITS=<ms>: a wait that took at least that long is logged with what it waited on, its
    // timeout and the guest call chain (finding what a stalled frame waits for).
    static const int longWaitMs = getenv("FM4_LONG_WAITS") ? atoi(getenv("FM4_LONG_WAITS")) : 0;
    struct Clear
    {
        Clock::time_point start = Clock::now();
        std::optional<Clock::time_point> deadline;
        const std::vector<std::shared_ptr<KernelObject>>* objects = nullptr;
        ~Clear()
        {
            GuestThread* t = CurrentGuestThread();
            if (longWaitMs > 0 && t)
            {
                const auto now = Clock::now();
                const double ms = std::chrono::duration<double, std::milli>(now - start).count();
                if (ms >= longWaitMs)
                {
                    char chain[160] = {};
                    int n = 0;
                    if (t->ctx)
                    {
                        n += snprintf(chain, sizeof(chain), " lr %08X", uint32_t(t->ctx->lr));
                        uint32_t sp = t->ctx->r1.u32;
                        for (int i = 0; i < 10 && sp; i++)
                        {
                            const uint32_t next = LoadBE32(sp);
                            if (!next || next <= sp)
                                break;
                            n += snprintf(chain + n, sizeof(chain) - n, " %08X", LoadBE32(next - 8));
                            sp = next;
                        }
                    }
                    const bool timedOut = deadline && now >= *deadline;
                    // A wait on another thread that timed out: what that thread is doing now (its wait and
                    // call chain), so a stall shows why the thread hadn't finished.
                    if (timedOut && objects && !objects->empty() && (*objects)[0]->kind == KernelObject::Kind::Thread)
                    {
                        auto target = std::static_pointer_cast<ThreadObject>((*objects)[0]);
                        GuestThread* g = target->thread.get();
                        char tchain[160] = {};
                        int m = 0;
                        if (g && g->ctx)
                        {
                            m += snprintf(tchain, sizeof(tchain), " lr %08X", uint32_t(g->ctx->lr));
                            uint32_t sp = g->ctx->r1.u32;
                            for (int i = 0; i < 10 && sp; i++)
                            {
                                const uint32_t next = LoadBE32(sp);
                                if (!next || next <= sp)
                                    break;
                                m += snprintf(tchain + m, sizeof(tchain) - m, " %08X", LoadBE32(next - 8));
                                sp = next;
                            }
                        }
                        Log("longwait:   target thread %u %s, waiting on: %s,%s", g ? g->id : 0, target->exited ? "exited" : "running",
                            g && !g->waitingOn.empty() ? g->waitingOn.c_str() : "nothing (running guest code)", tchain);
                        // And every thread's host stack (where in our runtime the target sits), a few times.
                        static int stackDumps = 0;
                        if (stackDumps++ < 4)
                        {
                            void LogAllHostThreadStacks(const char* why); // hang.cpp
                            LogAllHostThreadStacks("a wait on a thread timed out");
                        }
                    }
                    Log("longwait: t%u %.0f ms%s on %s, timeout %s:%s", CurrentGuestThreadId(), ms, timedOut ? " (timed out)" : "",
                        t->waitingOn.c_str(),
                        deadline ? std::to_string(int64_t(std::chrono::duration<double, std::milli>(*deadline - start).count())).append(" ms").c_str()
                                 : "none",
                        chain);
                }
            }
            if (longWaitMs > 0 && objects)
                for (auto& o : *objects)
                    if (o->waiter == CurrentGuestThreadId())
                    {
                        o->waitSinceMs = 0;
                        o->waiter = 0;
                    }
            if (t)
                t->waitingOn.clear();
        }
    } clear;
    clear.deadline = deadline;
    clear.objects = &objects;
    if (longWaitMs > 0)
    {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        for (auto& o : objects)
        {
            int64_t expected = 0;
            if (o->waitSinceMs.compare_exchange_strong(expected, nowMs))
                o->waiter = tid;
        }
    }
    std::unique_lock lock(g_objectMutex);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto& o : objects)
                all = all && o->IsSignaled(tid);
            if (all)
            {
                for (auto& o : objects)
                {
                    o->OnWaitSatisfied(tid);
                    SyncGuestSignalState(*o);
                }
                return X_STATUS_SUCCESS;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i]->IsSignaled(tid))
                {
                    objects[i]->OnWaitSatisfied(tid);
                    SyncGuestSignalState(*objects[i]);
                    return X_STATUS_SUCCESS + uint32_t(i);
                }
            }
        }

        auto wake = deadline;
        for (auto& o : objects)
        {
            if (auto d = o->NextDeadline(); d && (!wake || *d < *wake))
                wake = d;
        }
        if (deadline && Clock::now() >= *deadline)
            return X_STATUS_TIMEOUT;

        Waiter waiter{ {}, &objects, waitAll, tid };
        g_waiters.push_back(&waiter);
        if (wake)
            waiter.cv.wait_until(lock, *wake);
        else
            waiter.cv.wait(lock);
        g_waiters.erase(std::find(g_waiters.begin(), g_waiters.end(), &waiter));
    }
}

void SignalObjectsChanged()
{
    std::lock_guard lock(g_objectMutex);
    for (Waiter* w : g_waiters)
    {
        if (Satisfiable(*w))
            w->cv.notify_one();
    }
}

void SyncGuestSignalState(KernelObject& object)
{
    if (!object.guestObject)
        return;
    int32_t state = 0;
    switch (object.kind)
    {
    case KernelObject::Kind::Event:
        state = static_cast<EventObject&>(object).signaled;
        break;
    case KernelObject::Kind::Semaphore:
        state = static_cast<SemaphoreObject&>(object).count;
        break;
    case KernelObject::Kind::Mutant:
        state = static_cast<MutantObject&>(object).owner == 0;
        break;
    default:
        return;
    }
    StoreBE32(object.guestObject + 4, uint32_t(state));
}

void NoteSignal(KernelObject& object, const char* how, uint32_t lr)
{
    const int64_t since = object.waitSinceMs.load();
    if (!since)
        return;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (nowMs - since < 300)
        return;
    char chain[160] = {};
    int n = 0;
    if (GuestThread* t = CurrentGuestThread(); t && t->ctx)
    {
        uint32_t sp = t->ctx->r1.u32;
        for (int i = 0; i < 10 && sp; i++)
        {
            const uint32_t next = LoadBE32(sp);
            if (!next || next <= sp)
                break;
            n += snprintf(chain + n, sizeof(chain) - n, " %08X", LoadBE32(next - 8));
            sp = next;
        }
    }
    Log("signal: %s handle %08X guest %08X by t%u lr %08X%s releases t%u after %lld ms", how, object.handle, object.guestObject,
        CurrentGuestThreadId(), lr, chain, object.waiter.load(), (long long)(nowMs - since));
}
