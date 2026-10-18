#pragma once
#include <cstdint>
#include <string>

// Player settings: Documents\fm2\settings.ini, edited by the launcher. Environment
// variables (FM2_RESOLUTION, FM2_RENDER_SCALE, FM2_MAX_FPS, FM2_VSYNC, FM2_FULLSCREEN,
// FM2_PROFILE) override the file for testing.
struct Settings
{
    std::string profile = "Player";     // the gamer profile name the game sees
    uint32_t width = 1280, height = 720; // output resolution
    uint32_t aspectW = 0, aspectH = 0;   // forced aspect ratio (0 = from the resolution)
    bool fullscreen = false;             // borderless fullscreen on the primary display
    bool vsync = true;
    uint32_t maxFps = 60;                // frame cap, 30-360 (the physics tick rate)

    // The size the game renders at: the resolution, narrowed or shortened to the forced
    // aspect ratio (the rest of the screen is black bars).
    void RenderSize(uint32_t& w, uint32_t& h) const;
};

// Loaded on first use (after RuntimeInit has set the user directory).
Settings& GetSettings();
void SaveSettings();
