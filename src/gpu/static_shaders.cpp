// Finds every shader in the game data before the game asks for one.
//
// The title keeps shader microcode raw inside its .rmb.bin mesh files (deflated in the
// track, car and UI zips), 16-byte aligned and with no packet header, plus a few dozen
// in the executable. A candidate is a shader when its control flow decodes cleanly:
// valid opcodes up to an end flag, the first exec address equal to the control-flow
// length, and exec ranges that tile the instruction slots without gaps. Only the used
// slots are kept (the loaded copy has a per-load stamp after them; see
// ucode::UsedDwords). The result is saved so later boots skip the scan.
#include "gpu/static_shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "kernel/runtime.h"
#include "log.h"
#include "memory.h"

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

// Calls fn(name, data) for each member whose name contains `filter` (stored or deflated).
template <typename Fn> void ForEachZipMember(const std::vector<uint8_t>& zip, const char* filter, Fn&& fn)
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

// ---- Finder ----------------------------------------------------------------------

bool IsExecOp(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
bool IsEndOp(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }

uint32_t Be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Used length in dwords of the shader at words[start] (0 if it isn't one); sets pixel.
uint32_t ShaderAt(const uint8_t* bytes, size_t words, size_t start, bool& pixel)
{
    struct Cf { uint32_t op, lo, hi; };
    Cf cfs[130];
    uint32_t ncf = 0, triples = 0;
    for (bool ended = false; !ended;)
    {
        if (triples > 64 || start + triples * 3 + 2 >= words)
            return 0;
        const uint8_t* p = bytes + (start + triples * 3) * 4;
        const uint32_t w0 = Be(p), w1 = Be(p + 4), w2 = Be(p + 8);
        const uint32_t pair[2][2] = { { w0, w1 & 0xFFFF }, { (w1 >> 16) | (w2 << 16), w2 >> 16 } };
        triples++;
        for (const auto& c : pair)
        {
            const uint32_t op = (c[1] >> 12) & 0xF;
            cfs[ncf++] = { op, c[0], c[1] };
            if (IsEndOp(op))
            {
                ended = true;
                break;
            }
        }
    }
    struct Exec { uint32_t addr, count, seq; };
    Exec execs[130];
    uint32_t nexec = 0;
    for (uint32_t i = 0; i < ncf; i++)
    {
        const Cf& c = cfs[i];
        if (c.op == 12 && ((c.lo & 0xFFFFFFF8) || (c.hi & 0x09FF)))
            return 0; // alloc with stray bits
        if (IsExecOp(c.op) && ((c.lo >> 12) & 7))
            execs[nexec++] = { c.lo & 0xFFF, (c.lo >> 12) & 7, (c.lo >> 16) & 0xFFF };
    }
    if (!nexec)
        return 0;
    std::sort(execs, execs + nexec, [](const Exec& a, const Exec& b) { return a.addr < b.addr; });
    uint32_t slot = triples;
    for (uint32_t i = 0; i < nexec; i++)
    {
        if (execs[i].addr != slot)
            return 0;
        slot += execs[i].count;
    }
    if (slot > 512 || start + slot * 3 > words)
        return 0;
    bool vertex = false;
    for (uint32_t i = 0; i < nexec && !vertex; i++)
        for (uint32_t k = 0; k < execs[i].count; k++)
            if ((execs[i].seq >> (2 * k)) & 1)
                if ((Be(bytes + (start + (execs[i].addr + k) * 3) * 4) & 0x1F) == 0)
                    vertex = true;
    bool position = false;
    for (uint32_t i = 0; i < ncf; i++)
        if (cfs[i].op == 12 && ((cfs[i].hi >> 9) & 3) == 1)
            position = true;
    pixel = !vertex && !position;
    return slot * 3;
}

uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct Found
{
    std::mutex mutex;
    std::unordered_set<uint64_t> seen;
    std::vector<ScannedShader> shaders;
};

void ScanBytes(const uint8_t* bytes, size_t size, Found& found)
{
    const size_t words = size / 4;
    for (size_t i = 0; i + 4 <= words; i += 4)
    {
        const uint32_t w0 = Be(bytes + i * 4), w1 = Be(bytes + i * 4 + 4);
        const uint32_t op = (w1 >> 12) & 0xF, addr = w0 & 0xFFF;
        if (!((IsExecOp(op) && addr >= 1 && addr <= 64) || (op == 12 && !(w0 & 0xFFFFFFF8))))
            continue;
        bool pixel;
        const uint32_t used = ShaderAt(bytes, words, i, pixel);
        if (!used)
            continue;
        const uint8_t* code = bytes + i * 4;
        const uint64_t hash = Fnv(code, used * 4) ^ (pixel ? 1 : 0);
        std::lock_guard lock(found.mutex);
        if (!found.seen.insert(hash).second)
            continue;
        ScannedShader s{ pixel, std::vector<uint32_t>(used) };
        memcpy(s.code.data(), code, used * 4);
        found.shaders.push_back(std::move(s));
    }
}

constexpr uint32_t kFileMagic = 0x53534D46; // 'FMSS'
constexpr uint32_t kFileVersion = 1;

std::filesystem::path SavedPath() { return UserDir() / "static_shaders.bin"; }

bool LoadSaved(std::vector<ScannedShader>& out)
{
    std::ifstream in(SavedPath(), std::ios::binary);
    uint32_t head[3];
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != kFileMagic || head[1] != kFileVersion)
        return false;
    out.resize(head[2]);
    for (auto& s : out)
    {
        uint32_t meta[2];
        if (!in.read(reinterpret_cast<char*>(meta), sizeof(meta)) || meta[1] > 0x2000)
            return false;
        s.pixel = meta[0] != 0;
        s.code.resize(meta[1]);
        if (!in.read(reinterpret_cast<char*>(s.code.data()), std::streamsize(meta[1] * 4)))
            return false;
    }
    return true;
}

void Save(const std::vector<ScannedShader>& shaders)
{
    std::ofstream out(SavedPath(), std::ios::binary);
    const uint32_t head[3] = { kFileMagic, kFileVersion, uint32_t(shaders.size()) };
    out.write(reinterpret_cast<const char*>(head), sizeof(head));
    for (const auto& s : shaders)
    {
        const uint32_t meta[2] = { s.pixel ? 1u : 0u, uint32_t(s.code.size()) };
        out.write(reinterpret_cast<const char*>(meta), sizeof(meta));
        out.write(reinterpret_cast<const char*>(s.code.data()), std::streamsize(s.code.size() * 4));
    }
}
}

std::vector<ScannedShader> StaticShaders()
{
    std::vector<ScannedShader> result;
    if (LoadSaved(result))
    {
        Log("static shaders: %zu loaded from %s", result.size(), SavedPath().string().c_str());
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(GameDir(), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(tolower(c)); });
        if (ext == ".zip" || ext == ".bin")
            files.push_back(it->path());
    }
    Found found;
    std::atomic<size_t> next{ 0 };
    std::vector<std::thread> workers;
    const unsigned n = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < n; t++)
        workers.emplace_back([&] {
            for (size_t i; (i = next++) < files.size();)
            {
                std::ifstream in(files[i], std::ios::binary | std::ios::ate);
                if (!in)
                    continue;
                std::vector<uint8_t> data(size_t(in.tellg()));
                in.seekg(0);
                in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
                if (files[i].extension() == ".zip" || files[i].extension() == ".ZIP")
                    ForEachZipMember(data, ".rmb", [&](const std::string&, const std::vector<uint8_t>& member) { ScanBytes(member.data(), member.size(), found); });
                else
                    ScanBytes(data.data(), data.size(), found);
            }
        });
    for (auto& w : workers)
        w.join();
    const size_t fromFiles = found.shaders.size();
    // Shaders built into the executable (it is compressed on disc; scan the loaded image).
    ScanBytes(GuestPtr<uint8_t>(0x82000000), 0x2000000, found);
    result = std::move(found.shaders);
    Save(result);
    Log("static shaders: %zu found (%zu in %zu data files, %zu in the executable) in %.1f s", result.size(), fromFiles, files.size(),
        result.size() - fromFiles, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return result;
}
