# fm2

A native PC port of Forza Motorsport 2 (Xbox 360): the game's PowerPC code is
statically recompiled to C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp),
running on our own kernel layer and a native renderer. No game data is included;
you need your own disc image. See [FM2-native-scope.md](FM2-native-scope.md) for the plan.

## Recompiling

Needs Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja, Python 3.

```
git submodule update --init --recursive
git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm2.patch
py -3 tools/xiso.py extract "<your FM2 disc>.iso" game
tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
tools\vcenv.cmd ninja -C build/tools
mkdir out\ppc
build\tools\xenonrecomp\XenonRecomp\XenonRecomp.exe config/fm2.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h
```

Supported: European `default.xex`, SHA-256 `9726066f…9b120a`.

| Path | Contents |
|---|---|
| `config/` | XenonRecomp config and jump tables for FM2 |
| `patches/` | Our changes to XenonRecomp (FM2's extra VMX/PPC instructions) |
| `tools/` | Disc extraction, XEX dump, disassembler, analysis scripts |
