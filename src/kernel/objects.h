#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "memory.h"

// NTSTATUS values the kernel layer returns.
enum : uint32_t
{
    X_STATUS_SUCCESS = 0,
    X_STATUS_ABANDONED_WAIT_0 = 0x80,
    X_STATUS_USER_APC = 0xC0,
    X_STATUS_ALERTED = 0x101,
    X_STATUS_TIMEOUT = 0x102,
    X_STATUS_PENDING = 0x103,
    X_STATUS_OBJECT_NAME_EXISTS = 0x40000000,
    X_STATUS_BUFFER_OVERFLOW = 0x80000005,
    X_STATUS_NO_MORE_FILES = 0x80000006,
    X_STATUS_UNSUCCESSFUL = 0xC0000001,
    X_STATUS_NOT_IMPLEMENTED = 0xC0000002,
    X_STATUS_INVALID_HANDLE = 0xC0000008,
    X_STATUS_INVALID_PARAMETER = 0xC000000D,
    X_STATUS_NO_SUCH_FILE = 0xC000000F,
    X_STATUS_END_OF_FILE = 0xC0000011,
    X_STATUS_NO_MEMORY = 0xC0000017,
    X_STATUS_ACCESS_DENIED = 0xC0000022,
    X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
    X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
    X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
    X_STATUS_MUTANT_NOT_OWNED = 0xC0000046,
    X_STATUS_INSUFFICIENT_RESOURCES = 0xC000009A,
    X_STATUS_DEVICE_NOT_CONNECTED = 0xC000009D,
    X_STATUS_DLL_INIT_FAILED = 0xC0000142,
    X_STATUS_NOT_SUPPORTED = 0xC00000BB,
};

using Clock = std::chrono::steady_clock;

struct KernelObject
{
    enum class Kind
    {
        Event,
        Semaphore,
        Mutant,
        Timer,
        Thread,
        File,
        Other,
    };

    explicit KernelObject(Kind k) : kind(k) {}
    virtual ~KernelObject() = default;

    // Called with g_objectMutex held.
    virtual bool IsSignaled(uint32_t threadId) const { return false; }
    virtual void OnWaitSatisfied(uint32_t threadId) {}
    // Earliest time this object can become signaled on its own (timers).
    virtual std::optional<Clock::time_point> NextDeadline() const { return std::nullopt; }

    const Kind kind;
    uint32_t createdBy = 0; // guest lr of the creating call (debugging)
    // Guest address of the DISPATCHER_HEADER (or object body) the guest
    // sees, 0 if none yet.
    uint32_t guestObject = 0;
    uint32_t handle = 0;
};

struct EventObject : KernelObject
{
    EventObject(bool manual, bool initial) : KernelObject(Kind::Event), manualReset(manual), signaled(initial) {}
    bool IsSignaled(uint32_t) const override { return signaled; }
    void OnWaitSatisfied(uint32_t) override
    {
        if (!manualReset)
            signaled = false;
    }
    bool manualReset;
    bool signaled;
};

struct SemaphoreObject : KernelObject
{
    SemaphoreObject(int32_t initial, int32_t max) : KernelObject(Kind::Semaphore), count(initial), maximum(max) {}
    bool IsSignaled(uint32_t) const override { return count > 0; }
    void OnWaitSatisfied(uint32_t) override { --count; }
    int32_t count;
    int32_t maximum;
};

struct MutantObject : KernelObject
{
    explicit MutantObject(uint32_t initialOwner) : KernelObject(Kind::Mutant), owner(initialOwner), recursion(initialOwner ? 1 : 0) {}
    bool IsSignaled(uint32_t tid) const override { return owner == 0 || owner == tid; }
    void OnWaitSatisfied(uint32_t tid) override
    {
        owner = tid;
        ++recursion;
    }
    uint32_t owner;
    int32_t recursion;
};

struct TimerObject : KernelObject
{
    explicit TimerObject(bool manual) : KernelObject(Kind::Timer), manualReset(manual) {}
    bool IsSignaled(uint32_t) const override { return due && Clock::now() >= *due; }
    void OnWaitSatisfied(uint32_t) override;
    std::optional<Clock::time_point> NextDeadline() const override { return due; }
    bool manualReset;
    std::optional<Clock::time_point> due;
    std::chrono::nanoseconds period{ 0 };
};

// All dispatcher state is guarded by one lock; signals wake every waiter.
// Simple and correct; split per object if contention shows in profiles.
extern std::mutex g_objectMutex;
extern std::condition_variable g_objectCv;

uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
std::shared_ptr<KernelObject> GetHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);
uint32_t DuplicateHandle(uint32_t handle);

// Object behind a guest DISPATCHER_HEADER (KEVENT, KSEMAPHORE, ...), created
// on first use from the header's type and state.
std::shared_ptr<KernelObject> GetDispatcherObject(uint32_t header);
// Guest-visible body for a handle-only object (ObReferenceObjectByHandle).
uint32_t GetGuestObject(const std::shared_ptr<KernelObject>& object);

// Converts a guest LARGE_INTEGER timeout pointer (100 ns units, negative =
// relative, positive = absolute FILETIME) to a deadline; nullopt = forever.
std::optional<Clock::time_point> TimeoutFromGuest(uint32_t timeoutPtr);
std::optional<Clock::time_point> TimeoutFromGuestValue(int64_t value);

// Returns X_STATUS_WAIT_0 + index, X_STATUS_TIMEOUT.
uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
    std::optional<Clock::time_point> deadline);

void SignalObjectsChanged(); // notify after changing state (lock not held)

// Mirrors host state into the guest header's SignalState.
void SyncGuestSignalState(KernelObject& object);

uint32_t CurrentGuestThreadId();
