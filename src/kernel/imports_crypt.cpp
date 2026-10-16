// XeCryptSha*: SHA-1 on a guest XECRYPT_SHA_STATE
// { u32 count, u32 state[5], u8 buffer[64] }.
#include "kernel/imports.h"

namespace
{
uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t(block[i * 4]) << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

struct Sha
{
    uint32_t count = 0;
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buffer[64] = {};

    void Load(uint32_t g)
    {
        count = LoadBE32(g);
        for (int i = 0; i < 5; i++)
            state[i] = LoadBE32(g + 4 + i * 4);
        memcpy(buffer, GuestPtr(g + 0x18), 64);
    }
    void Store(uint32_t g) const
    {
        StoreBE32(g, count);
        for (int i = 0; i < 5; i++)
            StoreBE32(g + 4 + i * 4, state[i]);
        memcpy(GuestPtr(g + 0x18), buffer, 64);
    }
    void Update(const uint8_t* p, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            buffer[count % 64] = p[i];
            if (++count % 64 == 0)
                Transform(state, buffer);
        }
    }
    void Final(uint8_t out[20])
    {
        const uint64_t bits = uint64_t(count) * 8;
        const uint8_t pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (count % 64 != 56)
            Update(&zero, 1);
        for (int i = 7; i >= 0; i--)
        {
            const uint8_t b = uint8_t(bits >> (i * 8));
            Update(&b, 1);
        }
        for (int i = 0; i < 20; i++)
            out[i] = uint8_t(state[i / 4] >> (24 - (i % 4) * 8));
    }
};
}

GUEST_EXPORT(XeCryptShaInit)
{
    Sha().Store(ctx.r3.u32);
}

GUEST_EXPORT(XeCryptShaUpdate)
{
    Sha s;
    s.Load(ctx.r3.u32);
    s.Update(GuestPtr<uint8_t>(ctx.r4.u32), ctx.r5.u32);
    s.Store(ctx.r3.u32);
}

GUEST_EXPORT(XeCryptShaFinal)
{
    // (state, out, outSize)
    Sha s;
    s.Load(ctx.r3.u32);
    uint8_t digest[20];
    s.Final(digest);
    memcpy(GuestPtr(ctx.r4.u32), digest, std::min<uint32_t>(ctx.r5.u32, 20));
}

GUEST_EXPORT(XeCryptSha)
{
    // (in1, len1, in2, len2, in3, len3, out, outSize)
    Sha s;
    const uint32_t in[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (auto& p : in)
    {
        if (p[0] && p[1])
            s.Update(GuestPtr<uint8_t>(p[0]), p[1]);
    }
    uint8_t digest[20];
    s.Final(digest);
    memcpy(GuestPtr(ctx.r9.u32), digest, std::min<uint32_t>(ctx.r10.u32, 20));
}
