#pragma once
#include <cstdint>

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
// True while FM2_TRACE_DRAWS's frame is being recorded.
bool RendererTracing();
