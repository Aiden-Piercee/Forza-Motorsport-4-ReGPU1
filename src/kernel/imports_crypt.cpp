// XeCryptSha*: SHA-1 on a guest XECRYPT_SHA_STATE
// { u32 count, u32 state[5], u8 buffer[64] }.
// XeKeys*: the console-bound key functions (below).
#include "kernel/imports.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

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

// ---- XeKeys*: keys bound to the console ------------------------------------------------
// The title update signs and encrypts the profile with keys the console "obscures" with a
// secret in its fuses. This port has no such secret, so it uses a fixed one: what
// XeKeysObscureKey obscures, XeKeysAesCbcUsingKey and XeKeysHmacShaUsingKey accept.
namespace
{
const uint8_t kConsoleSecret[16] = { 'F', 'M', '2', '-', 'p', 'o', 'r', 't', '-', 'c', 'o', 'n', 's', 'o', 'l', 'e' };

bool AesRun(const uint8_t key[16], bool cbc, const uint8_t* iv16, const uint8_t* in, uint8_t* out, uint32_t size, bool encrypt)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
    {
        const wchar_t* mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, PUCHAR(mode), ULONG((wcslen(mode) + 1) * sizeof(wchar_t)), 0) == 0 &&
            BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, PUCHAR(key), 16, 0) == 0)
        {
            uint8_t iv[16] = {};
            if (iv16)
                memcpy(iv, iv16, 16);
            std::vector<uint8_t> input(in, in + size), output(size);
            ULONG got = 0;
            const NTSTATUS status = encrypt ? BCryptEncrypt(handle, input.data(), size, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, output.data(), size, &got, 0)
                                            : BCryptDecrypt(handle, input.data(), size, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, output.data(), size, &got, 0);
            if (status == 0)
            {
                memcpy(out, output.data(), size);
                ok = true;
            }
        }
    }
    if (handle)
        BCryptDestroyKey(handle);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// The key for obscuredKey (16 bytes in guest memory), or false.
bool Unobscure(uint32_t guestKey, uint8_t key[16])
{
    return guestKey && AesRun(kConsoleSecret, false, nullptr, GuestPtr<uint8_t>(guestKey), key, 16, false);
}

void HmacSha1(const uint8_t key[16], const std::vector<std::pair<const uint8_t*, uint32_t>>& parts, uint8_t out[20])
{
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++)
    {
        const uint8_t k = i < 16 ? key[i] : 0;
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5C;
    }
    uint8_t inner[20];
    {
        Sha s;
        s.Update(ipad, 64);
        for (const auto& p : parts)
            if (p.first && p.second)
                s.Update(p.first, p.second);
        s.Final(inner);
    }
    Sha s;
    s.Update(opad, 64);
    s.Update(inner, 20);
    s.Final(out);
}
}

GUEST_EXPORT(XeKeysObscureKey)
{
    // (input[16], output[16]) -> status
    uint8_t out[16];
    if (!ctx.r3.u32 || !ctx.r4.u32 || !AesRun(kConsoleSecret, false, nullptr, GuestPtr<uint8_t>(ctx.r3.u32), out, 16, true))
    {
        Return(ctx, 0xC0000001);
        return;
    }
    memcpy(GuestPtr(ctx.r4.u32), out, 16);
    Return(ctx, 0);
}

GUEST_EXPORT(XeKeysAesCbcUsingKey)
{
    // (obscuredKey[16], input, size, output, iv[16], encrypt) -> status
    uint8_t key[16];
    const uint32_t size = ctx.r5.u32 & ~15u; // whole blocks
    if (!Unobscure(ctx.r3.u32, key) || !ctx.r4.u32 || !ctx.r6.u32 || !ctx.r7.u32)
    {
        Return(ctx, 0xC0000001);
        return;
    }
    std::vector<uint8_t> out(size);
    if (size && !AesRun(key, true, GuestPtr<uint8_t>(ctx.r7.u32), GuestPtr<uint8_t>(ctx.r4.u32), out.data(), size, ctx.r8.u32 != 0))
    {
        Return(ctx, 0xC0000001);
        return;
    }
    if (size)
        memcpy(GuestPtr(ctx.r6.u32), out.data(), size);
    Return(ctx, 0);
}

GUEST_EXPORT(XeKeysHmacShaUsingKey)
{
    // (obscuredKey[16], in1, len1, in2, len2, in3, len3, out, outSize) -> status
    uint8_t key[16];
    if (!Unobscure(ctx.r3.u32, key) || !ctx.r10.u32)
    {
        Return(ctx, 0xC0000001);
        return;
    }
    const uint32_t inputs[3][2] = { { ctx.r4.u32, ctx.r5.u32 }, { ctx.r6.u32, ctx.r7.u32 }, { ctx.r8.u32, ctx.r9.u32 } };
    std::vector<std::pair<const uint8_t*, uint32_t>> parts;
    for (const auto& in : inputs)
        if (in[0] && in[1])
            parts.emplace_back(GuestPtr<uint8_t>(in[0]), in[1]);
    uint8_t digest[20];
    HmacSha1(key, parts, digest);
    memcpy(GuestPtr(ctx.r10.u32), digest, std::min<uint32_t>(StackArg(ctx, 8), 20));
    Return(ctx, 0);
}
