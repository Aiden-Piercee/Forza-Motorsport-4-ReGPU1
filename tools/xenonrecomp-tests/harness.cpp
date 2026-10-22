// Runs every recompiled test function and checks the results against values
// computed here directly from the PowerPC / VMX specifications, in guest
// (big-endian, element 0 = most significant) order.
#include "ppc_recomp_shared.h"
#include "test_funcs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

static PPCContext ctx;
static uint8_t* base;
constexpr uint32_t MEM_SIZE = 1u << 20;

// ---- result bookkeeping ----------------------------------------------------
static std::map<std::string, std::pair<int, int>> results;
static std::vector<std::string> order;
static std::string cur;
static void begin(const char* name)
{
    cur = name;
    if (!results.count(cur)) order.push_back(cur);
    results[cur];
}
#define CHECK(cond, ...)                                                             \
    do                                                                               \
    {                                                                                \
        if (cond) results[cur].first++;                                              \
        else                                                                         \
        {                                                                            \
            results[cur].second++;                                                   \
            if (results[cur].second <= 8)                                            \
            {                                                                        \
                printf("FAIL [%s] line %d: ", cur.c_str(), __LINE__);                \
                printf(__VA_ARGS__);                                                 \
                printf("\n");                                                        \
            }                                                                        \
        }                                                                            \
    } while (0)

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint64_t rnd()
{
    rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
    return rngState;
}

// ---- guest memory (big-endian) ---------------------------------------------
static void w8(uint32_t a, uint8_t v) { base[a] = v; }
static void w16(uint32_t a, uint16_t v) { base[a] = v >> 8; base[a + 1] = uint8_t(v); }
static void w32(uint32_t a, uint32_t v) { w16(a, v >> 16); w16(a + 2, uint16_t(v)); }
static void w64(uint32_t a, uint64_t v) { w32(a, v >> 32); w32(a + 4, uint32_t(v)); }
static uint8_t r8(uint32_t a) { return base[a]; }
static uint16_t r16(uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
static uint32_t r32(uint32_t a) { return uint32_t(r16(a)) << 16 | r16(a + 2); }
static uint64_t r64(uint32_t a) { return uint64_t(r32(a)) << 32 | r32(a + 4); }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static void resetCtx()
{
    memset(&ctx, 0, sizeof(ctx));
    memset(base, 0, MEM_SIZE);
    ctx.fpscr.csr = 0x1F80; // host default: all exceptions masked, no flush
    ctx.fpscr.setcsr(ctx.fpscr.csr);
}

// ---- guest-order vectors ---------------------------------------------------
struct GV
{
    uint8_t b[16];
    uint16_t h(int i) const { return uint16_t(b[2 * i] << 8 | b[2 * i + 1]); }
    int16_t sh(int i) const { return int16_t(h(i)); }
    uint32_t w(int i) const { return uint32_t(h(2 * i)) << 16 | h(2 * i + 1); }
    int32_t sw(int i) const { return int32_t(w(i)); }
    void seth(int i, uint16_t v) { b[2 * i] = v >> 8; b[2 * i + 1] = uint8_t(v); }
    void setw(int i, uint32_t v) { seth(2 * i, v >> 16); seth(2 * i + 1, uint16_t(v)); }
    bool operator==(const GV& o) const { return memcmp(b, o.b, 16) == 0; }
};
// the recompiler keeps the whole vector byte-reversed: host byte 15 - i is guest byte i
static void setV(PPCVRegister& r, const GV& g) { for (int i = 0; i < 16; i++) r.u8[15 - i] = g.b[i]; }
static GV getV(const PPCVRegister& r) { GV g; for (int i = 0; i < 16; i++) g.b[i] = r.u8[15 - i]; return g; }
static std::string hex(const GV& g)
{
    char s[64]; char* p = s;
    for (int i = 0; i < 16; i++) p += sprintf(p, "%02X%s", g.b[i], (i % 4 == 3 && i != 15) ? "_" : "");
    return s;
}

static const uint16_t kSpecialH[] = { 0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x00FF, 0x0100, 0x7F00, 0xFF80, 0x0080, 0x000F, 0x0010 };
static const uint32_t kSpecialW[] = { 0, 1, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0x0000FFFF, 0x00010000, 0x00007FFF, 0xFFFF8000, 0xFFFF7FFF, 0x00008000, 0x000000FF, 0x00000100 };

static GV randV(int mode)
{
    GV g;
    for (int i = 0; i < 8; i++)
    {
        uint64_t r = rnd();
        if (mode == 0) g.seth(i, uint16_t(r));
        else if (mode == 1) g.seth(i, kSpecialH[r % std::size(kSpecialH)]);
        else g.seth(i, uint16_t(r) & 0x801F);
    }
    if (mode == 3)
        for (int i = 0; i < 4; i++) g.setw(i, kSpecialW[rnd() % std::size(kSpecialW)]);
    return g;
}

// ---- generic vector op runner ----------------------------------------------
using RefFn = std::function<GV(const GV&, const GV&)>;
static void runVBin(PPCFunc* fn, PPCVRegister& D, PPCVRegister& A, PPCVRegister& B, const RefFn& ref,
    const std::vector<std::pair<GV, GV>>& extra = {})
{
    std::vector<std::pair<GV, GV>> cases = extra;
    for (int m = 0; m < 4; m++)
        for (int k = 0; k < 150; k++) cases.push_back({ randV(m), randV((m + k) % 4) });
    for (auto& [a, b] : cases)
    {
        GV junk = randV(0);
        setV(D, junk); setV(A, a); setV(B, b);
        fn(ctx, base);
        GV got = getV(D), exp = ref(a, b);
        CHECK(got == exp, "a=%s b=%s got=%s exp=%s", hex(a).c_str(), hex(b).c_str(), hex(got).c_str(), hex(exp).c_str());
    }
}
static GV perH(const GV& a, const GV& b, const std::function<uint16_t(uint16_t, uint16_t)>& f) { GV d; for (int i = 0; i < 8; i++) d.seth(i, f(a.h(i), b.h(i))); return d; }
static GV perB(const GV& a, const GV& b, const std::function<uint8_t(uint8_t, uint8_t)>& f) { GV d; for (int i = 0; i < 16; i++) d.b[i] = f(a.b[i], b.b[i]); return d; }
static GV perW(const GV& a, const GV& b, const std::function<uint32_t(uint32_t, uint32_t)>& f) { GV d; for (int i = 0; i < 4; i++) d.setw(i, f(a.w(i), b.w(i))); return d; }
template<class T> static T clampT(int64_t v, int64_t lo, int64_t hi) { return T(v < lo ? lo : v > hi ? hi : v); }

static std::map<uint32_t, PPCHostFunc*> indirect;
PPCHostFunc* TestLookupIndirect(uint32_t address)
{
    auto it = indirect.find(address);
    if (it == indirect.end()) { printf("indirect miss %08X\n", address); abort(); }
    return it->second;
}

// ---- 64-bit mask per spec (IBM bit numbering) ------------------------------
static uint64_t maskMB(unsigned mb, unsigned me)
{
    uint64_t m = 0;
    for (unsigned i = mb;; i = (i + 1) & 63) { m |= 1ull << (63 - i); if (i == me) break; }
    return m;
}
static uint64_t rotl64(uint64_t x, unsigned n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }

static void checkCR(const PPCCRRegister& c, int64_t v, uint8_t so, const char* what)
{
    CHECK(c.lt == (v < 0) && c.gt == (v > 0) && c.eq == (v == 0) && c.so == so,
        "%s cr lt%d gt%d eq%d so%d for %lld (so %d)", what, c.lt, c.gt, c.eq, c.so, (long long)v, so);
}

static const uint64_t kVals[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x100000000ull, 0x1FFFFFFFFull,
    0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF80000000ull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0x123456789ABCDEF0ull, 0xFFFF0000FFFF0000ull };

static std::vector<uint64_t> intVals()
{
    std::vector<uint64_t> v(std::begin(kVals), std::end(kVals));
    for (int i = 0; i < 40; i++) v.push_back(rnd());
    for (int i = 0; i < 20; i++) v.push_back(uint32_t(rnd()));
    return v;
}

// ======================================================================
static void testLoadsStores()
{
    // D-form: ea = rA + d, X-form: ea = rA + rB; rA <- ea afterwards
    struct L { const char* name; PPCFunc* fn; bool x; int d; };
    begin("lbzu");
    resetCtx(); ctx.r4.u64 = 0x10000; ctx.r3.u64 = ~0ull; w8(0x10005, 0x9C);
    T_lbzu(ctx, base);
    CHECK(ctx.r3.u64 == 0x9C && ctx.r4.u64 == 0x10005, "r3=%llx", (unsigned long long)ctx.r3.u64);
    begin("lwzu");
    resetCtx(); ctx.r4.u64 = 0x10010; ctx.r3.u64 = ~0ull; w32(0x10004, 0x80000001);
    T_lwzu(ctx, base);
    CHECK(ctx.r3.u64 == 0x80000001 && ctx.r4.u64 == 0x10004, "r3=%llx", (unsigned long long)ctx.r3.u64);
    begin("ldu");
    resetCtx(); ctx.r4.u64 = 0x10000; w64(0x10018, 0xFEDCBA9876543210ull);
    T_ldu(ctx, base);
    CHECK(ctx.r3.u64 == 0xFEDCBA9876543210ull && ctx.r4.u64 == 0x10018, "r3=%llx", (unsigned long long)ctx.r3.u64);
    begin("stbu");
    resetCtx(); ctx.r3.u64 = 0x1234; ctx.r4.u64 = 0x10000;
    T_stbu(ctx, base);
    CHECK(r8(0x10003) == 0x34 && r8(0x10002) == 0 && ctx.r4.u64 == 0x10003, "mem=%02x", r8(0x10003));
    begin("stwu");
    resetCtx(); ctx.r3.u64 = 0x1122334455667788ull; ctx.r4.u64 = 0x10020;
    T_stwu(ctx, base);
    CHECK(r32(0x10010) == 0x55667788 && ctx.r4.u64 == 0x10010, "mem=%08x", r32(0x10010));
    begin("stwux");
    resetCtx(); ctx.r3.u64 = 0x1122334455667788ull; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x34;
    T_stwux(ctx, base);
    CHECK(r32(0x10034) == 0x55667788 && ctx.r4.u64 == 0x10034, "mem=%08x", r32(0x10034));
    begin("stdu");
    resetCtx(); ctx.r3.u64 = 0x1122334455667788ull; ctx.r4.u64 = 0x10020;
    T_stdu(ctx, base);
    CHECK(r64(0x10018) == 0x1122334455667788ull && ctx.r4.u64 == 0x10018, "mem=%llx", (unsigned long long)r64(0x10018));

    begin("lbzux");
    resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x23; ctx.r3.u64 = ~0ull; w8(0x10023, 0xAB);
    T_lbzux(ctx, base);
    CHECK(ctx.r3.u64 == 0xAB && ctx.r4.u64 == 0x10023, "r3=%llx r4=%llx", (unsigned long long)ctx.r3.u64, (unsigned long long)ctx.r4.u64);

    for (uint16_t val : { 0x8123, 0x7FFF, 0xFFFF, 0x0000 })
    {
        begin("lhzu");
        resetCtx(); ctx.r4.u64 = 0x10010; ctx.r3.u64 = ~0ull; w16(0x1000E, val);
        T_lhzu(ctx, base);
        CHECK(ctx.r3.u64 == val && ctx.r4.u64 == 0x1000E, "r3=%llx r4=%llx", (unsigned long long)ctx.r3.u64, (unsigned long long)ctx.r4.u64);

        begin("lhzux");
        resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x40; ctx.r3.u64 = ~0ull; w16(0x10040, val);
        T_lhzux(ctx, base);
        CHECK(ctx.r3.u64 == val && ctx.r4.u64 == 0x10040, "r3=%llx", (unsigned long long)ctx.r3.u64);

        begin("lhau");
        resetCtx(); ctx.r4.u64 = 0x10000; ctx.r3.u64 = 0; w16(0x10006, val);
        T_lhau(ctx, base);
        CHECK(ctx.r3.s64 == int16_t(val) && ctx.r4.u64 == 0x10006, "r3=%llx", (unsigned long long)ctx.r3.u64);

        begin("lhaux");
        resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x102; ctx.r3.u64 = 0; w16(0x10102, val);
        T_lhaux(ctx, base);
        CHECK(ctx.r3.s64 == int16_t(val) && ctx.r4.u64 == 0x10102, "r3=%llx", (unsigned long long)ctx.r3.u64);

        begin("lhbrx");
        resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x30; ctx.r3.u64 = ~0ull; w16(0x10030, val);
        T_lhbrx(ctx, base);
        CHECK(ctx.r3.u64 == uint16_t(val << 8 | val >> 8) && ctx.r4.u64 == 0x10000, "r3=%llx", (unsigned long long)ctx.r3.u64);

        resetCtx(); ctx.r5.u64 = 0x10032; ctx.r0.u64 = 0x5000; ctx.r3.u64 = ~0ull; w16(0x10032, val);
        T_lhbrx0(ctx, base);
        CHECK(ctx.r3.u64 == uint16_t(val << 8 | val >> 8), "lhbrx 0: r3=%llx", (unsigned long long)ctx.r3.u64);
    }

    begin("lwzux");
    resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x80; ctx.r3.u64 = ~0ull; w32(0x10080, 0xDEADBEEF);
    T_lwzux(ctx, base);
    CHECK(ctx.r3.u64 == 0xDEADBEEF && ctx.r4.u64 == 0x10080, "r3=%llx", (unsigned long long)ctx.r3.u64);

    begin("ldux");
    resetCtx(); ctx.r4.u64 = 0x10008; ctx.r5.u64 = 0x10; w64(0x10018, 0x0123456789ABCDEFull);
    T_ldux(ctx, base);
    CHECK(ctx.r3.u64 == 0x0123456789ABCDEFull && ctx.r4.u64 == 0x10018, "r3=%llx", (unsigned long long)ctx.r3.u64);

    for (float fv : { -1.5f, 3.0e38f, 1e-40f /* single denormal */, -0.0f, INFINITY })
    {
        begin("lfsu");
        resetCtx(); ctx.r4.u64 = 0x10000; w32(0x10008, fbits(fv));
        T_lfsu(ctx, base);
        CHECK(dbits(ctx.f1.f64) == dbits(double(fv)) && ctx.r4.u64 == 0x10008, "f1=%a exp %a", ctx.f1.f64, double(fv));

        begin("lfsux");
        resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x24; w32(0x10024, fbits(fv));
        T_lfsux(ctx, base);
        CHECK(dbits(ctx.f1.f64) == dbits(double(fv)) && ctx.r4.u64 == 0x10024, "f1=%a exp %a", ctx.f1.f64, double(fv));
    }
    begin("lfdu");
    resetCtx(); ctx.r4.u64 = 0x10010; w64(0x10008, dbits(-2.0e-310));
    T_lfdu(ctx, base);
    CHECK(dbits(ctx.f1.f64) == dbits(-2.0e-310) && ctx.r4.u64 == 0x10008, "f1=%a", ctx.f1.f64);
    begin("lfdux");
    resetCtx(); ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x50; w64(0x10050, dbits(3.141592653589793));
    T_lfdux(ctx, base);
    CHECK(ctx.f1.f64 == 3.141592653589793 && ctx.r4.u64 == 0x10050, "f1=%a", ctx.f1.f64);

    begin("stbux");
    resetCtx(); ctx.r3.u64 = 0x1234567890ABCDEFull; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x7;
    T_stbux(ctx, base);
    CHECK(r8(0x10007) == 0xEF && r8(0x10006) == 0 && r8(0x10008) == 0 && ctx.r4.u64 == 0x10007, "mem=%02x", r8(0x10007));

    begin("sthu");
    resetCtx(); ctx.r3.u64 = 0x1234567890ABCDEFull; ctx.r4.u64 = 0x10000;
    T_sthu(ctx, base);
    CHECK(r16(0x1000A) == 0xCDEF && r8(0x1000C) == 0 && ctx.r4.u64 == 0x1000A, "mem=%04x r4=%llx", r16(0x1000A), (unsigned long long)ctx.r4.u64);

    begin("sthux");
    resetCtx(); ctx.r3.u64 = 0x8001; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x62;
    T_sthux(ctx, base);
    CHECK(r16(0x10062) == 0x8001 && ctx.r4.u64 == 0x10062, "mem=%04x", r16(0x10062));

    begin("stdux");
    resetCtx(); ctx.r3.u64 = 0x1234567890ABCDEFull; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x88;
    T_stdux(ctx, base);
    CHECK(r64(0x10088) == 0x1234567890ABCDEFull && ctx.r4.u64 == 0x10088, "mem=%llx", (unsigned long long)r64(0x10088));

    for (double dv : { 3.25, 1.0 / 3.0, -1e300, 1e-320, -0.0 })
    {
        begin("stfsu");
        resetCtx(); ctx.f1.f64 = dv; ctx.r4.u64 = 0x10010;
        T_stfsu(ctx, base);
        CHECK(r32(0x1000C) == fbits(float(dv)) && ctx.r4.u64 == 0x1000C, "mem=%08x exp %08x", r32(0x1000C), fbits(float(dv)));

        begin("stfsux");
        resetCtx(); ctx.f1.f64 = dv; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x44;
        T_stfsux(ctx, base);
        CHECK(r32(0x10044) == fbits(float(dv)) && ctx.r4.u64 == 0x10044, "mem=%08x exp %08x", r32(0x10044), fbits(float(dv)));

        begin("stfdu");
        resetCtx(); ctx.f1.f64 = dv; ctx.r4.u64 = 0x10000;
        T_stfdu(ctx, base);
        CHECK(r64(0x10010) == dbits(dv) && ctx.r4.u64 == 0x10010, "mem=%llx", (unsigned long long)r64(0x10010));

        begin("stfdux");
        resetCtx(); ctx.f1.f64 = dv; ctx.r4.u64 = 0x10000; ctx.r5.u64 = 0x98;
        T_stfdux(ctx, base);
        CHECK(r64(0x10098) == dbits(dv) && ctx.r4.u64 == 0x10098, "mem=%llx", (unsigned long long)r64(0x10098));
    }
}

// ======================================================================
static void setCRField(PPCCRRegister& c, unsigned bits) { c.lt = bits >> 3 & 1; c.gt = bits >> 2 & 1; c.eq = bits >> 1 & 1; c.so = bits & 1; }
static PPCCRRegister& crf(int i)
{
    PPCCRRegister* f[] = { &ctx.cr0, &ctx.cr1, &ctx.cr2, &ctx.cr3, &ctx.cr4, &ctx.cr5, &ctx.cr6, &ctx.cr7 };
    return *f[i];
}
static uint8_t& crbit(int bit)
{
    PPCCRRegister& c = crf(bit / 4);
    uint8_t* b[] = { &c.lt, &c.gt, &c.eq, &c.so };
    return *b[bit % 4];
}

// branch to target: r3 = 2 (in-function label), 7 (other function) or 1 (fallthrough)
static void testCtrBranch(const char* name, PPCFunc* fn, int bi, bool wantZero, bool wantBit, int takenR3, bool lr)
{
    begin(name);
    for (uint64_t ctrIn : { 0ull, 1ull, 2ull, 0x100000001ull, 0xFFFFFFFFFFFFFFFFull })
        for (int bit = 0; bit < 2; bit++)
            for (unsigned fill = 0; fill < 16; fill += 5)
            {
                resetCtx();
                for (int f = 0; f < 8; f++) setCRField(crf(f), fill ^ f);
                crbit(bi) = bit;
                ctx.ctr.u64 = ctrIn;
                ctx.r3.u64 = 99;
                fn(ctx, base);
                uint64_t ctrOut = ctrIn - 1;
                bool zero = uint32_t(ctrOut) == 0;
                bool taken = (zero == wantZero) && (bit == (int)wantBit);
                uint64_t exp = taken ? (lr ? 99 : takenR3) : 1;
                CHECK(ctx.r3.u64 == exp && ctx.ctr.u64 == ctrOut, "ctr=%llx bit=%d -> r3=%llu exp %llu ctr=%llx",
                    (unsigned long long)ctrIn, bit, (unsigned long long)ctx.r3.u64, (unsigned long long)exp, (unsigned long long)ctx.ctr.u64);
            }
}

static void testSoBranch(const char* name, PPCFunc* fn, int field, bool wantSo, int takenR3)
{
    begin(name);
    for (int so = 0; so < 2; so++)
        for (unsigned fill = 0; fill < 16; fill += 3)
        {
            resetCtx();
            for (int f = 0; f < 8; f++) setCRField(crf(f), (fill + f) & 0xF);
            crf(field).so = so;
            ctx.r3.u64 = 99;
            ctx.ctr.u64 = A_farget;
            fn(ctx, base);
            uint64_t exp = (so == wantSo) ? takenR3 : 1;
            CHECK(ctx.r3.u64 == exp, "so=%d fill=%u -> r3=%llu exp %llu", so, fill, (unsigned long long)ctx.r3.u64, (unsigned long long)exp);
        }
}

static void testBranches()
{
    testCtrBranch("bdzf", T_bdzf, 6, true, false, 2, false);
    testCtrBranch("bdzf", T_bdzf0, 0, true, false, 2, false);
    testCtrBranch("bdzt", T_bdzt, 9, true, true, 2, false);
    testCtrBranch("bdnzf", T_bdnzf, 8, false, false, 2, false);
    testCtrBranch("bdnzt", T_bdnzt, 31, false, true, 2, false);
    testCtrBranch("bdzf (target in another function)", T_bdzf_far, 5, true, false, 7, false);
    testCtrBranch("bdzflr", T_bdzflr, 14, true, false, 0, true);
    testCtrBranch("bdztlr", T_bdztlr, 1, true, true, 0, true);
    testCtrBranch("bdnzflr", T_bdnzflr, 19, false, false, 0, true);
    testCtrBranch("bdnztlr", T_bdnztlr, 26, false, true, 0, true);

    struct Z { const char* name; PPCFunc* fn; bool want; } zs[] = { { "bdnzlr", T_bdnzlr, false }, { "bdzlr", T_bdzlr, true } };
    for (auto& [name, fn, want] : zs)
    {
        begin(name);
        for (uint64_t ctrIn : { 0ull, 1ull, 2ull, 0x100000001ull })
        {
            resetCtx(); ctx.ctr.u64 = ctrIn; ctx.r3.u64 = 99;
            fn(ctx, base);
            bool zero = uint32_t(ctrIn - 1) == 0;
            CHECK(ctx.r3.u64 == (zero == want ? 99u : 1u) && ctx.ctr.u64 == ctrIn - 1, "ctr=%llx r3=%llu", (unsigned long long)ctrIn, (unsigned long long)ctx.r3.u64);
        }
    }

    testSoBranch("bso", T_bso, 3, true, 2);
    testSoBranch("bso", T_bso0, 0, true, 2);
    testSoBranch("bns", T_bns, 5, false, 2);
    testSoBranch("bso (target in another function)", T_bso_far, 1, true, 7);
    testSoBranch("bsolr", T_bsolr, 2, true, 99);
    testSoBranch("bnslr", T_bnslr, 6, false, 99);
    indirect[A_farget] = T_farget;
    testSoBranch("bsoctr", T_bsoctr, 1, true, 7);
    testSoBranch("bnsctr", T_bnsctr, 4, false, 7);
}

static void testCrLogical()
{
    struct C { const char* name; PPCFunc* fn; std::function<int(int, int)> f; bool unary; };
    C cs[] = {
        { "crand", T_crand, [](int a, int b) { return a & b; }, false },
        { "crandc", T_crandc, [](int a, int b) { return a & !b; }, false },
        { "creqv", T_creqv, [](int a, int b) { return int(a == b); }, false },
        { "crnand", T_crnand, [](int a, int b) { return !(a & b); }, false },
        { "crnor", T_crnor, [](int a, int b) { return !(a | b); }, false },
        { "cror", T_cror, [](int a, int b) { return a | b; }, false },
        { "crorc", T_crorc, [](int a, int b) { return a | !b; }, false },
        { "crxor", T_crxor, [](int a, int b) { return a ^ b; }, false },
        { "crset", T_crset, [](int, int) { return 1; }, false },
        { "crclr", T_crclr, [](int, int) { return 0; }, false },
        { "crmove", T_crmove, [](int a, int) { return a; }, true },
        { "crnot", T_crnot, [](int a, int) { return !a; }, true },
    };
    for (auto& c : cs)
    {
        begin(c.name);
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++)
                for (int old = 0; old < 2; old++)
                    for (unsigned fill : { 0u, 0xAu, 0x5u, 0xFu })
                    {
                        resetCtx();
                        for (int f = 0; f < 8; f++) setCRField(crf(f), fill);
                        crbit(8) = a; crbit(29) = b; crbit(7) = old;
                        PPCCRRegister before[8];
                        for (int f = 0; f < 8; f++) before[f] = crf(f);
                        c.fn(ctx, base);
                        int exp = c.f(a, b);
                        CHECK(crbit(7) == exp, "a=%d b=%d -> %d exp %d", a, b, crbit(7), exp);
                        before[1].so = crbit(7);
                        bool others = true;
                        for (int f = 0; f < 8; f++) others &= memcmp(&before[f], &crf(f), sizeof(PPCCRRegister)) == 0;
                        CHECK(others, "other CR bits changed");
                    }
    }
}

// ======================================================================
static void testCarry()
{
    auto vals = intVals();
    for (uint64_t a : vals)
        for (uint64_t b : vals)
            for (int so = 0; so < 2; so++)
            {
                for (int rc = 0; rc < 2; rc++)
                {
                    begin(rc ? "addc." : "addc");
                    resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b; ctx.xer.ca = 1 - so; ctx.xer.so = so;
                    (rc ? T_addc_dot : T_addc)(ctx, base);
                    uint64_t exp = a + b;
                    uint8_t ca = (uint64_t(uint32_t(a)) + uint32_t(b)) >> 32;
                    CHECK(ctx.r3.u64 == exp && ctx.xer.ca == ca, "a=%llx b=%llx -> %llx ca %d", (unsigned long long)a, (unsigned long long)b, (unsigned long long)ctx.r3.u64, ctx.xer.ca);
                    if (rc) checkCR(ctx.cr0, int32_t(exp), so, "addc.");
                }
                begin("addc");
                resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b;
                T_addc_alias(ctx, base);
                CHECK(ctx.r4.u64 == a + b && ctx.xer.ca == ((uint64_t(uint32_t(a)) + uint32_t(b)) >> 32), "alias rD=rA");
            }

    for (uint64_t a : vals)
        for (int ca = 0; ca < 2; ca++)
            for (int so = 0; so < 2; so++)
                for (int rc = 0; rc < 2; rc++)
                {
                    struct { const char* name; PPCFunc* fn; uint64_t res; uint8_t ca; } ops[] = {
                        { rc ? "addme." : "addme", rc ? T_addme_dot : T_addme, a + ca - 1, uint8_t((uint64_t(uint32_t(a)) + ca + 0xFFFFFFFFull) >> 32) },
                        { rc ? "subfme." : "subfme", rc ? T_subfme_dot : T_subfme, ~a + ca - 1, uint8_t((uint64_t(uint32_t(~a)) + ca + 0xFFFFFFFFull) >> 32) },
                        { rc ? "subfze." : "subfze", rc ? T_subfze_dot : T_subfze, ~a + ca, uint8_t((uint64_t(uint32_t(~a)) + ca) >> 32) },
                    };
                    for (auto& o : ops)
                    {
                        begin(o.name);
                        resetCtx(); ctx.r4.u64 = a; ctx.xer.ca = ca; ctx.xer.so = so; ctx.r3.u64 = 0x5555;
                        o.fn(ctx, base);
                        CHECK(ctx.r3.u64 == o.res && ctx.xer.ca == o.ca && ctx.r4.u64 == a, "a=%llx ca=%d -> %llx ca %d (exp %llx ca %d)",
                            (unsigned long long)a, ca, (unsigned long long)ctx.r3.u64, ctx.xer.ca, (unsigned long long)o.res, o.ca);
                        if (rc) checkCR(ctx.cr0, int32_t(o.res), so, o.name);
                    }
                    begin("subfze");
                    resetCtx(); ctx.r4.u64 = a; ctx.xer.ca = ca;
                    T_subfze_alias(ctx, base);
                    CHECK(ctx.r4.u64 == ~a + ca && ctx.xer.ca == ((uint64_t(uint32_t(~a)) + ca) >> 32), "alias rD=rA");
                }
}

static void testMulRot()
{
    auto vals = intVals();
    for (uint64_t a : vals)
        for (uint64_t b : vals)
        {
            int so = (a ^ b) & 1;
            begin("mulhd");
            resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b;
            T_mulhd(ctx, base);
            int64_t hd = int64_t((__int128)int64_t(a) * (__int128)int64_t(b) >> 64);
            CHECK(ctx.r3.s64 == hd, "a=%llx b=%llx", (unsigned long long)a, (unsigned long long)b);
            begin("mulhd.");
            resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b; ctx.xer.so = so;
            T_mulhd_dot(ctx, base);
            CHECK(ctx.r3.s64 == hd, "value");
            checkCR(ctx.cr0, hd, so, "mulhd.");

            begin("mulhw.");
            resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b; ctx.xer.so = so;
            T_mulhw_dot(ctx, base);
            int32_t hw = int32_t((int64_t(int32_t(a)) * int64_t(int32_t(b))) >> 32);
            CHECK(ctx.r3.s32 == hw, "value");
            checkCR(ctx.cr0, hw, so, "mulhw.");

            begin("mulld.");
            resetCtx(); ctx.r4.u64 = a; ctx.r5.u64 = b; ctx.xer.so = so;
            T_mulld_dot(ctx, base);
            CHECK(ctx.r3.u64 == a * b, "value");
            checkCR(ctx.cr0, int64_t(a * b), so, "mulld.");

        }

    for (uint64_t s : vals)
        for (uint64_t d : { 0ull, ~0ull, 0x0123456789ABCDEFull })
        {
            int so = s & 1;
            struct R { const char* name; PPCFunc* fn; uint64_t exp; bool w32; };
            uint64_t rlw = rotl64((s & 0xFFFFFFFF) | (s << 32), 8);
            uint64_t mi = maskMB(32, 47), mw = maskMB(4 + 32, 19 + 32);
            R rs[] = {
                { "rldicl.", T_rldicl_dot, rotl64(s, 12) & maskMB(20, 63), false },
                { "rldicl.", T_rldicl_dot63, rotl64(s, 63) & maskMB(1, 63), false },
                { "rldicr.", T_rldicr_dot, rotl64(s, 40) & maskMB(0, 47), false },
                { "rldimi.", T_rldimi_dot, (rotl64(s, 16) & mi) | (d & ~mi), false },
                { "clrldi.", T_clrldi_dot, s & maskMB(33, 63), false },
                { "rotldi.", T_rotldi_dot, rotl64(s, 8), false },
                { "rlwimi.", T_rlwimi_dot, (rlw & mw) | (d & ~mw), true },
            };
            for (auto& r : rs)
            {
                begin(r.name);
                resetCtx(); ctx.r4.u64 = s; ctx.r3.u64 = d; ctx.xer.so = so;
                r.fn(ctx, base);
                CHECK(ctx.r3.u64 == r.exp, "s=%llx d=%llx -> %llx exp %llx", (unsigned long long)s, (unsigned long long)d, (unsigned long long)ctx.r3.u64, (unsigned long long)r.exp);
                checkCR(ctx.cr0, r.w32 ? int64_t(int32_t(r.exp)) : int64_t(r.exp), so, r.name);
            }
        }
}

// ======================================================================
static void testVmx()
{
    using H = uint16_t;
    auto s16 = [](H x) { return int16_t(x); };
    begin("vslh");
    runVBin(T_vslh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return H(x << (y & 15)); }); });
    begin("vslh");
    for (int k = 0; k < 200; k++)
    {
        GV a = randV(k % 4); setV(ctx.v4, a);
        T_vslh_alias(ctx, base);
        CHECK(getV(ctx.v4) == perH(a, a, [](H x, H y) { return H(x << (y & 15)); }), "vD=vA=vB");
    }
    begin("vsrh");
    runVBin(T_vsrh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return H(x >> (y & 15)); }); });
    begin("vsrah");
    runVBin(T_vsrah, ctx.v3, ctx.v4, ctx.v5, [&](const GV& a, const GV& b) { return perH(a, b, [&](H x, H y) { return H(s16(x) >> (y & 15)); }); });
    begin("vsrab");
    runVBin(T_vsrab, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perB(a, b, [](uint8_t x, uint8_t y) { return uint8_t(int8_t(x) >> (y & 7)); }); });
    begin("vrlh");
    runVBin(T_vrlh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { unsigned n = y & 15; return H(n ? (x << n) | (x >> (16 - n)) : x); }); });
    begin("vsubshs");
    runVBin(T_vsubshs, ctx.v3, ctx.v4, ctx.v5, [&](const GV& a, const GV& b) { return perH(a, b, [&](H x, H y) { return H(clampT<int16_t>(int64_t(s16(x)) - s16(y), -32768, 32767)); }); });
    begin("vsubuhs");
    runVBin(T_vsubuhs, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return H(clampT<uint16_t>(int64_t(x) - y, 0, 65535)); }); });
    begin("vsububm");
    runVBin(T_vsububm, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perB(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x - y); }); });
    begin("vsubsbs");
    runVBin(T_vsubsbs, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perB(a, b, [](uint8_t x, uint8_t y) { return uint8_t(clampT<int8_t>(int64_t(int8_t(x)) - int8_t(y), -128, 127)); }); });
    begin("vadduhs");
    runVBin(T_vadduhs, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return H(clampT<uint16_t>(int64_t(x) + y, 0, 65535)); }); });
    begin("vaddshs");
    runVBin(T_vaddshs, ctx.v3, ctx.v4, ctx.v5, [&](const GV& a, const GV& b) { return perH(a, b, [&](H x, H y) { return H(clampT<int16_t>(int64_t(s16(x)) + s16(y), -32768, 32767)); }); });
    begin("vmaxsh");
    runVBin(T_vmaxsh, ctx.v3, ctx.v4, ctx.v5, [&](const GV& a, const GV& b) { return perH(a, b, [&](H x, H y) { return s16(x) > s16(y) ? x : y; }); });
    begin("vminsh");
    runVBin(T_vminsh, ctx.v3, ctx.v4, ctx.v5, [&](const GV& a, const GV& b) { return perH(a, b, [&](H x, H y) { return s16(x) < s16(y) ? x : y; }); });
    begin("vmaxuh");
    runVBin(T_vmaxuh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return x > y ? x : y; }); });
    begin("vminuh");
    runVBin(T_vminuh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return x < y ? x : y; }); });
    begin("vavguh");
    runVBin(T_vavguh, ctx.v3, ctx.v4, ctx.v5, [](const GV& a, const GV& b) { return perH(a, b, [](H x, H y) { return H((uint32_t(x) + y + 1) >> 1); }); });
    auto nor = [](const GV& a, const GV& b) { return perB(a, b, [](uint8_t x, uint8_t y) { return uint8_t(~(x | y)); }); };
    begin("vnor");
    runVBin(T_vnor, ctx.v3, ctx.v4, ctx.v5, nor);
    begin("vnor128");
    runVBin(T_vnor128, ctx.v70, ctx.v33, ctx.v100, nor);

    // packs: guest vD = pack(vA elements) || pack(vB elements)
    auto packH = [](const std::function<uint8_t(H)>& f) {
        return [f](const GV& a, const GV& b) { GV d; for (int i = 0; i < 8; i++) { d.b[i] = f(a.h(i)); d.b[8 + i] = f(b.h(i)); } return d; };
    };
    auto packW = [](const std::function<H(uint32_t)>& f) {
        return [f](const GV& a, const GV& b) { GV d; for (int i = 0; i < 4; i++) { d.seth(i, f(a.w(i))); d.seth(4 + i, f(b.w(i))); } return d; };
    };
    RefFn pkshss = packH([](H x) { return uint8_t(clampT<int8_t>(int16_t(x), -128, 127)); });
    RefFn pkshus = packH([](H x) { return clampT<uint8_t>(int16_t(x), 0, 255); });
    RefFn pkuhus = packH([](H x) { return clampT<uint8_t>(x, 0, 255); });
    RefFn pkuhum = packH([](H x) { return uint8_t(x); });
    RefFn pkswss = packW([](uint32_t x) { return H(clampT<int16_t>(int32_t(x), -32768, 32767)); });
    RefFn pkswus = packW([](uint32_t x) { return clampT<uint16_t>(int32_t(x), 0, 65535); });
    RefFn pkuwus = packW([](uint32_t x) { return clampT<uint16_t>(x, 0, 65535); });
    RefFn pkuwum = packW([](uint32_t x) { return H(x); });
    struct P { const char* name; PPCFunc* fn; bool v128; RefFn ref; } packs[] = {
        { "vpkshss", T_vpkshss, false, pkshss }, { "vpkshss128", T_vpkshss128, true, pkshss },
        { "vpkshus128", T_vpkshus128, true, pkshus },
        { "vpkuhus", T_vpkuhus, false, pkuhus }, { "vpkuhus128", T_vpkuhus128, true, pkuhus },
        { "vpkuhum", T_vpkuhum, false, pkuhum }, { "vpkuhum128", T_vpkuhum128, true, pkuhum },
        { "vpkswss", T_vpkswss, false, pkswss }, { "vpkswss128", T_vpkswss128, true, pkswss },
        { "vpkswus", T_vpkswus, false, pkswus }, { "vpkswus128", T_vpkswus128, true, pkswus },
        { "vpkuwus", T_vpkuwus, false, pkuwus }, { "vpkuwus128", T_vpkuwus128, true, pkuwus },
        { "vpkuwum", T_vpkuwum, false, pkuwum }, { "vpkuwum128", T_vpkuwum128, true, pkuwum },
    };
    for (auto& p : packs)
    {
        begin(p.name);
        if (p.v128) runVBin(p.fn, ctx.v70, ctx.v33, ctx.v100, p.ref);
        else runVBin(p.fn, ctx.v3, ctx.v4, ctx.v5, p.ref);
    }

    // whole-vector octet shifts: count = vB byte 15 bits 1:4
    auto slo = [](bool left) {
        return [left](const GV& a, const GV& b) {
            int n = (b.b[15] >> 3) & 0xF; GV d;
            for (int i = 0; i < 16; i++) { int j = left ? i + n : i - n; d.b[i] = (j >= 0 && j < 16) ? a.b[j] : 0; }
            return d;
        };
    };
    std::vector<std::pair<GV, GV>> shiftEdges;
    for (int n : { 0, 1, 7, 15 })
        for (int junk : { 0, 7, 0x80 })
        {
            GV a = randV(0), b = randV(0);
            b.b[15] = uint8_t((n << 3) | (junk & 7) | (junk & 0x80));
            shiftEdges.push_back({ a, b });
        }
    // vsldoi: guest vD = bytes sh..sh+15 of vA || vB
    auto sldoi = [](int sh) {
        return [sh](const GV& a, const GV& b) { GV d; for (int i = 0; i < 16; i++) d.b[i] = i + sh < 16 ? a.b[i + sh] : b.b[i + sh - 16]; return d; };
    };
    begin("vsldoi128"); runVBin(T_vsldoi128, ctx.v70, ctx.v33, ctx.v100, sldoi(5));
    begin("vsldoi128 (decoded as mullhwu.)"); runVBin(T_vsldoi128_mullhwu, ctx.v3, ctx.v4, ctx.v40, sldoi(12));
    begin("vsldoi128 (decoded as mulhhwu)"); runVBin(T_vsldoi128_mulhhwu, ctx.v3, ctx.v4, ctx.v5, sldoi(0));
    begin("vsldoi128 (decoded as macchw)"); runVBin(T_vsldoi128_macchw, ctx.v67, ctx.v4, ctx.v5, sldoi(5));
    begin("vslo128"); runVBin(T_vslo128, ctx.v70, ctx.v33, ctx.v100, slo(true), shiftEdges);
    begin("vsro128"); runVBin(T_vsro128, ctx.v70, ctx.v33, ctx.v100, slo(false), shiftEdges);
    begin("vslo"); runVBin(T_vslo, ctx.v3, ctx.v4, ctx.v5, slo(true), shiftEdges);
    begin("vsro"); runVBin(T_vsro, ctx.v3, ctx.v4, ctx.v5, slo(false), shiftEdges);

    // vspltish
    struct S { int simm; PPCFunc* fn; } splats[] = { { -16, T_vspltishm16 }, { -1, T_vspltishm1 }, { 0, T_vspltish0 }, { 1, T_vspltish1 }, { 15, T_vspltish15 } };
    begin("vspltish");
    for (auto& s : splats)
    {
        setV(ctx.v3, randV(0));
        s.fn(ctx, base);
        GV exp; for (int i = 0; i < 8; i++) exp.seth(i, H(int16_t(s.simm)));
        CHECK(getV(ctx.v3) == exp, "simm %d -> %s", s.simm, hex(getV(ctx.v3)).c_str());
    }

    // compares, with and without Rc
    struct Cmp { const char* name; PPCFunc* fn; PPCFunc* fnDot; RefFn ref; };
    auto cmpH = [](const std::function<bool(H, H)>& f) { return [f](const GV& a, const GV& b) { return perH(a, b, [f](H x, H y) { return H(f(x, y) ? 0xFFFF : 0); }); }; };
    auto cmpW = [](const std::function<bool(uint32_t, uint32_t)>& f) { return [f](const GV& a, const GV& b) { return perW(a, b, [f](uint32_t x, uint32_t y) { return f(x, y) ? 0xFFFFFFFFu : 0u; }); }; };
    auto cmpB = [](const std::function<bool(uint8_t, uint8_t)>& f) { return [f](const GV& a, const GV& b) { return perB(a, b, [f](uint8_t x, uint8_t y) { return uint8_t(f(x, y) ? 0xFF : 0); }); }; };
    Cmp cmps[] = {
        { "vcmpequh", T_vcmpequh, T_vcmpequh_dot, cmpH([](H x, H y) { return x == y; }) },
        { "vcmpgtsh", T_vcmpgtsh, T_vcmpgtsh_dot, cmpH([](H x, H y) { return int16_t(x) > int16_t(y); }) },
        { "vcmpgtuh", T_vcmpgtuh, T_vcmpgtuh_dot, cmpH([](H x, H y) { return x > y; }) },
        { "vcmpgtsw", T_vcmpgtsw, T_vcmpgtsw_dot, cmpW([](uint32_t x, uint32_t y) { return int32_t(x) > int32_t(y); }) },
        { "vcmpgtuw", T_vcmpgtuw, T_vcmpgtuw_dot, cmpW([](uint32_t x, uint32_t y) { return x > y; }) },
        { "vcmpgtsb", T_vcmpgtsb, T_vcmpgtsb_dot, cmpB([](uint8_t x, uint8_t y) { return int8_t(x) > int8_t(y); }) },
        { "vcmpgtub", T_vcmpgtub, T_vcmpgtub_dot, cmpB([](uint8_t x, uint8_t y) { return x > y; }) },
    };
    for (auto& c : cmps)
    {
        begin(c.name);
        std::vector<std::pair<GV, GV>> edges;
        GV z{}, ones; memset(ones.b, 0xFF, 16);
        GV seq; for (int i = 0; i < 16; i++) seq.b[i] = uint8_t(i * 17);
        edges.push_back({ z, z }); edges.push_back({ ones, z }); edges.push_back({ z, ones });
        edges.push_back({ seq, seq }); edges.push_back({ ones, ones });
        GV hi; for (int i = 0; i < 16; i++) hi.b[i] = 0x7F; GV lo; for (int i = 0; i < 16; i++) lo.b[i] = 0x01;
        edges.push_back({ hi, lo }); edges.push_back({ lo, hi });
        runVBin(c.fn, ctx.v3, ctx.v4, ctx.v5, c.ref, edges);
        std::string dn = std::string(c.name) + ".";
        begin(dn.c_str());
        runVBin(c.fnDot, ctx.v3, ctx.v4, ctx.v5, c.ref, edges);
        std::vector<std::pair<GV, GV>> all = edges;
        for (int k = 0; k < 300; k++) all.push_back({ randV(k % 4), randV((k / 4) % 4) });
        for (auto& [a, b] : all)
        {
            resetCtx();
            setCRField(ctx.cr6, 0xF);
            setV(ctx.v4, a); setV(ctx.v5, b);
            c.fnDot(ctx, base);
            GV exp = c.ref(a, b);
            bool allT = true, allF = true;
            for (int i = 0; i < 16; i++) { allT &= exp.b[i] == 0xFF; allF &= exp.b[i] == 0; }
            CHECK(getV(ctx.v3) == exp && ctx.cr6.lt == allT && ctx.cr6.gt == 0 && ctx.cr6.eq == allF && ctx.cr6.so == 0,
                "a=%s b=%s cr6 %d%d%d%d", hex(a).c_str(), hex(b).c_str(), ctx.cr6.lt, ctx.cr6.gt, ctx.cr6.eq, ctx.cr6.so);
        }
    }

    // float -> unsigned word with scale
    auto cvt = [](float f, int u) -> uint32_t {
        if (std::isnan(f)) return 0;
        long double x = (long double)f * ldexpl(1.0L, u);
        if (x <= 0) return 0;
        if (x >= 4294967295.0L) return 0xFFFFFFFFu;
        return uint32_t(truncl(x));
    };
    const float fins[] = { 0.0f, -0.0f, 1.5f, -1.5f, 0.99f, 3e9f, 4.3e9f, INFINITY, -INFINITY, NAN, 65535.75f, 4294967040.0f, 2147483648.0f, 0.124f, 1e-30f, 536870911.9f, -0.4f };
    struct U { int u; PPCFunc* fn; } us[] = { { 0, T_vcfpuxws128_0 }, { 3, T_vcfpuxws128_3 }, { 31, T_vcfpuxws128_31 } };
    begin("vcfpuxws128");
    for (auto& u : us)
        for (size_t i = 0; i < std::size(fins); i += 4)
        {
            GV b; for (int k = 0; k < 4; k++) b.setw(k, fbits(fins[(i + k) % std::size(fins)]));
            setV(ctx.v100, b); setV(ctx.v70, randV(0));
            u.fn(ctx, base);
            GV exp; for (int k = 0; k < 4; k++) exp.setw(k, cvt(bitsf(b.w(k)), u.u));
            CHECK(getV(ctx.v70) == exp, "u=%d in=%s got=%s exp=%s", u.u, hex(b).c_str(), hex(getV(ctx.v70)).c_str(), hex(exp).c_str());
        }
    begin("vctuxs");
    for (size_t i = 0; i < std::size(fins); i += 4)
    {
        GV b; for (int k = 0; k < 4; k++) b.setw(k, fbits(fins[(i + k) % std::size(fins)]));
        setV(ctx.v5, b);
        T_vctuxs(ctx, base);
        GV exp; for (int k = 0; k < 4; k++) exp.setw(k, cvt(bitsf(b.w(k)), 2));
        CHECK(getV(ctx.v3) == exp, "in=%s got=%s exp=%s", hex(b).c_str(), hex(getV(ctx.v3)).c_str(), hex(exp).c_str());
    }
}

static void testVmxMem()
{
    for (uint32_t off : { 0u, 1u, 3u, 8u, 13u, 15u })
    {
        const uint32_t blk = 0x20010, ea = blk + off;
        auto fillMem = [&]() { for (uint32_t i = 0; i < 64; i++) w8(0x20000 + i, uint8_t(0xA0 + i * 7)); };
        struct L { const char* name; PPCFunc* fn; PPCVRegister* vr; bool left; } loads[] = {
            { "lvlxl128", T_lvlxl128, &ctx.v101, true }, { "lvrxl128", T_lvrxl128, &ctx.v101, false },
            { "lvlxl", T_lvlxl, &ctx.v3, true }, { "lvrxl", T_lvrxl, &ctx.v3, false },
        };
        for (auto& l : loads)
        {
            begin(l.name);
            resetCtx(); fillMem(); ctx.r4.u64 = 0x20000; ctx.r5.u64 = ea - 0x20000; setV(*l.vr, randV(0));
            l.fn(ctx, base);
            GV exp{};
            if (l.left) for (uint32_t i = 0; i < 16 - off; i++) exp.b[i] = r8(ea + i);
            else for (uint32_t i = 0; i < off; i++) exp.b[16 - off + i] = r8(blk + i);
            CHECK(getV(*l.vr) == exp, "off %u got %s exp %s", off, hex(getV(*l.vr)).c_str(), hex(exp).c_str());
        }

        begin("lvxl128");
        resetCtx(); fillMem(); ctx.r4.u64 = 0x20000; ctx.r5.u64 = ea - 0x20000;
        T_lvxl128(ctx, base);
        { GV exp; for (int i = 0; i < 16; i++) exp.b[i] = r8(blk + i); CHECK(getV(ctx.v101) == exp, "off %u", off); }

        begin("lvehx");
        resetCtx(); fillMem(); ctx.r4.u64 = 0x20000; ctx.r5.u64 = ea - 0x20000;
        T_lvehx(ctx, base);
        { uint32_t e = ea & ~1u; GV g = getV(ctx.v3); CHECK(g.h((e & 15) / 2) == r16(e), "off %u", off); }

        begin("lvebx");
        resetCtx(); fillMem(); ctx.r4.u64 = 0x20000; ctx.r5.u64 = ea - 0x20000;
        T_lvebx(ctx, base);
        { GV g = getV(ctx.v3); CHECK(g.b[ea & 15] == r8(ea), "off %u", off); }

        struct St { const char* name; PPCFunc* fn; PPCVRegister* vr; int kind; } stores[] = {
            { "stvlxl128", T_stvlxl128, &ctx.v101, 0 }, { "stvrxl128", T_stvrxl128, &ctx.v101, 1 },
            { "stvlxl", T_stvlxl, &ctx.v3, 0 }, { "stvrxl", T_stvrxl, &ctx.v3, 1 }, { "stvxl128", T_stvxl128, &ctx.v101, 2 },
        };
        for (auto& s : stores)
        {
            begin(s.name);
            resetCtx(); fillMem(); ctx.r4.u64 = 0x20000; ctx.r5.u64 = ea - 0x20000;
            GV v = randV(0); setV(*s.vr, v);
            uint8_t expMem[64]; for (int i = 0; i < 64; i++) expMem[i] = r8(0x20000 + i);
            if (s.kind == 0) for (uint32_t i = 0; i < 16 - off; i++) expMem[ea - 0x20000 + i] = v.b[i];
            else if (s.kind == 1) for (uint32_t i = 0; i < off; i++) expMem[blk - 0x20000 + i] = v.b[16 - off + i];
            else for (uint32_t i = 0; i < 16; i++) expMem[blk - 0x20000 + i] = v.b[i];
            s.fn(ctx, base);
            CHECK(memcmp(expMem, base + 0x20000, 64) == 0, "off %u", off);
        }
    }
}

int main()
{
    base = (uint8_t*)aligned_alloc(0x10000, MEM_SIZE);
    resetCtx();
    testLoadsStores();
    testBranches();
    testCrLogical();
    testCarry();
    testMulRot();
    testVmx();
    testVmxMem();

    int failedTests = 0;
    for (auto& name : order)
    {
        auto [p, f] = results[name];
        printf("%-34s %s  (%d checks%s)\n", name.c_str(), f ? "FAIL" : "pass", p + f, f ? (", " + std::to_string(f) + " failed").c_str() : "");
        failedTests += f != 0;
    }
    printf("\n%zu instruction groups, %d failing\n", order.size(), failedTests);
    return failedTests ? 1 : 0;
}
