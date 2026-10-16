// Phase 1 GPU: a command-processor stand-in. It consumes the ring buffer the
// title's D3D writes, performs the packets that make the CPU side progress
// (memory writes, fences, interrupts, waits) and skips everything that would
// draw. Vblank interrupts run at 60 Hz. Phase 2 replaces drawing with a
// native renderer hooked at the D3D level.
#include <atomic>
#include <deque>
#include <chrono>
#include <thread>

#include <intrin.h>
#include <windows.h>

#include "kernel/imports.h"
#include "kernel/thread.h"

namespace
{
// GPU registers are memory-mapped at 0x7FC80000 in guest space.
constexpr uint32_t kRegisterBase = 0x7FC80000;
constexpr uint32_t CP_RB_RPTR = 0x01C4;
constexpr uint32_t CP_RB_WPTR = 0x01C5;
constexpr uint32_t SCRATCH_REG0 = 0x0578;
constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
constexpr uint32_t RBBM_STATUS = 0x05D0;

uint32_t Reg(uint32_t index) { return LoadBE32(kRegisterBase + index * 4); }
void SetReg(uint32_t index, uint32_t value) { StoreBE32(kRegisterBase + index * 4, value); }

// Physical addresses as the GPU sees them map onto our physical heap.
uint32_t PhysToGuest(uint32_t phys) { return 0xA0000000 + (phys & 0x1FFFFFFF); }

struct Gpu
{
    std::atomic<uint32_t> ringBase{ 0 };   // guest address
    std::atomic<uint32_t> ringDwords{ 0 };
    std::atomic<uint32_t> rptrWriteback{ 0 };
    std::atomic<uint32_t> interruptCallback{ 0 };
    std::atomic<uint32_t> interruptData{ 0 };
    uint32_t rptr = 0;

    std::shared_ptr<ThreadObject> interruptThread;
    std::mutex interruptMutex;
    std::condition_variable interruptCv;
    std::vector<std::pair<uint32_t, uint32_t>> pendingInterrupts; // source, cpu

    std::atomic<uint64_t> swaps{ 0 };
    std::atomic<uint32_t> lastPacket{ 0 };
    std::atomic<uint32_t> waitingOn{ 0 };
    std::atomic<uint32_t> waitingRef{ 0 }, waitingInfo{ 0 };
    std::atomic<uint32_t> interruptsRaised{ 0 };
    std::atomic<uint64_t> draws{ 0 };
    std::atomic<uint32_t> interruptsHandled{ 0 };
};

Gpu g_gpu;
bool g_trace = getenv("FM2_GPUTRACE") != nullptr;

void RaiseInterrupt(uint32_t source, uint32_t cpu)
{
    {
        std::lock_guard lock(g_gpu.interruptMutex);
        g_gpu.pendingInterrupts.push_back({ source, cpu });
    }
    g_gpu.interruptCv.notify_one();
}

thread_local PPCContext* t_cpContext = nullptr;

void DispatchInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu)
{
    const uint32_t callback = g_gpu.interruptCallback;
    if (!callback)
        return;
    // The handler acknowledges by clearing the bit of the CPU it runs on,
    // read from the PCR; it runs with that CPU's interrupts disabled.
    *GuestPtr<uint8_t>(CurrentGuestThread()->pcr + kpcr::CurrentCpu) = uint8_t(cpu);
    InterruptLockAcquire(int(cpu));
    ctx.msr &= ~0x8000ull;
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_gpu.interruptData;
    GuestCall(ctx, callback);
    ctx.msr |= 0x8000;
    InterruptLockRelease();
}

// Vblank callbacks run on a guest thread of their own, like the console's
// DPC; command-stream interrupts run on the command processor thread.
void InterruptLoop(PPCContext& ctx)
{
    for (;;)
    {
        std::vector<std::pair<uint32_t, uint32_t>> sources;
        {
            std::unique_lock lock(g_gpu.interruptMutex);
            g_gpu.interruptCv.wait(lock, [] { return !g_gpu.pendingInterrupts.empty(); });
            sources.swap(g_gpu.pendingInterrupts);
        }
        for (auto [source, cpu] : sources)
            DispatchInterrupt(ctx, source, cpu);
    }
}

// GPU memory accesses carry an endian mode in the low two address bits:
// 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(value);
    case 3: return (value << 16) | (value >> 16);
    default: return value;
    }
}

void WriteBack(uint32_t address, uint32_t value)
{
    *GuestPtr<uint32_t>(PhysToGuest(address & ~3u)) = GpuSwap(value, address);
}

uint32_t ReadBack(uint32_t address)
{
    return GpuSwap(*GuestPtr<uint32_t>(PhysToGuest(address & ~3u)), address);
}

bool WaitCompare(uint32_t function, uint32_t value, uint32_t reference)
{
    switch (function)
    {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;
    }
}

void ExecuteBuffer(uint32_t address, uint32_t dwords);

constexpr uint32_t SCRATCH_UMSK = 0x01DC;
constexpr uint32_t SCRATCH_ADDR = 0x01DD;

// Register writes from the command stream. Scratch registers with their
// mask bit set are mirrored to memory: D3D's fences rely on it.
void WriteRegister(uint32_t index, uint32_t value)
{
    SetReg(index, value);
    if (index >= SCRATCH_REG0 && index < SCRATCH_REG0 + 8)
    {
        const uint32_t n = index - SCRATCH_REG0;
        if ((Reg(SCRATCH_UMSK) >> n) & 1)
            StoreBE32(PhysToGuest(Reg(SCRATCH_ADDR) + n * 4), value);
    }
}

// Executes one packet at ptr; returns its size in dwords.
uint32_t ExecutePacket(uint32_t ptr)
{
    const uint32_t header = LoadBE32(ptr);
    g_gpu.lastPacket = ptr;
    static const int slow = getenv("FM2_GPUSLOW") ? atoi(getenv("FM2_GPUSLOW")) : 0;
    for (int i = 0; i < slow; i++)
        _mm_pause();
    switch (header >> 30)
    {
    case 0: // type 0: register writes
    {
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t base = header & 0x7FFF;
        const bool oneReg = header & 0x8000;
        for (uint32_t i = 0; i < count; i++)
            WriteRegister(oneReg ? base : base + i, LoadBE32(ptr + 4 + i * 4));
        return 1 + count;
    }
    case 1: // type 1: two register writes
        WriteRegister(header & 0x7FF, LoadBE32(ptr + 4));
        WriteRegister((header >> 11) & 0x7FF, LoadBE32(ptr + 8));
        return 3;
    case 2: // type 2: nop
        return 1;
    default:
        break;
    }

    // Type 3.
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t opcode = (header >> 8) & 0x7F;
    const uint32_t data = ptr + 4;
    auto arg = [&](uint32_t i) { return LoadBE32(data + i * 4); };
    if (g_trace && opcode != 0x10)
        Log("gpu: %08X op %02X count %u: %08X %08X %08X %08X", ptr, opcode, count, arg(0), count > 1 ? arg(1) : 0,
            count > 2 ? arg(2) : 0, count > 3 ? arg(3) : 0);
    switch (opcode)
    {
    case 0x3F: // INDIRECT_BUFFER
    case 0x37: // INDIRECT_BUFFER_PFD
        ExecuteBuffer(PhysToGuest(arg(0) & ~3u), arg(1) & 0xFFFFF);
        break;
    case 0x3D: // MEM_WRITE: address, values...
        for (uint32_t i = 1; i < count; i++)
            WriteBack(arg(0) + (i - 1) * 4, arg(i));
        break;
    case 0x58: // EVENT_WRITE_SHD: initiator, address, value
    {
        uint32_t value = arg(2);
        if ((arg(0) >> 31) & 1) // write a counter instead of the value
            value = uint32_t(g_gpu.swaps.load());
        WriteBack(arg(1), value);
        break;
    }
    case 0x59: // EVENT_WRITE_EXT: initiator, address, extents...
        WriteBack(arg(1), 0);
        break;
    case 0x45: // COND_WRITE: wait info, poll addr, ref, mask, write addr, write data
    {
        const uint32_t info = arg(0);
        const uint32_t value = (info & 0x10) ? ReadBack(arg(1)) : Reg(arg(1));
        if (WaitCompare(info & 7, value & arg(3), arg(2)))
        {
            if (info & 0x100)
                WriteBack(arg(4), arg(5));
            else
                WriteRegister(arg(4), arg(5));
        }
        break;
    }
    case 0x3C: // WAIT_REG_MEM: info, poll addr, ref, mask, interval
    {
        const uint32_t info = arg(0);
        g_gpu.waitingOn = arg(1);
        g_gpu.waitingRef = arg(2);
        g_gpu.waitingInfo = info;
        // Usually satisfied within microseconds by an interrupt handler:
        // spin, then yield, then sleep.
        const auto start = Clock::now();
        for (int tries = 0;; tries++)
        {
            const uint32_t value = (info & 0x10) ? ReadBack(arg(1)) : Reg(arg(1));
            if (WaitCompare(info & 7, value & arg(3), arg(2)))
                break;
            const auto waited = Clock::now() - start;
            if (waited > std::chrono::seconds(2))
            {
                LOG_ONCE("gpu: WAIT_REG_MEM on %08X never satisfied; continuing", arg(1));
                break;
            }
            if (tries < 200)
                _mm_pause();
            else if (waited < std::chrono::milliseconds(2))
                SwitchToThread();
            else
                Sleep(1);
        }
        g_gpu.waitingOn = 0;
        break;
    }
    case 0x21: // REG_RMW: reg, and mask, or mask
    {
        const uint32_t reg = arg(0) & 0x1FFF;
        WriteRegister(reg, (Reg(reg) & arg(1)) | arg(2));
        break;
    }
    case 0x54: // INTERRUPT: cpu mask
        for (uint32_t cpu = 0; cpu < 6; cpu++)
        {
            if ((arg(0) >> cpu) & 1)
            {
                // Synchronous, like the hardware: the handler runs on this
                // thread before the command processor reads on.
                g_gpu.interruptsRaised++;
                DispatchInterrupt(*t_cpContext, 1, cpu);
                g_gpu.interruptsHandled++;
            }
        }
        break;
    case 0x22: // DRAW_INDX
    case 0x36: // DRAW_INDX_2
        g_gpu.draws++;
        break;
    case 0x46: // EVENT_WRITE
    case 0x48: // ME_INIT
    case 0x10: // NOP
    default:
        // Draws, state, constants: nothing to do until the native renderer.
        break;
    }
    return 1 + count;
}

void ExecuteBuffer(uint32_t address, uint32_t dwords)
{
    for (uint32_t done = 0; done < dwords;)
        done += ExecutePacket(address + done * 4);
}

void CommandProcessorLoop(PPCContext& ctx)
{
    t_cpContext = &ctx;
    for (;;)
    {
        const uint32_t base = g_gpu.ringBase, size = g_gpu.ringDwords;
        if (!base || !size)
        {
            Sleep(1);
            continue;
        }
        const uint32_t wptr = Reg(CP_RB_WPTR) % size;
        if (g_gpu.rptr == wptr)
        {
            Sleep(0);
            continue;
        }
        while (g_gpu.rptr != wptr)
        {

            // A packet may wrap past the end of the ring: run it from a
            // contiguous copy.
            const uint32_t header = LoadBE32(base + g_gpu.rptr * 4);
            uint32_t length = 1;
            switch (header >> 30)
            {
            case 0:
            case 3: length = ((header >> 16) & 0x3FFF) + 2; break;
            case 1: length = 3; break;
            default: break;
            }
            uint32_t used;
            if (g_gpu.rptr + length <= size)
                used = ExecutePacket(base + g_gpu.rptr * 4);
            else
            {
                static const uint32_t scratch = GuestAlloc(Heap::Virtual, 0x4002 * 4);
                for (uint32_t i = 0; i < length; i++)
                    StoreBE32(scratch + i * 4, LoadBE32(base + ((g_gpu.rptr + i) % size) * 4));
                used = ExecutePacket(scratch);
            }
            g_gpu.rptr = (g_gpu.rptr + used) % size;
        }
        SetReg(CP_RB_RPTR, g_gpu.rptr);
        if (const uint32_t wb = g_gpu.rptrWriteback)
            StoreBE32(wb, g_gpu.rptr);
    }
}

void VblankLoop()
{
    SetThreadDescription(GetCurrentThread(), L"gpu vblank");
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        RaiseInterrupt(0, 2);
    }
}
}

void GpuInit()
{
    SetReg(RBBM_STATUS, 0); // idle
    SetReg(COHER_STATUS_HOST, 0);
    // Display registers D3D reads: vblank always pending in the interrupt
    // status (the vblank handler checks it), 720p viewport and counters.
    SetReg(0x1951, 1);          // D1MODE interrupt status: vblank
    SetReg(0x194C, 0x000002D0); // D1MODE_V_COUNTER
    SetReg(0x1961, 0x050002D0); // D1MODE_VIEWPORT_SIZE 1280x720
    SetReg(0x0F01, 0x0000200E); // RB_BC_CONTROL
    std::thread(VblankLoop).detach();
}

void GpuStartInterruptThread()
{
    if (g_gpu.interruptThread)
        return;
    g_gpu.interruptThread = CreateHostGuestThread("gpu vblank irq", InterruptLoop);
    CreateHostGuestThread("gpu command processor", CommandProcessorLoop);
}

uint64_t GpuSwapCount() { return g_gpu.swaps; }

void GpuLogState()
{
    const uint32_t w = g_gpu.waitingOn;
    if (g_gpu.rptrWriteback)
        Log("  gpu: rptr writeback reads %u", LoadBE32(g_gpu.rptrWriteback));
    Log("  gpu: rptr %u wptr %u last packet %08X waiting on %08X (= %08X, want fn %u ref %08X) swaps %llu", g_gpu.rptr, Reg(CP_RB_WPTR),
        g_gpu.lastPacket.load(), w, w ? ReadBack(w) : 0, g_gpu.waitingInfo & 7, g_gpu.waitingRef.load(), (unsigned long long)g_gpu.swaps.load());
    Log("  gpu: interrupts raised %u handled %u, draws %llu", g_gpu.interruptsRaised.load(), g_gpu.interruptsHandled.load(),
        (unsigned long long)g_gpu.draws.load());
    if (const uint32_t device = g_gpu.interruptData)
    {
        const uint32_t g = LoadBE32(device + 0x2A10);
        Log("  gpu: device %08X retire block %08X: %08X %08X %08X %08X", device, g, LoadBE32(g), LoadBE32(g + 4), LoadBE32(g + 8), LoadBE32(g + 12));
    }
}

// Imports ------------------------------------------------------------------

GUEST_EXPORT(VdInitializeEngines)
{
    Return(ctx, 1);
}

GUEST_EXPORT(VdShutdownEngines) {}

GUEST_EXPORT(VdSetGraphicsInterruptCallback)
{
    g_gpu.interruptCallback = ctx.r3.u32;
    g_gpu.interruptData = ctx.r4.u32;
    Log("gpu: interrupt callback %08X data %08X", ctx.r3.u32, ctx.r4.u32);
    GpuStartInterruptThread();
}

GUEST_EXPORT(VdInitializeRingBuffer)
{
    // (physical address, log2 of size in qwords? - size = 1 << (log2 + 3) bytes)
    g_gpu.ringBase = PhysToGuest(ctx.r3.u32);
    g_gpu.ringDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_gpu.rptr = 0;
    Log("gpu: ring buffer at %08X, %u dwords", g_gpu.ringBase.load(), g_gpu.ringDwords.load());
}

GUEST_EXPORT(VdEnableRingBufferRPtrWriteBack)
{
    // (address, block log2)
    g_gpu.rptrWriteback = PhysToGuest(ctx.r3.u32 & 0x1FFFFFFF);
    Log("gpu: rptr writeback at %08X (from %08X), block log2 %u", g_gpu.rptrWriteback.load(), ctx.r3.u32, ctx.r4.u32);
}

GUEST_EXPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

GUEST_EXPORT(VdGetSystemCommandBuffer)
{
    // (buffer*, gpuIdentifier*)
    static uint32_t buffer = GuestAlloc(Heap::Physical, 0x10000, 0x1000);
    StoreIfNonNull(ctx.r3.u32, buffer);
    StoreIfNonNull(ctx.r4.u32, 0xBEEF0000);
}

namespace
{
// X_VIDEO_MODE.
void WriteVideoMode(uint32_t mode)
{
    memset(GuestPtr(mode), 0, 0x30);
    StoreBE32(mode + 0x00, 1280); // display width
    StoreBE32(mode + 0x04, 720);  // display height
    StoreBE32(mode + 0x08, 0);    // interlaced
    StoreBE32(mode + 0x0C, 1);    // widescreen
    StoreBE32(mode + 0x10, 1);    // high definition
    const float refresh = 60.0f;
    uint32_t bits;
    memcpy(&bits, &refresh, 4);
    StoreBE32(mode + 0x14, bits);
    StoreBE32(mode + 0x18, 1);    // NTSC-M standard
    StoreBE32(mode + 0x1C, 0x4A);
    StoreBE32(mode + 0x20, 0x01);
}
}

GUEST_EXPORT(VdQueryVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}

GUEST_EXPORT(XGetVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}

GUEST_EXPORT(VdQueryVideoFlags)
{
    Return(ctx, 0x7); // widescreen | 1280 | hi-def
}

GUEST_EXPORT(VdGetCurrentDisplayGamma)
{
    // (type*, power*)
    StoreIfNonNull(ctx.r3.u32, 2);
    const float power = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &power, 4);
    StoreIfNonNull(ctx.r4.u32, bits);
}

GUEST_EXPORT(VdGetCurrentDisplayInformation)
{
    // X_DISPLAY_INFO (0x58 bytes); the title reads the sizes.
    const uint32_t info = ctx.r3.u32;
    memset(GuestPtr(info), 0, 0x58);
    auto set16 = [&](uint32_t off, uint16_t v) { *GuestPtr<uint16_t>(info + off) = __builtin_bswap16(v); };
    set16(0x00, 1280); // front buffer width
    set16(0x02, 720);  // front buffer height
    *GuestPtr<uint8_t>(info + 0x04) = 0;
    *GuestPtr<uint8_t>(info + 0x05) = 0;
    StoreBE32(info + 0x14, 320);  // horizontal pixels-per-inch-ish fields
    set16(0x40, 1280); // display width
    set16(0x42, 720);  // display height
    StoreBE32(info + 0x50, 60);   // refresh rate
}

GUEST_EXPORT(VdSetDisplayMode)
{
    Return(ctx, 0);
}

GUEST_EXPORT(VdPersistDisplay)
{
    StoreIfNonNull(ctx.r4.u32, 0);
    Return(ctx, 0);
}

GUEST_EXPORT(VdRetrainEDRAM) { Return(ctx, 0); }
GUEST_EXPORT(VdRetrainEDRAMWorker) { Return(ctx, 0); }
GUEST_EXPORT(VdIsHSIOTrainingSucceeded) { Return(ctx, 1); }
GUEST_EXPORT(VdEnableDisableClockGating) {}
GUEST_EXPORT(VdCallGraphicsNotificationRoutines) { Return(ctx, 0); }

GUEST_EXPORT(VdInitializeScalerCommandBuffer)
{
    // Last argument is the destination; fill it with type-2 NOPs.
    const uint32_t dest = StackArg(ctx, 8);
    if (dest)
    {
        for (uint32_t i = 0; i < 0x1CC / 4; i++)
            StoreBE32(dest + i * 4, 0x80000000);
    }
    Return(ctx, 0x1CC / 4);
}

GUEST_EXPORT(VdSwap)
{
    // (buffer (64 dwords in the ring), fetch constant, ..., frontbuffer*, format*, colorspace*, width*, height*)
    const uint32_t buffer = ctx.r3.u32;
    // The 64 dwords reserved for the swap become type-2 NOPs: D3D walks
    // retired command memory on the CPU and reads other headers as links.
    for (uint32_t i = 0; i < 64; i++)
        StoreBE32(buffer + i * 4, 0x80000000);
    const uint64_t n = ++g_gpu.swaps;
    if (n == 1 || (n & (n - 1)) == 0 || n % 60 == 0)
        Log("gpu: VdSwap #%llu", (unsigned long long)n);
    Return(ctx, 0);
}
