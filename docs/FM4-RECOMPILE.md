# Recompiling FM4 with XenonRecomp

Milestone 1 of the FM4 port: recompile FM4's three XEX images with upstream
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and link them into one
program. This uses the same approach FM2 uses (`tools/recomp.sh`, `config/fm2.toml`).
No ReXGlue code or data is used. FM4's function boundaries come from XenonRecomp's
own analysis, which reads `.pdata`, follows `bl` targets and sweeps the gaps between
them. The switch tables come from XenonAnalyse.

## The three images

| Image | Base | Config | Output | Symbol prefix |
|---|---|---|---|---|
| `default.xex` | 0x82000000 | `config/fm4.toml` | `out/ppc` | none |
| `XMediaFacade_default.xex` | 0x88050000 | `config/fm4_xmediafacade.toml` | `out/ppc_xmf` | `xmf_` |
| `SpeechFacade_default.xex` | 0x890A0000 | `config/fm4_speechfacade.toml` | `out/ppc_sph` | `sph_` |

Only the Europe Game of the Year disc is supported. `tools/recomp-fm4.sh` checks the
SHA-256 hashes listed in each config.

Upstream XenonRecomp assumes a single image. `patches/xenonrecomp-modules.patch`
adds a `symbol_prefix` key to `[main]`. The prefix is added to every function name
the analyser creates (`sub_`, the register save/restore helpers, `_xstart`) and to
the mapping table (`xmf_PPCFuncMappings`). Import names (`__imp__*`) stay shared, so
one kernel implementation serves all three images. The patch also prints a warning
when an import comes from a library other than `xam.xex` or `xboxkrnl.exe`.
XenonUtils has no names for such imports, so they become no-op thunks.

`patches/xenonrecomp-fm4.patch` covers what FM4's code needs beyond FM2. FM4's
compiler orders jump-table code differently (`lis; rlwinm; addi; lwzx; mtctr;
bctr`), so XenonAnalyse's fixed patterns found none of its tables. The patch adds
a pass that starts from every remaining `bctr` and traces back through the
registers that feed it. It resolves absolute, byte-offset, shifted byte-offset
and halfword-offset tables, and checks that every case is code near the jump.
Any `bctr` that reads a table it cannot resolve is printed as a warning, to be
added by hand. The patch also makes the loader allocate the whole image for
basic-compressed XEX files: before, it allocated only the summed block size,
which left the zero tail outside the buffer.

Each image builds as its own static library with its own `ppc_config.h`.
`tools/genmodules.py` writes the table the loader uses, declared in
`src/recomp_modules.h`: guest path, image base and size, code base and size, and
mappings for each image.

## Procedure (Windows, the machine with the game files)

Prerequisites are the same as for FM2 (see the README's Building section): VS 2022 Build Tools,
LLVM (clang-cl), CMake, Ninja, Python 3.11 or newer as `py -3`, and Git Bash for `sh`.

1. Point `game` at the extracted Disc 1 folder. It needs `default.xex` and both
   facade XEX files directly inside it:

   ```
   mklink /J game E:\Forza-4-compiling-stuff\extracted
   ```

2. Apply the three XenonRecomp patches, in this order, and build the tools:

   ```
   git submodule update --init --recursive
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm2.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-modules.patch
   git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm4.patch
   tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   ```

3. Prepare the configs:

   ```
   sh tools/recomp-fm4.sh --prepare
   ```

   For each image, this step dumps the image (`out/<name>.bin` and `.sections`).
   `tools/fm4_config.py` then finds the eight save/restore helpers and the
   exception-data words and writes them into `config/<name>.toml`. XenonAnalyse
   writes `config/<name>_switch_tables.toml`. Review the diff of `config/`. A helper
   that no image contains stays `0x0`, and XenonRecomp then prints "unspecified"
   for it. That is harmless: calls to it recompile as ordinary functions.

4. Check the setjmp/longjmp candidates in `out/fm4.config.txt`. Fiber switches and
   context captures match the same pattern, so disassemble each candidate before
   using it:

   ```
   build\tools\dis.exe game\default.xex <address> 64
   ```

   If FM4's CRT setjmp/longjmp are among them, set `setjmp_address` and
   `longjmp_address` in `config/fm4.toml`.

5. Recompile:

   ```
   sh tools/recomp-fm4.sh
   ```

   Read `out/<name>.recomp.log` for "Unrecognized instruction" and jump-table
   messages, and `out/<name>.fallthrough.txt` for functions whose last instruction
   can run into the next function. Each real case goes into the config's
   `functions` list with its reason, or, for an unhandled instruction, into a new
   XenonRecomp patch. This step also regenerates `src/kernel/stubs.cpp` from all
   three import lists.

6. Cross-check the boundaries against the earlier function list. This step reads
   the list and copies nothing from it. Pass the hook addresses from the
   carry-over inventory to `--where`:

   ```
   py -3 tools/fm4_crosscheck.py out/ppc <fm4-recomp>\fm4\fm4_functions.toml --where <hook addresses> > out/fm4.crosscheck.txt
   py -3 tools/fm4_crosscheck.py out/ppc_xmf <fm4-recomp>\fm4\xmediafacade_functions.toml > out/fm4_xmediafacade.crosscheck.txt
   py -3 tools/fm4_crosscheck.py out/ppc_sph <fm4-recomp>\fm4\speechfacade_functions.toml > out/fm4_speechfacade.crosscheck.txt
   ```

   Most of the old entries are `.pdata` "family" entries at return addresses.
   XenonRecomp doesn't need those, because returns use the host stack. The
   categories that matter are the ones reached by indirect calls: vtable targets,
   vcall thunks, runtime crash targets, re-entry continuations and unresolved
   targets. Confirm each one in the disassembly before adding it to `functions`.

7. Link check: compile all three images and link them with the stubs and the
   image table, without the runtime. Then run the result, which checks the table:

   ```
   tools\vcenv.cmd cmake -S tools/linkcheck -B build/linkcheck -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
   tools\vcenv.cmd ninja -C build/linkcheck
   build\linkcheck\fm4_linkcheck.exe
   ```

Milestone 1 is done when step 7 prints `ok`, the logs have no unhandled
instructions, and every fallthrough and cross-check entry is either fixed in the
config or explained.

## What to keep, what never to commit

Commit `config/*.toml`, including the generated switch tables, which hold
addresses only. Commit any new `functions` entries with their reasons. Never
commit `game/`, `out/` (the images, recompiled sources and logs) or anything
else derived from the game's code.
