#include "gpu/hlsl.h"

#include <cstdio>

namespace
{
using namespace ucode;

const char kPreamble[] = R"(
cbuffer VSConstants : register(b0)
{
    float4 vc[256];
    uint4 vb[2];      // 256 bool constants
    uint4 vf[96];     // vertex fetch: x byte offset (gmem, or gphys if z), y endian
    uint4 vinfo;      // x index list byte offset, y position mode (0 clip, 1 screen) | 2 points, z/w target size
    float4 vpoint;    // xy constant point diameter (pixels), z/w min/max diameter
};
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
    uint4 pb[2];
    uint4 pinfo;      // x alpha test function (7 = always), y generated-parameter register (0xFF none)
    float4 pinfo2;    // x alpha reference
    uint4 tinfo[32];  // x sampler (0 lin wrap, 1 lin clamp, 2 point clamp, 3 point wrap), y/z size
};
ByteAddressBuffer gmem : register(t0);
ByteAddressBuffer gphys : register(t33);  // guest physical memory (large vertex buffers)
Texture2D<float4> tex[32] : register(t1);
SamplerState sLinearWrap : register(s0);
SamplerState sLinearClamp : register(s1);
SamplerState sPointClamp : register(s2);
SamplerState sPointWrap : register(s3);

static const float FLT_MAX = 3.402823466e+38;

struct VSOut
{
    float4 pos : SV_Position;
    float4 o[16] : TEXCOORD0;
    float2 pcoord : TEXCOORD16; // point sprite coordinate
};

uint Swap(uint v, uint e)
{
    if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3) return (v << 16) | (v >> 16);
    return v;
}

uint LoadGuest(uint fetch, uint dword)
{
    uint a = vf[fetch].x + dword * 4;
    return Swap(vf[fetch].z ? gphys.Load(a) : gmem.Load(a), vf[fetch].y);
}

float SignExtend(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }

float4 Unpack(uint w0, uint w1, uint w2, uint w3, uint format, bool isSigned, bool norm)
{
    float4 r = float4(0, 0, 0, 1);
    switch (format)
    {
    case 6: // 8_8_8_8
    {
        uint4 u = uint4(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF, w0 >> 24);
        if (isSigned) { r = float4(SignExtend(u.x, 8), SignExtend(u.y, 8), SignExtend(u.z, 8), SignExtend(u.w, 8)); if (norm) r = max(r / 127.0, -1.0); }
        else { r = float4(u); if (norm) r /= 255.0; }
        break;
    }
    case 7: // 2_10_10_10
    {
        uint4 u = uint4(w0 & 0x3FF, (w0 >> 10) & 0x3FF, (w0 >> 20) & 0x3FF, w0 >> 30);
        if (isSigned) { r = float4(SignExtend(u.x, 10), SignExtend(u.y, 10), SignExtend(u.z, 10), SignExtend(u.w, 2)); if (norm) r = max(r / float4(511, 511, 511, 1), -1.0); }
        else { r = float4(u); if (norm) r /= float4(1023, 1023, 1023, 3); }
        break;
    }
    case 25: // 16_16
    case 26: // 16_16_16_16
    {
        uint4 u = uint4(w0 & 0xFFFF, w0 >> 16, w1 & 0xFFFF, w1 >> 16);
        if (isSigned) { r = float4(SignExtend(u.x, 16), SignExtend(u.y, 16), SignExtend(u.z, 16), SignExtend(u.w, 16)); if (norm) r = max(r / 32767.0, -1.0); }
        else { r = float4(u); if (norm) r /= 65535.0; }
        if (format == 25) r.zw = float2(0, 1);
        break;
    }
    case 31: r = float4(f16tof32(w0), f16tof32(w0 >> 16), 0, 1); break;
    case 32: r = float4(f16tof32(w0), f16tof32(w0 >> 16), f16tof32(w1), f16tof32(w1 >> 16)); break;
    case 33: r = float4(isSigned ? float(int(w0)) : float(w0), 0, 0, 1); break;
    case 34: r = float4(isSigned ? float2(int2(w0, w1)) : float2(w0, w1), 0, 1); break;
    case 35: r = isSigned ? float4(int4(w0, w1, w2, w3)) : float4(w0, w1, w2, w3); break;
    case 36: r = float4(asfloat(w0), 0, 0, 1); break;
    case 37: r = float4(asfloat(w0), asfloat(w1), 0, 1); break;
    case 57: r = float4(asfloat(w0), asfloat(w1), asfloat(w2), 1); break;
    case 38: r = asfloat(uint4(w0, w1, w2, w3)); break;
    case 16: case 17: r = float4((w0 & 0x7FF) / 2047.0, ((w0 >> 11) & 0x7FF) / 2047.0, (w0 >> 22) / 1023.0, 1); break;
    }
    return r;
}

float4 VFetch(uint fetch, float index, uint stride, uint offset, uint format, bool isSigned, bool norm)
{
    uint d = uint(max(index, 0.0)) * stride + offset;
    return Unpack(LoadGuest(fetch, d), LoadGuest(fetch, d + 1), LoadGuest(fetch, d + 2), LoadGuest(fetch, d + 3), format, isSigned, norm);
}

float4 TFetch(Texture2D<float4> t, uint slot, float2 uv, bool denorm, bool vertexStage)
{
    if (denorm) uv /= float2(max(tinfo[slot].yz, uint2(1, 1)));
    uint mode = tinfo[slot].x;
    if (vertexStage)
        return t.SampleLevel(sLinearClamp, uv, 0);
    if (mode == 1) return t.Sample(sLinearClamp, uv);
    if (mode == 2) return t.Sample(sPointClamp, uv);
    if (mode == 3) return t.Sample(sPointWrap, uv);
    return t.Sample(sLinearWrap, uv);
}

bool BoolConstant(uint4 b[2], uint index) { return (b[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1; }
)";

struct Emitter
{
    const Shader& shader;
    bool pixel;
    std::string out;
    std::string error;
    TranslatedShader& result;
    uint32_t lastStride = 0;
    int indent = 1;

    void Line(const std::string& s)
    {
        out.append(size_t(indent) * 4, ' ');
        out += s;
        out += '\n';
    }

    std::string Constant(uint32_t index) const
    {
        char b[16];
        snprintf(b, sizeof(b), "%s[%u]", pixel ? "pc" : "vc", index & 255);
        return b;
    }

    std::string Src(const Source& s) const
    {
        static const char comp[] = "xyzw";
        std::string base = s.isConstant ? Constant(s.reg) : "r[" + std::to_string(s.reg) + "]";
        if (s.abs)
            base = "abs(" + base + ")";
        std::string swz = ".";
        for (int c = 0; c < 4; c++)
            swz += comp[s.swizzle[c]];
        return std::string(s.negate ? "(-" : "(") + base + swz + ")";
    }

    // Export register to an HLSL lvalue, or empty if the write is ignored.
    std::string ExportTarget(uint32_t reg) const
    {
        if (pixel)
            return reg < 4 ? "oC[" + std::to_string(reg) + "]" : "";
        if (reg == 63)
            return "opts";
        if (reg == 62)
            return "opos";
        if (reg < 16)
            return "o[" + std::to_string(reg) + "]";
        return "";
    }

    static std::string Masked(const std::string& target, const std::string& value, uint32_t mask)
    {
        std::string s;
        for (int c = 0; c < 4; c++)
        {
            if ((mask >> c) & 1)
            {
                const char comp = "xyzw"[c];
                s += target + "." + comp + " = " + value + "." + comp + "; ";
            }
        }
        return s;
    }

    std::string VectorExpr(const Alu& a, const std::string& A, const std::string& B, const std::string& C)
    {
        switch (a.vectorOp)
        {
        case 0: return A + " + " + B;
        case 1: return A + " * " + B;
        case 2: return "max(" + A + ", " + B + ")";
        case 3: return "min(" + A + ", " + B + ")";
        case 4: return "float4(" + A + " == " + B + ")";
        case 5: return "float4(" + A + " > " + B + ")";
        case 6: return "float4(" + A + " >= " + B + ")";
        case 7: return "float4(" + A + " != " + B + ")";
        case 8: return "frac(" + A + ")";
        case 9: return "trunc(" + A + ")";
        case 10: return "floor(" + A + ")";
        case 11: return A + " * " + B + " + " + C;
        case 12: return "(" + A + " == 0.0) ? " + B + " : " + C;
        case 13: return "(" + A + " >= 0.0) ? " + B + " : " + C;
        case 14: return "(" + A + " > 0.0) ? " + B + " : " + C;
        case 15: return "dot(" + A + ", " + B + ").xxxx";
        case 16: return "dot((" + A + ").xyz, (" + B + ").xyz).xxxx";
        case 17: return "((" + A + ").x * (" + B + ").x + (" + A + ").y * (" + B + ").y + (" + C + ").x).xxxx";
        case 18: return "float4(0, 0, 0, 0)"; // cube: not used by the frontend yet
        case 19: { std::string t = A; return "max(max(" + t + ".x, " + t + ".y), max(" + t + ".z, " + t + ".w)).xxxx"; }
        case 28: return "float4(1.0, (" + A + ").y * (" + B + ").y, (" + A + ").z, (" + B + ").w)";
        case 29: return "max(" + A + ", " + B + ")";
        default: return "float4(0, 0, 0, 0)";
        }
    }

    // Emits code computing the scalar result into `s`; may update p.
    std::string ScalarCode(const Alu& a)
    {
        const std::string sv = Src(a.src[2]);
        const std::string A = sv + ".w", B = sv + ".x";
        auto set = [](const std::string& e) { return "s = " + e + ";"; };
        switch (a.scalarOp)
        {
        case 0: return set(A + " + " + B);
        case 1: return set(A + " + ps");
        case 2: return set(A + " * " + B);
        case 3: case 4: return set(A + " * ps");
        case 5: return set("max(" + A + ", " + B + ")");
        case 6: return set("min(" + A + ", " + B + ")");
        case 7: return set("float(" + A + " == 0.0)");
        case 8: return set("float(" + A + " > 0.0)");
        case 9: return set("float(" + A + " >= 0.0)");
        case 10: return set("float(" + A + " != 0.0)");
        case 11: return set("frac(" + A + ")");
        case 12: return set("trunc(" + A + ")");
        case 13: return set("floor(" + A + ")");
        case 14: return set("exp2(" + A + ")");
        case 15: return "s = log2(" + A + "); if (isinf(s)) s = -FLT_MAX;";
        case 16: return set("log2(" + A + ")");
        case 17: return "s = 1.0 / " + A + "; s = clamp(s, -FLT_MAX, FLT_MAX);";
        case 18: return "s = 1.0 / " + A + "; if (isinf(s)) s = 0.0;";
        case 19: return set("1.0 / " + A);
        case 20: return "s = rsqrt(" + A + "); s = clamp(s, -FLT_MAX, FLT_MAX);";
        case 21: return "s = rsqrt(" + A + "); if (isinf(s)) s = 0.0;";
        case 22: return set("rsqrt(" + A + ")");
        case 23: case 24: return set("max(" + A + ", " + B + ")");
        case 25: return set(A + " - " + B);
        case 26: return set(A + " - ps");
        case 27: return "p = (" + A + " == 0.0); s = p ? 0.0 : 1.0;";
        case 28: return "p = (" + A + " != 0.0); s = p ? 0.0 : 1.0;";
        case 29: return "p = (" + A + " > 0.0); s = p ? 0.0 : 1.0;";
        case 30: return "p = (" + A + " >= 0.0); s = p ? 0.0 : 1.0;";
        case 31: return "p = (" + A + " == 0.0); s = p ? 0.0 : ((" + A + " == 1.0) ? 0.0 : " + A + ");";
        case 32: return "p = (" + A + " - 1.0 <= 0.0); s = p ? 0.0 : " + A + " - 1.0;";
        case 33: return "p = false; s = FLT_MAX;";
        case 34: return "p = (" + A + " == 0.0); s = " + A + ";";
        case 35: case 36: case 37: case 38: case 39:
        {
            static const char* cmp[] = { "== 0.0", "> 0.0", ">= 0.0", "!= 0.0", "== 1.0" };
            const std::string c = A + " " + cmp[a.scalarOp - 35];
            return std::string(pixel ? "if (" + c + ") discard; " : "") + "s = float(" + c + ");";
        }
        case 40: return set("sqrt(" + A + ")");
        case 42: case 43: case 44: case 45: case 46: case 47:
        {
            // Constant operand (component W of src3's swizzle) with a temp
            // whose index is spread over the opcode and swizzle bits.
            const uint32_t temp = (a.scalarOp & 1) | (uint32_t(a.src3IsTemp) << 1) | (a.src3RawSwizzle & 0x3C);
            static const char comp[] = "xyzw";
            const std::string ca = Constant(a.src3RawReg) + "." + comp[((a.src3RawSwizzle >> 6) + 3) & 3];
            const std::string tb = "r[" + std::to_string(temp) + "]." + comp[a.src3RawSwizzle & 3];
            const char* op = a.scalarOp <= 43 ? " * " : (a.scalarOp <= 45 ? " + " : " - ");
            const char* neg = a.src[2].negate ? "-" : "";
            return set(std::string("(") + neg + ca + ")" + op + "(" + neg + tb + ")");
        }
        case 48: return set("sin(" + A + ")");
        case 49: return set("cos(" + A + ")");
        case 50: return "s = ps;";
        default:
            error = "scalar op " + std::to_string(a.scalarOp);
            return "s = 0.0;";
        }
    }

    void EmitAlu(const Alu& a)
    {
        std::string body = "{ float4 v = float4(0, 0, 0, 0); float s = ps; ";
        const std::string A = Src(a.src[0]), B = Src(a.src[1]), C = Src(a.src[2]);
        if (a.vectorOp >= 20 && a.vectorOp <= 23)
        {
            static const char* cmp[] = { "== 0.0", "!= 0.0", "> 0.0", ">= 0.0" };
            body += "{ bool c = (" + A + ".w " + cmp[a.vectorOp - 20] + ") && (" + B + ".w == 0.0); p = c; v = c ? 0.0 : (" + A +
                ".x + 1.0).xxxx; } ";
        }
        else if (a.vectorOp >= 24 && a.vectorOp <= 27)
        {
            static const char* cmp[] = { "==", ">", ">=", "!=" };
            body += "{ bool k = any(" + A + " " + cmp[a.vectorOp - 24] + " " + B + "); v = float(k).xxxx; ";
            if (pixel)
                body += "if (k) discard; ";
            body += "} ";
        }
        else
            body += "v = " + VectorExpr(a, A, B, C) + "; ";
        if (a.vectorClamp)
            body += "v = saturate(v); ";
        body += ScalarCode(a) + " ";
        if (a.scalarClamp)
            body += "s = saturate(s); ";
        body += "ps = s; ";

        if (a.exportData)
        {
            const std::string target = ExportTarget(a.vectorDest);
            if (!target.empty())
            {
                const uint32_t vm = a.vectorMask & ~a.scalarMask, sm = a.scalarMask & ~a.vectorMask;
                const uint32_t one = a.vectorMask & a.scalarMask;
                const uint32_t zero = a.scalarDestRelative ? (~(a.vectorMask | a.scalarMask) & 15) : 0;
                body += Masked(target, "v", vm) + Masked(target, "s.xxxx", sm) + Masked(target, "float4(1, 1, 1, 1)", one) +
                    Masked(target, "float4(0, 0, 0, 0)", zero);
            }
        }
        else
        {
            body += Masked("r[" + std::to_string(a.vectorDest) + "]", "v", a.vectorMask);
            body += Masked("r[" + std::to_string(a.scalarDest) + "]", "s.xxxx", a.scalarMask);
        }
        body += "}";
        if (a.predicated)
            body = std::string("if (") + (a.predCondition ? "p" : "!p") + ") " + body;
        Line(body);
    }

    static std::string DestSwizzleWrite(uint32_t dest, uint32_t swizzle, const std::string& value)
    {
        std::string s;
        const std::string target = "r[" + std::to_string(dest) + "]";
        for (int c = 0; c < 4; c++)
        {
            const uint32_t sel = (swizzle >> (3 * c)) & 7;
            const char comp = "xyzw"[c];
            if (sel < 4)
                s += target + "." + comp + " = " + value + "." + "xyzw"[sel] + "; ";
            else if (sel == 4)
                s += target + "." + comp + " = 0.0; ";
            else if (sel == 5)
                s += target + "." + comp + " = 1.0; ";
        }
        return s;
    }

    void EmitFetch(const Fetch& f)
    {
        std::string body;
        if (f.opcode == 0)
        {
            if (pixel)
            {
                error = "vertex fetch in a pixel shader";
                return;
            }
            if (!f.isMini)
                lastStride = f.stride;
            char call[256];
            snprintf(call, sizeof(call), "VFetch(%u, r[%u].%c, %u, %u, %u, %s, %s)", f.constIndex, f.src, "xyzw"[f.srcSwizzle & 3], lastStride,
                f.offset, f.format, f.isSigned ? "true" : "false", f.isNormalized ? "true" : "false");
            body = std::string("{ float4 t = ") + call + "; " + DestSwizzleWrite(f.dest, f.destSwizzle, "t") + "}";
            bool known = false;
            for (uint32_t c : result.vertexFetchConstants)
                known = known || c == f.constIndex;
            if (!known)
                result.vertexFetchConstants.push_back(f.constIndex);
        }
        else if (f.opcode == 1)
        {
            static const char comp[] = "xyzw";
            const std::string uv = std::string("float2(r[") + std::to_string(f.src) + "]." + comp[f.srcSwizzle & 3] + ", r[" +
                std::to_string(f.src) + "]." + comp[(f.srcSwizzle >> 2) & 3] + ")";
            char call[256];
            snprintf(call, sizeof(call), "TFetch(tex[%u], %u, %s, %s, %s)", f.constIndex, f.constIndex, uv.c_str(), f.denormalized ? "true" : "false",
                pixel ? "false" : "true");
            body = std::string("{ float4 t = ") + call + "; " + DestSwizzleWrite(f.dest, f.destSwizzle, "t") + "}";
            result.usedTextureSlots |= 1u << f.constIndex;
        }
        else
            body = DestSwizzleWrite(f.dest, f.destSwizzle, "float4(0, 0, 0, 0)"); // texture queries: not needed yet
        if (f.predicated)
            body = std::string("if (") + (f.predCondition ? "p" : "!p") + ") { " + body + " }";
        Line(body);
    }

    void EmitExec(const Cf& cf)
    {
        for (uint32_t k = 0; k < cf.count(); k++)
        {
            const uint32_t index = cf.address() + k;
            if ((index + 1) * 3 > shader.words.size())
            {
                error = "instruction out of range";
                return;
            }
            const uint32_t* w = &shader.words[index * 3];
            if ((cf.sequence() >> (2 * k)) & 1)
                EmitFetch(DecodeFetch(w));
            else
                EmitAlu(DecodeAlu(w));
        }
    }

    // Control flow as a program-counter state machine: one block per CF
    // instruction, blocks run in order within an iteration, jumps set pc.
    void EmitProgram()
    {
        bool linear = true;
        for (const Cf& cf : shader.cf)
            linear = linear && cf.op() != CfOp::CondJmp && cf.op() != CfOp::LoopStart && cf.op() != CfOp::LoopEnd &&
                cf.op() != CfOp::CondCall && cf.op() != CfOp::Return;
        if (!linear)
        {
            Line("uint cfpc = 0;");
            Line("[loop] for (uint guard = 0; guard < 1024 && cfpc != 0xFFFFu; guard++)");
            Line("{");
            indent++;
        }
        for (size_t i = 0; i < shader.cf.size(); i++)
        {
            const Cf& cf = shader.cf[i];
            const std::string next = cf.IsEnd() ? "0xFFFFu" : std::to_string(i + 1);
            if (!linear)
            {
                Line("if (cfpc == " + std::to_string(i) + "u)");
                Line("{");
                indent++;
            }
            switch (cf.op())
            {
            case CfOp::Nop:
            case CfOp::Alloc:
            case CfOp::MarkVsFetchDone:
                break;
            case CfOp::Exec:
            case CfOp::ExecEnd:
                EmitExec(cf);
                break;
            case CfOp::CondExec:
            case CfOp::CondExecEnd:
            case CfOp::CondExecPredClean:
            case CfOp::CondExecPredCleanEnd:
            {
                char cond[96];
                snprintf(cond, sizeof(cond), "if (BoolConstant(%s, %u) == %s)", pixel ? "pb" : "vb", (cf.hi >> 2) & 0xFF,
                    cf.condition() ? "true" : "false");
                Line(std::string(cond) + " {");
                indent++;
                EmitExec(cf);
                indent--;
                Line("}");
                break;
            }
            case CfOp::CondExecPred:
            case CfOp::CondExecPredEnd:
                Line(std::string("if (") + (cf.condition() ? "p" : "!p") + ") {");
                indent++;
                EmitExec(cf);
                indent--;
                Line("}");
                break;
            case CfOp::CondJmp:
            {
                const uint32_t target = cf.lo & 0x1FFF;
                std::string cond;
                if ((cf.lo >> 13) & 1)
                    cond = "true";
                else if ((cf.lo >> 14) & 1)
                    cond = cf.condition() ? "p" : "!p";
                else
                    cond = std::string("BoolConstant(") + (pixel ? "pb" : "vb") + ", " + std::to_string((cf.hi >> 2) & 0xFF) + ") == " +
                        (cf.condition() ? "true" : "false");
                Line("cfpc = (" + cond + ") ? " + std::to_string(target) + "u : " + next + "u;");
                Line("continue;");
                break;
            }
            default:
                error = std::string("control flow op ") + std::to_string(unsigned(cf.op())) + " not supported yet";
                return;
            }
            if (!error.empty())
                return;
            if (!linear)
            {
                if (cf.op() != CfOp::CondJmp)
                    Line("cfpc = " + next + (cf.IsEnd() ? ";" : "u;"));
                indent--;
                Line("}");
            }
            if (cf.IsEnd())
                break;
        }
        if (!linear)
        {
            indent--;
            Line("}");
        }
    }
};
}

const char* HlslPreamble()
{
    return kPreamble;
}

TranslatedShader TranslateShader(const Shader& shader)
{
    TranslatedShader result;
    if (!shader.valid)
    {
        result.error = "no end of program";
        return result;
    }
    Emitter e{ shader, shader.pixel, {}, {}, result };
    e.EmitProgram();
    if (!e.error.empty())
    {
        result.error = e.error;
        return result;
    }

    std::string h = kPreamble;
    if (!shader.pixel)
    {
        h += "void GuestVS(uint vindex, out float4 opos, out float4 o[16], out float4 opts)\n{\n"
             "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) o[j] = 0;\n"
             "    r[0].x = float(vindex); opos = float4(0, 0, 0, 1); opts = 0; float ps = 0; bool p = false;\n";
        h += e.out;
        h += "}\n"
             "void GuestVS(uint vindex, out float4 opos, out float4 o[16])\n{\n"
             "    float4 opts; GuestVS(vindex, opos, o, opts);\n}\n"
             "VSOut main(uint id : SV_VertexID)\n{\n"
             "    VSOut r;\n"
             "    r.pcoord = 0;\n"
             "    uint e = gmem.Load(vinfo.x + id * 4);\n"
             "    float2 pointOffset = 0;\n"
             "    if (vinfo.y & 2u)\n    {\n"
             "        // Point sprite: corner in bits 29-30, expanded around the point in screen space.\n"
             "        uint corner = (e >> 29) & 3u;\n"
             "        float4 opts;\n"
             "        GuestVS(e & 0x1FFFFFFFu, r.pos, r.o, opts);\n"
             "        float2 d = opts.x > 0 ? opts.xx : vpoint.xy;\n"
             "        d = clamp(d, vpoint.zz, vpoint.ww);\n"
             "        float2 c = float2(corner & 1u, corner >> 1);\n"
             "        r.pcoord = c;\n"
             "        pointOffset = (c * 2.0 - 1.0) * d * 0.5;\n"
             "    }\n"
             "    else if (e & 0x80000000u)\n    {\n"
             "        // Rectangle second triangle: role 0 is the computed fourth corner, roles 1 and 2 the\n"
             "        // ends of the hypotenuse (the corners next to the right angle).\n"
             "        uint b = vinfo.x + (e & 0x1FFFFFFFu) * 4, role = (e >> 29) & 3u;\n"
             "        float4 p[3]; float4 q[3][16];\n"
             "        GuestVS(gmem.Load(b), p[0], q[0]); GuestVS(gmem.Load(b + 4), p[1], q[1]); GuestVS(gmem.Load(b + 8), p[2], q[2]);\n"
             "        float d01 = distance(p[0].xy / p[0].w, p[1].xy / p[1].w), d02 = distance(p[0].xy / p[0].w, p[2].xy / p[2].w);\n"
             "        float d12 = distance(p[1].xy / p[1].w, p[2].xy / p[2].w);\n"
             "        uint ra = (d12 >= d01 && d12 >= d02) ? 0u : ((d02 >= d01) ? 1u : 2u);\n"
             "        uint e1 = (ra + 1u) % 3u, e2 = (ra + 2u) % 3u;\n"
             "        if (role == 1u) { r.pos = p[e1]; r.o = q[e1]; }\n"
             "        else if (role == 2u) { r.pos = p[e2]; r.o = q[e2]; }\n"
             "        else { r.pos = p[e1] + p[e2] - p[ra]; [unroll] for (int i = 0; i < 16; i++) r.o[i] = q[e1][i] + q[e2][i] - q[ra][i]; }\n"
             "    }\n"
             "    else\n        GuestVS(e, r.pos, r.o);\n"
             "    float2 size = float2(vinfo.zw);\n"
             "    if (vinfo.y & 1u)\n    {\n"
             "        // Screen-space positions: to clip space for the target size.\n"
             "        r.pos.xy = float2(r.pos.x / r.pos.w / size.x * 2.0 - 1.0, 1.0 - r.pos.y / r.pos.w / size.y * 2.0) * r.pos.w;\n"
             "    }\n"
             "    r.pos.xy += float2(pointOffset.x / size.x * 2.0, -pointOffset.y / size.y * 2.0) * r.pos.w;\n"
             "    return r;\n}\n";
    }
    else
    {
        h += "float4 main(VSOut input) : SV_Target0\n{\n"
             "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) r[j] = input.o[j];\n"
             "    float4 oC[4]; [unroll] for (int k = 0; k < 4; k++) oC[k] = 0;\n"
             "    float ps = 0; bool p = false;\n"
             "    // Generated parameter: pixel position, point sprite coordinate.\n"
             "    if (pinfo.y < 16u) r[pinfo.y] = float4(input.pos.xy, input.pcoord);\n";
        h += e.out;
        h += "    switch (pinfo.x)\n    {\n"
             "    case 0: discard; break;\n"
             "    case 1: if (!(oC[0].a < pinfo2.x)) discard; break;\n"
             "    case 2: if (!(oC[0].a == pinfo2.x)) discard; break;\n"
             "    case 3: if (!(oC[0].a <= pinfo2.x)) discard; break;\n"
             "    case 4: if (!(oC[0].a > pinfo2.x)) discard; break;\n"
             "    case 5: if (!(oC[0].a != pinfo2.x)) discard; break;\n"
             "    case 6: if (!(oC[0].a >= pinfo2.x)) discard; break;\n"
             "    }\n"
             "    return oC[0];\n}\n";
    }
    result.hlsl = h;
    result.ok = true;
    return result;
}
