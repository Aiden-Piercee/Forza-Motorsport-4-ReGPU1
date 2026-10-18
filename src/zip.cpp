// Reading members of the game's zip archives (stored or deflated).
#include "zip.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

namespace
{
// ---- Inflate (raw deflate, RFC 1951) ---------------------------------------------

struct Huffman
{
    uint16_t count[16];
    uint16_t symbol[320];
};

struct Inflater
{
    const uint8_t* in;
    size_t inSize, inPos = 0;
    uint32_t bitBuf = 0, bitCount = 0;
    std::vector<uint8_t>& out;
    bool error = false;

    Inflater(const uint8_t* data, size_t size, std::vector<uint8_t>& o) : in(data), inSize(size), out(o) {}

    uint32_t Bits(uint32_t n)
    {
        while (bitCount < n)
        {
            if (inPos >= inSize)
            {
                error = true;
                return 0;
            }
            bitBuf |= uint32_t(in[inPos++]) << bitCount;
            bitCount += 8;
        }
        const uint32_t v = bitBuf & ((1u << n) - 1);
        bitBuf >>= n;
        bitCount -= n;
        return v;
    }

    int Decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++)
        {
            code |= int(Bits(1));
            if (error)
                return -1;
            const int count = h.count[len];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        error = true;
        return -1;
    }

    static bool Build(Huffman& h, const uint8_t* lengths, int n)
    {
        memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; i++)
            h.count[lengths[i]]++;
        h.count[0] = 0;
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++)
            offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i])
                h.symbol[offs[lengths[i]]++] = uint16_t(i);
        return true;
    }

    bool Codes(const Huffman& lit, const Huffman& dist)
    {
        static const uint16_t lbase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const uint8_t lext[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const uint16_t dbase[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
            8193, 12289, 16385, 24577 };
        static const uint8_t dext[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;)
        {
            int sym = Decode(lit);
            if (sym < 0)
                return false;
            if (sym < 256)
                out.push_back(uint8_t(sym));
            else if (sym == 256)
                return true;
            else
            {
                sym -= 257;
                if (sym >= 29)
                    return false;
                const uint32_t len = lbase[sym] + Bits(lext[sym]);
                const int ds = Decode(dist);
                if (ds < 0 || ds >= 30)
                    return false;
                const uint32_t d = dbase[ds] + Bits(dext[ds]);
                if (error || d > out.size())
                    return false;
                const size_t from = out.size() - d;
                for (uint32_t i = 0; i < len; i++)
                    out.push_back(out[from + i]);
            }
        }
    }

    bool Run()
    {
        bool last;
        do
        {
            last = Bits(1);
            const uint32_t type = Bits(2);
            if (error)
                return false;
            if (type == 0)
            {
                bitBuf = 0;
                bitCount = 0;
                if (inPos + 4 > inSize)
                    return false;
                const uint32_t len = in[inPos] | (in[inPos + 1] << 8);
                inPos += 4;
                if (inPos + len > inSize)
                    return false;
                out.insert(out.end(), in + inPos, in + inPos + len);
                inPos += len;
            }
            else if (type == 1)
            {
                static Huffman fixedLit, fixedDist;
                static std::once_flag once;
                std::call_once(once, [] {
                    uint8_t l[288];
                    int i = 0;
                    for (; i < 144; i++) l[i] = 8;
                    for (; i < 256; i++) l[i] = 9;
                    for (; i < 280; i++) l[i] = 7;
                    for (; i < 288; i++) l[i] = 8;
                    Build(fixedLit, l, 288);
                    uint8_t d[30];
                    std::fill(d, d + 30, uint8_t(5));
                    Build(fixedDist, d, 30);
                });
                if (!Codes(fixedLit, fixedDist))
                    return false;
            }
            else if (type == 2)
            {
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                const uint32_t nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (error || nlen > 286 || ndist > 30)
                    return false;
                uint8_t lengths[320] = {};
                for (uint32_t i = 0; i < ncode; i++)
                    lengths[order[i]] = uint8_t(Bits(3));
                Huffman lencode, lit, dist;
                Build(lencode, lengths, 19);
                uint32_t i = 0;
                while (i < nlen + ndist)
                {
                    int sym = Decode(lencode);
                    if (sym < 0)
                        return false;
                    if (sym < 16)
                        lengths[i++] = uint8_t(sym);
                    else
                    {
                        uint8_t len = 0;
                        uint32_t rep;
                        if (sym == 16)
                        {
                            if (i == 0)
                                return false;
                            len = lengths[i - 1];
                            rep = 3 + Bits(2);
                        }
                        else if (sym == 17)
                            rep = 3 + Bits(3);
                        else
                            rep = 11 + Bits(7);
                        if (i + rep > nlen + ndist)
                            return false;
                        while (rep--)
                            lengths[i++] = len;
                    }
                }
                Build(lit, lengths, int(nlen));
                Build(dist, lengths + nlen, int(ndist));
                if (!Codes(lit, dist))
                    return false;
            }
            else
                return false;
        } while (!last);
        return !error;
    }
};

// ---- Zip -------------------------------------------------------------------------

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

}

void ForEachZipMember(const std::vector<uint8_t>& zip, const char* filter, const std::function<void(const std::string&, const std::vector<uint8_t>&)>& fn)
{
    if (zip.size() < 22)
        return;
    size_t eocd = zip.size() - 22;
    while (eocd > 0 && Le32(&zip[eocd]) != 0x06054B50 && zip.size() - eocd < 0x10000 + 22)
        eocd--;
    if (Le32(&zip[eocd]) != 0x06054B50)
        return;
    const uint32_t entries = Le16(&zip[eocd + 10]);
    size_t cd = Le32(&zip[eocd + 16]);
    std::vector<uint8_t> data;
    for (uint32_t e = 0; e < entries && cd + 46 <= zip.size() && Le32(&zip[cd]) == 0x02014B50; e++)
    {
        const uint16_t method = Le16(&zip[cd + 10]);
        const uint32_t csize = Le32(&zip[cd + 20]), usize = Le32(&zip[cd + 24]);
        const uint16_t nameLen = Le16(&zip[cd + 28]), extraLen = Le16(&zip[cd + 30]), commentLen = Le16(&zip[cd + 32]);
        const uint32_t local = Le32(&zip[cd + 42]);
        std::string name(reinterpret_cast<const char*>(&zip[cd + 46]), nameLen);
        cd += 46 + nameLen + extraLen + commentLen;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(tolower(c)); });
        if (lower.find(filter) == std::string::npos || local + 30 > zip.size())
            continue;
        const size_t start = local + 30 + Le16(&zip[local + 26]) + Le16(&zip[local + 28]);
        if (start + csize > zip.size())
            continue;
        data.clear();
        if (method == 0)
            data.assign(zip.begin() + start, zip.begin() + start + csize);
        else if (method == 8)
        {
            data.reserve(usize);
            if (!Inflater(&zip[start], csize, data).Run() || data.size() != usize)
                continue;
        }
        else
            continue;
        fn(name, data);
    }
}

bool ReadZipMember(const std::filesystem::path& zipPath, const std::string& member, std::vector<uint8_t>& out)
{
    std::ifstream in(zipPath, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    std::vector<uint8_t> zip(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(zip.data()), std::streamsize(zip.size()));
    std::string want = member;
    std::replace(want.begin(), want.end(), char(92), '/');
    std::transform(want.begin(), want.end(), want.begin(), [](char c) { return char(tolower(c)); });
    bool found = false;
    ForEachZipMember(zip, want.c_str(), [&](const std::string& name, const std::vector<uint8_t>& data) {
        if (!found && name.size() == want.size())
        {
            out = data;
            found = true;
        }
    });
    return found;
}
