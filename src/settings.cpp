// Player settings (see settings.h): a small key=value file.
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "kernel/runtime.h"
#include "log.h"

namespace
{
Settings g_settings;
bool g_loaded = false;

std::filesystem::path Path() { return UserDir() / "settings.ini"; }

void Load()
{
    std::ifstream in(Path());
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "profile" && !value.empty())
            g_settings.profile = value.substr(0, 15);
        else if (key == "resolution")
            sscanf(value.c_str(), "%ux%u", &g_settings.width, &g_settings.height);
        else if (key == "aspect")
        {
            if (sscanf(value.c_str(), "%u:%u", &g_settings.aspectW, &g_settings.aspectH) != 2)
                g_settings.aspectW = g_settings.aspectH = 0;
        }
        else if (key == "fullscreen")
            g_settings.fullscreen = value == "1";
        else if (key == "vsync")
            g_settings.vsync = value == "1";
        else if (key == "max_fps")
            g_settings.maxFps = uint32_t(atoi(value.c_str()));
    }
    // Overrides for testing.
    if (const char* v = getenv("FM2_PROFILE"))
        g_settings.profile = std::string(v).substr(0, 15);
    if (const char* v = getenv("FM2_RESOLUTION"))
        sscanf(v, "%ux%u", &g_settings.width, &g_settings.height);
    else if (const char* k = getenv("FM2_RENDER_SCALE"))
    {
        const uint32_t s = std::clamp<uint32_t>(uint32_t(atoi(k)), 1, 4);
        g_settings.width = 1280 * s;
        g_settings.height = 720 * s;
    }
    if (const char* v = getenv("FM2_MAX_FPS"))
        g_settings.maxFps = uint32_t(atoi(v));
    if (const char* v = getenv("FM2_VSYNC"))
        g_settings.vsync = atoi(v) != 0;
    if (const char* v = getenv("FM2_FULLSCREEN"))
        g_settings.fullscreen = atoi(v) != 0;
    g_settings.width = std::clamp<uint32_t>(g_settings.width, 640, 15360);
    g_settings.height = std::clamp<uint32_t>(g_settings.height, 360, 8640);
    g_settings.maxFps = std::clamp<uint32_t>(g_settings.maxFps ? g_settings.maxFps : 60, 30, 360);
    if (g_settings.profile.empty())
        g_settings.profile = "Player";
}
}

void Settings::RenderSize(uint32_t& w, uint32_t& h) const
{
    w = width;
    h = height;
    // The forced aspect, or the resolution's own; never narrower than 16:9 (narrower views
    // are not supported yet: such resolutions get 16:9 with bars).
    uint64_t aw = aspectW, ah = aspectH;
    if (!aw || !ah)
        aw = width, ah = height;
    if (aw * 9 < ah * 16)
        aw = 16, ah = 9;
    // Fit that aspect inside the resolution.
    if (uint64_t(width) * ah > uint64_t(height) * aw)
        w = uint32_t(uint64_t(height) * aw / ah) & ~1u;
    else
        h = uint32_t(uint64_t(width) * ah / aw) & ~1u;
}

Settings& GetSettings()
{
    if (!g_loaded)
    {
        g_loaded = true;
        Load();
    }
    return g_settings;
}

void SaveSettings()
{
    const Settings& s = GetSettings();
    std::ofstream out(Path());
    out << "# Forza Motorsport 2 (fm2) settings; the launcher edits this file.\n";
    out << "profile=" << s.profile << "\n";
    out << "resolution=" << s.width << "x" << s.height << "\n";
    out << "aspect=" << (s.aspectW ? std::to_string(s.aspectW) + ":" + std::to_string(s.aspectH) : std::string("auto")) << "\n";
    out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
    out << "max_fps=" << s.maxFps << "\n";
}
