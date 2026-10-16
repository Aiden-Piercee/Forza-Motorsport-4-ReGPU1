# fm2

A native PC port of Forza Motorsport 2 (Xbox 360): the game's PowerPC code is
statically recompiled to C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp),
running on our own kernel layer and, from phase 2, a native renderer. No game
data is included; you need your own disc image. See
[FM2-native-scope.md](FM2-native-scope.md) for the plan.

## Status

Phase 1 (recompile and boot) is done. The game boots on our kernel layer,
plays its intro sequence, passes the title screen (Start) and reaches the
main menu, presenting at a steady 60 Hz with the menu videos and showcase car
loaded. A window with a D3D12 swap chain presents every frame (a placeholder
clear for now; `FM2_HEADLESS=1` runs without it).

Renderer progress: the command stream is tracked (constants, shader loads,
draws), Xenos textures decode correctly (tiling, endianness, DXT1/3/5,
8888, 565, 8-bit), and the window shows the textures each frame draws with.
Shader microcode decodes and disassembles; translation to HLSL is next. Phase 2 is under way: the
renderer will be driven by the PM4 command stream (see the scope, section 4a).

Stand-ins until their phase: XMA decoding writes silence, audio output is
discarded, user 0 is a keyboard-driven gamepad (Enter = Start, Space = A,
Backspace = B, arrows = D-pad).

## Building

Needs Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja, Python 3.

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
| `FM2_DUMP_SHADERS=1` | Save every distinct shader's microcode to `Documentsm2\shaders` |
| `FM2_DUMP_FRAME=<n>` | Write the n-th composed frame to `frame.png` |
| `FM2_HEADLESS=1` | No window |

`build/rel/xenos_dis.exe <shaders...>` disassembles captured Xenos microcode.

`tools/callgraph.py` indexes the generated code (`build`, `callers sub_X`,
`at <address>`); `build/tools/dis.exe <xex> <address> <count>` disassembles.

## Layout

| Path | Contents |
|---|---|
| `config/` | XenonRecomp config and jump tables for FM2 |
| `patches/` | Our XenonRecomp changes: FM2's extra VMX/PPC instructions, non-trapping divides, MSR[EE] hook |
| `src/` | Runtime: guest memory, loader, kernel/XAM imports, threads, GPU and audio stand-ins |
| `src/hooks/` | Overrides of recompiled functions (`sub_X`): XAPI fibers on host fibers, D3D instrumentation |
| `tools/` | Disc extraction, XEX dump/disassembler, analysis scripts |
