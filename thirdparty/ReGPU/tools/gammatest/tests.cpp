// Display gamma ramp register tests (src/gpu/gamma_ramp.cpp). Linux: see run.sh.
#include "gpu/gamma_ramp.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace regpu::gamma;

static int g_fail = 0;
static void Check(bool ok, const std::string& what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_fail += !ok;
}
static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

int main()
{
    Curve c;
    printf("power-on ramps\n");
    Build(false, c);
    Check(c.size == 256 && c.identity, "table is the identity");
    Build(true, c);
    Check(c.size == 1024 && c.identity, "PWL is the identity");
    Check(Near(c.rgb[1][1023], 1.0f) && Near(c.rgb[0][512], 512.0f / 1023.0f), "PWL identity values");

    printf("DC_LUT_30_COLOR\n");
    Reset();
    const uint64_t g0 = Generation();
    WriteRegister(DC_LUT_RW_MODE, 0);
    WriteRegister(DC_LUT_WRITE_EN_MASK, 7);
    WriteRegister(DC_LUT_RW_INDEX, 1);
    WriteRegister(DC_LUT_30_COLOR, (100u << 20) | (200u << 10) | 300u); // entry 1
    WriteRegister(DC_LUT_30_COLOR, (400u << 20) | (500u << 10) | 600u); // entry 2: the index advances
    Build(false, c);
    Check(Near(c.rgb[0][1], 100 / 1023.0f) && Near(c.rgb[1][1], 200 / 1023.0f) && Near(c.rgb[2][1], 300 / 1023.0f), "entry 1, red high, blue low");
    Check(Near(c.rgb[0][2], 400 / 1023.0f) && Near(c.rgb[2][2], 600 / 1023.0f), "index advances after each entry");
    Check(!c.identity && Generation() != g0, "not the identity; generation changed");
    WriteRegister(DC_LUT_WRITE_EN_MASK, 2); // green only
    WriteRegister(DC_LUT_RW_INDEX, 1);
    WriteRegister(DC_LUT_30_COLOR, (7u << 20) | (8u << 10) | 9u);
    Build(false, c);
    Check(Near(c.rgb[0][1], 100 / 1023.0f) && Near(c.rgb[1][1], 8 / 1023.0f) && Near(c.rgb[2][1], 300 / 1023.0f), "write mask: green only");
    Build(true, c);
    Check(c.identity, "the table leaves the PWL ramp alone");

    printf("DC_LUT_SEQ_COLOR\n");
    Reset();
    WriteRegister(DC_LUT_WRITE_EN_MASK, 7);
    WriteRegister(DC_LUT_RW_INDEX, 255);
    for (uint32_t v : { 10u, 20u, 30u, 40u, 50u, 60u }) // entries 255 then 0 (the index wraps)
        WriteRegister(DC_LUT_SEQ_COLOR, (v << 6) | 0x3F);  // bits 0-5 are not stored
    Build(false, c);
    Check(Near(c.rgb[0][255], 10 / 1023.0f) && Near(c.rgb[1][255], 20 / 1023.0f) && Near(c.rgb[2][255], 30 / 1023.0f), "red, green, blue in turn");
    Check(Near(c.rgb[0][0], 40 / 1023.0f) && Near(c.rgb[2][0], 60 / 1023.0f), "index advances after blue, wrapping");
    WriteRegister(DC_LUT_RW_INDEX, 7);
    WriteRegister(DC_LUT_SEQ_COLOR, 1u << 6); // red of 7
    WriteRegister(DC_LUT_RW_INDEX, 8);        // restarts at red
    WriteRegister(DC_LUT_SEQ_COLOR, 2u << 6);
    Build(false, c);
    Check(Near(c.rgb[0][7], 1 / 1023.0f) && Near(c.rgb[1][7], float((7 * 1023 + 127) / 255) / 1023.0f) && Near(c.rgb[0][8], 2 / 1023.0f), "an index write restarts at red");

    printf("DC_LUT_PWL_DATA\n");
    Reset();
    WriteRegister(DC_LUT_RW_MODE, 1);
    WriteRegister(DC_LUT_WRITE_EN_MASK, 7);
    WriteRegister(DC_LUT_RW_INDEX, 0);
    // Square curve on red, green and blue alike: segment i from (i/128)^2 with the slope to the next.
    auto unit = [](double y) { return uint32_t(std::lround(y * 64 * 1023)) & 0xFFC0; };
    for (uint32_t i = 0; i < 128; i++)
    {
        const uint32_t base = unit(std::pow(i * 8 / 1023.0, 2));
        const uint32_t next = i < 127 ? unit(std::pow((i + 1) * 8 / 1023.0, 2)) : unit(1.0);
        const uint32_t delta = (next - base) & 0xFFC0;
        for (int k = 0; k < 3; k++)
            WriteRegister(DC_LUT_PWL_DATA, base | delta << 16);
    }
    Build(true, c);
    Check(!c.identity, "not the identity");
    bool close = true;
    for (uint32_t v = 0; v < 1024; v++)
        close = close && Near(c.rgb[1][v], float(std::pow(v / 1023.0, 2)), 4.0f / 1023);
    Check(close, "follows the uploaded curve, interpolated within segments");
    Check(Near(c.rgb[0][1023], c.rgb[2][1023]), "all channels");
    Build(false, c);
    Check(c.identity, "the PWL ramp leaves the table alone");
    // Bit 7 of the index is kept for PWL: 0x85 writes entry 5 and goes on to 0x86.
    Reset();
    WriteRegister(DC_LUT_WRITE_EN_MASK, 7);
    WriteRegister(DC_LUT_RW_INDEX, 0x85);
    for (int k = 0; k < 6; k++)
        WriteRegister(DC_LUT_PWL_DATA, 0x0040u * 64 | 0u << 16);
    Build(true, c);
    Check(Near(c.rgb[0][5 * 8], 64.0f / 1023) && Near(c.rgb[0][6 * 8 + 7], 64.0f / 1023), "index bit 7 ignored for the entry");

    printf(g_fail ? "FAILED: %d failure(s)\n" : "PASSED: 0 failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
