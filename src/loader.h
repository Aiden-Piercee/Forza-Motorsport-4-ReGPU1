#pragma once
#include <cstdint>
#include <filesystem>

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

// Loads default.xex into guest memory and wires up imports.
void LoadTitle(const std::filesystem::path& xexPath);

// Optional header value (low byte 0x00/0x01) or guest pointer to its data.
uint32_t XexHeaderField(uint32_t headerBase, uint32_t key);
