// Tests for the EDRAM render target model (src/gpu/edram.*): layout math, pass decoding and
// ownership. Build and run: tools/edramtest/run.sh
#include <cstdio>
#include <cstring>
#include <string>

#include "gpu/edram.h"

using namespace regpu::edram;

static int g_failures = 0;
static void Expect(bool ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += !ok;
}
#define EXPECT_EQ(a, b) do { char m[256]; snprintf(m, sizeof(m), "%s == %s (%lld vs %lld)", #a, #b, (long long)(a), (long long)(b)); Expect((a) == (b), m); } while (0)

static uint32_t Xy(uint32_t x, uint32_t y) { return x | y << 16; }
static uint32_t Offset(int32_t x, int32_t y) { return (uint32_t(x) & 0x7FFF) | (uint32_t(y) & 0x7FFF) << 16; }

static PassRegisters Base(uint32_t pitch, uint32_t msaa, uint32_t height)
{
    PassRegisters r;
    r.surfaceInfo = pitch | msaa << 16;
    r.modeControl = 4;
    r.colorMask = 0xF;
    r.psColorTargets = 1;
    r.scissorTL = Xy(0, 0) | 0x80000000u; // window offset off
    r.scissorBR = Xy(pitch, height);
    return r;
}

int main()
{
    printf("layout\n");
    EXPECT_EQ(PitchTiles32(1280, k1x), 16u);
    EXPECT_EQ(PitchTiles32(1280, k2x), 16u);
    EXPECT_EQ(PitchTiles32(1280, k4x), 32u);
    EXPECT_EQ(PitchTiles32(1152, k1x), 15u);
    EXPECT_EQ(HeightPixels(16, k1x), 2048u);
    EXPECT_EQ(HeightPixels(16, k2x), 1024u);
    EXPECT_EQ(HeightPixels(32, k4x), 512u);
    EXPECT_EQ(HeightPixels(15, k1x), 2192u);  // 137 rows of tiles, the last one partial
    EXPECT_EQ(HeightPixels(1, k1x), 8192u);   // clamped to the largest guest target
    EXPECT_EQ(HeightPixels(1, k4x), 8192u);
    SurfaceKey wide{ 0, 32, k4x, false, k16161616Float };
    EXPECT_EQ(wide.PitchTiles(), 64u);
    EXPECT_EQ(wide.WidthPixels(), 1280u);
    EXPECT_EQ(StorageFormat(k2101010FloatAs16161616), uint32_t(k2101010Float));

    printf("scissor\n");
    {
        // A predicated tile: screen rows 240..480 drawn into EDRAM rows 0..240.
        PassRegisters r = Base(1280, k4x, 0);
        r.scissorTL = Xy(0, 240);
        r.scissorBR = Xy(1280, 480);
        r.windowOffset = Offset(0, -240);
        const Rect s = Scissor(r);
        EXPECT_EQ(s.top, 0);
        EXPECT_EQ(s.bottom, 240);
        r.scissorTL |= 0x80000000u; // window offset disabled for the scissor
        EXPECT_EQ(Scissor(r).top, 240);
        // Past the pitch, and inverted.
        r = Base(640, k1x, 100);
        r.scissorBR = Xy(1000, 100);
        EXPECT_EQ(Scissor(r).right, 640);
        r.scissorTL = Xy(50, 80) | 0x80000000u;
        r.scissorBR = Xy(40, 60);
        EXPECT_EQ(Scissor(r).right, 50);
        EXPECT_EQ(Scissor(r).bottom, 80);
    }

    printf("pass\n");
    {
        // 1280x720 colour at 0, depth at 400.
        PassRegisters r = Base(1280, k1x, 720);
        r.colorInfo[0] = 0 | k8888 << 16;
        r.depthInfo = 400 | kD24FS8 << 16;
        r.depthControl = 2 | 4; // z test and write
        Pass p = DescribePass(r);
        EXPECT_EQ(p.used, 3u);
        EXPECT_EQ(p.heightUsed, 720u);
        EXPECT_EQ(p.lengthTiles[1], 400u);  // up to the depth base
        EXPECT_EQ(p.lengthTiles[0], 720u);  // 45 rows of 16 tiles
        Expect(p.targets[0].depth && p.targets[0].format == kD24FS8, "depth target decoded");
        // 64 bpp colour covers twice the tiles.
        r.colorInfo[0] = 0 | k16161616Float << 16;
        r.depthInfo = 1500;
        p = DescribePass(r);
        EXPECT_EQ(p.lengthTiles[1], 1440u);
        // Colour and depth at the same base: colour stays.
        r.depthInfo = 0;
        p = DescribePass(r);
        EXPECT_EQ(p.used, 2u);
        // Depth only (mode 5), copy (mode 6), and no depth test.
        r.modeControl = 5;
        EXPECT_EQ(DescribePass(r).used, 1u);
        r.modeControl = 6;
        EXPECT_EQ(DescribePass(r).used, 0u);
        r.modeControl = 4;
        r.depthControl = 0;
        r.depthInfo = 400;
        EXPECT_EQ(DescribePass(r).used, 2u);
        // A target the pixel shader does not write, or masked off, is not used.
        r.colorMask = 0xFF;
        r.colorInfo[1] = 800;
        EXPECT_EQ(DescribePass(r).used, 2u);
        r.psColorTargets = 3;
        EXPECT_EQ(DescribePass(r).used, 6u);
        r.colorMask = 0xF0;
        EXPECT_EQ(DescribePass(r).used, 4u);
    }
    {
        // The viewport bounds the rows: a 1280x720 viewport under an 8192-row scissor.
        PassRegisters r = Base(1280, k1x, 8000);
        r.colorInfo[0] = 0;
        r.vteCntl = 0x3F;
        r.viewportYScale = -360.0f;
        r.viewportYOffset = 360.0f;
        EXPECT_EQ(DescribePass(r).heightUsed, 720u);
        // Without a viewport, the scissor, clamped to the target's height.
        r.vteCntl = 0;
        EXPECT_EQ(DescribePass(r).heightUsed, 2048u);
        // 4x MSAA strip: 240 rows are 30 rows of 32 tiles.
        r = Base(1280, k4x, 240);
        r.colorInfo[0] = 0;
        EXPECT_EQ(DescribePass(r).lengthTiles[1], 960u);
    }

    printf("ownership\n");
    {
        Ownership o;
        std::vector<Transfer> t;
        const SurfaceKey a{ 0, 16, k1x, false, k8888 }, b{ 50, 16, k4x, false, k8888 }, c{ 2000, 16, k1x, true, kD24S8 };
        o.Claim(a, 0, 100, &t);
        EXPECT_EQ(t.size(), 0u); // nothing written before
        o.Claim(b, 0, 100, &t);
        EXPECT_EQ(t.size(), 1u);
        Expect(t.size() == 1 && t[0].startTiles == 50 && t[0].endTiles == 100 && t[0].source == a, "b takes 50..100 from a");
        Expect(o.OwnerAt(49) == a && o.OwnerAt(50) == b && o.OwnerAt(149) == b && o.OwnerAt(150).Empty(), "owners after b");
        t.clear();
        o.Claim(a, 0, 150, &t);
        Expect(t.size() == 1 && t[0].startTiles == 50 && t[0].endTiles == 150 && t[0].source == b, "a takes 50..150 back from b");
        EXPECT_EQ(o.Ranges().size(), 1u); // merged into one range
        t.clear();
        o.Claim(a, 0, 150, &t);
        EXPECT_EQ(t.size(), 0u); // already its own
        // Wrapping past the end of the EDRAM.
        o.Claim(c, 0, 100, &t);
        Expect(t.size() == 1 && t[0].startTiles == 0 && t[0].endTiles == 52 && t[0].source == a, "c wraps and takes 0..52 from a");
        Expect(o.OwnerAt(2047) == c && o.OwnerAt(51) == c && o.OwnerAt(52) == a && o.OwnerAt(1999).Empty(), "owners after the wrap");
        EXPECT_EQ(o.Ranges().size(), 3u);
        // Adjacent ranges from the same source become one transfer.
        Ownership q;
        q.Claim(a, 0, 10, nullptr);
        q.Claim(a, 10, 10, nullptr);
        t.clear();
        q.Claim(b, 0, 30, &t); // b's base is 50: unrelated tiles
        EXPECT_EQ(t.size(), 0u);
        t.clear();
        q.Claim(SurfaceKey{ 0, 8, k1x, false, k8888 }, 0, 20, &t);
        Expect(t.size() == 1 && t[0].startTiles == 0 && t[0].endTiles == 20, "one transfer for a merged range");
        q.Reset();
        EXPECT_EQ(q.Ranges().size(), 0u);
    }

    printf(g_failures ? "FAILED: %d failure(s)\n" : "PASSED: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
