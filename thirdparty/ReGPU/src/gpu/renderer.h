#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "regpu/regpu.h"

namespace regpu
{
// Native renderer, called on the command processor thread.
void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords);
// The same with the used length and hash already known (Fnv of the used dwords ^ type).
void RendererSetShader(uint32_t type, const uint32_t* microcodeBE, uint32_t usedDwords, uint64_t hash);
void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
// frontBuffer: physical address of the resolved image to show.
void RendererOnSwap(uint64_t frame, uint32_t frontBuffer);
// Lookahead hooks (see GpuLookahead): shader loads and draws the command processor has
// not executed yet, seen on a shadow copy of the registers.
void RendererPredictBegin();
void RendererPredictShader(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords);
void RendererPredictDraw(uint32_t initiator);
// True when no shader/pipeline compile is running and none finished in the last 2 s
// (the shader tour waits for this between steps).
bool RendererCompileIdle();
// Flushes the pipeline library to disk (called when the tour finishes).
void RendererSaveCaches();
// (BenchmarkRecord, BenchmarkWriteReport: include/regpu/regpu.h.)
// True while REGPU_TRACE_DRAWS's frame is being recorded.
bool RendererTracing();
// RendererFenceSync even with record-time fences: run what is recorded and wait for the host GPU.
void RendererFenceSyncForced();
// After a device loss: the device-removed reason and, with REGPU_DRED=1, the commands DRED saw (which command
// list and operation the GPU stopped at, and any page fault).
void RendererLogDred();
void RendererLogCrashContext();

// Parallel draw translation (gpu.cpp cuts the draw stream between resolves into pieces).
struct RendererTile
{
    int32_t originX = 0, originY = 0;
    int32_t rect[4] = { 0, 0, 1 << 14, 1 << 14 };
};
// Whether draws may be translated on workers now (game settled, no upscaler, ...).
bool RendererParallelReady();
uint32_t RendererDrawWorkers();
// The current draw is a resolve (EDRAM copy): a point the stream is not cut across.
bool RendererIsResolveDraw();
// Before a fence write: the host GPU finishes what has been recorded (see renderer.cpp).
void RendererFenceSync();
// Occlusion queries (EVENT_WRITE_ZPD): Begin starts counting the samples later draws pass;
// End stops and gives the count in console samples (of the same query, by its sample-count block
// address, in the last completed frame), false while there is none yet.
void RendererOcclusionBegin(uint32_t address);
bool RendererOcclusionEnd(uint32_t address, uint32_t& samples);
// The counter model of EVENT_WRITE_ZPD (renderer.cpp): every event closes the segment since the
// previous one; an end answers with its segment's count (true), begins answer nothing (false).
bool RendererOcclusionEvent(uint32_t address, bool end, uint32_t& samples);
// Before a fence write: what has been recorded is submitted, and the host GPU fence value
// that marks its end comes back (0: nothing to wait for). The write itself waits for that
// value on another thread (gpu.cpp), so the command processor reads on.
uint64_t RendererFenceSubmit();
// With REGPU_FENCE_SPLIT: submit what has been recorded at a fence write and cap the pieces in flight.
void RendererSplitAndCap();
// Blocks until the host GPU has reached a value from RendererFenceSubmit.
void RendererFenceWait(uint64_t value, void* event);
// The fence writer, after RendererFenceWait(value): resolve write-backs recorded before it land in guest memory.
void RendererFinishFencedWritebacks(uint64_t completed);
// Follows the tile state draws set (see DrawImpl), for the state at each piece's start.
void RendererTrackTile(RendererTile& tile);
void RendererSetTile(const RendererTile& tile);
// Runs translate(i), i in [0, n), on the draw workers (and this thread), each into its own
// command list, and executes them in order after everything recorded so far.
void RendererRunPieces(uint32_t n, const std::function<void(uint32_t)>& translate);
} // namespace regpu
