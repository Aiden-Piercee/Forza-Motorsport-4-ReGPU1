#pragma once
#include <cstdint>

// Native renderer, called on the command processor thread.
void RendererOnShaderLoad(uint32_t type, const uint32_t* microcodeBE, uint32_t dwords);
void RendererOnDraw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
// frontBuffer: physical address of the resolved image to show.
void RendererOnSwap(uint64_t frame, uint32_t frontBuffer);
// True when no shader/pipeline compile is running and none finished in the last 2 s
// (the shader tour waits for this between steps).
bool RendererCompileIdle();
// Flushes the pipeline library to disk (called when the tour finishes).
void RendererSaveCaches();
// True while FM2_TRACE_DRAWS's frame is being recorded.
bool RendererTracing();
