// Tiny Xenos microcode assembler for translator tests.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

struct CfI { uint32_t lo = 0, hi = 0; };
inline CfI Exec(uint32_t addr, uint32_t count, uint32_t seq, bool end) { return { addr | (count << 12) | (seq << 16), uint32_t(end ? 2 : 1) << 12 }; }
inline CfI CondExecB(uint32_t addr, uint32_t count, uint32_t seq, uint32_t b, bool cond) { return { addr | (count << 12) | (seq << 16), (b << 2) | (uint32_t(cond) << 10) | (3u << 12) }; }
inline CfI LoopStart(uint32_t skip, uint32_t id, bool repeat = false) { return { skip | (uint32_t(repeat) << 13) | (id << 16), 7u << 12 }; }
inline CfI LoopEnd(uint32_t body, uint32_t id, bool predBreak = false, bool cond = true) { return { body | (id << 16) | (uint32_t(predBreak) << 21), (uint32_t(cond) << 10) | (8u << 12) }; }
inline CfI Call(uint32_t target, bool uncond, uint32_t b = 0, bool cond = true) { return { target | (uint32_t(uncond) << 13), (b << 2) | (uint32_t(cond) << 10) | (9u << 12) }; }
inline CfI Ret() { return { 0, 10u << 12 }; }
inline CfI Jmp(uint32_t target, bool uncond, uint32_t b = 0, bool cond = true) { return { target | (uint32_t(uncond) << 13), (b << 2) | (uint32_t(cond) << 10) | (11u << 12) }; }
inline CfI Alloc(uint32_t type, uint32_t size = 0) { return { size, (type << 9) | (12u << 12) }; }
inline CfI Nop() { return { 0, 0 }; }

struct Op
{
    uint32_t reg = 0;
    bool temp = true;
    uint8_t swz = 0; // component-relative swizzle, 0 = xyzw
    bool neg = false;
    bool rel = false; // temp: + aL; constant: relative (see Alu)
};
inline Op R(uint32_t n, bool rel = false) { Op o; o.reg = n; o.temp = true; o.rel = rel; return o; }
inline Op C(uint32_t n, bool rel = false) { Op o; o.reg = n; o.temp = false; o.rel = rel; return o; }
// Absolute swizzle (e.g. "xxxx") to the component-relative encoding.
inline Op Sw(Op o, const char* s) { uint8_t v = 0; for (int c = 0; c < 4; c++) { uint32_t comp = std::strchr("xyzw", s[c]) - "xyzw"; v |= uint8_t(((comp - c) & 3) << (2 * c)); } o.swz = v; return o; }
inline Op Neg(Op o) { o.neg = true; return o; }

struct AluSpec
{
    uint32_t vop = 2, vdst = 0, vmask = 0, sop = 50, sdst = 0, smask = 0;
    bool exportData = false, vdstRel = false, sdstRel = false, pred = false, predCond = true, absConst = false, constA0 = false;
    Op s1, s2, s3;
};
inline std::array<uint32_t, 3> EncodeAlu(const AluSpec& a)
{
    auto reg = [](const Op& o) { return o.temp ? ((o.reg & 0x3F) | (o.rel ? 0x40u : 0u)) : (o.reg & 0xFF); };
    // Constant relative bits: the first constant operand uses const_0, later ones const_1 unless all before were temps.
    bool c0 = false, c1 = false;
    const Op* ops[3] = { &a.s1, &a.s2, &a.s3 };
    for (int i = 0; i < 3; i++)
    {
        if (ops[i]->temp || !ops[i]->rel) continue;
        bool allTemp = (i < 1 || ops[0]->temp) && (i < 2 || ops[1]->temp);
        (allTemp ? c0 : c1) = true;
    }
    uint32_t w0 = a.vdst | (uint32_t(a.vdstRel) << 6) | (uint32_t(a.absConst) << 7) | (a.sdst << 8) | (uint32_t(a.sdstRel) << 14) |
        (uint32_t(a.exportData) << 15) | (a.vmask << 16) | (a.smask << 20) | (a.sop << 26);
    uint32_t w1 = a.s3.swz | (a.s2.swz << 8) | (a.s1.swz << 16) | (uint32_t(a.s3.neg) << 24) | (uint32_t(a.s2.neg) << 25) | (uint32_t(a.s1.neg) << 26) |
        (uint32_t(a.predCond) << 27) | (uint32_t(a.pred) << 28) | (uint32_t(a.constA0) << 29) | (uint32_t(c1) << 30) | (uint32_t(c0) << 31);
    uint32_t w2 = reg(a.s3) | (reg(a.s2) << 8) | (reg(a.s1) << 16) | (a.vop << 24) | (uint32_t(a.s3.temp) << 29) | (uint32_t(a.s2.temp) << 30) |
        (uint32_t(a.s1.temp) << 31);
    return { w0, w1, w2 };
}
// Vector op only (scalar retains).
inline std::array<uint32_t, 3> V(uint32_t vop, uint32_t dst, uint32_t mask, Op s1, Op s2, Op s3 = R(0), bool exp = false, bool dstRel = false)
{
    AluSpec a; a.vop = vop; a.vdst = dst; a.vmask = mask; a.s1 = s1; a.s2 = s2; a.s3 = s3; a.exportData = exp; a.vdstRel = dstRel; return EncodeAlu(a);
}
enum { ADD = 0, MUL = 1, MAX = 2, MAD = 11 };

struct VFetchSpec { uint32_t src = 0, srcSwz = 0, dst = 1, dstSwz = 0x688 /* xyzw */, constIndex = 0, format = 38, stride = 4; int32_t offset = 0; bool isSigned = false, normalized = true, noZero = false, mini = false; };
inline std::array<uint32_t, 3> EncodeVFetch(const VFetchSpec& f)
{
    uint32_t w0 = 0 | (f.src << 5) | (f.dst << 12) | (1u << 19) | ((f.constIndex / 3) << 20) | ((f.constIndex % 3) << 25) | (f.srcSwz << 30);
    uint32_t w1 = f.dstSwz | (uint32_t(f.isSigned) << 12) | (uint32_t(!f.normalized) << 13) | (uint32_t(f.noZero) << 14) | (f.format << 16) | (uint32_t(f.mini) << 30);
    uint32_t w2 = f.stride | ((uint32_t(f.offset) & 0x7FFFFF) << 8);
    return { w0, w1, w2 };
}
struct TFetchSpec { uint32_t opcode = 1, src = 0, srcSwz = 0 /* xyz: 0 | 1<<2 | 2<<4 */, dst = 1, dstSwz = 0x688, slot = 0, mag = 3, min = 3, mip = 3; bool compLod = true, regLod = false, regGrad = false, denorm = false; int lodBias = 0; uint32_t dim = 1; };
inline std::array<uint32_t, 3> EncodeTFetch(const TFetchSpec& f)
{
    uint32_t w0 = f.opcode | (f.src << 5) | (f.dst << 12) | (f.slot << 20) | (uint32_t(f.denorm) << 25) | (f.srcSwz << 26);
    uint32_t w1 = f.dstSwz | (f.mag << 12) | (f.min << 14) | (f.mip << 16) | (7u << 18) | (uint32_t(f.compLod) << 28) | (uint32_t(f.regLod) << 29);
    uint32_t w2 = uint32_t(f.regGrad) | ((uint32_t(f.lodBias) & 0x7F) << 2) | (f.dim << 14);
    return { w0, w1, w2 };
}

// CF list and instruction slots (relative); returns big-endian dwords with exec addresses relocated.
struct Program
{
    std::vector<CfI> cf;
    std::vector<std::array<uint32_t, 3>> ins;
    std::vector<size_t> execs; // indices of exec-family CF whose address is relative
    size_t AddExec(CfI e) { execs.push_back(cf.size()); cf.push_back(e); return cf.size() - 1; }
    std::vector<uint32_t> Build() const
    {
        std::vector<CfI> c = cf;
        if (c.size() & 1) c.push_back(Nop());
        const uint32_t base = uint32_t(c.size() / 2);
        for (size_t i : execs) c[i].lo += base;
        std::vector<uint32_t> w;
        for (size_t i = 0; i < c.size(); i += 2)
        {
            w.push_back(c[i].lo);
            w.push_back((c[i].hi & 0xFFFF) | (c[i + 1].lo << 16));
            w.push_back((c[i + 1].lo >> 16) | (c[i + 1].hi << 16));
        }
        for (auto& i : ins) w.insert(w.end(), i.begin(), i.end());
        for (auto& x : w) x = __builtin_bswap32(x);
        return w;
    }
};
