// Runs the EDRAM transfer shader (src/gpu/edram_transfer_hlsl.h) as compute on a Vulkan device
// (lavapipe) and checks it: which source sample each destination sample reads, against an
// EDRAM model filled forwards from the source layout; and that the format conversions keep
// the guest's bits. Needs dxc (DXC=...) and tools/shadertest/vkrun.cpp; see run.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/edram.h"
#include "gpu/edram_transfer_hlsl.h"

using namespace regpu::edram;

static int g_failures = 0;
static void Expect(bool ok, const std::string& what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += !ok;
}
static std::string Dxc() { const char* d = getenv("DXC"); return d ? d : "dxc"; }
static bool Run(const std::string& cmd) { return system((cmd + " > out/last.log 2>&1").c_str()) == 0; }
static void Write(const std::string& path, const void* data, size_t bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data, 1, bytes, f);
    fclose(f);
}
static std::vector<uint32_t> Read(const std::string& path, size_t words)
{
    std::vector<uint32_t> v(words);
    FILE* f = fopen(path.c_str(), "rb");
    if (f)
    {
        if (fread(v.data(), 4, words, f) != words)
            v.clear();
        fclose(f);
    }
    return v;
}

// Compiles `body` (after the transfer shader with TRANSFER_TEST) and runs cs_main once per thread.
static std::vector<uint32_t> RunCompute(const std::string& name, const std::string& head, const std::string& body, const uint32_t cb0[4],
    const uint32_t cb1[4], uint32_t threads, uint32_t outWords)
{
    const std::string src = "#define TRANSFER_TEST 1\n" + head + kEdramTransferHlsl + "\nRWByteAddressBuffer gout : register(u1);\n" +
        "cbuffer Test : register(b1) { uint width; uint samples; uint t2; uint t3; };\n" + body;
    Write("out/" + name + ".hlsl", src.data(), src.size());
    if (!Run(Dxc() + " -HV 2016 -spirv -fspv-target-env=vulkan1.2 -T cs_6_0 -E cs_main -fvk-u-shift 40 0 out/" + name + ".hlsl -Fo out/" + name + ".spv"))
    {
        (void)!system("head -20 out/last.log");
        Expect(false, name + ": compiles");
        return {};
    }
    Write("out/" + name + ".cb0", cb0, 16);
    Write("out/" + name + ".cb1", cb1, 16);
    if (!Run("./vkrun out/" + name + ".spv out/" + name + ".out " + std::to_string(outWords * 4) + " " + std::to_string(threads) + " 0:out/" + name +
            ".cb0 1:out/" + name + ".cb1"))
    {
        (void)!system("cat out/last.log");
        Expect(false, name + ": runs");
        return {};
    }
    return Read("out/" + name + ".out", outWords);
}

// The convention the shader uses for host sample order (standard D3D sample patterns).
static uint32_t HostSample(uint32_t msaa, uint32_t bx, uint32_t by) { return msaa == 2 ? (bx | by << 1) : msaa == 1 ? 1 - by : 0; }
static void GuestBits(uint32_t msaa, uint32_t s, uint32_t& bx, uint32_t& by)
{
    bx = msaa == 2 ? s & 1 : 0;
    by = msaa == 2 ? s >> 1 : msaa == 1 ? 1 - s : 0;
}
// EDRAM dword of dword k of guest sample (x, y) of a layout.
static uint32_t Address(const SurfaceKey& l, uint32_t x, uint32_t y, uint32_t k)
{
    const uint32_t tileW = kTileWidthSamples >> uint32_t(l.Is64bpp());
    const uint32_t tile = (l.baseTiles + (y / 16) * l.PitchTiles() + x / tileW) % kTileCount;
    return tile * 1280 + (y % 16) * 80 + (x % tileW) * (1u << uint32_t(l.Is64bpp())) + k;
}

static void CheckMapping(const char* name, const SurfaceKey& src, const SurfaceKey& dst, uint32_t scale, uint32_t destRows)
{
    // Every source dword, written forwards into the EDRAM: which source sample (and dword of it) holds it.
    std::vector<int64_t> edram(size_t(kTileCount) * 1280, -1);
    const uint32_t srcWidthSamples = src.PitchTiles() * (kTileWidthSamples >> uint32_t(src.Is64bpp()));
    const uint32_t srcRowsSamples = (kTileCount / src.PitchTiles()) * kTileHeightSamples; // whole rows of tiles only (no wrap)
    for (uint32_t y = 0; y < srcRowsSamples; y++)
        for (uint32_t x = 0; x < srcWidthSamples; x++)
            for (uint32_t k = 0; k <= uint32_t(src.Is64bpp()); k++)
                edram[Address(src, x, y, k)] = int64_t(x) | int64_t(y) << 16 | int64_t(k) << 32;

    const uint32_t width = dst.WidthPixels() * scale, samples = 1u << dst.msaa;
    const uint32_t threads = width * destRows * samples;
    const uint32_t cb0[4] = { TransferInfo(dst), TransferInfo(src), scale | scale << 4, 0 };
    const uint32_t cb1[4] = { width, samples, 0, 0 };
    // A synthetic source: each sample returns its host pixel and sample (and which dword).
    const std::string head = "uint2 SourceGuest(uint2 p, uint s, uint format) { uint v = p.x | p.y << 13 | s << 26; return uint2(v, v | 0x80000000u); }\n";
    const std::string body = R"(
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    uint s = id.x % samples, pixel = id.x / samples;
    gout.Store2(id.x * 8, GuestAt(uint2(pixel % width, pixel / width), s));
}
)";
    const std::vector<uint32_t> out = RunCompute(name, head, body, cb0, cb1, threads, threads * 2);
    if (out.empty())
        return;
    uint32_t checked = 0, wrong = 0;
    char first[200] = "";
    for (uint32_t i = 0; i < threads; i++)
    {
        const uint32_t s = i % samples, hx = (i / samples) % width, hy = (i / samples) / width;
        uint32_t bx, by;
        GuestBits(dst.msaa, s, bx, by);
        const uint32_t gx = hx / scale, gy = hy / scale, subX = hx % scale, subY = hy % scale;
        const uint32_t dx = gx << SamplesXLog2(dst.msaa) | bx, dy = gy << SamplesYLog2(dst.msaa) | by;
        for (uint32_t k = 0; k <= uint32_t(dst.Is64bpp()); k++)
        {
            const int64_t v = edram[Address(dst, dx, dy, k)];
            if (v < 0)
                continue; // not a tile the source covers
            const uint32_t sx = uint32_t(v & 0xFFFF), sy = uint32_t((v >> 16) & 0xFFFF), sk = uint32_t(v >> 32);
            const uint32_t px = (sx >> SamplesXLog2(src.msaa)) * scale + subX, py = (sy >> SamplesYLog2(src.msaa)) * scale + subY;
            const uint32_t hs = HostSample(src.msaa, sx & ((1u << SamplesXLog2(src.msaa)) - 1), sy & ((1u << SamplesYLog2(src.msaa)) - 1));
            const uint32_t expected = px | py << 13 | hs << 26 | (sk ? 0x80000000u : 0);
            const uint32_t got = out[i * 2 + k];
            checked++;
            if (got != expected && wrong++ == 0)
                snprintf(first, sizeof(first), " (first: host %u,%u sample %u dword %u: got %08X, expected %08X)", hx, hy, s, k, got, expected);
        }
    }
    Expect(checked > 0 && wrong == 0, std::string(name) + ": " + std::to_string(checked) + " dwords, " + std::to_string(wrong) + " wrong" + first);
}

int main()
{
    (void)!system("mkdir -p out");
    printf("transfer mapping\n");
    // The 4x tile reread at 1x (resolves of MSAA tiles), and back.
    CheckMapping("1x32_to_4x32", SurfaceKey{ 0, 16, k1x, false, k8888 }, SurfaceKey{ 0, 16, k4x, false, k8888 }, 1, 48);
    CheckMapping("4x32_to_1x32", SurfaceKey{ 0, 16, k4x, false, k8888 }, SurfaceKey{ 0, 16, k1x, false, k8888 }, 1, 48);
    // 2x 64 bpp read as 1x 32 bpp, at scale 2, from a base past zero.
    CheckMapping("2x64_to_1x32_scale2", SurfaceKey{ 100, 8, k2x, false, k16161616Float }, SurfaceKey{ 100, 8, k1x, false, k8888 }, 2, 40);
    // 32 bpp 4x into 64 bpp 1x, wrapping past the end of the EDRAM.
    CheckMapping("4x32_to_1x64_wrap", SurfaceKey{ 2000, 4, k4x, false, k8888 }, SurfaceKey{ 2000, 8, k1x, false, k16161616 }, 1, 64);
    // Different pitches and bases (a depth buffer reread as colour).
    CheckMapping("pitch_change", SurfaceKey{ 40, 15, k2x, true, kD24S8 }, SurfaceKey{ 0, 12, k1x, false, k8888 }, 1, 60);

    printf("format round trips\n");
    {
        // Every 7e3 colour (with its 2-bit alpha) through half floats and back; every
        // 20e4 depth (in steps) through a float and back, with stencil; D24S8 likewise.
        const uint32_t cb0[4] = {}, cb1[4] = {};
        const std::string body = R"(
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    uint c = (i & 1023u) | ((i * 7u) & 1023u) << 10 | ((i * 13u) & 1023u) << 20 | (i & 3u) << 30;
    uint2 back = ColorToGuest(3u, GuestToColor(3u, uint2(c, 0)));
    uint z = (i * 4099u) & 0xFFFFFFu;
    uint w = z << 8 | (i & 0xFFu);
    uint f = DepthToGuest(1u, GuestToDepth(1u, w), w & 0xFFu);
    uint u = DepthToGuest(0u, GuestToDepth(0u, w), w & 0xFFu);
    uint r = 0;
    [unroll] for (uint fmt = 0; fmt < 16u; fmt++)
    {
        if (fmt == 3u || (fmt > 7u && fmt < 14u)) continue;
        uint2 g = uint2(w ^ 0x5A5A5A5Au, w * 2654435761u);
        bool wide = fmt == 5u || fmt == 7u || fmt == 15u;
        uint2 b = ColorToGuest(fmt, GuestToColor(fmt, g));
        if (b.x != g.x || (wide && b.y != g.y)) r |= 1u << fmt;
    }
    gout.Store4(i * 16, uint4(back.x ^ c, f ^ w, u ^ w, r));
}
)";
        const uint32_t threads = 4096;
        const std::vector<uint32_t> out = RunCompute("roundtrip", "uint2 SourceGuest(uint2 p, uint s, uint format) { return 0; }\n", body, cb0, cb1, threads, threads * 4);
        if (!out.empty())
        {
            uint32_t bad7e3 = 0, bad20e4 = 0, bad24 = 0, badRaw = 0;
            for (uint32_t i = 0; i < threads; i++)
            {
                bad7e3 += out[i * 4] != 0;
                bad20e4 += out[i * 4 + 1] != 0;
                bad24 += out[i * 4 + 2] != 0;
                badRaw += out[i * 4 + 3] != 0;
            }
            Expect(bad7e3 == 0, "7e3 colour keeps its bits (" + std::to_string(bad7e3) + " wrong of 4096)");
            Expect(bad20e4 == 0, "20e4 depth and stencil keep their bits (" + std::to_string(bad20e4) + " wrong)");
            Expect(bad24 == 0, "24-bit unorm depth and stencil keep their bits (" + std::to_string(bad24) + " wrong)");
            Expect(badRaw == 0, "8888, 2_10_10_10, 16_16(_16_16)(_FLOAT), 32(_32)_FLOAT keep their bits (" + std::to_string(badRaw) + " wrong)");
        }
    }
    {
        // Every 24-bit depth, both encodings: 4096 threads of 4096 values.
        const uint32_t cb0[4] = {}, cb1[4] = {};
        const std::string body = R"(
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    uint bad = 0, bad20 = 0;
    for (uint j = 0; j < 4096u; j++)
    {
        uint z = id.x * 4096u + j;
        bad += EncodeUnorm24(DecodeUnorm24(z)) != z ? 1u : 0u;
        bad20 += Encode20e4(Decode20e4(z)) != z ? 1u : 0u;
    }
    gout.Store2(id.x * 8, uint2(bad, bad20));
}
)";
        const std::vector<uint32_t> o = RunCompute("depth24", "uint2 SourceGuest(uint2 p, uint s, uint format) { return 0; }\n", body, cb0, cb1, 4096, 8192);
        if (!o.empty())
        {
            uint64_t bad = 0, bad20 = 0;
            for (uint32_t i = 0; i < 4096; i++)
            {
                bad += o[i * 2];
                bad20 += o[i * 2 + 1];
            }
            Expect(bad == 0, "all 2^24 unorm depths keep their bits (" + std::to_string(bad) + " wrong)");
            Expect(bad20 == 0, "all 2^24 20e4 depths keep their bits (" + std::to_string(bad20) + " wrong)");
        }
    }
    {
        // Known values: 7e3 1.0 is exponent 3; the largest is 31.875; 20e4's largest is just under 2.
        const uint32_t cb0[4] = {}, cb1[4] = {};
        const std::string body = R"(
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    gout.Store4(0, uint4(Encode7e3(1.0), asuint(Decode7e3(0x3FFu)), Encode7e3(100.0), asuint(Decode20e4(0xFFFFFFu))));
    gout.Store4(16, uint4(Encode7e3(0.125), Encode20e4(1.0), Encode7e3(0.001953125), asuint(Decode7e3(1u))));
}
)";
        const std::vector<uint32_t> o = RunCompute("known", "uint2 SourceGuest(uint2 p, uint s, uint format) { return 0; }\n", body, cb0, cb1, 1, 8);
        if (!o.empty())
        {
            float max7e3, max20e4, min7e3;
            memcpy(&max7e3, &o[1], 4);
            memcpy(&max20e4, &o[3], 4);
            memcpy(&min7e3, &o[7], 4);
            Expect(o[0] == 0x180, "7e3 of 1.0 is 0x180");
            Expect(max7e3 == 31.875f, "largest 7e3 is 31.875");
            Expect(o[2] == 0x3FF, "7e3 clamps at 31.875");
            Expect(max20e4 > 1.9999f && max20e4 < 2.0f, "largest 20e4 is just under 2");
            Expect(o[4] == 0x40, "7e3 of 0.125 is the denormal 0x40 (normals start at 0.25)");
            Expect(o[5] == 0xF00000, "20e4 of 1.0 is exponent 15");
            Expect(o[6] == 1, "smallest 7e3 denormal is 2^-9");
            Expect(min7e3 == 0.001953125f, "7e3 denormal 1 decodes to 2^-9");
        }
    }
    printf(g_failures ? "FAILED: %d failure(s)\n" : "PASSED: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
