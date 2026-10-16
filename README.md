# fm2

A native PC port of Forza Motorsport 2 (Xbox 360): the game's PowerPC code is
statically recompiled to C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp),
running on our own kernel layer and, from phase 2, a native renderer. No game
data is included; you need your own disc image. See
[FM2-native-scope.md](FM2-native-scope.md) for the plan.

## Status

Phase 1 (recompile and boot): the game boots on our kernel layer, starts its
threads, initialises Direct3D and presents its first frames (VdSwap) about
0.45 s after launch. Nothing is drawn yet: a command-processor stand-in
retires the GPU work so the D3D side keeps running. After ~87 frames the game
stops presenting while it waits on audio (XMA decoding is still a stub).

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

`tools/callgraph.py` indexes the generated code (`build`, `callers sub_X`,
`at <address>`); `build/tools/dis.exe <xex> <address> <count>` disassembles.

## Layout

| Path | Contents |
|---|---|
| `config/` | XenonRecomp config and jump tables for FM2 |
| `patches/` | Our XenonRecomp changes: FM2's extra VMX/PPC instructions, non-trapping divides, MSR[EE] hook |
| `src/` | Runtime: guest memory, loader, kernel/XAM imports, threads, GPU and audio stand-ins |
| `src/hooks/` | Overrides of recompiled functions (`sub_X`), currently D3D instrumentation |
| `tools/` | Disc extraction, XEX dump/disassembler, analysis scripts |
