#include "gpu/gamma_ramp.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace regpu
{
namespace gamma
{
namespace
{
struct Pwl
{
    uint16_t base, delta; // output units: 64 * 1023 is 1.0
};

struct State
{
    uint16_t table[256][3]; // 10-bit, red green blue
    Pwl pwl[128][3];
    uint32_t mode = 0, index = 0, component = 0, writeMask = 7;
    State() { ResetRamps(); }
    void ResetRamps()
    {
        for (uint32_t i = 0; i < 256; i++)
            for (uint32_t c = 0; c < 3; c++)
                table[i][c] = uint16_t((i * 1023 + 127) / 255);
        // Identity: segment i covers inputs 8i..8i+7, 512 output units per segment.
        for (uint32_t i = 0; i < 128; i++)
            for (uint32_t c = 0; c < 3; c++)
                pwl[i][c] = { uint16_t(i * 512), uint16_t(512) };
    }
};

std::mutex g_mutex;
State g_state;
std::atomic<uint64_t> g_generation{ 0 };

// The write enable mask is blue, green, red from bit 0; components are written red first.
bool ComponentEnabled(uint32_t component) { return (g_state.writeMask >> (2 - component)) & 1; }
}

void WriteRegister(uint32_t index, uint32_t value)
{
    std::lock_guard lock(g_mutex);
    State& s = g_state;
    switch (index)
    {
    case DC_LUT_RW_MODE:
        s.mode = value & 1;
        break;
    case DC_LUT_RW_INDEX:
        s.index = value & 0xFF;
        s.component = 0;
        break;
    case DC_LUT_WRITE_EN_MASK:
        s.writeMask = value & 7;
        break;
    case DC_LUT_SEQ_COLOR:
        if (ComponentEnabled(s.component))
        {
            s.table[s.index][s.component] = uint16_t((value & 0xFFC0) >> 6);
            g_generation++;
        }
        if (++s.component == 3)
        {
            s.component = 0;
            s.index = (s.index + 1) & 0xFF;
        }
        break;
    case DC_LUT_PWL_DATA:
    {
        const uint32_t entry = s.index & 0x7F;
        if (ComponentEnabled(s.component))
        {
            s.pwl[entry][s.component] = { uint16_t(value & 0xFFC0), uint16_t((value >> 16) & 0xFFC0) };
            g_generation++;
        }
        if (++s.component == 3)
        {
            s.component = 0;
            s.index = (s.index & 0x80) | ((entry + 1) & 0x7F);
        }
        break;
    }
    case DC_LUT_30_COLOR:
        for (uint32_t c = 0; c < 3; c++)
            if (ComponentEnabled(c))
                s.table[s.index][c] = uint16_t((value >> (20 - 10 * c)) & 0x3FF);
        if (s.writeMask)
            g_generation++;
        s.index = (s.index + 1) & 0xFF;
        s.component = 0;
        break;
    default:
        break;
    }
}

void Build(bool tenBit, Curve& out)
{
    std::lock_guard lock(g_mutex);
    const State& s = g_state;
    out.size = tenBit ? 1024 : 256;
    out.identity = true;
    for (uint32_t c = 0; c < 3; c++)
        for (uint32_t v = 0; v < out.size; v++)
        {
            float y;
            if (tenBit)
            {
                // Segment v >> 3; the low 3 bits step along it by delta / 8.
                const Pwl& p = s.pwl[v >> 3][c];
                y = std::fmin((float(p.base) + float(v & 7) * float(p.delta) / 8.0f) / (64.0f * 1023.0f), 1.0f);
            }
            else
                y = float(s.table[v][c]) / 1023.0f;
            out.rgb[c][v] = y;
            const float x = float(v) / float(out.size - 1);
            out.identity = out.identity && std::fabs(y - x) < 0.5f / 1023.0f;
        }
}

uint64_t Generation() { return g_generation.load(std::memory_order_relaxed); }

void Reset()
{
    std::lock_guard lock(g_mutex);
    g_state = State();
    g_generation++;
}
} // namespace gamma
} // namespace regpu
