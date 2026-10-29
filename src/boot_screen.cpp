// The first-boot screen: while every game shader compiles, show the game's own loading
// background and logo with a progress bar instead of a window that looks hung (FM2's screen,
// with FM4's art). The art is decoded from the player's UI/Textures.zip (.xds: a 52-byte header
// ending in the texture fetch constant, then the tiled texture data), so nothing is bundled.
#include "boot_screen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

#include <windows.h>

#include "regpu/regpu.h"

#include "kernel/runtime.h"
#include "log.h"
#include "zip.h"

namespace
{
constexpr uint32_t kW = 1280, kH = 720;
constexpr const char* kBackground = "ui4/common/bg_splash.xds"; // the menus' world-map backdrop
constexpr const char* kLogo = "ui4/common/forza_logo3.xds";      // the only logo with alpha (Logo1/2/5 are boxed)

struct Image
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Decodes the named members (lowercase, all under ui4/common/) of Textures.zip, which is 435 MB:
// only its directory and those members are read.
std::map<std::string, Image> LoadXds(const std::vector<std::string>& names)
{
    std::map<std::string, Image> out;
    ForEachZipMemberInFile(GameDir() / "Media" / "UI" / "Textures.zip", "ui4/common/", [&](const std::string& name, const std::vector<uint8_t>& file) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(tolower(c)); });
        if (std::find(names.begin(), names.end(), lower) == names.end() || file.size() < 52)
            return;
        uint32_t fetch[6];
        for (int i = 0; i < 6; i++)
            fetch[i] = Be32(&file[28 + i * 4]);
        Image img;
        if (regpu::DecodeTexture(fetch, file.data() + 52, file.size() - 52, img.w, img.h, img.rgba))
            out[lower] = std::move(img);
    });
    return out;
}

struct Assets
{
    std::once_flag once;
    bool ok = false;
    Image background, logo;
} g_assets;

bool EnsureAssets()
{
    std::call_once(g_assets.once, [] {
        auto images = LoadXds({ kBackground, kLogo });
        g_assets.background = std::move(images[kBackground]);
        g_assets.logo = std::move(images[kLogo]);
        g_assets.ok = g_assets.background.w == kW && g_assets.background.h == kH;
        if (!g_assets.ok)
            Log("boot screen: %s not found or not 1280x720; using the plain overlay", kBackground);
    });
    return g_assets.ok;
}

// Draws src scaled to (dw x dh) at (dx, dy), alpha blended, bilinear.
void Blit(std::vector<uint8_t>& dst, const Image& src, int dx, int dy, int dw, int dh, float opacity = 1.0f)
{
    for (int y = std::max(0, dy); y < std::min(int(kH), dy + dh); y++)
        for (int x = std::max(0, dx); x < std::min(int(kW), dx + dw); x++)
        {
            const float u = (x - dx + 0.5f) * src.w / dw - 0.5f, v = (y - dy + 0.5f) * src.h / dh - 0.5f;
            const int x0 = std::clamp(int(std::floor(u)), 0, int(src.w) - 1), y0 = std::clamp(int(std::floor(v)), 0, int(src.h) - 1);
            const int x1 = std::min(x0 + 1, int(src.w) - 1), y1 = std::min(y0 + 1, int(src.h) - 1);
            const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
            float c[4];
            for (int k = 0; k < 4; k++)
            {
                auto at = [&](int sx, int sy) { return float(src.rgba[(size_t(sy) * src.w + sx) * 4 + k]); };
                c[k] = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
            }
            const float a = c[3] / 255.0f * opacity;
            uint8_t* d = &dst[(size_t(y) * kW + x) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - a) + c[k] * a + 0.5f);
        }
}

void FillRect(std::vector<uint8_t>& dst, int x0, int y0, int x1, int y1, const uint8_t rgb[3], float alpha)
{
    for (int y = std::max(0, y0); y < std::min(int(kH), y1); y++)
        for (int x = std::max(0, x0); x < std::min(int(kW), x1); x++)
        {
            uint8_t* d = &dst[(size_t(y) * kW + x) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - alpha) + rgb[k] * alpha + 0.5f);
        }
}

// Text through GDI (grayscale antialiasing, used as coverage). align: 0 left, 1 centre, 2 right of x.
void DrawText(std::vector<uint8_t>& dst, const wchar_t* text, int x, int y, int height, int weight, int align, const uint8_t rgb[3],
    float alpha = 1.0f)
{
    static std::mutex gdiMutex;
    std::lock_guard lock(gdiMutex);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = int(kW);
    bi.bmiHeader.biHeight = -height * 2;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SIZE size{};
    GetTextExtentPoint32W(dc, text, int(wcslen(text)), &size);
    const int left = align == 1 ? x - size.cx / 2 : align == 2 ? x - size.cx : x;
    TextOutW(dc, 0, 0, text, int(wcslen(text)));
    GdiFlush();
    const uint8_t* px = static_cast<const uint8_t*>(bits);
    for (int ty = 0; ty < height * 2; ty++)
        for (int tx = 0; tx < size.cx && tx < int(kW); tx++)
        {
            const int dxp = left + tx, dyp = y + ty;
            if (dxp < 0 || dxp >= int(kW) || dyp < 0 || dyp >= int(kH))
                continue;
            const float a = px[(size_t(ty) * kW + tx) * 4 + 1] / 255.0f * alpha;
            if (a <= 0)
                continue;
            uint8_t* d = &dst[(size_t(dyp) * kW + dxp) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - a) + rgb[k] * a + 0.5f);
        }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}
}

bool BootScreenCompose(const wchar_t* title, uint32_t done, uint32_t total, std::vector<uint8_t>& rgba)
{
    if (!EnsureAssets())
        return false;
    rgba = g_assets.background.rgba;
    static const uint8_t white[3] = { 255, 255, 255 }, grey[3] = { 170, 170, 170 }, black[3] = { 0, 0, 0 };
    // The logo's red (238, 54, 16), from a lighter orange-red on the left; the bar runs left to right.
    static const uint8_t barFrom[3] = { 246, 104, 36 }, barTo[3] = { 238, 54, 16 };

    // Logo, centred, its aspect kept; the whole block sits a little above the middle.
    int y = 176;
    if (!g_assets.logo.rgba.empty())
    {
        const int lw = 440, lh = int(float(lw) * g_assets.logo.h / g_assets.logo.w);
        Blit(rgba, g_assets.logo, int(kW - lw) / 2, y, lw, lh);
        y += lh + 28;
    }
    DrawText(rgba, title, int(kW) / 2, y, 40, FW_SEMIBOLD, 1, white);
    y += 76;

    // Progress bar with the percentage to its right.
    const float progress = total ? std::min(1.0f, float(done) / float(total)) : 0.0f;
    const int bx0 = 240, bx1 = 1000, by0 = y, by1 = y + 30;
    FillRect(rgba, bx0 - 2, by0 - 2, bx1 + 2, by1 + 2, white, 0.85f);
    FillRect(rgba, bx0, by0, bx1, by1, black, 0.75f);
    const int fill = bx0 + int((bx1 - bx0) * progress + 0.5f);
    for (int x = bx0 + 3; x < fill - 3; x++)
    {
        const float t = float(x - bx0) / float(bx1 - bx0);
        const uint8_t c[3] = { uint8_t(barFrom[0] + (barTo[0] - barFrom[0]) * t), uint8_t(barFrom[1] + (barTo[1] - barFrom[1]) * t),
            uint8_t(barFrom[2] + (barTo[2] - barFrom[2]) * t) };
        FillRect(rgba, x, by0 + 3, x + 1, by1 - 3, c, 1.0f);
    }
    wchar_t text[96];
    if (total)
    {
        swprintf(text, 96, L"%d%%", int(progress * 100.0f));
        DrawText(rgba, text, bx1 + 22, by0 - 4, 32, FW_SEMIBOLD, 0, white);
        swprintf(text, 96, L"%u / %u game shaders · first start only", done, total);
    }
    else
        swprintf(text, 96, L"Searching the game files · first start only");
    DrawText(rgba, text, int(kW) / 2, by1 + 18, 22, FW_NORMAL, 1, grey);
    return true;
}

void BootScreenPreview()
{
    const auto dir = std::filesystem::path("out") / "bootscreen";
    std::filesystem::create_directories(dir);
    static const std::vector<std::string> candidates = { "ui4/common/bg_splash.xds", "ui4/common/logo_bkgrnd.xds",
        "ui4/common/forza_text.xds", "ui4/common/loading_background.xds", "ui4/common/forza_logo1.xds", "ui4/common/forza_logo2.xds",
        "ui4/common/forza_logo3.xds", "ui4/common/forza_logo3_gradient.xds", "ui4/common/forza_logo4.xds",
        "ui4/common/forza_logo5.xds", "ui4/common/loading_bar.xds" };
    const auto images = LoadXds(candidates);
    for (const std::string& name : candidates)
    {
        const auto it = images.find(name);
        if (it == images.end())
        {
            Log("boot screen preview: %s not decoded", name.c_str());
            continue;
        }
        regpu::WritePng(dir / (std::filesystem::path(name).stem().string() + ".png"), it->second.w, it->second.h, it->second.rgba);
        Log("boot screen preview: %s %ux%u", name.c_str(), it->second.w, it->second.h);
    }
    std::vector<uint8_t> frame;
    if (BootScreenCompose(L"COMPILING SHADERS", 2731, 4566, frame))
        regpu::WritePng(dir / "composed.png", kW, kH, frame);
}
