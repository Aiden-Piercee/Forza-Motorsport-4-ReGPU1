#pragma once
// The recompiled images linked into this program: default.xex and the modules it
// loads (tools/genmodules.py writes the table from the XenonRecomp configs).
#include <cstddef>
#include <cstdint>

struct PPCFuncMapping;

struct RecompiledModule
{
    const char* guestPath; // e.g. "game:\\XMediaFacade_default.xex"
    uint32_t imageBase;
    uint32_t imageSize;
    uint32_t codeBase;
    uint32_t codeSize;
    const PPCFuncMapping* mappings; // ends with { 0, nullptr }
};

extern const RecompiledModule g_recompiledModules[];
extern const size_t g_recompiledModuleCount;
