// Test launcher: a small window before the game starts with the player settings
// (settings.h): profile name, resolution, aspect ratio, display mode, vsync and max FPS.
// Play saves them to settings.ini and starts the game; Quit exits. Skipped for scripted
// runs (FM2_TOUR, FM2_HEADLESS) and with FM2_NO_LAUNCHER=1.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "log.h"
#include "settings.h"

namespace
{
enum : int
{
    kProfile = 100, kResolution, kAspect, kDisplay, kVsync, kMaxFps, kUpscaler, kQuality, kPlay, kQuit
};

const std::pair<uint32_t, uint32_t> kAspects[] = { { 0, 0 }, { 16, 9 }, { 21, 9 }, { 32, 9 } };
const wchar_t* kAspectNames[] = { L"Auto (match resolution)", L"16:9", L"21:9", L"32:9" };
const uint32_t kFps[] = { 30, 60, 120, 144, 165, 240, 360 };
// Upscalers offered so far (DLSS and FSR 3 follow), and the quality presets.
const std::pair<Settings::Upscaler, const wchar_t*> kUpscalers[] = { { Settings::Upscaler::Off, L"Off (native)" }, { Settings::Upscaler::Fsr1, L"AMD FSR 1" }, { Settings::Upscaler::Fsr3, L"AMD FSR 3" }, { Settings::Upscaler::Dlss, L"NVIDIA DLSS (DLAA at native)" } };
const wchar_t* kQualities[] = { L"Native (DLAA / sharpen)", L"Quality (67%)", L"Balanced (59%)", L"Performance (50%)", L"Ultra performance (33%)" };

struct State
{
    HWND window = nullptr;
    HWND profile = nullptr, resolution = nullptr, aspect = nullptr, display = nullptr, vsync = nullptr, maxFps = nullptr, upscaler = nullptr,
         quality = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> resolutions;
    bool play = false, done = false;
} g_state;

// Layout in 96-DPI units, scaled to the display.
int S(int v) { return MulDiv(v, int(GetDpiForSystem()), 96); }

std::vector<std::pair<uint32_t, uint32_t>> Resolutions(const Settings& s)
{
    std::set<std::pair<uint32_t, uint32_t>> set{ { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 }, { s.width, s.height } };
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); i++)
        if (mode.dmPelsWidth >= 1280 && mode.dmPelsHeight >= 720)
            set.insert({ mode.dmPelsWidth, mode.dmPelsHeight });
    std::vector<std::pair<uint32_t, uint32_t>> list(set.begin(), set.end());
    std::sort(list.begin(), list.end(), [](auto a, auto b) { return uint64_t(a.first) * a.second < uint64_t(b.first) * b.second; });
    return list;
}

HWND Label(HWND parent, const wchar_t* text, int x, int y, HFONT font)
{
    HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, S(x), S(y + 4), S(110), S(20), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
    return h;
}

HWND Control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, S(x), S(y), S(w), S(h), parent, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
    return c;
}

void Apply()
{
    Settings& s = GetSettings();
    wchar_t name[32] = {};
    GetWindowTextW(g_state.profile, name, 16);
    std::string profile;
    for (const wchar_t* p = name; *p; p++)
        if (*p >= 32 && *p < 127)
            profile += char(*p);
    s.profile = profile.empty() ? "Player" : profile;
    const LRESULT r = SendMessageW(g_state.resolution, CB_GETCURSEL, 0, 0);
    if (r >= 0 && size_t(r) < g_state.resolutions.size())
        std::tie(s.width, s.height) = g_state.resolutions[size_t(r)];
    const LRESULT a = SendMessageW(g_state.aspect, CB_GETCURSEL, 0, 0);
    if (a >= 0 && size_t(a) < std::size(kAspects))
        std::tie(s.aspectW, s.aspectH) = kAspects[size_t(a)];
    s.fullscreen = SendMessageW(g_state.display, CB_GETCURSEL, 0, 0) == 1;
    s.vsync = SendMessageW(g_state.vsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const LRESULT f = SendMessageW(g_state.maxFps, CB_GETCURSEL, 0, 0);
    if (f >= 0 && size_t(f) < std::size(kFps))
        s.maxFps = kFps[size_t(f)];
    const LRESULT u = SendMessageW(g_state.upscaler, CB_GETCURSEL, 0, 0);
    if (u >= 0 && size_t(u) < std::size(kUpscalers))
        s.upscaler = kUpscalers[size_t(u)].first;
    const LRESULT q = SendMessageW(g_state.quality, CB_GETCURSEL, 0, 0);
    if (q >= 0 && q <= 4)
        s.quality = uint32_t(q);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (LOWORD(wp) == kPlay)
        {
            Apply();
            g_state.play = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kQuit)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_state.done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

// Shows the launcher; false if the player quit.
bool RunLauncher()
{
    // Real pixels everywhere (the launcher, the game window and its fullscreen size), not
    // the stretched 96-DPI view Windows gives unaware programs on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (getenv("FM2_NO_LAUNCHER") || getenv("FM2_TOUR") || getenv("FM2_HEADLESS"))
        return true;
    Settings& s = GetSettings();
    g_state.resolutions = Resolutions(s);

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"fm2launcher";
    RegisterClassW(&wc);
    RECT r{ 0, 0, S(380), S(380) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g_state.window = CreateWindowW(L"fm2launcher", L"Forza Motorsport 2 - Launcher", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    HFONT font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    const HWND w = g_state.window;
    const int lx = 20, cx = 140, cw = 220;
    int y = 20;

    Label(w, L"Profile name", lx, y, font);
    std::wstring name(s.profile.begin(), s.profile.end());
    g_state.profile = Control(w, L"EDIT", name.c_str(), WS_BORDER | ES_AUTOHSCROLL, cx, y, cw, 26, kProfile, font);
    SendMessageW(g_state.profile, EM_SETLIMITTEXT, 15, 0); // gamertags are at most 15 characters
    y += 40;

    Label(w, L"Resolution", lx, y, font);
    g_state.resolution = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, cx, y, cw, 300, kResolution, font);
    for (size_t i = 0; i < g_state.resolutions.size(); i++)
    {
        wchar_t t[32];
        swprintf(t, 32, L"%u x %u", g_state.resolutions[i].first, g_state.resolutions[i].second);
        SendMessageW(g_state.resolution, CB_ADDSTRING, 0, LPARAM(t));
        if (g_state.resolutions[i] == std::pair{ s.width, s.height })
            SendMessageW(g_state.resolution, CB_SETCURSEL, i, 0);
    }
    y += 40;

    Label(w, L"Aspect ratio", lx, y, font);
    g_state.aspect = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, cx, y, cw, 200, kAspect, font);
    for (size_t i = 0; i < std::size(kAspects); i++)
    {
        SendMessageW(g_state.aspect, CB_ADDSTRING, 0, LPARAM(kAspectNames[i]));
        if (kAspects[i] == std::pair{ s.aspectW, s.aspectH })
            SendMessageW(g_state.aspect, CB_SETCURSEL, i, 0);
    }
    y += 40;

    Label(w, L"Display mode", lx, y, font);
    g_state.display = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, cx, y, cw, 200, kDisplay, font);
    SendMessageW(g_state.display, CB_ADDSTRING, 0, LPARAM(L"Windowed"));
    SendMessageW(g_state.display, CB_ADDSTRING, 0, LPARAM(L"Fullscreen (borderless)"));
    SendMessageW(g_state.display, CB_SETCURSEL, s.fullscreen ? 1 : 0, 0);
    y += 40;

    Label(w, L"Max FPS", lx, y, font);
    g_state.maxFps = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, cx, y, 100, 250, kMaxFps, font);
    size_t fpsIndex = 1;
    for (size_t i = 0; i < std::size(kFps); i++)
    {
        SendMessageW(g_state.maxFps, CB_ADDSTRING, 0, LPARAM(std::to_wstring(kFps[i]).c_str()));
        if (kFps[i] <= s.maxFps)
            fpsIndex = i;
    }
    SendMessageW(g_state.maxFps, CB_SETCURSEL, fpsIndex, 0);
    g_state.vsync = Control(w, L"BUTTON", L"VSync", BS_AUTOCHECKBOX, cx + 120, y, 100, 26, kVsync, font);
    SendMessageW(g_state.vsync, BM_SETCHECK, s.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 40;

    Label(w, L"Upscaling", lx, y, font);
    g_state.upscaler = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, cx, y, cw, 200, kUpscaler, font);
    for (size_t i = 0; i < std::size(kUpscalers); i++)
    {
        SendMessageW(g_state.upscaler, CB_ADDSTRING, 0, LPARAM(kUpscalers[i].second));
        if (kUpscalers[i].first == s.upscaler)
            SendMessageW(g_state.upscaler, CB_SETCURSEL, i, 0);
    }
    if (SendMessageW(g_state.upscaler, CB_GETCURSEL, 0, 0) < 0)
        SendMessageW(g_state.upscaler, CB_SETCURSEL, 0, 0);
    y += 40;

    Label(w, L"Upscale quality", lx, y, font);
    g_state.quality = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, cx, y, cw, 200, kQuality, font);
    for (const wchar_t* name : kQualities)
        SendMessageW(g_state.quality, CB_ADDSTRING, 0, LPARAM(name));
    SendMessageW(g_state.quality, CB_SETCURSEL, std::min<uint32_t>(s.quality, 4), 0);
    y += 52;

    Control(w, L"BUTTON", L"Play", BS_DEFPUSHBUTTON, cx, y, 105, 32, kPlay, font);
    Control(w, L"BUTTON", L"Quit", BS_PUSHBUTTON, cx + 115, y, 105, 32, kQuit, font);

    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(GetDlgItem(w, kPlay));
    MSG m;
    while (!g_state.done && GetMessageW(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(w, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    DeleteObject(font);
    if (!g_state.play)
        return false;
    SaveSettings();
    Log("launcher: profile %s, %ux%u, aspect %s, %s, vsync %s, max %u fps", s.profile.c_str(), s.width, s.height,
        s.aspectW ? (std::to_string(s.aspectW) + ":" + std::to_string(s.aspectH)).c_str() : "auto", s.fullscreen ? "fullscreen" : "windowed",
        s.vsync ? "on" : "off", s.maxFps);
    return true;
}
