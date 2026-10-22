#pragma once
// Force-included into the recompiled code (see CMakeLists.txt).
#include <cstdint>

struct PPCContext;

// MSR[EE] cleared -> take the global interrupt lock; set -> release it.
void PPCMsrChanged(PPCContext& ctx, uint64_t oldMsr);
#define PPC_MSR_CHANGED(ctx, oldMsr)                       \
    do                                                     \
    {                                                      \
        if (((oldMsr) ^ (ctx).msr) & 0x8000)               \
            PPCMsrChanged((ctx), (oldMsr));                \
    } while (0)

// IEEE half -> float (vupkd3d128 FLOAT16 variants).
inline float ppc_half_to_float(uint16_t h)
{
    uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0)
    {
        if (man == 0) bits = sign;
        else
        {
            exp = 113;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

// float -> Xenos half (no inf/NaN: saturates to 0x7FFF, truncates).
inline uint16_t ppc_float_to_half(float f)
{
    uint32_t bits;
    __builtin_memcpy(&bits, &f, 4);
    uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    uint32_t a = bits & 0x7FFFFFFF;
    if (a > 0x477FE000) return sign | 0x7FFF;      // > 65504 or NaN
    if (a < 0x38800000)                            // denormal / zero
    {
        uint32_t e = a >> 23;
        if (e < 103) return sign;
        return sign | uint16_t(((a & 0x7FFFFF) | 0x800000) >> (126 - e));
    }
    return sign | uint16_t((a - 0x38000000) >> 13);
}

// Indirect calls (bctrl, function pointers, vtables) go through one table for every
// recompiled image, so default.xex and the modules it loads can call into each other.
// XenonRecomp's own PPC_LOOKUP_FUNC keeps a table after each image, indexed from that
// image's code base, which cannot resolve a call into another image. See indirect.cpp.
using PPCHostFunc = void(PPCContext&, uint8_t*);
constexpr uint32_t kIndirectLow = 0x82000000;  // lowest guest code address of any image
constexpr uint32_t kIndirectHigh = 0x8C000000; // end of the range the table covers
extern PPCHostFunc** g_indirectTable;          // one entry per instruction; only image code is committed
[[noreturn]] void PPCIndirectMiss(uint32_t address);
inline PPCHostFunc* PPCLookupIndirect(uint32_t address)
{
    PPCHostFunc* f = address - kIndirectLow < kIndirectHigh - kIndirectLow ? g_indirectTable[(address - kIndirectLow) >> 2] : nullptr;
    if (!f) [[unlikely]]
        PPCIndirectMiss(address);
    return f;
}
#define PPC_CALL_INDIRECT_FUNC(x) (PPCLookupIndirect(uint32_t(x)))(ctx, base)
