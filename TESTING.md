# Testing the current build

Build state: boots, menus, race setup and races render natively. The game
is fully playable with a controller or the keyboard. Audio is silent (XMA is
stubbed), and there is no ultrawide support yet.

## Run it

Double-click `play.cmd` (or run `build\rel\fm2.exe` from the repository root).
The game needs `game\` (the extracted disc) next to it. Logs are written to
`out\play.log`.

The first boot compiles shaders in the background, so some effects pop in
for the first minute. Later runs load them from `Documents\fm2\shadercache`.

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
- The game presents at about 30 fps; performance work has not started yet.
- No audio.

## What to report

- Anything that renders wrong: which screen, car, track and camera, plus a
  screenshot.
- Crashes: the end of `out\play.log` (it has `CRASH:` lines with a stack).
- Controls that feel wrong (dead zones, steering response).
