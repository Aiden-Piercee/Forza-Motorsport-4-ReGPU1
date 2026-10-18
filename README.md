# fm2

A native Windows port of **Forza Motorsport 2** (Xbox 360). Nothing is emulated:

- **CPU:** the game's entire PowerPC executable is statically recompiled to x64 C++
  with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (51,034 functions), running on our own
  implementation of the Xbox 360 kernel and XAM. No interpreter, no JIT.
- **GPU:** a native Direct3D 12 renderer replaces Xenos GPU emulation. The game's
  Xenos shader microcode is translated to HLSL, its render state to D3D12 pipelines,
  EDRAM resolves to GPU copies, and its tiled textures are decoded natively.

No game data is included: you need your own copy of the disc. See
[FM2-native-scope.md](FM2-native-scope.md) for the plan and [TESTING.md](TESTING.md)
for playing it.

## Shaders compiled before you drive, smooth frame times while you do

Shader compilation stutter is the usual weak point of running Xbox 360 games on PC.
Emulators and emulator-based ports translate each shader the first time the game uses
it, so every new car, track and effect hitches the first time it appears, unless you
first play through the game to fill a cache, or run a long "prepare shaders" step.

This port finds every shader up front instead:

- **On the first start**, it scans the game's files (the mesh archives and the
  executable) and finds the shader microcode directly, about 4,800 shaders in
  about 4 seconds. All of them are compiled behind a loading screen built from the game's own art
  (about 3.5 minutes on 16 threads, once). Later starts load the compiled shaders from
  the cache instantly.
- **Copies the game patches at load** (each vertex shader is rewritten for the vertex
  layout of the mesh it draws) share the bytecode of the original: the vertex layout
  is passed to the shader per draw instead of being compiled in.
- **The thread that feeds the GPU never blocks on disk or bookkeeping**: pipeline
  cache saves, cache file writes and shader lookups all happen off it, and draws never
  wait on a scan of guest memory.

Measured on the test machine below, through a full race with every GPU pipeline
built fresh: **no renderer-caused frame-time spikes, 59.9 fps average, 59.1 fps
minimum**, against hundreds of slow frames before this work. Run with `FM2_PERF=1`
and every slow frame is logged with what caused it.

For comparison, the existing emulator-based port
([hl360-Forza2](https://github.com/0herculabs/hl360-Forza2), v0.3.0) runs the Xbox 360 GPU
through a Xenia-derived emulation layer with a shader warm-up step; its own changelog
notes menus locked to half rate and 144 Hz running slower than 60 Hz. Details in
[FM2-native-scope.md](FM2-native-scope.md).

## System requirements

| | Minimum (estimated) | Tested on |
|---|---|---|
| OS | Windows 10 64-bit (version 1803 or later) | Windows 11 |
| CPU | 4 cores / 8 threads, x64 with SSSE3 | AMD Ryzen 7 5800X (8 cores / 16 threads) |
| GPU | DirectX 12, feature level 12_0 | NVIDIA GeForce RTX 4070 |
| RAM | 8 GB | 64 GB |
| Disk | 6 GB (extracted game ~4.7 GB, plus shader and pipeline caches) | |

The first start compiles every shader once; it takes longer with fewer CPU threads.
The minimum column has not been tested yet.

## Status

The game boots, plays its intro, and runs the menus, race setup and races natively at
up to 360 fps (menus included) and up to 4K, and experimentally on ultrawide displays, with car and track
rendering, shadows, HDR resolves and the HUD, and full 5.1
audio: the game's sound engine (FMOD) mixes into our render driver, which plays through
XAudio2, and its XMA sound data is decoded with FFmpeg's XMA decoder. Game time follows
the clock, so the game runs at real speed at any frame rate. Races are currently limited
by the renderer's CPU time (around 75 fps on a fast desktop CPU); menus reach the cap.

Saves work: the signed-in gamer profile is the profile name from the launcher (`Player`
by default), and its saves live in `Documents\fm2\profiles\<name>\content`.

## Launcher and settings

A small launcher opens before the game with the player settings: profile name,
resolution, aspect ratio (Auto, 16:9, 21:9, 32:9; the image gets black bars when it
differs from the resolution), windowed or borderless fullscreen, VSync, and max FPS
(30-360). Play saves them to `Documents\fm2\settings.ini`. With VSync on, the cap
is also the display's refresh rate. `FM2_NO_LAUNCHER=1` skips the launcher and uses the
saved settings; these variables override the file for one run: `FM2_PROFILE`,
`FM2_RESOLUTION`, `FM2_RENDER_SCALE`, `FM2_MAX_FPS`, `FM2_VSYNC`, `FM2_FULLSCREEN`.

## Building

Needs Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja, Python 3, and an
FFmpeg Windows x64 **LGPL shared** build (for XMA audio decoding), for example
`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases): extract it so that
`thirdparty/ffmpeg/include`, `lib` and `bin` exist. The build copies the DLLs it needs
next to `fm2.exe`; FFmpeg is linked dynamically and stays under its own LGPL license
(`thirdparty/ffmpeg/LICENSE.txt`).

```
git submodule update --init --recursive
git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm2.patch
py -3 tools/xiso.py extract "<your FM2 disc>.iso" game
tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
sh tools/recomp.sh
tools\vcenv.cmd cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
tools\vcenv.cmd ninja -C build/rel fm2
build\rel\fm2.exe game
```

Supported: European `default.xex`, SHA-256 `9726066f…9b120a`. The recompiled
code (`out/ppc`, ~131 MB) is generated from your own copy and never committed.

## Debugging switches

| Variable | Effect |
|---|---|
| `FM2_WATCHDOG=1` (or a period in ms) | Every 3 s: each guest thread's call stack (recompiled frames are `sub_<guest address>`), what it waits on, GPU state |
| `FM2_GPUTRACE=1` | Log every PM4 packet the command processor runs |
| `FM2_TRACE_OBJ=<hex>` | Log KeSetEvent/KeResetEvent/KeWait on one guest object |
| `FM2_TRACE_SWAP=1` | Log D3D presents and swaps |
| `FM2_WATCH=<hex>` | Log every change of one guest word |
| `FM2_WATCHW=<hex>` or `<ptr>,<offset>` | Hardware write watchpoint on a guest byte; logs the writing guest functions |
| `FM2_TRACE_IMPORTS=<seconds>` | Log kernel/XAM calls made after that time (`FM2_TRACE_LIMIT` per name, default 5) |
| `FM2_TRACE_FILES=1` | Log file opens and misses |
| `FM2_AUTOSTART=1` | Tap Start + A every 3 s |
| `FM2_DUMP_SHADERS=1` | Save every distinct shader's microcode to `Documents/fm2/shaders` |
| `FM2_DUMP_FRAME=<n>` | Write the n-th rendered frame to `frame.png` |
| `FM2_RESOLUTION=<w>x<h>` | Override the launcher resolution: render at that size, any aspect (e.g. `3440x1440`). Wider than 16:9 is **experimental**: the 3D view widens (Hor+), the HUD and menus keep 16:9 proportions in the centre, but some objects at the sides still pop in (game-side culling) |
| `FM2_WIDE_FOV=<0-100>` | Ultrawide only: how far the view widens (100 full Hor+, 0 the 16:9 view zoomed to fill) |
| `FM2_RENDER_SCALE=<1-4>` | Render at that multiple of 1280x720 (2 = 2560x1440, 3 = 3840x2160) |
| `FM2_DUMP_AUDIO=<file.wav>` | Write everything the game plays (6-channel float, 48 kHz) to a WAV file |
| `FM2_TRACE_DRAWS=<n>` | Log every draw, skip, resolve, PM4 packet and texture upload of frame n |
| `FM2_DUMP_TEXTURE=<hex address>` | Write that texture, decoded, to `texture.png` |
| `FM2_DUMP_HLSL=1` | Log each translated shader |
| `FM2_TRACE_PRED=1` | Log predicated packets under tiling |
| `FM2_TEXVIEW=1` | Show decoded textures instead of rendering |
| `FM2_HEADLESS=1` | No window |
| `FM2_SKIP_VS=<hash>,...` | With `FM2_DUMP_FRAME=n`: drop those vertex shaders in frame n+1 and write it to `frame2.png` (A/B under the same camera) |
| `FM2_SKIP_DRAWS=a-b` | Drop draws a..b-1 of the dumped frame |
| `FM2_TRACE_VFETCH=1` | Print every translated vertex fetch (format, stride, offset, scale) |
| `FM2_NO_DIRECT_FETCH=1` | Copy all vertex data instead of reading large buffers in place |

`build/rel/xenos_dis.exe <shaders...>` disassembles captured Xenos microcode;
`--hlsl` translates and compiles them instead.

`tools/callgraph.py` indexes the generated code (`build`, `callers sub_X`,
`at <address>`); `build/tools/dis.exe <xex> <address> <count>` disassembles.

## Layout

| Path | Contents |
|---|---|
| `config/` | XenonRecomp config and jump tables for FM2 |
| `patches/` | Our XenonRecomp changes: FM2's extra VMX/PPC instructions, non-trapping divides, MSR[EE] hook |
| `src/` | Runtime: guest memory, loader, kernel/XAM imports, threads, renderer, audio output and XMA decoding |
| `src/hooks/` | Overrides of recompiled functions (`sub_X`): XAPI fibers on host fibers, D3D instrumentation |
| `tools/` | Disc extraction, XEX dump/disassembler, analysis scripts |
