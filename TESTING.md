# Testing the current build

Build state: boots, menus, race setup and races render natively. The game
is fully playable with a controller or the keyboard, with sound (5.1, downmixed by
Windows for stereo devices). Saves work.

## Run it

Double-click `play.cmd` (or run `build\tu\fm2.exe` from the repository root).
The game needs `game\` (the extracted disc) next to it. Logs are written to
`out\play.log`.

This build is made for the title update (the game as patched by its last update, which also
enables the downloadable cars and tracks). `update\` and `dlc\` next to `play.cmd` hold the
update and the packs; `install-content.cmd` copies them into `Documents\fm2` when you start
the game (nothing is overwritten).

A launcher opens first: pick the profile name (your save slot; `Player` by
default), resolution, aspect ratio, windowed or fullscreen, VSync and max FPS, then
Play. The choices are kept in `Documents\fm2\settings.ini` for next time. Each
profile name has its own saves (`Documents\fm2\profiles\<name>`); saves made
before profiles existed belong to `Player`.

The first start finds and compiles every shader in the game (about 3.5 minutes on
16 threads) behind a loading screen with a progress bar. Let it finish; the game
has not hung. Later starts load the compiled shaders from `Documents\fm2\shadercache`.
`play-cold.cmd` moves the caches aside and starts like a first boot with
`FM2_PERF=1`: slow frames are logged to `out\play.log` with their cause.

## Benchmark

`benchmark.cmd` measures performance the same way on every machine: it drives the
menus into a Sunset race with 8 cars, holds the throttle for 25 seconds, and writes
`out\benchmark.txt` with the average fps, the 1% and 0.1% lows, the slowest frame, the
CPU, GPU and RAM, and the resolution and upscaler. It takes about 75 seconds after the intro videos.

- Start the game once with `play.cmd` first (the first start compiles shaders).
- The intro videos play first (about 90 seconds), so the whole run takes about 3 minutes.
- Pick the resolution and upscaler in the launcher beforehand: the benchmark uses the
  saved settings, without a frame cap or VSync, and its own `Benchmark` profile.
- Don't touch the keyboard or controller while it runs.
- Send `out\benchmark.txt` with your results.

## Controls

An Xbox controller (XInput) works as on the console, rumble included.
Pads 1-4 map to players 1-4. The keyboard drives player 1, merged with
pad 1:

| Key | Pad |
|---|---|
| W / S | Throttle / brake (triggers) |
| A / D | Steer |
| Arrow keys | D-pad (menus) |
| Enter | A (select) |
| Backspace | B (back) |
| Space | X (handbrake) |
| Y | Y |
| Q / E | Shoulders (gear down / up) |
| I J K L | Right stick (camera) |
| Escape | Start (pause) |
| Tab | Back (change view) |

The keyboard only works while the game window has focus.

## Known issues

- Races run at about 140-165 fps with a full grid at 2560x1440 on a fast desktop; set
  Max FPS to 360 to see how far your machine goes. Report stutters with the track, car
  and place (out\play.log has the slow frames when run with FM2_PERF=1).
- Ultrawide (a resolution or aspect ratio wider than 16:9) is experimental: some objects at the
  sides of the screen still pop in late, and the HUD stays in the centre 16:9 area.
- The intro (legal text and the Turn 10, Microsoft, Dolby and Forza videos: about 107 seconds)
  cannot be skipped by a button. Tick "Skip intro videos" in the launcher (or set
  FM2_SKIP_INTRO=1) to go straight to the title screen. Input recordings made before the clock
  fix start about 90 seconds too early.
- Photo Mode shows a black screen. Probably the same cause as the garage thumbnails below: it
  reads a render target back through memory. FM2_RESOLVE_WRITEBACK handles small ones; a
  full-screen readback would need an on-demand path (the capture tools in out/capture and
  FM2_TRACE_RESOLVES show what it resolves).
- Garage thumbnails: cars created from now on get a correct thumbnail. Thumbnails saved by
  earlier runs are noise and stay that way in that profile.
- "Xbox 360" text: the wording is UTF-16 inside the LSB2 string tables of every language zip
  (Media/StringTables/*.zip, e.g. Forza_Flow.str, Livery_Decals.str), and the decal names
  "XBOX 360 (Vertical Format)" are in Media/DB/gamedb.slt. Changing the length needs the LSB2
  offsets rebuilt. The car name "#5 XBOX 360 911 GT3-RSR" is a livery name, so decide whether
  it changes.
- Not traced yet: Laguna Seca kerbs show a smeared red line instead of the pattern, race dirt
  cuts across the car body, the Hire Driver text overflows after a few selections, a blue stripe
  appears on tyres at random, a faint occlusion ghost sits above the car model, and Tsukuba
  variant 2 has the wrong skybox colours. Use F12 on each, and see the shader translation
  failures in out/play.log (X3504 array index out of bounds, and "invalid access of unbound
  variable" in a pixel shader) as a first lead for the tyre, dirt and kerb artefacts.

## Capturing a rendering bug (F12)

With the game window focused, press F12 when the wrong picture is on screen (it pauses for a
moment while it writes). Then keep, from the game folder:

- `frame.png`: what the game drew,
- `out/capture/textures.txt` and `out/capture/tex/`: every texture the frame used, whether it was
  bound or fell back to white (and why), with its decoded image,
- `out/res/`: the render targets the frame drew into,
- `out/play.log`: the frame's draw calls (shaders, textures, geometry).

## What to report

- Anything that renders wrong: which screen, car, track and camera, plus a
  screenshot.
- Crashes: the end of `out\play.log` (it has `CRASH:` lines with a stack).
- Controls that feel wrong (dead zones, steering response).
