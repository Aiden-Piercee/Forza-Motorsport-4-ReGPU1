#include <algorithm>
#include <cmath>
#include "gpu/hlsl.h"
#include "live_switch.h"

#include <cstdio>

namespace
{
using namespace ucode;

const char kPreamble[] = R"(
#ifndef GUEST_PIXEL
#define GUEST_PIXEL 0
#endif
cbuffer VSConstants : register(b0)
{
    float4 vc[256];
    uint4 vb[2];      // 256 bool constants
    uint4 vl[8];      // 32 loop constants (see LoopAddress)
    uint4 vf[96];     // vertex fetch: x byte offset (gmem, or gphys if z), y endian
    uint4 vinfo;      // x index list byte offset, y position mode (0 clip, 1 screen) | 2 points, z/w target size
    float4 vpoint;    // xy constant point diameter (pixels), z/w min/max diameter
    float4 vscale;    // PA_CL_VPORT_X/Y/ZSCALE
    float4 voffset;   // PA_CL_VPORT_X/Y/ZOFFSET
    uint4 vte;        // x PA_CL_VTE_CNTL
    // Vertex fetch k of this shader (see VFetchK): x fetch constant | format << 8 | signed << 14
    // | normalized << 15 | round << 16, y stride, z offset (dwords), w dest swizzle | exp << 16.
    uint4 vfl[32];
    // xy: clip-space factors for perspective geometry in a wide pass (outputs wider than
    // 16:9; 1 otherwise): x narrows the view to the wider screen (Hor+), y zooms in when
    // FM2_WIDE_FOV limits how far the view widens; z keeps 2D (HUD, menus) at 16:9; w is 0
    // for draws that sample a resolved render target (screen layers, never squeezed).
    float4 vwide;
};
cbuffer PSConstants : register(b1)
{
    float4 pc[256];
    uint4 pb[2];
    uint4 pl[8];
    uint4 pinfo;      // x alpha test function (7 = always), y generated-parameter register (0xFF none)
    float4 pinfo2;    // x alpha reference, y/z 1 / render scale (pixel positions back to game pixels), w colour ceiling (0 none)
    uint4 tinfo[32];  // x sampler (0 lin wrap, 1 lin clamp, 2 point clamp, 3 point wrap), y/z size, w dimension (3 cube)
    float4 tbias[32]; // x fetch-constant LOD bias, y (as uint) bindless descriptor index
};
ByteAddressBuffer gmem : register(t0);
ByteAddressBuffer gphys : register(t1);   // guest physical memory (large vertex buffers, indices)
// Bindless textures: the fetch slot's heap index is in tbias[slot].y.
Texture2D<float4> gtex[] : register(t0, space1);
TextureCube<float4> gcube[] : register(t0, space2);
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
    if (fetch >= 96)
        return 0; // slot 96 (index 31, select 3) has no fetch constant behind it: the data reads as zero
    uint a = vf[fetch].x + dword * 4;
    return Swap(vf[fetch].z ? gphys.Load(a) : gmem.Load(a), vf[fetch].y);
}

float SignExtend(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }

// Xenos 16-bit float: like IEEE half but exponent 31 is an ordinary exponent
// (no infinities or NaNs), so the range reaches 131008.
float XHalf(uint h)
{
    h &= 0xFFFF;
    if (((h >> 10) & 31) == 31)
    {
        float v = (1.0 + float(h & 1023) / 1024.0) * 65536.0;
        return (h & 0x8000) ? -v : v;
    }
    return f16tof32(h);
}

// Signed normalised components, half = 2^(n-1) - 1 per component: by default -2^(n-1) and
// -half both give -1; the no-zero mode maps the codes evenly onto [-1, 1] with no exact zero.
float4 SNorm(float4 v, float4 half, bool noZero)
{
    return noZero ? (v * 2.0 + 1.0) / (half * 2.0 + 1.0) : max(v / half, -1.0);
}

float4 Unpack(uint w0, uint w1, uint w2, uint w3, uint format, bool isSigned, bool norm, bool noZero)
{
    float4 r = float4(0, 0, 0, 1);
    switch (format)
    {
    case 6: // 8_8_8_8
    {
        uint4 u = uint4(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF, w0 >> 24);
        if (isSigned) { r = float4(SignExtend(u.x, 8), SignExtend(u.y, 8), SignExtend(u.z, 8), SignExtend(u.w, 8)); if (norm) r = SNorm(r, 127.0, noZero); }
        else { r = float4(u); if (norm) r /= 255.0; }
        break;
    }
    case 7: // 2_10_10_10
    {
        uint4 u = uint4(w0 & 0x3FF, (w0 >> 10) & 0x3FF, (w0 >> 20) & 0x3FF, w0 >> 30);
        if (isSigned) { r = float4(SignExtend(u.x, 10), SignExtend(u.y, 10), SignExtend(u.z, 10), SignExtend(u.w, 2)); if (norm) r = SNorm(r, float4(511, 511, 511, 1), noZero); }
        else { r = float4(u); if (norm) r /= float4(1023, 1023, 1023, 3); }
        break;
    }
    case 25: // 16_16
    case 26: // 16_16_16_16
    {
        uint4 u = uint4(w0 & 0xFFFF, w0 >> 16, w1 & 0xFFFF, w1 >> 16);
        if (isSigned) { r = float4(SignExtend(u.x, 16), SignExtend(u.y, 16), SignExtend(u.z, 16), SignExtend(u.w, 16)); if (norm) r = SNorm(r, 32767.0, noZero); }
        else { r = float4(u); if (norm) r /= 65535.0; }
        if (format == 25) r.zw = float2(0, 1);
        break;
    }
    case 31: r = float4(XHalf(w0), XHalf(w0 >> 16), 0, 1); break;
    case 32: r = float4(XHalf(w0), XHalf(w0 >> 16), XHalf(w1), XHalf(w1 >> 16)); break;
    case 33: // 32
    case 34: // 32_32
    case 35: // 32_32_32_32
    {
        uint4 u = uint4(w0, format >= 34 ? w1 : 0u, format == 35 ? w2 : 0u, format == 35 ? w3 : 0u);
        r = isSigned ? float4(int4(u)) : float4(u);
        if (norm) r = isSigned ? (noZero ? (r * 2.0 + 1.0) / 4294967295.0 : r / 2147483647.0) : r / 4294967295.0;
        if (format == 33) r.yzw = float3(0, 0, 1);
        else if (format == 34) r.zw = float2(0, 1);
        break;
    }
    case 36: r = float4(asfloat(w0), 0, 0, 1); break;
    case 37: r = float4(asfloat(w0), asfloat(w1), 0, 1); break;
    case 57: r = float4(asfloat(w0), asfloat(w1), asfloat(w2), 1); break;
    case 38: r = asfloat(uint4(w0, w1, w2, w3)); break;
    case 16: // 10_11_11: X 11 bits, Y 11 bits, Z 10 bits from the bottom
    case 17: // 11_11_10: X 10 bits, Y 11 bits, Z 11 bits
    {
        uint3 bits = format == 16 ? uint3(11, 11, 10) : uint3(10, 11, 11);
        uint3 u = uint3(w0 & ((1u << bits.x) - 1), (w0 >> bits.x) & ((1u << bits.y) - 1), w0 >> (bits.x + bits.y));
        if (isSigned)
        {
            r.xyz = float3(SignExtend(u.x, bits.x), SignExtend(u.y, bits.y), SignExtend(u.z, bits.z));
            if (norm) r.xyz = max(r.xyz / float3((1u << (bits - 1)) - 1), -1.0);
        }
        else
        {
            r.xyz = float3(u);
            if (norm) r.xyz /= float3((1u << bits) - 1);
        }
        break;
    }
    }
    return r;
}

float4 VFetch(uint fetch, float index, uint stride, uint offset, uint format, bool isSigned, bool norm, bool noZero)
{
    uint d = uint(max(index, 0.0)) * stride + offset; // index already rounded or floored; offset may be negative (wraps)
    return Unpack(LoadGuest(fetch, d), LoadGuest(fetch, d + 1), LoadGuest(fetch, d + 2), LoadGuest(fetch, d + 3), format, isSigned, norm, noZero);
}

// The title binds each vertex shader to the vertex layout it draws by patching its fetch
// instructions (constant slot, format, stride, offset, swizzle). Reading those fields from
// vfl instead of baking them in makes every patched copy of a shader the same HLSL, so they
// share one compile.
float4 VFetchK(uint k, float i)
{
    uint4 p = vfl[k];
    float idx = (p.x & 0x10000u) ? floor(i + 0.5) : floor(i);
    return VFetch(p.x & 0x7Fu, idx, p.y, p.z, (p.x >> 8) & 0x3Fu, (p.x & 0x4000u) != 0, (p.x & 0x8000u) != 0, (p.x & 0x20000u) != 0) *
        exp2(float(int(p.w << 10) >> 26));
}

float4 VMerge(uint k, float4 old, float4 t)
{
    uint sw = vfl[k].w & 0xFFFu;
    float4 r = old;
    [unroll] for (uint c = 0; c < 4; c++)
    {
        uint s = (sw >> (3 * c)) & 7u;
        r[c] = s < 4 ? t[s] : (s == 4 ? 0.0 : (s == 5 ? 1.0 : old[c]));
    }
    return r;
}

// Cube fetch coordinates are (s, t, face) as produced by the CUBE instruction
// plus the usual 1.5 bias: s, t in [1, 2]. Back to a direction for the host.
float3 CubeDirection(float3 stf)
{
    float sc = (stf.x - 1.5) * 2.0, tc = (stf.y - 1.5) * 2.0;
    uint face = uint(round(stf.z));
    switch (face)
    {
    case 0: return float3(1, -tc, -sc);
    case 1: return float3(-1, -tc, sc);
    case 2: return float3(sc, 1, tc);
    case 3: return float3(sc, -1, -tc);
    case 4: return float3(sc, -tc, 1);
    default: return float3(-sc, -tc, -1);
    }
}

// Texture fetch LOD selection (lodMode): 0 the hardware's own LOD plus lod as a bias (pixel
// shaders), 1 lod as the level, 2 the given gradients scaled by 2^lod. lod already includes the
// fetch constant's and the instruction's biases and, for register LOD, setTextureLod's value.
float4 SampleWith(Texture2D<float4> t, SamplerState s, float2 uv, uint lodMode, float lod, float2 gh, float2 gv)
{
#if GUEST_PIXEL
    [branch] if (lodMode == 0u) return t.SampleBias(s, uv, lod);
#endif
    [branch] if (lodMode == 2u) return t.SampleGrad(s, uv, gh, gv);
    return t.SampleLevel(s, uv, lod);
}

float4 TFetchRaw(uint slot, float3 coord, bool denorm, uint lodMode, float lod, float3 gh, float3 gv, float2 texel, uint filter);

// Fetch swizzle (3 bits per channel: 0-3 component, 4 zero, 5 one) for
// textures that live on the GPU; CPU-decoded textures are swizzled already.
// texel: the instruction's texel offset (PCF taps in shadow filters), in texels.
// filter: the instruction's min/mag filter override (0 the fetch constant's, 1 point, 2 linear).
float4 TFetch(uint slot, float3 coord, bool denorm, uint lodMode, float lod, float3 gh, float3 gv, float2 texel, uint filter)
{
    float4 v = TFetchRaw(slot, coord, denorm, lodMode, lod, gh, gv, texel, filter);
    // Exponent adjust (fetch constant): texels scaled by 2^n, n in tinfo.x bits 8-13.
    int expAdjust = int(tinfo[slot].x << 18) >> 26;
    if (expAdjust != 0) v *= exp2(float(expAdjust));
    uint w = tinfo[slot].w;
    if (!(w >> 31))
        return v;
    float4 r;
    [unroll] for (uint c = 0; c < 4; c++)
    {
        uint sel = (w >> (8 + 3 * c)) & 7u;
        r[c] = sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
    }
    return r;
}

float4 TFetchRaw(uint slot, float3 coord, bool denorm, uint lodMode, float lod, float3 gh, float3 gv, float2 texel, uint filter)
{
    uint index = asuint(tbias[slot].y);
    Texture2D<float4> t = gtex[index];
    TextureCube<float4> tc = gcube[index];
    const float2 texelSize = float2(max(tinfo[slot].yz & 0xFFFFu, uint2(1, 1)));
    if ((tinfo[slot].w & 0xFFu) == 3)
    {
        float3 dir = CubeDirection(coord);
#if GUEST_PIXEL
        [branch] if (lodMode == 0u) return tc.SampleBias(sLinearClamp, dir, lod);
#endif
        // Register gradients are in face (s, t) space, where a face is one unit wide: the
        // level the hardware picks for them (the larger footprint, as an isotropic fetch).
        if (lodMode == 2u)
            lod += log2(max(max(length(gh.xy), length(gv.xy)) * texelSize.x, 1e-6));
        return tc.SampleLevel(sLinearClamp, dir, lod);
    }
    float2 uv = coord.xy;
    if (lodMode == 2u)
    {
        const float scale = exp2(lod);
        gh *= scale;
        gv *= scale;
        if (denorm) { gh.xy /= texelSize; gv.xy /= texelSize; }
    }
    if (denorm) uv = (uv + texel) / texelSize;
    else uv += texel / texelSize;
    // Clamp to border (tinfo.x bit 16; bit 17 white): outside the image the border colour, as
    // shadow maps rely on (white: nothing casts a shadow beyond the map).
    if (((tinfo[slot].x >> 16) & 1u) && any(uv < 0.0 || uv > 1.0))
        return ((tinfo[slot].x >> 17) & 1u) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    // One axis clamped, the other wrapping (tinfo.x bits 18 U, 19 V): clamp that axis here to
    // the edge texels' centres; the sampler wraps.
    if ((tinfo[slot].x >> 18) & 1u) uv.x = clamp(uv.x, 0.5 / texelSize.x, 1.0 - 0.5 / texelSize.x);
    if ((tinfo[slot].x >> 19) & 1u) uv.y = clamp(uv.y, 0.5 / texelSize.y, 1.0 - 0.5 / texelSize.y);
    if (tbias[slot].z > 0.0) // declared image inside a wider resolved texture (renderer.cpp)
    {
        uv *= tbias[slot].zw;
        gh.xy *= tbias[slot].zw;
        gv.xy *= tbias[slot].zw;
    }
    uint mode = tinfo[slot].x & 3u;
    if (filter != 0u)
    {
        const bool wrap = mode == 0u || mode == 3u;
        mode = filter == 1u ? (wrap ? 3u : 2u) : (wrap ? 0u : 1u);
    }
    if (mode == 1u) return SampleWith(t, sLinearClamp, uv, lodMode, lod, gh.xy, gv.xy);
    if (mode == 2u) return SampleWith(t, sPointClamp, uv, lodMode, lod, gh.xy, gv.xy);
    if (mode == 3u) return SampleWith(t, sPointWrap, uv, lodMode, lod, gh.xy, gv.xy);
    return SampleWith(t, sLinearWrap, uv, lodMode, lod, gh.xy, gv.xy);
}

// getTextureWeights: the bilinear lerp factors of a fetch at coord (x, y). The LOD lerp factor
// (w) and the level the factors are taken at are not modelled: level 0 (shadow-map PCF, the
// usual reader, has no mips).
float4 TWeights(uint slot, float3 coord, bool denorm, float2 texel)
{
    const float2 size = float2(max(tinfo[slot].yz & 0xFFFFu, uint2(1, 1)));
    return float4(frac((denorm ? coord.xy : coord.xy * size) + texel - 0.5), 0, 0);
}

#if GUEST_PIXEL
// getTextureComputedLod: the level the hardware would sample at, with the biases.
float TComputedLod(uint slot, float3 coord, bool denorm, float bias)
{
    uint index = asuint(tbias[slot].y);
    if ((tinfo[slot].w & 0xFFu) == 3)
        return gcube[index].CalculateLevelOfDetailUnclamped(sLinearClamp, CubeDirection(coord)) + bias;
    float2 uv = denorm ? coord.xy / float2(max(tinfo[slot].yz & 0xFFFFu, uint2(1, 1))) : coord.xy;
    if (tbias[slot].z > 0.0) uv *= tbias[slot].zw;
    return gtex[index].CalculateLevelOfDetailUnclamped(sLinearWrap, uv) + bias;
}
#endif

float4 CubeOp(float4 a, float4 b)
{
    float3 v = float3(a.z, a.w, a.x);
    float3 m = abs(v);
    float sc, tc, ma, face;
    if (m.z >= m.x && m.z >= m.y) { ma = v.z; face = v.z < 0 ? 5 : 4; sc = v.z < 0 ? -v.x : v.x; tc = -v.y; }
    else if (m.y >= m.x) { ma = v.y; face = v.y < 0 ? 3 : 2; sc = v.x; tc = v.y < 0 ? -v.z : v.z; }
    else { ma = v.x; face = v.x < 0 ? 1 : 0; sc = v.x < 0 ? v.z : -v.z; tc = -v.y; }
    return float4(tc, sc, 2.0 * abs(ma), face);
}

// Direct3D 9 multiplication, as the Xenos ALU does it: zero times anything (even
// infinity or NaN) is zero. Shaders rely on it to switch terms off by weight.
float4 Mul9(float4 a, float4 b) { return (a == 0 || b == 0) ? 0 : a * b; }
float Mul9(float a, float b) { return (a == 0 || b == 0) ? 0 : a * b; }
float Dot9(float4 a, float4 b) { float4 m = Mul9(a, b); return m.x + m.y + m.z + m.w; }

bool BoolConstant(uint4 b[2], uint index) { return (b[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1; }
uint LoopConstant(uint4 l[8], uint index) { return l[index >> 2][index & 3]; }
// aL in a loop: iteration * step + start (count bits 0-7, start 8-15, signed step 16-23),
// within the hardware's range of [-256, 256].
int LoopAddress(uint constant, uint iteration)
{
    return clamp(int(iteration) * (int(constant << 8) >> 24) + int((constant >> 8) & 0xFFu), -256, 256);
}
)";

// Memory export (eA / eM#), declared only by shaders that use it. eA is a stream constant the
// shader completes: x the base physical address in dwords (bits 0-29, 0b01 above), y 2^23 plus
// the element index, z 2^23 plus the format (endian 0-2, colour format 8-13, signed 16,
// integer 17, red/blue swap 19), w 2^23 plus the element count. The eM# written since the last
// export go to elements index + #. Packing follows the Direct3D conversion rules.
const char kMemExportPreamble[] = R"(
RWByteAddressBuffer gexport : register(u0); // guest physical memory

// Up to four components of the given bit widths (low first) into one dword.
uint MxPack(float4 v, uint4 bits, bool isSigned, bool isInteger)
{
    uint packed = 0u, offset = 0u;
    [unroll] for (uint c = 0u; c < 4u; c++)
    {
        const uint n = bits[c];
        if (n != 0u)
        {
            float x = v[c] != v[c] ? 0.0 : v[c];
            uint u;
            if (isSigned)
            {
                const float m = float((1u << (n - 1u)) - 1u);
                if (isInteger) x = clamp(x, -1.0 - m, m);
                else { x = clamp(x, -1.0, 1.0); if (n > 2u) x *= m; }
                u = uint(int(x + (x < 0.0 ? -0.5 : 0.5)));
            }
            else
            {
                const float m = float((1u << n) - 1u);
                x = isInteger ? clamp(x, 0.0, m) : (n > 1u ? saturate(x) * m : saturate(x));
                u = uint(x + 0.5);
            }
            packed |= (u & ((1u << n) - 1u)) << offset;
            offset += n;
        }
    }
    return packed;
}

// One element in the colour format; false for formats memory export can't write.
bool MxEncode(float4 v, uint format, bool isSigned, bool isInteger, out uint4 d, out uint sizeLog2)
{
    d = 0u;
    sizeLog2 = 2u;
    switch (format)
    {
    case 2: case 8: case 9: d.x = MxPack(v, uint4(8, 0, 0, 0), isSigned, isInteger); sizeLog2 = 0u; return true; // 8, 8_A, 8_B
    case 3: d.x = MxPack(v, uint4(5, 5, 5, 1), isSigned, isInteger); sizeLog2 = 1u; return true;  // 1_5_5_5
    case 4: d.x = MxPack(v, uint4(5, 6, 5, 0), isSigned, isInteger); sizeLog2 = 1u; return true;  // 5_6_5
    case 5: d.x = MxPack(v, uint4(5, 5, 6, 0), isSigned, isInteger); sizeLog2 = 1u; return true;  // 6_5_5
    case 6: case 14: case 50: d.x = MxPack(v, uint4(8, 8, 8, 8), isSigned, isInteger); return true; // 8_8_8_8 (_A, _AS_16_16_16_16)
    case 7: case 54: d.x = MxPack(v, uint4(10, 10, 10, 2), isSigned, isInteger); return true;    // 2_10_10_10
    case 10: d.x = MxPack(v, uint4(8, 8, 0, 0), isSigned, isInteger); sizeLog2 = 1u; return true; // 8_8
    case 15: d.x = MxPack(v, uint4(4, 4, 4, 4), isSigned, isInteger); sizeLog2 = 1u; return true; // 4_4_4_4
    case 16: case 55: d.x = MxPack(v, uint4(11, 11, 10, 0), isSigned, isInteger); return true;   // 10_11_11
    case 17: case 56: d.x = MxPack(v, uint4(10, 11, 11, 0), isSigned, isInteger); return true;   // 11_11_10
    case 24: d.x = MxPack(v, uint4(16, 0, 0, 0), isSigned, isInteger); sizeLog2 = 1u; return true; // 16
    case 25: d.x = MxPack(v, uint4(16, 16, 0, 0), isSigned, isInteger); return true;              // 16_16
    case 26: // 16_16_16_16
        d.x = MxPack(float4(v.xy, 0, 0), uint4(16, 16, 0, 0), isSigned, isInteger);
        d.y = MxPack(float4(v.zw, 0, 0), uint4(16, 16, 0, 0), isSigned, isInteger);
        sizeLog2 = 3u;
        return true;
    case 30: d.x = f32tof16(v.x); sizeLog2 = 1u; return true;                                   // 16_FLOAT
    case 31: d.x = f32tof16(v.x) | (f32tof16(v.y) << 16); return true;                          // 16_16_FLOAT
    case 32: // 16_16_16_16_FLOAT
        d.xy = f32tof16(v.xz) | (f32tof16(v.yw) << 16);
        sizeLog2 = 3u;
        return true;
    case 36: d.x = asuint(v.x); return true;                                                    // 32_FLOAT
    case 37: d.xy = asuint(v.xy); sizeLog2 = 3u; return true;                                   // 32_32_FLOAT
    case 38: d = asuint(v); sizeLog2 = 4u; return true;                                         // 32_32_32_32_FLOAT
    default: return false;
    }
}

uint4 MxSwap(uint4 d, uint endian)
{
    if (endian == 4u) { d = d.yxwz; endian = 2u; }      // 8-in-64
    else if (endian == 5u) { d = d.wzyx; endian = 2u; } // 8-in-128
    if (endian == 1u || endian == 2u) d = ((d & 0x00FF00FFu) << 8) | ((d >> 8) & 0x00FF00FFu);
    if (endian == 2u || endian == 3u) d = (d << 16) | (d >> 16);
    return d;
}

void MemExport(float4 eA, inout float4 eM[5], inout uint eMw)
{
    const uint4 a = asuint(eA);
    if (eMw != 0u && (a.x >> 30) == 1u && (a.y >> 23) == 0x96u && (a.z >> 23) == 0x96u && (a.w >> 23) == 0x96u)
    {
        const uint format = (a.z >> 8) & 63u, endian = a.z & 7u;
        const bool isSigned = ((a.z >> 16) & 1u) != 0u, isInteger = ((a.z >> 17) & 1u) != 0u, swapRB = ((a.z >> 19) & 1u) != 0u;
        const uint index = a.y & 0x7FFFFFu, count = a.w & 0x7FFFFFu, base = (a.x & 0x3FFFFFFFu) << 2;
        [unroll] for (uint k = 0u; k < 5u; k++)
        {
            uint4 d;
            uint sizeLog2;
            if (((eMw >> k) & 1u) != 0u && index + k < count &&
                MxEncode(swapRB ? eM[k].zyxw : eM[k], format, isSigned, isInteger, d, sizeLog2))
            {
                d = MxSwap(d, endian);
                const uint at = base + ((index + k) << sizeLog2);
                if (sizeLog2 == 4u) gexport.Store4(at, d);
                else if (sizeLog2 == 3u) gexport.Store2(at, d.xy);
                else if (sizeLog2 == 2u) gexport.Store(at, d.x);
                else
                {
                    // 8 and 16 bits: within the dword (memory exports don't overlap in practice).
                    const uint shift = (at & 3u) * 8u, mask = (sizeLog2 == 0u ? 0xFFu : 0xFFFFu) << shift;
                    gexport.InterlockedAnd(at & ~3u, ~mask);
                    gexport.InterlockedOr(at & ~3u, (d.x << shift) & mask);
                }
            }
        }
    }
    eMw = 0u;
    [unroll] for (uint i = 0u; i < 5u; i++) eM[i] = 0;
}
)";

// The shape of a program's control flow, which decides how it is emitted.
struct ControlFlowShape
{
    size_t end = 0;         // index of the first ending CF instruction
    bool linear = true;     // no jumps, loops, calls or returns before it
    bool structured = false; // only properly nested loops: emitted as HLSL loops
    bool hasLoops = false, hasCalls = false;
};

ControlFlowShape AnalyzeControlFlow(const Shader& shader)
{
    ControlFlowShape shape;
    shape.end = shader.cf.size();
    for (size_t i = 0; i < shader.cf.size(); i++)
        if (shader.cf[i].IsEnd())
        {
            shape.end = i;
            break;
        }
    bool jumps = false;
    for (size_t i = 0; i < shader.cf.size() && i <= shape.end; i++)
    {
        const CfOp op = shader.cf[i].op();
        shape.hasLoops = shape.hasLoops || op == CfOp::LoopStart || op == CfOp::LoopEnd;
        shape.hasCalls = shape.hasCalls || op == CfOp::CondCall || op == CfOp::Return;
        jumps = jumps || op == CfOp::CondJmp;
    }
    shape.linear = !shape.hasLoops && !shape.hasCalls && !jumps;
    if (shape.linear || shape.hasCalls || jumps)
        return shape;
    // Loops only: structured if each loop start skips to just after its own loop end, that end
    // goes back to just after the start, they share the loop constant, nesting is at most four
    // deep, no loop repeats the previous iterator and the program doesn't end inside a loop.
    std::vector<size_t> open;
    for (size_t i = 0; i <= shape.end; i++)
    {
        const Cf& cf = shader.cf[i];
        if (cf.op() == CfOp::LoopStart)
        {
            if (cf.isRepeat() || open.size() == 4)
                return shape;
            open.push_back(i);
        }
        else if (cf.op() == CfOp::LoopEnd)
        {
            if (open.empty())
                return shape;
            const Cf& start = shader.cf[open.back()];
            if (cf.target() != open.back() + 1 || start.target() != i + 1 || cf.loopId() != start.loopId())
                return shape;
            open.pop_back();
        }
        else if (cf.IsEnd() && !open.empty())
            return shape;
    }
    shape.structured = open.empty();
    return shape;
}

struct Emitter
{
    const Shader& shader;
    bool pixel;
    std::string out;
    std::string error;
    TranslatedShader& result;
    bool layoutOnly = false;   // only collect the vertex fetch layout (no HLSL)
    bool literalFetch = false; // a fetch had to be baked into the code
    uint32_t lastStride = 0;
    uint32_t lastSrc = 0, lastSrcSwizzle = 0; // mini fetches reuse the full fetch's index
    bool lastSrcRelative = false;
    bool lastRound = false;
    bool memExport = false;    // the program writes eA / eM#
    int indent = 1;

    void Line(const std::string& s)
    {
        out.append(size_t(indent) * 4, ' ');
        out += s;
        out += '\n';
    }

    // Temporary register n, or n + aL.
    static std::string Reg(uint32_t n, bool relative)
    {
        return relative ? "r[(" + std::to_string(n) + " + aL) & 63]" : "r[" + std::to_string(n) + "]";
    }

    std::string Constant(uint32_t index, uint32_t relative = 0) const
    {
        result.constantsUsed = relative ? 256 : std::max(result.constantsUsed, (index & 255) + 1);
        char b[48];
        if (relative)
            snprintf(b, sizeof(b), "%s[(%u + %s) & 255]", pixel ? "pc" : "vc", index & 255, relative == 1 ? "a0" : "aL");
        else
            snprintf(b, sizeof(b), "%s[%u]", pixel ? "pc" : "vc", index & 255);
        return b;
    }

    std::string Src(const Source& s) const
    {
        static const char comp[] = "xyzw";
        std::string base = s.isConstant ? Constant(s.reg, s.relative) : Reg(s.reg, s.tempRelative);
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
        if (reg == 32)
            return "eA";
        if (reg >= 33 && reg <= 37)
            return "eM[" + std::to_string(reg - 33) + "]";
        if (pixel)
        {
            if (reg == 61)
                return "oDep";
            return reg < 4 ? "oC[" + std::to_string(reg) + "]" : "";
        }
        if (reg == 63)
            return "opts";
        if (reg == 62)
            return "opos";
        if (reg < 16)
            return "o[" + std::to_string(reg) + "]";
        return "";
    }

    // Before emitting: what the program exports (colour targets, depth, memory), so the
    // declarations and kills can be set up for it.
    void ScanExports()
    {
        for (const Cf& cf : shader.cf)
        {
            if (!cf.IsExec())
                continue;
            for (uint32_t k = 0; k < cf.count(); k++)
            {
                const uint32_t index = cf.address() + k;
                if ((index + 1) * 3 > shader.words.size() || ((cf.sequence() >> (2 * k)) & 1))
                    continue;
                const Alu a = DecodeAlu(&shader.words[index * 3]);
                if (!a.exportData || !(a.vectorMask | a.scalarMask | (a.scalarDestRelative ? 15u : 0u)))
                    continue;
                if (a.vectorDest >= 33 && a.vectorDest <= 37)
                    result.memExport |= 1u << (a.vectorDest - 33);
                else if (pixel && a.vectorDest < 4)
                    result.colorTargetsWritten |= 1u << a.vectorDest;
                else if (pixel && a.vectorDest == 61)
                    result.writesDepth = true;
            }
        }
        memExport = result.memExport != 0;
    }

    // Pending memory exports are written at allocs, before a pixel is killed and at the end.
    std::string MemExportFlush() const
    {
        return memExport ? "MemExport(eA, eM, eMw); " : "";
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
        case 1: return "Mul9(" + A + ", " + B + ")";
        case 2: return "max(" + A + ", " + B + ")";
        case 3: return "min(" + A + ", " + B + ")";
        case 4: return "float4(" + A + " == " + B + ")";
        case 5: return "float4(" + A + " > " + B + ")";
        case 6: return "float4(" + A + " >= " + B + ")";
        case 7: return "float4(" + A + " != " + B + ")";
        case 8: return "frac(" + A + ")";
        case 9: return "trunc(" + A + ")";
        case 10: return "floor(" + A + ")";
        case 11: return "Mul9(" + A + ", " + B + ") + " + C;
        case 12: return "(" + A + " == 0.0) ? " + B + " : " + C;
        case 13: return "(" + A + " >= 0.0) ? " + B + " : " + C;
        case 14: return "(" + A + " > 0.0) ? " + B + " : " + C;
        case 15: return "Dot9(" + A + ", " + B + ").xxxx";
        case 16: return "Dot9(float4((" + A + ").xyz, 0), float4((" + B + ").xyz, 0)).xxxx";
        case 17: return "(Dot9(float4((" + A + ").xy, 0, 0), float4((" + B + ").xy, 0, 0)) + (" + C + ").x).xxxx";
        case 18: // CUBE: operands are v.zzxy and v.yxzz; result (tc, sc, 2 * major axis, face)
            return "CubeOp(" + A + ", " + B + ")";
        case 19: { std::string t = A; return "max(max(" + t + ".x, " + t + ".y), max(" + t + ".z, " + t + ".w)).xxxx"; }
        case 28: return "float4(1.0, (" + A + ").y * (" + B + ").y, (" + A + ").z, (" + B + ").w)";
        case 29: return "max(" + A + ", " + B + ")"; // MAXAv: a0 is set by the caller
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
        case 2: return set("Mul9(" + A + ", " + B + ")");
        case 3: case 4: return set("Mul9(" + A + ", ps)");
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
        // MAXAs / MAXAsf: also load the address register.
        case 23: return set("max(" + A + ", " + B + ")") + " a0 = clamp(int(floor(" + A + " + 0.5)), -256, 255);";
        case 24: return set("max(" + A + ", " + B + ")") + " a0 = clamp(int(floor(" + A + ")), -256, 255);";
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
            return std::string(pixel ? "if (" + c + ") { " + MemExportFlush() + "discard; } " : "") + "s = float(" + c + ");";
        }
        case 40: return set("sqrt(" + A + ")");
        case 42: case 43: case 44: case 45: case 46: case 47:
        {
            // Constant operand (component W of src3's swizzle) with a temp
            // whose index is spread over the opcode and swizzle bits. The
            // instruction's abs_constants and src3 negation apply to both.
            const uint32_t temp = (a.scalarOp & 1) | (uint32_t(a.src3IsTemp) << 1) | (a.src3RawSwizzle & 0x3C);
            static const char comp[] = "xyzw";
            std::string ca = Constant(a.src3RawReg, a.scalarConstRelative) + "." + comp[((a.src3RawSwizzle >> 6) + 3) & 3];
            std::string tb = "r[" + std::to_string(temp) + "]." + comp[a.src3RawSwizzle & 3];
            if (a.absConstants)
            {
                ca = "abs(" + ca + ")";
                tb = "abs(" + tb + ")";
            }
            const char* op = a.scalarOp <= 43 ? " * " : (a.scalarOp <= 45 ? " + " : " - ");
            const char* neg = a.src[2].negate ? "-" : "";
            if (a.scalarOp <= 43)
                return set(std::string("Mul9(") + neg + ca + ", " + neg + tb + ")");
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
                body += "if (k) { " + MemExportFlush() + "discard; } ";
            body += "} ";
        }
        else
            body += "v = " + VectorExpr(a, A, B, C) + "; ";
        if (a.vectorClamp)
            body += "v = saturate(v); ";
        body += ScalarCode(a) + " ";
        if (a.vectorOp == 29) // MAXAv: a0 from the first operand's W, after the reads
            body += "a0 = clamp(int(floor((" + A + ").w + 0.5)), -256, 255); ";
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
                if (a.vectorDest >= 33 && a.vectorDest <= 37 && (vm | sm | one | zero))
                    body += "eMw |= " + std::to_string(1u << (a.vectorDest - 33)) + "u; ";
            }
        }
        else
        {
            body += Masked(Reg(a.vectorDest, a.vectorDestRelative), "v", a.vectorMask);
            body += Masked(Reg(a.scalarDest, a.scalarDestRelative), "s.xxxx", a.scalarMask);
        }
        body += "}";
        if (a.predicated)
            body = std::string("if (") + (a.predCondition ? "p" : "!p") + ") " + body;
        Line(body);
    }

    static std::string DestSwizzleWrite(uint32_t dest, bool relative, uint32_t swizzle, const std::string& value)
    {
        std::string s;
        const std::string target = Reg(dest, relative);
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

    // The texture fetch source operand: three components by the 6-bit swizzle.
    static std::string TexCoord(const Fetch& f)
    {
        static const char comp[] = "xyzw";
        const std::string reg = Reg(f.src, f.srcRelative) + ".";
        return "float3(" + reg + comp[f.srcSwizzle & 3] + ", " + reg + comp[(f.srcSwizzle >> 2) & 3] + ", " + reg +
            comp[(f.srcSwizzle >> 4) & 3] + ")";
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
            {
                lastStride = f.stride;
                lastSrc = f.src;
                lastSrcSwizzle = f.srcSwizzle;
                lastSrcRelative = f.srcRelative;
                lastRound = f.roundIndex;
            }
            if (getenv("FM2_TRACE_VFETCH"))
                fprintf(stderr, "vfetch: fmt %u signed %d norm %d exp %d stride %u offset %d mini %d round %d\n", f.format, f.isSigned, f.isNormalized,
                    f.expAdjust, lastStride, f.offset, f.isMini, lastRound);
            const uint32_t k = uint32_t(result.vertexFetchLayout.size());
            static LiveSwitch bakedFetch("FM2_BAKED_VFETCH");
            const bool dynamic = !bakedFetch && k < 32 && f.src == lastSrc && f.srcSwizzle == lastSrcSwizzle && f.srcRelative == lastSrcRelative;
            literalFetch = literalFetch || !dynamic;
            const std::array<uint32_t, 4> layout = { f.constIndex | (f.format << 8) | (f.isSigned ? 0x4000u : 0) | (f.isNormalized ? 0x8000u : 0) |
                    (lastRound ? 0x10000u : 0) | (f.signedNoZero ? 0x20000u : 0),
                lastStride, uint32_t(f.offset), f.destSwizzle | ((uint32_t(f.expAdjust) & 0x3F) << 16) };
            if (layoutOnly)
            {
                if (dynamic)
                    result.vertexFetchLayout.push_back(layout);
                if (std::find(result.vertexFetchConstants.begin(), result.vertexFetchConstants.end(), f.constIndex) ==
                    result.vertexFetchConstants.end())
                    result.vertexFetchConstants.push_back(f.constIndex);
                return;
            }
            const std::string dest = Reg(f.dest, f.destRelative);
            if (dynamic)
            {
                // Layout from vfl[k]; only the registers stay in the code.
                result.vertexFetchLayout.push_back(layout);
                char call[200];
                snprintf(call, sizeof(call), "{ float4 t = VFetchK(%u, %s.%c); %s = VMerge(%u, %s, t); }", k, Reg(f.src, f.srcRelative).c_str(),
                    "xyzw"[f.srcSwizzle & 3], dest.c_str(), k, dest.c_str());
                body = call;
            }
            else
            {
                char call[360];
                const std::string index = lastRound ? "floor(" + Reg(lastSrc, lastSrcRelative) + "." + "xyzw"[lastSrcSwizzle & 3] + " + 0.5)"
                                                    : "floor(" + Reg(lastSrc, lastSrcRelative) + "." + "xyzw"[lastSrcSwizzle & 3] + ")";
                snprintf(call, sizeof(call), "VFetch(%u, %s, %u, %uu, %u, %s, %s, %s) * %.9g", f.constIndex, index.c_str(), lastStride,
                    uint32_t(f.offset), f.format, f.isSigned ? "true" : "false", f.isNormalized ? "true" : "false",
                    f.signedNoZero ? "true" : "false", std::ldexp(1.0, f.expAdjust));
                body = std::string("{ float4 t = ") + call + "; " + DestSwizzleWrite(f.dest, f.destRelative, f.destSwizzle, "t") + "}";
            }
            bool known = false;
            for (uint32_t c : result.vertexFetchConstants)
                known = known || c == f.constIndex;
            if (!known)
                result.vertexFetchConstants.push_back(f.constIndex);
        }
        else if (f.opcode == 1 || f.opcode == 17 || f.opcode == 19)
        {
            // Texture fetch (1), getTextureComputedLod (17), getTextureWeights (19).
            const std::string coord = TexCoord(f);
            char call[400];
            if (f.opcode == 19)
                snprintf(call, sizeof(call), "TWeights(%u, %s, %s, float2(%.1f, %.1f))", f.constIndex, coord.c_str(),
                    f.denormalized ? "true" : "false", f.offsetX, f.offsetY);
            else if (f.opcode == 17)
            {
                if (pixel)
                    snprintf(call, sizeof(call), "TComputedLod(%u, %s, %s, %.4f + tbias[%u].x).xxxx", f.constIndex, coord.c_str(),
                        f.denormalized ? "true" : "false", f.lodBias, f.constIndex);
                else
                    snprintf(call, sizeof(call), "float4(0, 0, 0, 0)"); // no implicit LOD outside pixel shaders
            }
            else
            {
                // LOD: the hardware's own (pixel shaders) or the given gradients, else explicit; the
                // biases of the fetch constant and the instruction, and setTextureLod's value, add up.
                // A base-map mip filter samples level 0 only.
                const bool computed = f.useComputedLod && (pixel || f.useRegisterGradients);
                uint32_t lodMode = computed ? (f.useRegisterGradients ? 2u : 0u) : 1u;
                char lod[64];
                if (f.mipFilter == 2)
                {
                    lodMode = 1;
                    snprintf(lod, sizeof(lod), "0.0");
                }
                else
                    snprintf(lod, sizeof(lod), "%.4f + tbias[%u].x%s", f.lodBias, f.constIndex, f.useRegisterLod ? " + tlod" : "");
                // Instruction min/mag filter override, when both agree on point or linear.
                const uint32_t filter = (f.magFilter == f.minFilter && f.magFilter < 2) ? f.magFilter + 1 : 0;
                snprintf(call, sizeof(call), "TFetch(%u, %s, %s, %uu, %s, %s, float2(%.1f, %.1f), %uu)", f.constIndex, coord.c_str(),
                    f.denormalized ? "true" : "false", lodMode, lod, lodMode == 2 ? "tgh, tgv" : "0, 0", f.offsetX, f.offsetY, filter);
            }
            body = std::string("{ float4 t = ") + call + "; " + DestSwizzleWrite(f.dest, f.destRelative, f.destSwizzle, "t") + "}";
            result.usedTextureSlots |= 1u << f.constIndex;
        }
        else if (f.opcode == 18 && pixel)
        {
            // getGradients: screen-space derivatives of two source components,
            // (ddx(a), ddy(a), ddx(b), ddy(b)). Vector-font shaders use it for
            // anti-aliased curve edges.
            static const char comp[] = "xyzw";
            const std::string a = Reg(f.src, f.srcRelative) + "." + comp[f.srcSwizzle & 3];
            const std::string b = Reg(f.src, f.srcRelative) + "." + comp[(f.srcSwizzle >> 2) & 3];
            body = "{ float4 t = float4(ddx(" + a + "), ddy(" + a + "), ddx(" + b + "), ddy(" + b + ")); " +
                DestSwizzleWrite(f.dest, f.destRelative, f.destSwizzle, "t") + "}";
        }
        else if (f.opcode == 24)
            body = "tlod = " + TexCoord(f) + ".x;"; // setTextureLod
        else if (f.opcode == 25)
            body = "tgh = " + TexCoord(f) + ";"; // setTextureGradientsHorz
        else if (f.opcode == 26)
            body = "tgv = " + TexCoord(f) + ";"; // setTextureGradientsVert
        else
            body = DestSwizzleWrite(f.dest, f.destRelative, f.destSwizzle, "float4(0, 0, 0, 0)"); // getTextureBorderColorFrac, vertex-stage getGradients
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
            if (layoutOnly)
            {
                if ((cf.sequence() >> (2 * k)) & 1)
                    if (const Fetch f = DecodeFetch(w); f.opcode == 0)
                        EmitFetch(f);
                continue;
            }
            if ((cf.sequence() >> (2 * k)) & 1)
                EmitFetch(DecodeFetch(w));
            else
                EmitAlu(DecodeAlu(w));
        }
    }

    std::string BoolCondition(uint32_t index, bool condition) const
    {
        return std::string("BoolConstant(") + (pixel ? "pb" : "vb") + ", " + std::to_string(index) + ") == " + (condition ? "true" : "false");
    }

    // Jumps and calls: always, on the predicate or on a bool constant.
    std::string JumpCondition(const Cf& cf) const
    {
        if (cf.isUnconditional())
            return "true";
        if (cf.isPredicated())
            return cf.condition() ? "p" : "!p";
        return BoolCondition(cf.cfBoolAddress(), cf.condition());
    }

    // An exec-family instruction (and alloc / no-op ones) in place.
    void EmitStraight(const Cf& cf)
    {
        switch (cf.op())
        {
        case CfOp::Nop:
        case CfOp::MarkVsFetchDone:
            break;
        case CfOp::Alloc:
            // An alloc ends the open memory export; allocating export memory starts a new one.
            if (memExport)
            {
                Line(MemExportFlush());
                if (cf.allocType() == 3)
                    Line("eA = 0;");
            }
            break;
        case CfOp::Exec:
        case CfOp::ExecEnd:
            EmitExec(cf);
            break;
        case CfOp::CondExec:
        case CfOp::CondExecEnd:
        case CfOp::CondExecPredClean:
        case CfOp::CondExecPredCleanEnd:
            Line("if (" + BoolCondition((cf.hi >> 2) & 0xFF, cf.condition()) + ") {");
            indent++;
            EmitExec(cf);
            indent--;
            Line("}");
            break;
        case CfOp::CondExecPred:
        case CfOp::CondExecPredEnd:
            Line(std::string("if (") + (cf.condition() ? "p" : "!p") + ") {");
            indent++;
            EmitExec(cf);
            indent--;
            Line("}");
            break;
        default:
            error = std::string("control flow op ") + std::to_string(unsigned(cf.op())) + " not supported here";
            break;
        }
    }

    // Straight-line programs, and ones whose only control flow is nested loops: in order up to
    // the end, loops as HLSL loops (aL from the innermost loop's constant and iteration).
    void EmitStructured(const ControlFlowShape& shape)
    {
        const char* loops = pixel ? "pl" : "vl";
        int depth = 0;
        for (size_t i = 0; i <= shape.end && i < shader.cf.size(); i++)
        {
            const Cf& cf = shader.cf[i];
            if (cf.op() == CfOp::LoopStart)
            {
                const std::string d = std::to_string(depth);
                Line("{");
                indent++;
                Line("const uint lc" + d + " = LoopConstant(" + loops + ", " + std::to_string(cf.loopId()) + ");");
                Line("[loop] for (uint li" + d + " = 0; li" + d + " < (lc" + d + " & 0xFFu); li" + d + "++)");
                Line("{");
                indent++;
                Line("aL = LoopAddress(lc" + d + ", li" + d + ");");
                depth++;
            }
            else if (cf.op() == CfOp::LoopEnd)
            {
                depth--;
                if (cf.isPredicatedBreak())
                    Line(std::string("if (") + (cf.condition() ? "p" : "!p") + ") break;");
                indent--;
                Line("}");
                const std::string outer = std::to_string(depth - 1);
                Line(depth ? "aL = LoopAddress(lc" + outer + ", li" + outer + ");" : "aL = 0;");
                indent--;
                Line("}");
            }
            else
                EmitStraight(cf);
            if (!error.empty())
                return;
        }
    }

    // Anything else (jumps, calls, loops that don't nest): a program-counter state machine, one
    // block per CF instruction including subroutines after the end; blocks run in order within
    // an iteration, jumps set the counter. Loops and calls keep the hardware's four-deep stacks.
    void EmitStateMachine(const ControlFlowShape& shape)
    {
        Line("uint cfpc = 0;");
        if (shape.hasLoops)
            Line("uint lsp = 0; uint lcs[4] = { 0, 0, 0, 0 }; uint lit[4] = { 0, 0, 0, 0 };");
        if (shape.hasCalls)
            Line("uint csp = 0; uint cret[4] = { 0, 0, 0, 0 };");
        // The guard only stops a runaway program: loops run up to 255 times, four deep.
        Line(std::string("[loop] for (uint guard = 0; guard < ") + (shape.hasLoops ? "0x100000u" : "1024u") + " && cfpc != 0xFFFFu; guard++)");
        Line("{");
        indent++;
        const char* loops = pixel ? "pl" : "vl";
        for (size_t i = 0; i < shader.cf.size(); i++)
        {
            const Cf& cf = shader.cf[i];
            const std::string next = cf.IsEnd() ? "0xFFFFu" : std::to_string(i + 1) + "u";
            Line("if (cfpc == " + std::to_string(i) + "u)");
            Line("{");
            indent++;
            switch (cf.op())
            {
            case CfOp::CondJmp:
                Line("cfpc = (" + JumpCondition(cf) + ") ? " + std::to_string(cf.target()) + "u : " + next + ";");
                Line("continue;");
                break;
            case CfOp::CondCall:
                // A fifth nested call is dropped, as on the hardware.
                Line("if ((" + JumpCondition(cf) + ") && csp < 4u) { cret[csp] = " + next + "; csp++; cfpc = " + std::to_string(cf.target()) +
                    "u; } else cfpc = " + next + ";");
                Line("continue;");
                break;
            case CfOp::Return:
                // Outside a call, a return does nothing.
                Line("if (csp == 0u) cfpc = " + next + "; else { csp--; cfpc = cret[csp]; }");
                Line("continue;");
                break;
            case CfOp::LoopStart:
            {
                // Iterator 0 (kept from the last loop at this depth when repeating); skip the loop
                // when it is already at the count.
                const std::string skip = std::to_string(cf.target()) + "u";
                Line("if (lsp >= 4u) { cfpc = " + skip + "; continue; }");
                Line("lcs[lsp] = LoopConstant(" + std::string(loops) + ", " + std::to_string(cf.loopId()) + ");");
                if (!cf.isRepeat())
                    Line("lit[lsp] = 0;");
                Line("if (lit[lsp] >= (lcs[lsp] & 0xFFu)) { cfpc = " + skip + "; continue; }");
                Line("aL = LoopAddress(lcs[lsp], lit[lsp]); lsp++; cfpc = " + next + ";");
                Line("continue;");
                break;
            }
            case CfOp::LoopEnd:
            {
                const std::string brk = cf.isPredicatedBreak() ? std::string(" && ") + (cf.condition() ? "!p" : "p") : "";
                Line("if (lsp == 0u) { cfpc = " + next + "; continue; }");
                Line("lit[lsp - 1u]++;");
                Line("if (lit[lsp - 1u] < (lcs[lsp - 1u] & 0xFFu)" + brk + ") { aL = LoopAddress(lcs[lsp - 1u], lit[lsp - 1u]); cfpc = " +
                    std::to_string(cf.target()) + "u; }");
                Line("else { lsp--; aL = lsp ? LoopAddress(lcs[lsp - 1u], lit[lsp - 1u]) : 0; cfpc = " + next + "; }");
                Line("continue;");
                break;
            }
            default:
                EmitStraight(cf);
                Line("cfpc = " + next + ";");
                break;
            }
            if (!error.empty())
                return;
            indent--;
            Line("}");
        }
        indent--;
        Line("}");
    }

    void EmitProgram()
    {
        ScanExports();
        const ControlFlowShape shape = AnalyzeControlFlow(shader);
        if (shape.linear || shape.structured)
            EmitStructured(shape);
        else
            EmitStateMachine(shape);
    }
};
}

const char* HlslPreamble()
{
    return kPreamble;
}

bool TranslateVertexFetchLayout(const Shader& shader, TranslatedShader& out)
{
    TranslatedShader result;
    if (!shader.valid || shader.pixel)
        return false;
    // The same walk as EmitProgram for programs emitted in CF order up to the end; the state
    // machine also emits subroutines after it, so those are translated in full.
    const ControlFlowShape shape = AnalyzeControlFlow(shader);
    if (!shape.linear && !shape.structured)
        return false;
    Emitter e{ shader, shader.pixel, {}, {}, result };
    e.layoutOnly = true;
    for (size_t i = 0; i <= shape.end && i < shader.cf.size(); i++)
    {
        const Cf& cf = shader.cf[i];
        if (cf.IsExec())
            e.EmitExec(cf);
        if (!e.error.empty() || e.literalFetch)
            return false;
    }
    out.vertexFetchLayout = std::move(result.vertexFetchLayout);
    out.vertexFetchConstants = std::move(result.vertexFetchConstants);
    return true;
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

    std::string h = shader.pixel ? "#define GUEST_PIXEL 1\n" : "";
    h += kPreamble;
    if (result.memExport)
        h += kMemExportPreamble;
    // Shader state beyond the temporaries: texture LOD and gradients from the set* fetches,
    // and the open memory export.
    std::string state = "    float tlod = 0; float3 tgh = 0, tgv = 0;\n";
    if (result.memExport)
        state += "    float4 eA = 0; float4 eM[5]; [unroll] for (int m = 0; m < 5; m++) eM[m] = 0; uint eMw = 0;\n";
    if (!shader.pixel)
    {
        h += "void GuestVS(uint vindex, out float4 opos, out float4 o[16], out float4 opts)\n{\n"
             "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) o[j] = 0;\n"
             "    r[0].x = float(vindex); opos = float4(0, 0, 0, 1); opts = 0; float ps = 0; bool p = false; int a0 = 0, aL = 0;\n";
        h += state;
        h += e.out;
        if (result.memExport)
            h += "    " + e.MemExportFlush() + "\n";
        h += "}\n"
             "void GuestVS(uint vindex, out float4 opos, out float4 o[16])\n{\n"
             "    float4 opts; GuestVS(vindex, opos, o, opts);\n}\n"
             "VSOut main(uint id : SV_VertexID)\n{\n"
             "    VSOut r;\n"
             "    r.pcoord = 0;\n"
             "    uint e;\n"
             "    if (vte.y & 1u)\n    {\n"
             "        // Direct indices (triangle lists and restart-free strips): read the\n"
             "        // guest index buffer in place. y: 1 direct, 2 strip, 4 32-bit, 8 auto, endian in bits 4-5.\n"
             "        uint i = id;\n"
             "        if (vte.y & 2u)\n        {\n"
             "            uint t = id / 3u, k = id % 3u;\n"
             "            i = t + ((t & 1u) ? (k == 0u ? 1u : (k == 1u ? 0u : 2u)) : k);\n"
             "        }\n"
             "        else if (vte.y & 64u)\n        {\n"
             "            // Quad list: two triangles (0 1 2, 0 2 3) per quad.\n"
             "            uint q = id / 6u, k = id % 6u;\n"
             "            i = q * 4u + (k < 3u ? k : (k == 3u ? 0u : k - 2u));\n"
             "        }\n"
             "        uint endian = (vte.y >> 4) & 3u;\n"
             "        if (vte.y & 8u)\n            e = i;\n"
             "        else if (vte.y & 4u)\n        {\n"
             "            e = gphys.Load(vte.z + i * 4u);\n"
             "            if (endian == 2u) e = (e >> 24) | ((e >> 8) & 0xFF00u) | ((e << 8) & 0xFF0000u) | (e << 24);\n"
             "            else if (endian == 1u) e = ((e & 0x00FF00FFu) << 8) | ((e >> 8) & 0x00FF00FFu);\n"
             "        }\n"
             "        else\n        {\n"
             "            uint a = vte.z + i * 2u;\n"
             "            uint w = gphys.Load(a & ~3u);\n"
             "            e = (a & 2u) ? (w >> 16) : (w & 0xFFFFu);\n"
             "            if (endian == 1u || endian == 2u) e = ((e & 0xFFu) << 8) | (e >> 8);\n"
             "        }\n"
             "        e += vte.w;\n"
             "    }\n"
             "    else\n"
             "        e = gmem.Load(vinfo.x + id * 4);\n"
             "    float2 pointOffset = 0;\n"
             "    // fxc inlines every GuestVS call; a large shader inlined once per path (point,\n"
             "    // three rectangle corners, plain) took 15 s to compile. Plain vertices keep their\n"
             "    // own call; points and rectangles share one in a loop.\n"
             "    const bool isPoint = (vinfo.y & 2u) != 0, rect = !isPoint && (e & 0x80000000u);\n"
             "    [branch] if (!isPoint && !rect)\n"
             "        GuestVS(e, r.pos, r.o);\n"
             "    else\n"
             "    {\n"
             "        uint vi[3];\n"
             "        vi[0] = isPoint ? (e & 0x1FFFFFFFu) : e; vi[1] = 0; vi[2] = 0;\n"
             "        if (rect)\n"
             "        {\n"
             "            uint b = vinfo.x + (e & 0x1FFFFFFFu) * 4;\n"
             "            vi[0] = gmem.Load(b); vi[1] = gmem.Load(b + 4); vi[2] = gmem.Load(b + 8);\n"
             "        }\n"
             "        float4 vp[3]; float4 vq[3][16]; float4 opts = 0;\n"
             "        [loop] for (uint k = 0; k < (rect ? 3u : 1u); k++)\n"
             "        {\n"
             "            float4 kp, ko[16], kpts;\n"
             "            GuestVS(vi[k], kp, ko, kpts);\n"
             "            vp[k] = kp; vq[k] = ko;\n"
             "            if (k == 0u) opts = kpts;\n"
             "        }\n"
             "        if (isPoint)\n"
             "        {\n"
             "            // Point sprite: corner in bits 29-30, expanded around the point in screen space.\n"
             "            uint corner = (e >> 29) & 3u;\n"
             "            r.pos = vp[0]; r.o = vq[0];\n"
             "            float2 d = opts.x > 0 ? opts.xx : vpoint.xy;\n"
             "            d = clamp(d, vpoint.zz, vpoint.ww);\n"
             "            float2 c = float2(corner & 1u, corner >> 1);\n"
             "            r.pcoord = c;\n"
             "            pointOffset = (c * 2.0 - 1.0) * d * 0.5;\n"
             "        }\n"
             "        else\n"
             "        {\n"
             "            // Rectangle second triangle: role 0 is the computed fourth corner, roles 1 and 2 the\n"
             "            // ends of the hypotenuse (the corners next to the right angle).\n"
             "            uint role = (e >> 29) & 3u;\n"
             "            float d01 = distance(vp[0].xy / vp[0].w, vp[1].xy / vp[1].w), d02 = distance(vp[0].xy / vp[0].w, vp[2].xy / vp[2].w);\n"
             "            float d12 = distance(vp[1].xy / vp[1].w, vp[2].xy / vp[2].w);\n"
             "            uint ra = (d12 >= d01 && d12 >= d02) ? 0u : ((d02 >= d01) ? 1u : 2u);\n"
             "            uint e1 = (ra + 1u) % 3u, e2 = (ra + 2u) % 3u;\n"
             "            if (role == 1u) { r.pos = vp[e1]; r.o = vq[e1]; }\n"
             "            else if (role == 2u) { r.pos = vp[e2]; r.o = vq[e2]; }\n"
             "            else { r.pos = vp[e1] + vp[e2] - vp[ra]; [unroll] for (int i = 0; i < 16; i++) r.o[i] = vq[e1][i] + vq[e2][i] - vq[ra][i]; }\n"
             "        }\n"
             "    }\n"
             "    float2 size = float2(vinfo.zw);\n"
             "    // The Xenos viewport transform (PA_CL_VTE_CNTL): optional divide by w, then\n"
             "    // per-axis scale and offset to screen pixels; back to D3D clip space after.\n"
             "    // Hor+: on a wider-than-16:9 output a full-width pass is drawn wider; perspective\n"
             "    // geometry is narrowed to match, so the view widens instead of stretching.\n"
             "    const bool flat = abs(r.pos.w - 1.0) <= 1e-4;\n"
             "    if (!flat) r.pos.xy *= vwide.xy;\n"
             "    float w = r.pos.w;\n"
             "    // (VTX_W0_FMT set means w is the true W0, the normal case.)\n"
             "    float3 p = r.pos.xyz;\n"
             "    if (!(vte.x & (1u << 8))) p.xy /= w;\n"
             "    if (!(vte.x & (1u << 9))) p.z /= w;\n"
             "    if (vte.x & 1u) p.x *= vscale.x;\n"
             "    if (vte.x & 2u) p.x += voffset.x;\n"
             "    if (vte.x & 4u) p.y *= vscale.y;\n"
             "    if (vte.x & 8u) p.y += voffset.y;\n"
             "    if (vte.x & 16u) p.z *= vscale.z;\n"
             "    if (vte.x & 32u) p.z += voffset.z;\n"
             "    // Positions the title gives in screen space (no viewport scale or offset: its\n"
             "    // rectangle clears and filter passes, authored for D3D9's integer pixel centres,\n"
             "    // e.g. -0.5 .. 255.5) must cover the last row and column too: shift by half a pixel.\n"
             "    if ((vte.x & 0x30Fu) == 0x300u) p.xy += 0.5;\n"
             "    // vscale.w/voffset.w: pixel scale of MSAA clear rectangles (renderer.cpp).\n"
             "    if (vscale.w > 0.0) p.xy *= float2(vscale.w, voffset.w);\n"
             "    p.xy += pointOffset;\n"
             "    if (vte.x & (1u << 8)) w = 1.0;\n"
             "    float2 ndc = float2(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0);\n"
             "    // Flat (2D) geometry in a wide pass keeps 16:9 proportions in a centred 16:9 area\n"
             "    // (HUD, menus, text); vertices on the left or right screen edge stay there, so\n"
             "    // full-screen layers (backgrounds, fades) still span the width. Screen-space passes\n"
             "    // (no viewport transform: the title's filter and post-processing passes) are left\n"
             "    // alone: squeezing their inner vertices warped the image outside 16:9.\n"
             "    if (flat && vwide.w > 0.5 && vwide.z < 1.0 && (vte.x & 0x30Fu) != 0x300u && abs(ndc.x) < 0.999) ndc.x *= vwide.z;\n"
             "    r.pos = float4(ndc * w, p.z * w, w);\n"
             "    return r;\n}\n";
    }
    else
    {
        // A shader that exports depth always writes SV_Depth: where it doesn't, the rasterised depth.
        h += result.writesDepth ? "float4 main(VSOut input, out float oDepthOut : SV_Depth) : SV_Target0\n{\n"
                                : "float4 main(VSOut input) : SV_Target0\n{\n";
        h += "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; i++) r[i] = 0;\n"
             "    [unroll] for (int j = 0; j < 16; j++) r[j] = input.o[j];\n"
             "    float4 oC[4]; [unroll] for (int k = 0; k < 4; k++) oC[k] = 0;\n"
             "    float ps = 0; bool p = false; int a0 = 0, aL = 0;\n";
        h += state;
        if (result.writesDepth)
            h += "    float4 oDep = input.pos.zzzz;\n";
        h += "    // Generated parameter: pixel position, point sprite coordinate.\n"
             "    if (pinfo.y < 16u) r[pinfo.y] = float4(input.pos.xy * pinfo2.yz, input.pcoord);\n";
        h += e.out;
        if (result.memExport)
            h += "    " + e.MemExportFlush() + "\n";
        if (result.writesDepth)
            h += "    oDepthOut = oDep.x;\n";
        h += "    switch (pinfo.x)\n    {\n"
             "    case 0: discard; break;\n"
             "    case 1: if (!(oC[0].a < pinfo2.x)) discard; break;\n"
             "    case 2: if (!(oC[0].a == pinfo2.x)) discard; break;\n"
             "    case 3: if (!(oC[0].a <= pinfo2.x)) discard; break;\n"
             "    case 4: if (!(oC[0].a > pinfo2.x)) discard; break;\n"
             "    case 5: if (!(oC[0].a != pinfo2.x)) discard; break;\n"
             "    case 6: if (!(oC[0].a >= pinfo2.x)) discard; break;\n"
             "    }\n"
             "    if (pinfo2.w > 0.0) oC[0].rgb = clamp(oC[0].rgb, 0.0, pinfo2.w); // 7e3 target range\n"
             "    return oC[0];\n}\n";
    }
    result.hlsl = h;
    result.ok = true;
    return result;
}
