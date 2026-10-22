// Translator tests: hand-assembled Xenos microcode through ucode.cpp / hlsl.cpp, each stage
// compiled with dxc (standing in for fxc) and the vertex programs run as compute shaders on a
// Vulkan device (lavapipe works) to check what they compute. Linux; see run.sh.
#include "asm.h"
#include "gpu/hlsl.h"
#include "gpu/ucode.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>

static int g_fail = 0;
static const std::string kDxc = getenv("DXC") ? getenv("DXC") : "dxc";

static void Write(const std::string& path, const void* d, size_t n) { FILE* f = fopen(path.c_str(), "wb"); fwrite(d, 1, n, f); fclose(f); }
static std::vector<char> Read(const std::string& p) { std::ifstream f(p, std::ios::binary); return { std::istreambuf_iterator<char>(f), {} }; }
static void Sh(const std::string& cmd) { if (system(cmd.c_str()) != 0) {} }
static bool Run(const std::string& cmd) { int r = system((cmd + " > out/last.log 2>&1").c_str()); return r == 0; }
static void Check(bool ok, const std::string& what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_fail++;
}

static TranslatedShader Translate(const std::string& name, const Program& p, bool pixel)
{
    auto words = p.Build();
    ucode::Shader s = ucode::Parse(words.data(), uint32_t(words.size()), pixel);
    TranslatedShader t = TranslateShader(s);
    Write("out/" + name + ".dis", ucode::Disassemble(s).data(), ucode::Disassemble(s).size());
    // Also in the shader-cache naming, as input for run.sh survey.
    Write(std::string("out/ucode/") + (pixel ? "ps_" : "vs_") + name + ".ucode", words.data(), words.size() * 4);
    if (!t.ok) { printf("  FAIL translate %s: %s\n", name.c_str(), t.error.c_str()); g_fail++; return t; }
    Write("out/" + name + ".hlsl", t.hlsl.data(), t.hlsl.size());
    // The stage itself must compile (dxc as a stand-in for fxc: syntax and semantics).
    const bool compiled = Run(kDxc + " -HV 2016 -T " + (pixel ? "ps_6_0" : "vs_6_0") + " -E main out/" + name + ".hlsl -Fo out/" + name + ".dxil");
    Check(compiled, name + ": " + (pixel ? "ps" : "vs") + " compiles");
    if (!compiled) Sh("head -30 out/last.log");
    return t;
}

// VS constant buffer layout (hlsl.cpp kPreamble).
struct VSCB
{
    float c[256][4]; uint32_t b[8]; uint32_t l[32]; uint32_t vf[96][4]; uint32_t info[4]; float point[4]; float vscale[4], voffset[4];
    uint32_t vte[4]; uint32_t vfl[32][4]; float vwide[4];
};

// Runs GuestVS for `threads` vertex indices on the software Vulkan device; returns per-thread opos, o[16], opts.
static std::vector<std::array<float, 4>> RunVS(const std::string& name, const TranslatedShader& t, VSCB cb, uint32_t threads,
    const std::vector<uint32_t>& gmem = {}, std::vector<uint8_t>* exported = nullptr)
{
    for (size_t k = 0; k < t.vertexFetchLayout.size(); k++)
        for (int c = 0; c < 4; c++) cb.vfl[k][c] = t.vertexFetchLayout[k][c];
    std::string cs = t.hlsl + R"(
RWByteAddressBuffer gout : register(u1);
[numthreads(1, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    float4 opos, o[16], opts;
    GuestVS(tid.x, opos, o, opts);
    uint base = tid.x * 18 * 16;
    gout.Store4(base, asuint(opos));
    [unroll] for (int i = 0; i < 16; i++) gout.Store4(base + 16 + i * 16, asuint(o[i]));
    gout.Store4(base + 17 * 16, asuint(opts));
}
)";
    Write("out/" + name + ".cs.hlsl", cs.data(), cs.size());
    if (!Run(kDxc + " -HV 2016 -spirv -fspv-target-env=vulkan1.2 -T cs_6_0 -E cs_main -fvk-t-shift 10 0 -fvk-u-shift 40 0 -fvk-s-shift 20 0 out/" + name +
            ".cs.hlsl -Fo out/" + name + ".spv"))
    {
        Sh("head -30 out/last.log");
        Check(false, name + ": compute wrapper compiles");
        return {};
    }
    Write("out/" + name + ".cb", &cb, sizeof(cb));
    std::vector<uint32_t> g = gmem; g.resize(std::max<size_t>(g.size(), 4096));
    Write("out/" + name + ".gmem", g.data(), g.size() * 4);
    std::vector<uint8_t> u0(65536, 0xCD);
    Write("out/" + name + ".u0in", u0.data(), u0.size());
    const uint32_t bytes = threads * 18 * 16;
    if (!Run("./vkrun out/" + name + ".spv out/" + name + ".out " + std::to_string(bytes) + " " + std::to_string(threads) + " 0:out/" + name + ".cb 1:out/" +
            name + ".cb 10:out/" + name + ".gmem 11:out/" + name + ".gmem 40:out/" + name + ".u0in"))
    {
        Sh("cat out/last.log");
        Check(false, name + ": runs");
        return {};
    }
    auto d = Read("out/" + name + ".out");
    std::vector<std::array<float, 4>> r(threads * 18);
    memcpy(r.data(), d.data(), bytes);
    if (exported)
    {
        auto e = Read("out/" + name + ".out.u0");
        exported->assign(e.begin(), e.end());
    }
    return r;
}

static VSCB BaseCB()
{
    VSCB cb{};
    for (int i = 0; i < 256; i++) for (int c = 0; c < 4; c++) cb.c[i][c] = float(i);
    return cb;
}
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static uint32_t LoopConst(uint32_t count, uint32_t start, int32_t step) { return count | (start << 8) | ((uint32_t(step) & 0xFF) << 16); }
// Export register n (o#) from temp r.
static std::array<uint32_t, 3> Out(uint32_t o, uint32_t r) { return V(MAX, o, 15, R(r), R(r), R(0), true); }
static std::array<uint32_t, 3> OutPos(uint32_t r) { return V(MAX, 62, 15, R(r), R(r), R(0), true); }

int main()
{
    Sh("mkdir -p out/ucode");

    // 1. One loop: aL-relative constant reads and an aL-relative temp destination; aL back to 0 after.
    {
        printf("loop with aL addressing\n");
        Program p;
        p.ins = {
            V(MAX, 9, 15, C(7), C(7)),                       // 0: r9 = c7
            V(ADD, 1, 15, R(1), C(0, true)),                 // 1: r1 += c[aL]
            V(MAX, 20, 15, C(0, true), C(0, true), R(0), false, true), // 2: r[20 + aL] = c[aL]
            Out(0, 1), Out(1, 22), Out(2, 25), Out(3, 28), Out(4, 31), // 3-7
            V(MAX, 5, 15, R(9, true), R(9, true), R(0), true), // 8: o5 = r[9 + aL]
            OutPos(1),                                        // 9
        };
        p.AddExec(Exec(0, 1, 0, false));
        p.cf.push_back(LoopStart(4, 0));    // CF 1: skip to 4
        p.AddExec(Exec(1, 2, 0, false));    // CF 2
        p.cf.push_back(LoopEnd(2, 0));      // CF 3: body at 2
        p.AddExec(Exec(3, 6, 0, false));    // CF 4
        p.AddExec(Exec(9, 1, 0, true));     // CF 5
        auto t = Translate("loop1", p, false);
        VSCB cb = BaseCB();
        cb.l[0] = LoopConst(4, 2, 3); // aL = 2, 5, 8, 11
        auto r = RunVS("loop1", t, cb, 1);
        if (!r.empty())
        {
            Check(Near(r[1][0], 26), "sum of c[aL] = 26 (got " + std::to_string(r[1][0]) + ")");
            Check(Near(r[2][0], 2) && Near(r[3][0], 5) && Near(r[4][0], 8) && Near(r[5][0], 11), "r[20 + aL] written at 22, 25, 28, 31");
            Check(Near(r[6][0], 7), "aL is 0 after the loop (r[9 + aL] = 7)");
            Check(t.hlsl.find("cfpc") == std::string::npos, "emitted as a structured loop");
        }
        // Zero count: the body never runs.
        cb.l[0] = LoopConst(0, 2, 3);
        r = RunVS("loop1z", t, cb, 1);
        if (!r.empty()) Check(Near(r[1][0], 0), "count 0 skips the loop");
    }

    // 2. Nested loops, predicated break in the inner one, outer aL restored after the inner loop.
    auto nested = [](bool forceStateMachine) {
        Program p;
        AluSpec setp; // p = r4.w >= 0 (scalar setp_ge)
        p.ins = {
            V(ADD, 2, 15, R(2), C(1)),                       // 0: r2 += 1
            V(ADD, 4, 15, R(2), Neg(C(4))),                  // 1: r4 = r2 - 4
            {}, // 2: setp_ge
            V(ADD, 5, 15, R(5), C(1)),                       // 3: r5 += 1 (outer)
            V(ADD, 6, 15, R(6), C(0, true)),                 // 4: r6 += c[aL] (outer aL)
            Out(0, 2), Out(1, 5), Out(2, 6), OutPos(2),       // 5-8
        };
        setp.vop = MAX; setp.vmask = 0; setp.s1 = R(0); setp.s2 = R(0); setp.sop = 30; setp.s3 = R(4); setp.smask = 0;
        p.ins[2] = EncodeAlu(setp);
        if (forceStateMachine)
            p.cf.push_back(Jmp(0, false, 0, true));        // b0 == true: not taken (b0 clear), but makes it a state machine
        const uint32_t o = forceStateMachine ? 1 : 0;
        p.cf.push_back(LoopStart(o + 6, 1));               // o+0: outer
        p.cf.push_back(LoopStart(o + 4, 2));               // o+1: inner
        p.AddExec(Exec(0, 3, 0, false));                   // o+2
        p.cf.push_back(LoopEnd(o + 2, 2, true, true));     // o+3: break when p
        p.AddExec(Exec(3, 2, 0, false));                   // o+4
        p.cf.push_back(LoopEnd(o + 1, 1));                 // o+5
        p.AddExec(Exec(5, 4, 0, true));                    // o+6
        return p;
    };
    for (bool sm : { false, true })
    {
        printf("nested loops, predicated break%s\n", sm ? " (state machine)" : "");
        const std::string name = sm ? "nested_sm" : "nested";
        auto t = Translate(name, nested(sm), false);
        VSCB cb = BaseCB();
        cb.l[1] = LoopConst(3, 0, 1);
        cb.l[2] = LoopConst(5, 0, 10);
        auto r = RunVS(name, t, cb, 1);
        if (!r.empty())
        {
            Check(Near(r[1][0], 6), "inner iterations with breaks = 6 (got " + std::to_string(r[1][0]) + ")");
            Check(Near(r[2][0], 3), "outer iterations = 3");
            Check(Near(r[3][0], 3), "outer aL restored after inner loop: 0 + 1 + 2 = 3 (got " + std::to_string(r[3][0]) + ")");
            Check((t.hlsl.find("cfpc") != std::string::npos) == sm, sm ? "state machine" : "structured");
        }
    }

    // 3. Calls: nested subroutines after the end, conditional calls on bool constants.
    {
        printf("calls and returns\n");
        Program p;
        p.ins = {
            V(MAX, 1, 15, C(1), C(1)),      // 0: r1 = 1
            V(ADD, 1, 15, R(1), C(10)),     // 1: r1 += 10
            Out(0, 1), OutPos(1),           // 2-3
            V(ADD, 1, 15, R(1), C(2)),      // 4: sub A: r1 += 2
            V(ADD, 1, 15, R(1), C(3)),      // 5: sub B: r1 += 3
            V(ADD, 1, 15, R(1), C(5)),      // 6: never called: r1 += 5
        };
        p.AddExec(Exec(0, 1, 0, false));   // 0
        p.cf.push_back(Call(7, true));     // 1: call A
        p.AddExec(Exec(1, 1, 0, false));   // 2
        p.cf.push_back(Call(7, false, 3)); // 3: call A if b3
        p.cf.push_back(Call(12, false, 4)); // 4: call C if b4 (clear)
        p.AddExec(Exec(2, 2, 0, true));    // 5: end
        p.cf.push_back(Nop());             // 6
        p.AddExec(Exec(4, 1, 0, false));   // 7: A
        p.cf.push_back(Call(10, true));    // 8: A calls B
        p.cf.push_back(Ret());             // 9
        p.AddExec(Exec(5, 1, 0, false));   // 10: B
        p.cf.push_back(Ret());             // 11
        p.AddExec(Exec(6, 1, 0, false));   // 12: C
        p.cf.push_back(Ret());             // 13
        auto t = Translate("calls", p, false);
        VSCB cb = BaseCB();
        cb.b[0] = 1u << 3; // b3 set
        auto r = RunVS("calls", t, cb, 1);
        if (!r.empty()) Check(Near(r[1][0], 21), "1 + (2 + 3) + 10 + (2 + 3) = 21 (got " + std::to_string(r[1][0]) + ")");
    }

    // 4. A repeat loop keeps the iterator of the previous loop at its depth.
    {
        printf("repeat loop\n");
        Program p;
        p.ins = { V(ADD, 1, 15, R(1), C(1)), Out(0, 1), OutPos(1) };
        p.cf.push_back(LoopStart(3, 0));        // 0: 4 iterations
        p.AddExec(Exec(0, 1, 0, false));        // 1
        p.cf.push_back(LoopEnd(1, 0));          // 2
        p.cf.push_back(LoopStart(6, 1, true));  // 3: repeat, count 6: continues from iterator 4 -> 2 more
        p.AddExec(Exec(0, 1, 0, false));        // 4
        p.cf.push_back(LoopEnd(4, 1));          // 5
        p.AddExec(Exec(1, 2, 0, true));         // 6
        auto t = Translate("repeat", p, false);
        VSCB cb = BaseCB();
        cb.l[0] = LoopConst(4, 0, 1);
        cb.l[1] = LoopConst(6, 0, 1);
        auto r = RunVS("repeat", t, cb, 1);
        if (!r.empty()) Check(Near(r[1][0], 6), "4 + 2 iterations (got " + std::to_string(r[1][0]) + ")");
    }

    // 5. Vertex fetch: negative offset, signed no-zero normalisation, 32-bit normalised.
    {
        printf("vertex fetch formats\n");
        Program p;
        VFetchSpec a; a.dst = 1; a.format = 38; a.stride = 4; a.offset = -4; a.constIndex = 0;        // 32_32_32_32_FLOAT at index*4 - 4
        VFetchSpec b; b.dst = 2; b.format = 6; b.isSigned = true; b.noZero = true; b.stride = 4; b.offset = 4; b.constIndex = 0;
        VFetchSpec c; c.dst = 3; c.format = 6; c.isSigned = true; c.noZero = false; c.stride = 4; c.offset = 4; c.constIndex = 0;
        VFetchSpec d; d.dst = 4; d.format = 33; d.isSigned = false; d.stride = 4; d.offset = 5; d.constIndex = 0;
        p.ins = { EncodeVFetch(a), EncodeVFetch(b), EncodeVFetch(c), EncodeVFetch(d), Out(0, 1), Out(1, 2), Out(2, 3), Out(3, 4), OutPos(1) };
        p.AddExec(Exec(0, 4, 0b01010101, false));
        p.AddExec(Exec(4, 5, 0, true));
        auto t = Translate("vfetch", p, false);
        VSCB cb = BaseCB();
        cb.vf[0][0] = 64; cb.vf[0][1] = 2; // gmem byte 64, 8-in-32
        // Vertex 1 reads dwords 16 + 0..3 (offset -4 + 4); bytes stored big-endian.
        std::vector<uint32_t> g(64, 0);
        auto be = [](uint32_t v) { return __builtin_bswap32(v); };
        auto fbits = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
        g[16] = be(fbits(1.5f)); g[17] = be(fbits(-2.0f)); g[18] = be(fbits(3.0f)); g[19] = be(fbits(4.0f));
        g[24] = be(0xFF007F80u); // 8_8_8_8 x = 0x80, y = 0x7F, z = 0x00, w = 0xFF
        g[25] = be(0xFFFFFFFFu);  // 32 unorm = 1.0
        auto r = RunVS("vfetch", t, cb, 2, g);
        if (!r.empty())
        {
            const auto* v = &r[18]; // thread 1
            Check(Near(v[1][0], 1.5f) && Near(v[1][1], -2) && Near(v[1][3], 4), "negative offset fetch");
            Check(Near(v[2][0], -1) && Near(v[2][1], 1) && Near(v[2][2], 1.0f / 255) && Near(v[2][3], -1.0f / 255), "signed no-zero 8_8_8_8");
            Check(Near(v[3][0], -1) && Near(v[3][1], 1) && Near(v[3][2], 0) && Near(v[3][3], -1.0f / 127), "signed clamp 8_8_8_8");
            Check(Near(v[4][0], 1) && Near(v[4][3], 1), "32 unorm (got " + std::to_string(v[4][0]) + ")");
        }
    }

    // 6. Memory export: two streams in one shader, separated by alloc export.
    {
        printf("memory export\n");
        Program p;
        // r0.x = index. eA = r0.x * c10 (0, 1, 0, 0) + c20 (stream 1); eM0 = c30, eM1 = r0.xxxx.
        AluSpec ea; ea.vop = MAD; ea.vdst = 32; ea.vmask = 15; ea.exportData = true; ea.s1 = Sw(R(0), "xxxx"); ea.s2 = C(10); ea.s3 = C(20);
        AluSpec eb = ea; eb.s3 = C(21);
        AluSpec m0; m0.vop = MAX; m0.vdst = 33; m0.vmask = 15; m0.exportData = true; m0.s1 = C(30); m0.s2 = C(30);
        AluSpec m1; m1.vop = MAX; m1.vdst = 34; m1.vmask = 15; m1.exportData = true; m1.s1 = Sw(R(0), "xxxx"); m1.s2 = Sw(R(0), "xxxx");
        AluSpec n0 = m0; n0.s1 = C(31); n0.s2 = C(31);
        // Doubling the index for stream 1 (two elements per invocation): r1 = r0 + r0.
        p.ins = { V(ADD, 1, 15, Sw(R(0), "xxxx"), Sw(R(0), "xxxx")), {}, EncodeAlu(m0), EncodeAlu(m1), {}, EncodeAlu(n0), OutPos(0) };
        ea.s1 = Sw(R(1), "xxxx");
        p.ins[1] = EncodeAlu(ea);
        p.ins[4] = EncodeAlu(eb);
        p.cf.push_back(Alloc(3));
        p.AddExec(Exec(0, 4, 0, false));
        p.cf.push_back(Alloc(3));
        p.AddExec(Exec(4, 2, 0, false));
        p.cf.push_back(Alloc(1));
        p.AddExec(Exec(6, 1, 0, true));
        auto t = Translate("memexport", p, false);
        Check(t.memExport == 3, "eM0 and eM1 detected");
        VSCB cb = BaseCB();
        auto setu = [&](int reg, uint32_t x, uint32_t y, uint32_t z, uint32_t w) { uint32_t v[4] = { x, y, z, w }; memcpy(cb.c[reg], v, 16); };
        cb.c[10][0] = 0; cb.c[10][1] = 1; cb.c[10][2] = 0; cb.c[10][3] = 0;
        // Stream 1: 32_32_32_32_FLOAT, 8-in-32, at physical 0x100, 6 elements.
        setu(20, 0x40000000u | (0x100 >> 2), 0x4B000000u, 0x4B000000u | (38u << 8) | (7u << 16) | 2u, 0x4B000000u | 6);
        // Stream 2: 16 unorm, 8-in-16, at physical 0x400, 3 elements (the 4th invocation is out of bounds).
        setu(21, 0x40000000u | (0x400 >> 2), 0x4B000000u, 0x4B000000u | (24u << 8) | 1u, 0x4B000000u | 3);
        cb.c[30][0] = 1.0f; cb.c[30][1] = 2.0f; cb.c[30][2] = 3.0f; cb.c[30][3] = 4.0f;
        cb.c[31][0] = 0.5f; cb.c[31][1] = 0; cb.c[31][2] = 0; cb.c[31][3] = 0;
        std::vector<uint8_t> u0;
        auto r = RunVS("memexport", t, cb, 4, {}, &u0);
        if (!r.empty() && u0.size() > 0x500)
        {
            auto rd = [&](uint32_t at) { uint32_t v; memcpy(&v, &u0[at], 4); return __builtin_bswap32(v); };
            auto f = [&](uint32_t at) { uint32_t u = rd(at); float x; memcpy(&x, &u, 4); return x; };
            bool s1 = true;
            for (uint32_t i = 0; i < 3; i++) // invocations 0-2: elements 2i (c30) and 2i + 1 (index)
                s1 = s1 && Near(f(0x100 + 32 * i), 1) && Near(f(0x100 + 32 * i + 12), 4) && Near(f(0x100 + 32 * i + 16), float(i));
            Check(s1, "stream 1 elements (big-endian floats)");
            Check(u0[0x100 + 6 * 16] == 0xCD, "stream 1 bounded by its count (invocation 3 dropped)");
            // 16-bit unorm 0.5 -> 0x8000 (32767.5 + 0.5), big-endian bytes 80 00.
            Check(u0[0x400] == 0x80 && u0[0x401] == 0x00 && u0[0x404] == 0x80 && u0[0x405] == 0x00, "stream 2 16-bit elements");
            Check(u0[0x406] == 0xCD && u0[0x407] == 0xCD, "16-bit element 3 out of bounds, neighbours untouched");
        }
    }

    // 7. Pixel shaders: texture fetch modes, register LOD / gradients, weights, computed LOD, kill, oDepth.
    {
        printf("pixel shader fetch modes\n");
        Program p;
        TFetchSpec plain;                                              // computed LOD
        TFetchSpec level = plain; level.dst = 2; level.compLod = false; level.lodBias = 8;   // explicit LOD 0.5
        TFetchSpec setLod; setLod.opcode = 24; setLod.src = 3;          // setTextureLod r3.x
        TFetchSpec regLod = plain; regLod.dst = 4; regLod.compLod = false; regLod.regLod = true;
        TFetchSpec gh; gh.opcode = 25; gh.src = 5; TFetchSpec gv; gv.opcode = 26; gv.src = 6;
        TFetchSpec grad = plain; grad.dst = 7; grad.regGrad = true; grad.mag = 0; grad.min = 0; // point override
        TFetchSpec weights = plain; weights.opcode = 19; weights.dst = 8;
        TFetchSpec clod = plain; clod.opcode = 17; clod.dst = 9;
        TFetchSpec base = plain; base.dst = 10; base.mip = 2;
        TFetchSpec cube = plain; cube.dst = 11; cube.dim = 3; cube.slot = 1; cube.regGrad = true;
        AluSpec kill; kill.vop = 24; kill.s1 = R(1); kill.s2 = C(0); // kill_eq r1, c0
        AluSpec depth; depth.vop = MAX; depth.vdst = 61; depth.vmask = 1; depth.exportData = true; depth.s1 = R(2); depth.s2 = R(2);
        p.ins = { EncodeTFetch(plain), EncodeTFetch(level), EncodeTFetch(setLod), EncodeTFetch(regLod), EncodeTFetch(gh), EncodeTFetch(gv),
            EncodeTFetch(grad), EncodeTFetch(weights), EncodeTFetch(clod), EncodeTFetch(base), EncodeTFetch(cube), EncodeAlu(kill),
            EncodeAlu(depth), Out(0, 4) };
        p.AddExec(Exec(0, 6, 0b010101010101, false));
        p.AddExec(Exec(6, 5, 0b0101010101, false));
        p.AddExec(Exec(11, 3, 0, true));
        auto t = Translate("psfetch", p, true);
        Check(t.writesDepth && t.colorTargetsWritten == 1, "oDepth and oC0 detected");
        Check(t.hlsl.find("SV_Depth") != std::string::npos, "SV_Depth output");
        // The same fetches in a vertex shader (explicit LOD only).
        Program v = p;
        v.ins.resize(11);
        v.ins.push_back(OutPos(1));
        v.cf.clear(); v.execs.clear();
        v.AddExec(Exec(0, 6, 0b010101010101, false));
        v.AddExec(Exec(6, 5, 0b0101010101, false));
        v.AddExec(Exec(11, 1, 0, true));
        Translate("vsfetch", v, false);
    }

    // 8. Pixel shader memory export with a kill: pending exports are written before the discard.
    {
        printf("pixel shader memory export\n");
        Program p;
        AluSpec ea; ea.vop = MAD; ea.vdst = 32; ea.vmask = 15; ea.exportData = true; ea.s1 = Sw(R(0), "xxxx"); ea.s2 = C(10); ea.s3 = C(20);
        AluSpec m0; m0.vop = MAX; m0.vdst = 33; m0.vmask = 15; m0.exportData = true; m0.s1 = C(30); m0.s2 = C(30);
        AluSpec kill; kill.vop = 24; kill.s1 = R(1); kill.s2 = C(0);
        p.ins = { EncodeAlu(ea), EncodeAlu(m0), EncodeAlu(kill), Out(0, 1) };
        p.cf.push_back(Alloc(3));
        p.AddExec(Exec(0, 4, 0, true));
        auto t = Translate("psmemexport", p, true);
        Check(t.hlsl.find("{ MemExport(eA, eM, eMw); discard; }") != std::string::npos, "export flushed before discard");
    }

    // 9. A plain shader is unchanged in shape: no state machine, no loop state.
    {
        printf("linear shader\n");
        Program p;
        p.ins = { V(MAX, 1, 15, C(3), C(3)), Out(0, 1), OutPos(1) };
        p.AddExec(Exec(0, 3, 0, true));
        auto t = Translate("linear", p, false);
        Check(t.hlsl.find("cfpc") == std::string::npos && t.hlsl.find("LoopAddress(lc") == std::string::npos, "straight-line code");
        auto r = RunVS("linear", t, BaseCB(), 1);
        if (!r.empty()) Check(Near(r[1][0], 3), "o0 = c3");
        // Layout-only translation agrees with the full one.
        TranslatedShader l;
        auto w = p.Build();
        Check(TranslateVertexFetchLayout(ucode::Parse(w.data(), uint32_t(w.size()), false), l), "layout-only translation");
    }

    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
