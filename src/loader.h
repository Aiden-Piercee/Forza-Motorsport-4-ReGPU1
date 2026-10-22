#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#include <ppc_context.h>
#include "memory.h"

struct LoadedTitle
{
    uint32_t entryPoint = 0;
    uint32_t stackSize = 0;
    uint32_t xexHeader = 0;   // guest copy of the XEX header
    uint32_t ldrEntry = 0;    // LDR_DATA_TABLE_ENTRY (the module handle)
    uint32_t tlsTemplate = 0;
    uint32_t tlsDataSize = 0;
    uint32_t tlsSlotCount = 0;
    uint32_t systemFlags = 0;
    uint32_t titleId = 0;
    uint32_t executionId = 0; // guest pointer to XEX_EXECUTION_ID
    uint32_t keTimeStampBundle = 0;
};

extern LoadedTitle g_title;

// Loads default.xex into guest memory and wires up imports. A build for the title update
// (FM2_TITLE_UPDATE) applies the update's default.xexp to it first.
void LoadTitle(const std::filesystem::path& xexPath);

// Where the title update lives (the console's update: device): default.xexp, Media.zip.
// FM2_TITLE_UPDATE_DIR overrides <user>\fm2\update.
const std::filesystem::path& TitleUpdateDir();

// Optional header value (low byte 0x00/0x01) or guest pointer to its data.
uint32_t XexHeaderField(uint32_t headerBase, uint32_t key);

// Modules ---------------------------------------------------------------------------------
// Secondary XEXs (XMediaFacade, SpeechFacade) are described by g_recompiledModules
// (recomp_modules.h); their mapping tables are already in the shared indirect-call table, so
// loading one only places its image in guest memory.
struct RecompiledModule;

struct LoadedModule
{
    std::string name;                 // lower-case file name
    const RecompiledModule* desc = nullptr; // null for the title
    uint32_t handle = 0;              // LDR_DATA_TABLE_ENTRY
    uint32_t xexHeader = 0;
    uint32_t imageBase = 0, imageSize = 0;
    uint32_t entryPoint = 0, stackSize = 0, systemFlags = 0;
    uint32_t tlsTemplate = 0, tlsDataSize = 0, tlsSlotCount = 0;
    uint32_t titleId = 0, executionId = 0;
    uint32_t refCount = 1;
};

LoadedModule* FindModuleByHandle(uint32_t handle);
LoadedModule* FindModuleByName(const std::string& nameOrPath); // "game:\\X.xex", "X.xex", case-insensitive
LoadedModule* LoadModule(const std::string& nameOrPath);       // loads on first use
// Export by ordinal (name == nullptr) or by name, from the PE export directory.
bool FindModuleExport(const LoadedModule& module, uint32_t ordinal, const char* name, uint32_t* address);
