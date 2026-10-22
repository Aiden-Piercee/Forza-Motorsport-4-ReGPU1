# FM4 kernel and module gap analysis

Status: written from a read of this repository (ReGPU 0.5.1, the FM2 port) and of
Aiden-Piercee/fm4-recomp. **FM4's own import list has not been read yet**; the XEX files
live on the developer's PC. Everything marked *(verify)* depends on that list.
`tools/importgap.py` turns the list into the exact version of this document (see the end).

## What ReGPU's kernel is

`src/kernel` is about 7,000 lines. Roughly 195 imports have real code (`GUEST_EXPORT`), about
70 more are weak stubs in `stubs.cpp` that log once and return 0, and `tools/genstubs.py`
regenerates the stub file from the recompiled `ppc_recomp_shared.h`, so every import the
title calls always links. The XAM layer is deliberately headless: one signed-in local profile,
every system dialog answered with its default, saves and DLC as directories on disk.

Things that are single-title assumptions and matter for FM4:

| Area | FM2 behaviour | Why it matters for FM4 |
|---|---|---|
| Modules | One image. `LoadTitle` checks `PPC_IMAGE_BASE`, there is one `PPCFuncMappings`, `g_title.ldrEntry` is the only module handle. `XexGetModuleHandle` answers only for `default.xex`; `XexGetProcedureAddress` always fails. | FM4 loads `XMediaFacade_default.xex` and `SpeechFacade_default.xex` at run time. |
| Weak stubs | Return 0 (= success for NTSTATUS/HRESULT) and log once. | A stub that "succeeds" without filling its out-parameters is a silent lie. Every FM4 stub needs a review, not just a link. |
| Profile settings | `XamUserReadProfileSettings` answers "not set" for every id; there is no write. | FM4 stores options and career state; see M3. |
| Content | Directory-based: saves in the profile, DLC unpacked by `tools/stfs.py install` into `<user>\dlc\<package>` with `.title` and `.license` sidecars. | FM4's Disc 2 and marketplace packs are STFS (type 2, title `4D530910`) and the game enumerates them (M4). |
| Audio | XMA hardware contexts emulated in `src/audio/xma.cpp` (FFmpeg), XAudio render driver exports stubbed. | Depends on whether FM4 uses the same FMOD/XMA path *(verify)*. |

## Gaps in milestone order

### M1: boot to the first swap

1. **Multi-module loader (new work, the biggest item).** Generalise `LoadTitle` into a
   per-module loader that owns: image, XEX header copy, `LDR_DATA_TABLE_ENTRY`, TLS template,
   entry point, import-variable resolution (`ResolveVariableImports`) and the
   `PPC_LOOKUP_FUNC` mapping for the module's code range. Then add:
   - `XexLoadImage` / `XexUnloadImage` / `XexLoadImageFromMemory`, resolving
     `game:\XMediaFacade_default.xex` through the VFS and returning a module handle;
   - `XexGetModuleHandle(name)` for the three modules and `XexGetProcedureAddress(handle,
     ordinal | name)`, which needs the module's export table *(verify the table layout
     against the real facade XEX before coding; Xenia and the XDK disagree in places)*;
   - `RtlImageXexHeaderField` and `XexGetModuleSection` keyed by the handle, not by
     `g_title`;
   - running the module entry point (DLL_PROCESS_ATTACH) with the module's TLS set up.
   Agreed with the XenonRecomp thread: each XEX is recompiled into its own directory with a
   `symbol_prefix` (`xmf_`, `sph_`) and built as its own static library, `__imp__` names stay
   shared so one kernel serves all three, and `tools/genmodules.py` emits a registry of
   {guest path, image base/size, code base/size, mappings} for the loader. The indirect-call
   table moves out of guest space (host base + 4 GB + (guest - 0x80000000) * 2) behind a
   `GuestFuncSlot(addr)` helper, so module load/unload only commits or decommits pages.
   `loader.cpp` and `thread.cpp` switch from `PPC_LOOKUP_FUNC` to that helper.

   Status: the runtime side is written but **not yet built or run** (no Windows toolchain or
   XenonUtils in the authoring environment). `loader.cpp` now has a shared `LoadXex` used by
   the title and by modules, a `ModuleDesc` registry (`ModuleDescs()`, weak and empty until
   `tools/genmodules.py` supplies it), and `GuestFuncSlot()` in `loader.h`, which still points
   at `PPC_LOOKUP_FUNC` until the call-table move lands. `XexLoadImage`, `XexUnloadImage`,
   `XexGetModuleHandle`, `XexGetModuleSection` and `XexGetProcedureAddress` are keyed by
   module handle; exports are read from the PE export directory *(verify against the real
   facade XEX)*. Not done: module TLS (logged, not allocated), running the module entry point
   (decide once the facades' load sequence is visible), real unload.
2. **Non-kernel import libraries.** XenonUtils resolves import names only for `xam.xex` and
   `xboxkrnl.exe`; an import from any other library becomes a silent no-op thunk. The raw
   `xexheader.py` output for all three XEXs shows whether FM4 or a facade imports from
   anything else (for example `xbdm.xex`), which would be a silent gap.
3. **Import list and kernel variables.** `KernelVariable` knows a fixed set of ordinals and
   gives anything else zeroed storage with a log line. Check FM4's variable imports against it.
4. **Stub review.** Run `tools/importgap.py` on the three import lists; every `stub` row in
   M1 is a candidate boot blocker until shown otherwise.
5. **Title update.** ReGPU can apply a `default.xexp` (`FM2_TITLE_UPDATE`). Decide which FM4
   executable is the baseline (the existing fork targets the Europe GOTY Disc 1 XEX with its
   documented hashes) and keep the patch path only if that build needs it.
6. **Memory map.** `memory.cpp` hands out the virtual heap first-fit. Module images need
   their ranges reserved before the allocator starts, and the ranges depend on the facade
   image bases *(verify)*.

### M2: menus

- Same profile and notification surface as FM2 (`XamUserGetSigninState`, `XamUserGetXUID`,
  `XNotifyGetNext`, input, message boxes), so most of this should already work.
- **Kinect/NUI.** FM2 never touched it; FM4 has Kinect head tracking, so expect
  `XamNui*` imports *(verify)*. Correct behaviour is "no sensor present": status calls must
  fill their out-structure with a not-ready state, not return 0 with garbage.
- **Speech.** `SpeechFacade_default.xex` is the Kinect/voice layer. With no sensor it should
  load and report no recogniser; confirm what the game does when its calls fail.
- Movie skip hooks in `fm4_hooks.toml` (Turn 10 intro, Clarkson voice-over) are recomp hooks,
  not kernel work; they carry over through the recompile thread's hook port.

### M3: race, saves, profile

- **Profile writes.** Add `XamUserWriteProfileSettings` backed by a per-profile settings
  store, and make `XamUserReadProfileSettings` return what was written. Exact argument layout
  to be taken from the XDK headers/Xenia, not guessed.
- **Saves.** `XamContentCreateEx` already maps save games to profile directories; check the
  FM4 save-device flow (device selector, `XamContentGetDeviceData` capacity, create flags).
- **Achievements and stats.** FM4's validated build saves achievements. Needs
  `XamUserWriteAchievements`-style calls accepted and stored (or dropped) deliberately, and
  `XamUserCreateStatsEnumerator` currently fails; decide per call what the UI needs.
- **Threads and fibers.** FM2 relies on `src/hooks/fibers.cpp`. FM4's addresses for the same
  functions come from the recompile thread; the kernel side (`ExCreateThread`, affinity,
  priority, TLS) is shared.
- **Physics timing.** `KeQueryPerformanceFrequency`/`mftb` use the 49.875 MHz timebase; FM4
  needs the same, plus whatever the 360 Hz physics loop expects of sleep precision.

### M4: audio, content, DLC

- **Audio path.** If FM4 imports the `XMA*` and `XAudio*` families, the existing emulation
  covers it, but the render driver exports are only stubs; `audio.cpp` must be checked
  against what FM4 submits. If FM4's middleware differs, this is a rewrite *(verify)*.
- **`XMediaFacade`** is the media playback module (music/video). Needs its module loader work
  from M1 plus whatever it imports beyond the main title *(verify)*.
- **Disc 2 and marketplace DLC.** fm4-recomp validates four packages
  (`4d53091000000001`..`04`, content type marketplace, title `4D530910`) before installing.
  ReGPU's `stfs.py install` already unpacks type-2 packages and records the license mask, so
  the install is a port of the validation rules, not new STFS code. Open points: the game
  enumerates a fixed set of entries and every stray folder in the content directory takes a
  slot (documented in the fm4-recomp knowledge base), so the enumerator must list only real
  packages in a stable order; DLC file lookups probe `Media\DLCZips\<zip>` folders and the
  existing recomp reads the zips directly, which maps onto ReGPU's `zip_overlay.cpp`.
- **Online.** `NetDll_*` and XNet are stubbed or answer "network down". Keep it that way;
  FM4 must treat a missing network as normal.

## Using this with the real import list

```
py -3 tools\xexheader.py default.xex                # raw import records, plain Python
py -3 tools\importgap.py out\ppc\ppc_recomp_shared.h   # after the recompile
```

`importgap.py` merges several lists (default plus both facades), classifies each import as
implemented, stub-only or missing, and groups the result by the milestone above. It does not
need the game; it reads names only.

## Build layout (milestone 2)

The top-level `CMakeLists.txt` now builds FM4: one static library per recompiled image
(`out/ppc`, `out/ppc_xmf`, `out/ppc_sph`), generated `stubs.cpp` and `recomp_modules.cpp`, and
`patch_mftb.py` over all three. `src/hooks` is split so FM2's address-bound hooks cannot
hijack FM4 functions:

- `src/hooks/fm2/` - FM2's hooks (they override FM2 `sub_` addresses). Not compiled unless
  `-DFM4_BUILD_FM2_HOOKS=ON`; kept as the reference for re-addressing.
- `src/hooks/common/` - title-independent code the runtime calls: `mem_scan.cpp`, and
  `diagnostics.cpp` (`StartWatch`, an empty `D3DDebugLog`, no-op easter-egg entry points, and
  the idle `g_patchConfig` / `g_cameraAspectWidened` the renderer reads).
- `src/hooks/fm4/` - FM4 hooks. `fibers.cpp` is FM2's host-fiber implementation behind the
  macros in `addresses.h`; **it is not built until those six addresses are filled in** (the
  build prints a message), and the title will not get past its first fiber without them.

