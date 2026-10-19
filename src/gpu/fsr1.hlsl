// FSR 1 (AMD FidelityFX Super Resolution 1.0, MIT): EASU upscale and RCAS sharpen as
// compute shaders. Flattened with the FidelityFX headers into fsr1_hlsl.h at build time
// (tools/embed_hlsl.py) and compiled at startup.
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#include "ffx_core.h"

cbuffer Constants : register(b0)
{
    uint4 con0, con1, con2, con3; // EASU
    uint4 rcasCon;                // RCAS
    uint4 outSize;                // xy output size
};
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState linearClamp : register(s0);

#define FFX_FSR_EASU_FLOAT 1
FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { return src.GatherRed(linearClamp, p, int2(0, 0)); }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { return src.GatherGreen(linearClamp, p, int2(0, 0)); }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { return src.GatherBlue(linearClamp, p, int2(0, 0)); }

#define FSR_RCAS_F 1
FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) { return src.Load(int3(p, 0)); }
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "ffx_fsr1.h"

void EasuPixel(uint2 p)
{
    if (any(p >= outSize.xy))
        return;
    FfxFloat32x3 c;
    ffxFsrEasuFloat(c, p, con0, con1, con2, con3);
    dst[p] = float4(c, 1);
}

void RcasPixel(uint2 p)
{
    if (any(p >= outSize.xy))
        return;
    FfxFloat32 r, g, b;
    FsrRcasF(r, g, b, p, rcasCon);
    dst[p] = float4(r, g, b, 1);
}

// 64 threads, each a 2x2 quad of a 16x16 tile (the layout FSR 1 is written for).
[numthreads(64, 1, 1)]
void EasuMain(uint3 lid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint2 p = ffxRemapForQuad(lid.x) + uint2(gid.x << 4, gid.y << 4);
    EasuPixel(p);
    p.x += 8u;
    EasuPixel(p);
    p.y += 8u;
    EasuPixel(p);
    p.x -= 8u;
    EasuPixel(p);
}

[numthreads(64, 1, 1)]
void RcasMain(uint3 lid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint2 p = ffxRemapForQuad(lid.x) + uint2(gid.x << 4, gid.y << 4);
    RcasPixel(p);
    p.x += 8u;
    RcasPixel(p);
    p.y += 8u;
    RcasPixel(p);
    p.x -= 8u;
    RcasPixel(p);
}
