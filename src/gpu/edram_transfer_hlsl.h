#pragma once
// HLSL of the EDRAM transfer shader (gpu/edram_targets.cpp), also built and run by
// tools/edramtest with TRANSFER_TEST defined (which supplies its own SourceGuest).
namespace edram
{
// The transfer shader. Each destination sample finds the EDRAM dwords it covers (base, pitch
// in tiles, 80x16-sample tiles at 32 bpp, 40x16 at 64 bpp), the source sample that holds each
// dword, and converts through the guest's bits: source host value -> guest EDRAM dword(s) ->
// destination host value. Formats are the RB_COLOR_INFO / RB_DEPTH_INFO numbers.
//   info: base tiles 0-10, pitch tiles (at the layout's bpp) 11-21, MSAA 22-23, 64 bpp 24,
//         format 26-29.   misc: scale x 0-3, scale y 4-7, stencil bit 8-10.
// Host samples follow the standard D3D patterns: 2x has sample 1 above sample 0 (the guest's
// two samples are one above the other), 4x is 0 1 / 2 3.
inline constexpr char kEdramTransferHlsl[] = R"(
cbuffer Transfer : register(b0) { uint destInfo; uint srcInfo; uint misc; uint unused; };
#ifndef TRANSFER_TEST // (tools/edramtest supplies its own source)
#if SRC_SAMPLES > 1
#if SRC_DEPTH
Texture2DMS<float, SRC_SAMPLES> srcDepth : register(t0);
Texture2DMS<uint2, SRC_SAMPLES> srcStencil : register(t1);
#else
Texture2DMS<uint4, SRC_SAMPLES> srcColor : register(t0);
#endif
#define LOAD(t, p, s) t.Load(int2(p), s)
#else
#if SRC_DEPTH
Texture2D<float> srcDepth : register(t0);
Texture2D<uint2> srcStencil : register(t1);
#else
Texture2D<uint4> srcColor : register(t0);
#endif
#define LOAD(t, p, s) t.Load(int3(p, 0))
#endif
#endif

// 7e3 (2_10_10_10_FLOAT colour): 3-bit exponent biased by 3, 7-bit mantissa, [0, 31.875].
uint Encode7e3(float f)
{
    uint b = asuint(clamp(f, 0.0, 31.875));
    uint v = b < 0x3E800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> min(125u - (b >> 23), 24u) : b + 0xC2000000u;
    return ((v + 0x7FFFu + ((v >> 16) & 1u)) >> 16) & 0x3FFu;
}
float Decode7e3(uint v)
{
    int e = int((v >> 7) & 7u);
    uint m = v & 0x7Fu;
    if (e == 0)
    {
        if (m == 0u)
            return 0.0;
        int s = 7 - int(firstbithigh(m));
        e = 1 - s;
        m = (m << uint(s)) & 0x7Fu;
    }
    return asfloat(uint(e + 124) << 23 | m << 16);
}
// 20e4 (D24FS8 depth): 4-bit exponent biased by 15, 20-bit mantissa, [0, 2).
uint Encode20e4(float f)
{
    uint b = asuint(clamp(f, 0.0, 1.99999988));
    uint v = b < 0x38800000u ? ((b & 0x7FFFFFu) | 0x800000u) >> min(113u - (b >> 23), 24u) : b + 0xC8000000u;
    return ((v + 3u + ((v >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
float Decode20e4(uint v)
{
    int e = int((v >> 20) & 15u);
    uint m = v & 0xFFFFFu;
    if (e == 0)
    {
        if (m == 0u)
            return 0.0;
        int s = 20 - int(firstbithigh(m));
        e = 1 - s;
        m = (m << uint(s)) & 0xFFFFFu;
    }
    return asfloat(uint(e + 112) << 23 | m << 3);
}

uint2 ColorToGuest(uint f, uint4 c)
{
    if (f <= 1u) // 8_8_8_8 (and _GAMMA)
        return uint2(c.r | c.g << 8 | c.b << 16 | c.a << 24, 0);
    if (f == 2u) // 2_10_10_10
        return uint2(c.r | c.g << 10 | c.b << 20 | c.a << 30, 0);
    if (f == 3u) // 2_10_10_10_FLOAT, held as half floats
    {
        float4 h = f16tof32(c);
        return uint2(Encode7e3(h.r) | Encode7e3(h.g) << 10 | Encode7e3(h.b) << 20 | uint(round(saturate(h.a) * 3.0)) << 30, 0);
    }
    if (f == 4u || f == 6u) // 16_16, 16_16_FLOAT
        return uint2(c.r | c.g << 16, 0);
    if (f == 5u || f == 7u) // 16_16_16_16, 16_16_16_16_FLOAT
        return uint2(c.r | c.g << 16, c.b | c.a << 16);
    if (f == 14u) // 32_FLOAT
        return uint2(c.r, 0);
    return c.rg; // 32_32_FLOAT
}
uint4 GuestToColor(uint f, uint2 w)
{
    if (f <= 1u)
        return uint4(w.x & 0xFFu, (w.x >> 8) & 0xFFu, (w.x >> 16) & 0xFFu, w.x >> 24);
    if (f == 2u)
        return uint4(w.x & 0x3FFu, (w.x >> 10) & 0x3FFu, (w.x >> 20) & 0x3FFu, w.x >> 30);
    if (f == 3u)
        return f32tof16(float4(Decode7e3(w.x & 0x3FFu), Decode7e3((w.x >> 10) & 0x3FFu), Decode7e3((w.x >> 20) & 0x3FFu), float(w.x >> 30) / 3.0));
    if (f == 4u || f == 6u)
        return uint4(w.x & 0xFFFFu, w.x >> 16, 0, 0);
    if (f == 5u || f == 7u)
        return uint4(w.x & 0xFFFFu, w.x >> 16, w.y & 0xFFFFu, w.y >> 16);
    if (f == 14u)
        return uint4(w.x, 0, 0, 0);
    return uint4(w.x, w.y, 0, 0);
}
// Depth and stencil dword: depth in the upper 24 bits (D24S8 unorm, D24FS8 20e4), stencil below.
uint EncodeUnorm24(float d) { return uint(round(saturate(d) * 16777215.0)); }
float DecodeUnorm24(uint z)
{
    // z / (2^24 - 1) through the reciprocal can land an ulp off: step to the float that
    // encodes back to z, so a transfer keeps the guest's depth exactly.
    float d = float(z) * (1.0 / 16777215.0);
    [unroll] for (int i = 0; i < 2; i++)
    {
        uint e = EncodeUnorm24(d);
        d = e < z ? asfloat(asuint(d) + 1u) : (e > z ? asfloat(asuint(d) - 1u) : d);
    }
    return d;
}
uint DepthToGuest(uint f, float d, uint stencil)
{
    uint z = f == 0u ? EncodeUnorm24(d) : Encode20e4(d);
    return z << 8 | (stencil & 0xFFu);
}
float GuestToDepth(uint f, uint w)
{
    return f == 0u ? DecodeUnorm24(w >> 8) : Decode20e4(w >> 8);
}

#ifndef TRANSFER_TEST
// The guest dwords of one source sample (host pixel p, host sample s).
uint2 SourceGuest(uint2 p, uint s, uint format)
{
#if SRC_DEPTH
    return uint2(DepthToGuest(format, LOAD(srcDepth, p, s), LOAD(srcStencil, p, s).y), 0);
#else
    return ColorToGuest(format, LOAD(srcColor, p, s));
#endif
}
#endif

// Guest sample (x, y) inside its pixel <-> host sample index.
uint HostSample(uint msaa, uint2 b) { return msaa == 2u ? (b.x | b.y << 1) : (msaa == 1u ? 1u - b.y : 0u); }
uint2 GuestSampleBits(uint msaa, uint s) { return msaa == 2u ? uint2(s & 1u, s >> 1) : (msaa == 1u ? uint2(0, 1u - s) : uint2(0, 0)); }

float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

struct Out
{
#if DEST_DEPTH
    float depth : SV_Depth;
#if STENCIL == 1
    uint stencil : SV_StencilRef;
#endif
#else
    uint4 color : SV_Target0;
#endif
};

// The guest dwords the destination's host pixel hp, host sample `sampleIndex` holds.
uint2 GuestAt(uint2 hp, uint sampleIndex)
{
    uint2 scale = uint2(misc & 15u, (misc >> 4) & 15u);
    uint2 gp = hp / scale, sub = hp % scale;
    uint dBase = destInfo & 0x7FFu, dPitch = (destInfo >> 11) & 0x7FFu, dMsaa = (destInfo >> 22) & 3u, d64 = (destInfo >> 24) & 1u;
    uint sBase = srcInfo & 0x7FFu, sPitch = (srcInfo >> 11) & 0x7FFu, sMsaa = (srcInfo >> 22) & 3u, s64 = (srcInfo >> 24) & 1u;
    uint sFormat = (srcInfo >> 26) & 15u;
    uint2 db = GuestSampleBits(dMsaa, sampleIndex);
    // The destination sample in EDRAM: its tile and the dwords it covers in the tile's row.
    uint2 dxy = uint2(gp.x << (dMsaa >= 2u ? 1u : 0u) | db.x, gp.y << (dMsaa >= 1u ? 1u : 0u) | db.y);
    uint dTileW = 80u >> d64;
    uint tile = (dBase + (dxy.y >> 4) * dPitch + dxy.x / dTileW) & 2047u;
    uint row = dxy.y & 15u, dword0 = (dxy.x % dTileW) << d64;
    // The same tile in the source layout.
    uint rel = (tile + 2048u - sBase) & 2047u;
    uint2 sTile = uint2(rel % sPitch, rel / sPitch);
    uint sTileW = 80u >> s64;
    uint2 guest = uint2(0, 0);
    [unroll] for (uint k = 0; k < 2u; k++)
    {
        if (k <= d64)
        {
            uint dword = dword0 + k;
            uint2 sxy = uint2(sTile.x * sTileW + (dword >> s64), sTile.y * 16u + row);
            uint2 sx = uint2(sMsaa >= 2u ? 1u : 0u, sMsaa >= 1u ? 1u : 0u);
            uint ss = HostSample(sMsaa, sxy & sx);
            uint2 shp = (sxy >> sx) * scale + sub;
            uint2 w = SourceGuest(shp, ss, sFormat);
            uint word = (dword & s64) != 0u ? w.y : w.x;
            if (k == 0u)
                guest.x = word;
            else
                guest.y = word;
        }
    }
    return guest;
}

#ifndef TRANSFER_TEST
Out ps(float4 pos : SV_Position
#if DEST_SAMPLES > 1
    , uint sampleIndex : SV_SampleIndex
#endif
    )
{
#if DEST_SAMPLES == 1
    uint sampleIndex = 0;
#endif
    uint2 guest = GuestAt(uint2(pos.xy), sampleIndex);
    uint dFormat = (destInfo >> 26) & 15u;
    Out o;
#if DEST_DEPTH
    o.depth = GuestToDepth(dFormat, guest.x);
#if STENCIL == 1
    o.stencil = guest.x & 0xFFu;
#elif STENCIL == 2
    if (((guest.x >> ((misc >> 8) & 7u)) & 1u) == 0u)
        discard;
#endif
#else
    o.color = GuestToColor(dFormat, guest);
#endif
    return o;
}
#endif
)";
} // namespace edram
