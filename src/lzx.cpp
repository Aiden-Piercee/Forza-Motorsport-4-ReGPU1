// LZX as the CAB format defines it (block types, pretree-coded lengths, R0-R2 repeat
// offsets, E8 translation), fed one Xbox chunk at a time. Tree lengths, repeat offsets and
// the current block carry over from chunk to chunk; the bit reader restarts at each one.
#include "lzx.h"

#include <cstring>

namespace
{
constexpr int kMainTreeMax = 256 + 50 * 8, kLengthTree = 249, kPretree = 20, kAligned = 8;

struct Tree
{
    uint16_t count[17];
    uint16_t symbol[kMainTreeMax];

    bool Build(const uint8_t* lens, int n)
    {
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++)
            count[lens[i]]++;
        count[0] = 0;
        uint16_t offs[17] = {};
        for (int l = 1; l < 16; l++)
            offs[l + 1] = uint16_t(offs[l] + count[l]);
        for (int i = 0; i < n; i++)
            if (lens[i])
                symbol[offs[lens[i]]++] = uint16_t(i);
        return true;
    }
};

struct Bits
{
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf = 0;
    int left = 0;

    void Ensure(int n)
    {
        while (left < n)
        {
            const uint32_t w = p + 1 < end ? uint32_t(p[0] | (p[1] << 8)) : 0;
            p += 2;
            buf |= w << (16 - left);
            left += 16;
        }
    }
    uint32_t Read(int n)
    {
        if (!n)
            return 0;
        Ensure(n);
        const uint32_t v = buf >> (32 - n);
        buf <<= n;
        left -= n;
        return v;
    }
    int Decode(const Tree& t)
    {
        Ensure(16);
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 16; len++)
        {
            code |= int((buf >> (32 - len)) & 1);
            const int c = t.count[len];
            if (code - first < c)
            {
                buf <<= len;
                left -= len;
                return t.symbol[index + code - first];
            }
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

struct Decoder
{
    int slots;
    uint8_t mainLen[kMainTreeMax] = {}, lengthLen[kLengthTree] = {}, alignedLen[kAligned] = {};
    Tree mainTree, lengthTree, alignedTree;
    uint32_t r0 = 1, r1 = 1, r2 = 1;
    int blockType = 0;
    uint32_t blockRemaining = 0, blockLength = 0;
    bool headerRead = false, intelStarted = false;
    uint32_t intelSize = 0;
    uint32_t extra[50], base[50];

    explicit Decoder(int windowBits)
    {
        static const int kSlots[] = { 30, 32, 34, 36, 38, 42, 50 };
        slots = kSlots[windowBits - 15];
        uint32_t b = 0;
        for (int i = 0; i < 50; i++)
        {
            extra[i] = i < 4 ? 0 : (i / 2 - 1 > 17 ? 17 : i / 2 - 1);
            base[i] = b;
            b += 1u << extra[i];
        }
    }

    bool ReadLengths(Bits& bits, uint8_t* lens, int first, int last)
    {
        uint8_t pre[kPretree];
        for (int i = 0; i < kPretree; i++)
            pre[i] = uint8_t(bits.Read(4));
        Tree t;
        t.Build(pre, kPretree);
        for (int x = first; x < last;)
        {
            int z = bits.Decode(t);
            if (z < 0)
                return false;
            if (z == 17 || z == 18)
            {
                int run = z == 17 ? int(bits.Read(4)) + 4 : int(bits.Read(5)) + 20;
                while (run-- && x < last)
                    lens[x++] = 0;
            }
            else if (z == 19)
            {
                int run = int(bits.Read(1)) + 4;
                z = bits.Decode(t);
                if (z < 0 || z > 16)
                    return false;
                const uint8_t v = uint8_t((lens[x] - z + 17) % 17);
                while (run-- && x < last)
                    lens[x++] = v;
            }
            else
            {
                lens[x] = uint8_t((lens[x] - z + 17) % 17);
                x++;
            }
        }
        return true;
    }

    // One chunk: `frameSize` more bytes of output from `bits`, which holds this chunk only.
    bool Frame(Bits& bits, std::vector<uint8_t>& out, size_t& pos, size_t frameSize)
    {
        const size_t frameStart = pos, frameEnd = pos + frameSize;
        if (!headerRead)
        {
            headerRead = true;
            if (bits.Read(1))
            {
                const uint32_t hi = bits.Read(16);
                intelSize = (hi << 16) | bits.Read(16);
            }
        }
        while (pos < frameEnd)
        {
            if (blockRemaining == 0)
            {
                if (blockType == 3)
                {
                    if (blockLength & 1)
                        bits.p++;
                    bits.buf = 0;
                    bits.left = 0;
                }
                blockType = int(bits.Read(3));
                const uint32_t hi = bits.Read(16);
                blockRemaining = blockLength = (hi << 8) | bits.Read(8);
                if (blockType == 2)
                {
                    for (int i = 0; i < kAligned; i++)
                        alignedLen[i] = uint8_t(bits.Read(3));
                    alignedTree.Build(alignedLen, kAligned);
                }
                if (blockType == 1 || blockType == 2)
                {
                    if (!ReadLengths(bits, mainLen, 0, 256) || !ReadLengths(bits, mainLen, 256, 256 + slots * 8))
                        return false;
                    mainTree.Build(mainLen, 256 + slots * 8);
                    if (mainLen[0xE8])
                        intelStarted = true;
                    if (!ReadLengths(bits, lengthLen, 0, kLengthTree))
                        return false;
                    lengthTree.Build(lengthLen, kLengthTree);
                }
                else if (blockType == 3)
                {
                    intelStarted = true;
                    bits.Ensure(16);
                    if (bits.left > 16)
                        bits.p -= 2;
                    bits.buf = 0;
                    bits.left = 0;
                    if (bits.p + 12 > bits.end)
                        return false;
                    uint32_t r[3];
                    memcpy(r, bits.p, 12);
                    r0 = r[0], r1 = r[1], r2 = r[2];
                    bits.p += 12;
                }
                else
                    return false;
            }

            if (blockType == 3)
            {
                size_t n = blockRemaining;
                if (n > frameEnd - pos)
                    n = frameEnd - pos;
                if (bits.p + n > bits.end)
                    return false;
                memcpy(&out[pos], bits.p, n);
                bits.p += n;
                pos += n;
                blockRemaining -= uint32_t(n);
                continue;
            }

            while (blockRemaining > 0 && pos < frameEnd)
            {
                const int sym = bits.Decode(mainTree);
                if (sym < 0)
                    return false;
                if (sym < 256)
                {
                    out[pos++] = uint8_t(sym);
                    blockRemaining--;
                    continue;
                }
                const int s = sym - 256;
                uint32_t length = s & 7;
                if (length == 7)
                {
                    const int l = bits.Decode(lengthTree);
                    if (l < 0)
                        return false;
                    length += uint32_t(l);
                }
                length += 2;
                const int slot = s >> 3;
                uint32_t offset;
                if (slot == 0)
                    offset = r0;
                else if (slot == 1)
                {
                    offset = r1;
                    r1 = r0;
                    r0 = offset;
                }
                else if (slot == 2)
                {
                    offset = r2;
                    r2 = r0;
                    r0 = offset;
                }
                else
                {
                    uint32_t e = extra[slot];
                    offset = base[slot] - 2;
                    if (blockType == 2 && e >= 3)
                    {
                        offset += bits.Read(int(e - 3)) << 3;
                        const int a = bits.Decode(alignedTree);
                        if (a < 0)
                            return false;
                        offset += uint32_t(a);
                    }
                    else if (e > 0)
                        offset += bits.Read(int(e));
                    else
                        offset = 1;
                    r2 = r1;
                    r1 = r0;
                    r0 = offset;
                }
                if (offset > pos || pos + length > out.size() || length > blockRemaining)
                    return false;
                for (uint32_t i = 0; i < length; i++, pos++)
                    out[pos] = out[pos - offset];
                blockRemaining -= length;
            }
        }

        // E8 translation (x86 call targets), undone per frame.
        if (intelStarted && intelSize && frameSize > 10 && frameStart < 32768u * 32768u)
        {
            const size_t end = frameStart + frameSize - 10;
            for (size_t i = frameStart; i < end; i++)
            {
                if (out[i] != 0xE8)
                    continue;
                int32_t abs;
                memcpy(&abs, &out[i + 1], 4);
                const int32_t cur = int32_t(i);
                if (abs >= -cur && abs < int32_t(intelSize))
                {
                    const int32_t rel = abs >= 0 ? abs - cur : abs + int32_t(intelSize);
                    memcpy(&out[i + 1], &rel, 4);
                }
                i += 4;
            }
        }
        return true;
    }
};

bool Run(int windowBits, const uint8_t* in, size_t inSize, size_t size, std::vector<uint8_t>& out)
{
    Decoder d(windowBits);
    out.assign(size, 0);
    size_t pos = 0, at = 0;
    while (pos < size)
    {
        if (at + 2 > inSize)
            return false;
        size_t frame = 0x8000, chunk;
        if (in[at] == 0xFF)
        {
            if (at + 5 > inSize)
                return false;
            frame = (in[at + 1] << 8) | in[at + 2];
            chunk = (in[at + 3] << 8) | in[at + 4];
            at += 5;
        }
        else
        {
            chunk = (in[at] << 8) | in[at + 1];
            at += 2;
        }
        if (!chunk || at + chunk > inSize)
            return false;
        if (frame > size - pos)
            frame = size - pos;
        Bits bits{ in + at, in + at + chunk };
        if (!d.Frame(bits, out, pos, frame))
            return false;
        at += chunk;
    }
    return pos == size;
}
}

bool LzxDecompress(const uint8_t* in, size_t inSize, size_t size, std::vector<uint8_t>& out)
{
    // XMemCompress's default window is 128 KB; the others are tried if that one fails.
    for (int windowBits : { 17, 16, 15, 18, 19, 20, 21 })
        if (Run(windowBits, in, inSize, size, out))
            return true;
    out.clear();
    return false;
}
