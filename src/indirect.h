#pragma once
#include <cstdint>

// Builds the indirect-call table (g_indirectTable, ppc_hooks.h) from every recompiled
// image in g_recompiledModules (recomp_modules.h). Call once before running guest code.
void IndirectInit();
// The host function for a guest code address, or null if none was recompiled there.
struct PPCContext;
using PPCHostFunc = void(PPCContext&, uint8_t*);
PPCHostFunc* IndirectFind(uint32_t address);
