# Testing the current build

Build state: boots, menus, race setup and races render natively. The game
is fully playable with a controller or the keyboard, with sound (5.1, downmixed by
Windows for stereo devices). There is no ultrawide support yet.

## Run it

Double-click `play.cmd` (or run `build\rel\fm2.exe` from the repository root).
The game needs `game\` (the extracted disc) next to it. Logs are written to
`out\play.log`.

The first start finds and compiles every shader in the game (about 3.5 minutes on
16 threads) behind a loading screen with a progress bar. Let it finish; the game
has not hung. Later starts load the compiled shaders from `Documents\fm2\shadercache`.
`play-cold.cmd` moves the caches aside and starts like a first boot with
`FM2_PERF=1`: slow frames are logged to `out\play.log` with their cause.

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

- Bright surfaces (the asphalt) may be overexposed; not yet compared with
  the console.
- Face culling is off (only a performance cost).
- The game runs at 60 fps; higher refresh rates are not supported yet.
- Looping sounds restart at the start of the 2 KB packet holding the loop point, not
  the exact sample: listen for clicks or stutters in engine and ambient loops.

## What to report

- Anything that renders wrong: which screen, car, track and camera, plus a
  screenshot.
- Crashes: the end of `out\play.log` (it has `CRASH:` lines with a stack).
- Controls that feel wrong (dead zones, steering response).
