// The title update's Media.zip, with one change: the main menu's X button label. The button opened the
// Xbox Live Marketplace, which never answers here (the game sat on a loading screen); it now asks to quit
// (src/hooks/quit_button.cpp), so IDS_Marketplace in each loadnewprofile_Flow.str reads "Quit" in that
// language. The string tables are LSB2 files of big-endian UTF-16; the new word is written over the old
// one and padded with NULs, so no offset in the table moves. The patched copy is built once into
// Documents\fm2\cache (again when the update's zip changes) and served for update:\media.zip; members
// other than those tables are copied byte for byte. FM2_NO_QUIT_LABEL=1 serves the original.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kernel/runtime.h"
#include "log.h"
#include "zip.h"

namespace fs = std::filesystem;

namespace
{
// The menu entry's word in each language, and "Quit" in the same language (no longer than it).
struct Word
{
    const char* table;
    const char16_t* marketplace;
    const char16_t* quit;
};
const Word kWords[] = {
    { "en", u"Marketplace", u"Quit" },      { "it", u"Marketplace", u"Esci" },    { "de", u"Marktplatz", u"Beenden" },
    { "fr", u"Marché", u"Sortir" },     { "es", u"Bazar", u"Salir" },         { "jp", u"マーケットプレース", u"終了" },
    { "ko", u"마켓플레이스", u"종료" }, { "chs", u"卖场", u"退出" },
    { "cht", u"賣場", u"離開" }, { "pl", u"Rynek usług", u"Wyjdź" }, { "cz", u"Tržiště", u"Konec" },
    { "hu", u"Piactér", u"Kilépés" },
};

std::vector<uint8_t> Utf16Be(const char16_t* s)
{
    std::vector<uint8_t> out;
    for (; *s; s++)
        out.push_back(uint8_t(*s >> 8)), out.push_back(uint8_t(*s));
    return out;
}

// Replaces the whole string `from` (NUL on both sides, at an even offset) with `to` padded with NULs.
bool PatchTable(std::vector<uint8_t>& t, const char16_t* from, const char16_t* to)
{
    const std::vector<uint8_t> a = Utf16Be(from), b = Utf16Be(to);
    if (b.size() > a.size())
        return false;
    for (size_t i = 2; i + a.size() + 2 <= t.size(); i += 2)
        if (!t[i - 2] && !t[i - 1] && !memcmp(&t[i], a.data(), a.size()) && !t[i + a.size()] && !t[i + a.size() + 1])
        {
            std::fill(t.begin() + ptrdiff_t(i), t.begin() + ptrdiff_t(i + a.size()), uint8_t(0));
            std::copy(b.begin(), b.end(), t.begin() + ptrdiff_t(i));
            return true;
        }
    return false;
}

uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t table[256] = {};
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void Put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); }
void Put32(std::vector<uint8_t>& o, uint32_t v) { Put16(o, v & 0xFFFF), Put16(o, v >> 16); }

// The patched archive, or empty if the source isn't a zip this can rewrite.
std::vector<uint8_t> Rewrite(const std::vector<uint8_t>& zip, int& patched)
{
    patched = 0;
    if (zip.size() < 22)
        return {};
    size_t eocd = zip.size() - 22;
    while (eocd > 0 && U32(&zip[eocd]) != 0x06054B50)
        eocd--;
    if (U32(&zip[eocd]) != 0x06054B50)
        return {};
    const uint32_t count = U16(&zip[eocd + 10]), cdOffset = U32(&zip[eocd + 16]);
    std::vector<uint8_t> out, central;
    size_t p = cdOffset;
    for (uint32_t e = 0; e < count; e++)
    {
        if (p + 46 > zip.size() || U32(&zip[p]) != 0x02014B50)
            return {};
        uint16_t method = U16(&zip[p + 10]);
        const uint16_t time = U16(&zip[p + 12]), date = U16(&zip[p + 14]);
        uint32_t crc = U32(&zip[p + 16]), csize = U32(&zip[p + 20]), usize = U32(&zip[p + 24]);
        const uint16_t nameLen = U16(&zip[p + 28]), extraLen = U16(&zip[p + 30]), commentLen = U16(&zip[p + 32]);
        const uint32_t local = U32(&zip[p + 42]);
        const std::string name(reinterpret_cast<const char*>(&zip[p + 46]), nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (local + 30 > zip.size() || U32(&zip[local]) != 0x04034B50)
            return {};
        const size_t data = local + 30 + U16(&zip[local + 26]) + U16(&zip[local + 28]);
        if (data + csize > zip.size())
            return {};
        std::vector<uint8_t> body(zip.begin() + ptrdiff_t(data), zip.begin() + ptrdiff_t(data + csize));
        // StringTables/<LANG>/loadnewprofile_Flow.str: inflated, patched, stored.
        std::string lower = name;
        for (char& c : lower)
            c = char(tolower(uint8_t(c)));
        if (lower.size() > 13 && lower.rfind("stringtables/", 0) == 0 && lower.find("/loadnewprofile_flow.str") != std::string::npos)
        {
            const std::string lang = lower.substr(13, lower.find('/', 13) - 13);
            std::vector<uint8_t> table;
            bool ok = false;
            ForEachZipMember(zip, lower.c_str(), [&](const std::string& n, const std::vector<uint8_t>& d) {
                std::string l = n;
                for (char& c : l)
                    c = char(tolower(uint8_t(c)));
                if (l == lower)
                    table = d, ok = true;
            });
            for (const Word& w : kWords)
                if (ok && lang == w.table && PatchTable(table, w.marketplace, w.quit))
                {
                    body = table;
                    method = 0;
                    csize = usize = uint32_t(table.size());
                    crc = Crc32(table.data(), table.size());
                    patched++;
                    break;
                }
        }
        const uint32_t offset = uint32_t(out.size());
        Put32(out, 0x04034B50), Put16(out, 20), Put16(out, 0), Put16(out, method), Put16(out, time), Put16(out, date);
        Put32(out, crc), Put32(out, csize), Put32(out, usize), Put16(out, uint32_t(name.size())), Put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body.begin(), body.end());
        Put32(central, 0x02014B50), Put16(central, 20), Put16(central, 20), Put16(central, 0), Put16(central, method);
        Put16(central, time), Put16(central, date), Put32(central, crc), Put32(central, csize), Put32(central, usize);
        Put16(central, uint32_t(name.size())), Put16(central, 0), Put16(central, 0), Put16(central, 0), Put16(central, 0);
        Put32(central, 0), Put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
    }
    const uint32_t cdStart = uint32_t(out.size());
    out.insert(out.end(), central.begin(), central.end());
    Put32(out, 0x06054B50), Put16(out, 0), Put16(out, 0), Put16(out, count), Put16(out, count);
    Put32(out, uint32_t(central.size())), Put32(out, cdStart), Put16(out, 0);
    return out;
}
}

// update:\media.zip -> the patched copy (built on first use), or the original if that fails.
fs::path UpdateMediaZip(const fs::path& original)
{
    static const fs::path result = [&]() -> fs::path {
        if (getenv("FM2_NO_QUIT_LABEL"))
            return original;
        std::error_code ec;
        const auto size = fs::file_size(original, ec);
        const auto stamp = ec ? 0 : fs::last_write_time(original, ec).time_since_epoch().count();
        const fs::path dir = UserDir() / "cache";
        const fs::path patched = dir / "update-media-quit.zip", stampFile = dir / "update-media-quit.stamp";
        const std::string want = std::to_string(size) + " " + std::to_string(stamp) + " 1";
        {
            std::ifstream in(stampFile);
            std::string have;
            std::getline(in, have);
            if (have == want && fs::exists(patched, ec))
                return patched;
        }
        std::ifstream in(original, std::ios::binary);
        std::vector<uint8_t> zip{ std::istreambuf_iterator<char>(in), {} };
        int count = 0;
        const std::vector<uint8_t> out = Rewrite(zip, count);
        if (out.empty() || !count)
        {
            Log("update media: no Marketplace label found to replace; serving the original");
            return original;
        }
        fs::create_directories(dir, ec);
        std::ofstream(patched, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        std::ofstream(stampFile) << want << "\n";
        Log("update media: X on the main menu reads Quit (%d string tables), %s", count, patched.string().c_str());
        return patched;
    }();
    return result;
}
