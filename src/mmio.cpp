// Hardware register stores from the recompiled code (PPC_MM_STORE_U32, ppc_hooks.h).
#include <cstdint>

void XmaWriteRegister(uint32_t address, uint32_t value); // audio/xma.cpp

void PPCMmioStore32(uint8_t* base, uint32_t address, uint32_t y)
{
    // The guest's store puts y in memory big-endian. Device registers are little-endian, so the
    // register takes the byte-swapped value (the mask a stwbrx stores arrives as written).
    if ((address >> 16) == 0x7FEA)
        XmaWriteRegister(address, __builtin_bswap32(y));
    else
        *reinterpret_cast<volatile uint32_t*>(base + address) = __builtin_bswap32(y);
}
