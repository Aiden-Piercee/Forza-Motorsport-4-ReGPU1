#pragma once
// The display controller's gamma ramps (DC_LUT_* registers). The title uploads two: a
// 256-entry table of 10-bit values per channel, and a piecewise-linear (PWL) ramp of 128
// segments per channel. Direct3D sets the display up to apply the table to an 8-bit-per-channel
// front buffer and the PWL ramp to a 10-bit one (2_10_10_10), so the ramp the presenter applies
// follows the front buffer's format.
//
// The register behaviour follows the M56 display controller documentation (RRG-216M56-03oOEM,
// DC_LUT_*), as the console's D3D writes them.
#include <cstdint>

namespace regpu
{
namespace gamma
{
constexpr uint32_t DC_LUT_RW_MODE = 0x1921;    // bit 0: 0 table, 1 PWL (which one SEQ/PWL writes go to)
constexpr uint32_t DC_LUT_RW_INDEX = 0x1922;   // entry index (bit 7 ignored for PWL); resets the component
constexpr uint32_t DC_LUT_SEQ_COLOR = 0x1923;  // table: red, green, blue in turn, 16 bits (bits 0-5 zero)
constexpr uint32_t DC_LUT_PWL_DATA = 0x1924;   // PWL: red, green, blue in turn, base 0-15, delta 16-31
constexpr uint32_t DC_LUT_30_COLOR = 0x1925;   // table: one entry, 10:10:10 (blue low)
constexpr uint32_t DC_LUT_WRITE_EN_MASK = 0x1927; // bit 0 blue, 1 green, 2 red
constexpr uint32_t kFirstRegister = DC_LUT_RW_MODE, kLastRegister = DC_LUT_WRITE_EN_MASK;

// Every register write in [kFirstRegister, kLastRegister], in order (other registers in the
// range are ignored). Called on the command processor thread.
void WriteRegister(uint32_t index, uint32_t value);

// What the display outputs for each front buffer value, per channel, in [0, 1]: 256 entries
// for an 8-bit front buffer (the table), 1024 for a 10-bit one (the PWL ramp).
struct Curve
{
    uint32_t size = 256;
    float rgb[3][1024];
    bool identity = true; // output == input for every entry (no pass needed)
};
void Build(bool tenBit, Curve& out);

// Changes whenever a write changes either ramp (to rebuild a curve only when needed).
uint64_t Generation();

// Back to the power-on state (identity ramps); for tests.
void Reset();
} // namespace gamma
} // namespace regpu
