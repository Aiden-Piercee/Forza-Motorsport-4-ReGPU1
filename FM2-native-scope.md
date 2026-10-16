# Scope: a better native PC port of Forza Motorsport 2

Reference project: [0herculabs/hl360-Forza2](https://github.com/0herculabs/hl360-Forza2) (v0.3.0, Oct 2026)

## 1. What the reference project actually is

Read this first, because it decides where the speed is lost.

| Layer | How hl360-Forza2 does it | Cost |
|---|---|---|
| CPU | `rexglue codegen` statically recompiles the PowerPC `default.xex` into ~131 MB of C++ (`generated/`). No JIT, no interpreter. | Fine in principle. Each guest instruction becomes C++ that byte-swaps big-endian memory and keeps a full PPC register context, so it's slower than hand-written code but much faster than emulation. |
| GPU | The **ReXGlue SDK's `xenos` plugin**: it emulates the Xbox 360 GPU (Xenos) on D3D12. It translates Xenos microcode shaders, emulates the 10 MB EDRAM, resolves and tiled textures. Essentially Xenia's GPU path. | **This is where most of the time goes.** It's emulating the GPU, not rendering natively. |
| Shaders | Translated at runtime, with `async_shader_compilation = false` and a warm-up pass in the launcher. | Stutter the first time you see a new track or car, and a long precompile step. |
| Kernel/OS | ReXGlue's HLE of the 360 kernel/XAM, with XAPI fibers mapped to host fibers. | Moderate. Thread and sync emulation adds latency. |
| Port code | About 640 lines in `src/`: profiles, DLC, a null-stream crash guard, a log filter, crash reports. | Small. Almost all of the behaviour lives in the SDK, which this repo doesn't control. |

**What their own changelog says:** races at 120 Hz went from 64–105 fps to 111–115 fps just by dropping log spam (1,400 GPU errors a second). Menus are locked to half rate. 144 Hz runs *slower* than 60 Hz. A fast CPU is needed for 120 Hz. All of this points to a frame that's bound by the GPU emulation and frame pacing, not by game logic.

**So "way better" doesn't mean cleaning up their code.** It means the same static-recompilation CPU approach with a **native renderer** in place of Xenos emulation. Sonic Unleashed got the same treatment in *Unleashed Recompiled* (XenonRecomp + a hand-written renderer), and that's why it runs far beyond the 360 version.

## 2. Goal

A Windows (later Linux/Steam Deck) build of FM2 that:

- Holds a locked 60 fps on mid-range hardware and runs **uncapped/high refresh (120/144/165/240 Hz)** with the simulation decoupled from rendering.
- Has **no shader compilation stutter**: every pipeline is known ahead of time.
- Starts in seconds, without a "prepare shaders" step.
- Has stable, accurate frame pacing (no 144 Hz regression).
- Keeps the FM2 physics/AI behaviour bit-identical to the original (360 Hz physics tick).
- Supports **21:9 ultrawide** (and 32:9) with true Hor+ field of view and a correctly scaled HUD: the HUD keeps its 16:9 proportions, with elements anchored to the screen edges and centre elements staying centred. No stretching, no cropping, no clipped menus.

Non-goals for v1: Xbox Live, split-screen online, shipping any game data.

## 3. Proposed architecture

```
default.xex (user's disc) ──► XenonRecomp-style static recompiler ──► C++ (game code)
                                                                │
                      ┌─────────────────────────────────────────┤
                      ▼                                         ▼
      Thin kernel/XAM HLE (own, minimal)        Hooked render layer (D3D9-on-360 API calls
      threads, files, sync, XMA audio           intercepted at the game's D3D wrapper level)
                                                                │
                                                                ▼
                                    Native renderer (D3D12 + Vulkan via a thin RHI)
                                    - pre-translated shaders (offline, from disc)
                                    - real render targets, no EDRAM emulation
                                    - FSR/DLSS/XeSS, MSAA, higher-res shadows
```

Key decisions:

1. **Hook at the game's D3D API, not at the GPU command buffer.** FM2 is built on the 360's D3D9-like API (`D3DDevice_SetRenderState`, `DrawIndexedVertices`, `Resolve`, etc.). Find those functions in the XEX (they're statically linked XDK code with known signatures) and replace them with native calls. This removes the whole PM4 packet parser, EDRAM emulation and the resolve path that floods the reference port with errors.
2. **Translate shaders offline.** Extract every Xenos microcode shader from the disc at install time and convert them to DXIL/SPIR-V once (Xenia's / UnleashedRecomp's shader recompiler as a base). Build the pipeline cache from a recorded play-through manifest that ships with the port, so no shader compiles at runtime.
3. **Decouple simulation from presentation.** FM2 runs physics at 360 Hz internally and renders at 60. Pace the game loop on a high-precision timer rather than VSync, and (stretch) interpolate car/camera transforms for frames above 60. This fixes the "144 Hz is slower" problem at its root.
4. **Own the runtime.** A small purpose-built kernel HLE instead of a general SDK. Only implement what FM2 calls, and profile it.
5. **Recompiler quality.** Add codegen passes that matter for speed: skip byte-swaps on stack-local data, cache VMX128 registers in host SIMD, drop CR/XER updates that nothing reads, use `-O3`/LTO/PGO for the generated code, and partition it for fast incremental builds.

## 4. Phases and milestones

| # | Phase | Deliverable | Est. effort* |
|---|---|---|---|
| 0 | **Baseline and tooling** | Build hl360-Forza2 and profile it (PIX/Tracy, Superluminal). Capture a per-frame breakdown (CPU game vs GPU emulation vs present) at Mugello, 60/120 Hz. These are the numbers to beat. | 1–2 wks |
| 1 | **Recompiler and boot** | XEX → C++ with XenonRecomp (or a fork of rexglue codegen), reusing their manifest of missed functions and fiber addresses as a head start. Minimal kernel HLE, file I/O from extracted disc, boot to the first `D3DDevice_Present`. | 4–6 wks |
| 2 | **D3D-level render hooks** | Identify and hook the XDK D3D functions. Native RHI (D3D12 first). Menus and the frontend render correctly. | 6–10 wks |
| 3 | **Shader pipeline** | Offline Xenos → DXIL translator, install-time conversion, shipped pipeline manifest. In-race rendering at Mugello/Laguna Seca with correct materials, shadows, reflections, HDR/bloom. | 6–8 wks |
| 4 | **Audio, input, saves** | XMA decode, XAudio2 mixer, XInput/SDL3 gamepads with force feedback and wheel support, saves and profiles, DLC. | 3–4 wks |
| 5 | **Frame pacing and high refresh** | Timer-driven loop, uncapped rendering with transform interpolation, menu rate fix, VRR/G-Sync friendly. | 3–5 wks |
| 6 | **Enhancements** | Internal res and upscalers (FSR 3/DLSS/XeSS), MSAA via real render targets, higher-res shadow maps, anisotropic filtering, FOV. **Ultrawide (required):** Hor+ projection fix in the camera code; HUD/UI layout hooks that keep 16:9 proportions and anchor each element (speedo, minimap, lap/position, menus) left, right or centre; pillarboxed 16:9 FMVs; tested at 2560×1080, 3440×1440, 5120×1440. | 5–7 wks |
| 7 | **Coverage and QA** | Every track and car, career, DLC, Forza Plus 2 compatibility, scripted regression runs (borrow their `tools/session.ps1` idea), crash reporting. | 4+ wks |
| 8 | **Vulkan / Linux / Deck** | Second RHI backend, Proton and native Linux builds. | 3–4 wks |

\*For one experienced reverse engineer/graphics programmer, part time. The critical path is phases 2–3. That's where projects like this succeed or stall.

## 4a. Phase 2 finding: render from the command stream (Oct 2026)

Measured at the main menu: in 5 s the GPU executed ~9,600 draws, but only
~600 calls went through the game's D3D draw function (`sub_8237D158`, the
only code that builds draw packets besides one init path). FM2 renders
mostly from **prerecorded D3D command buffers**, replayed by D3D's swap
thread, and uses **predicated tiling** (each frame replayed per EDRAM tile).
Hooking the D3D API alone would miss ~94% of the draws.

Revised phase 2: a native renderer driven by the PM4 command stream the
runtime already executes, designed around speed rather than emulation:

- **One pass per frame, no tiles.** Predicated tile replays exist only
  because of the 10 MB EDRAM. Render the first tile's pass at full
  resolution into real render targets and skip the replays.
- **No EDRAM emulation.** Map EDRAM render targets to D3D12 render targets
  and resolves to copies (or aliasing) of real textures.
- **Shaders translated ahead of time.** Xenos microcode found in the stream
  (and on disc) is converted to DXIL once and cached; pipelines precompiled
  from a recorded manifest. No runtime compile stutter.
- **D3D API hooks where they pay off**: Present/VdSwap (pacing, ultrawide,
  frame interpolation), render target and viewport setup (resolution
  scaling, ultrawide HUD anchoring), and Bink/video upload paths.
- Command translation runs on its own thread(s); the guest never waits on
  host GPU work except at real fences.

This keeps the performance goals: the reference port's costs (EDRAM
emulation, tile replays, runtime shader compiles, logging) are exactly what
this design removes.

## 5. Performance targets (vs reference)

| Metric | hl360-Forza2 v0.3.0 | Target |
|---|---|---|
| Race fps, 120 Hz mode | 111–115 fps, "needs a fast CPU" | Locked 120, uncapped 240+ on mid-range |
| 144 Hz | Not offered (slower than 60) | Works, any refresh rate |
| First-visit stutter | Yes (shader compile) | None |
| Shader prep step | Required after res/MSAA changes | None (resolution-independent pipelines) |
| Menus | Half rate (30 fps) | Full refresh |
| Startup to menu | Measure in phase 0 | < 5 s |

## 6. Risks

- **Render hooking is the hardest part.** If FM2 writes raw command buffers in hot paths (some 360 titles do, for car rendering), parts of those still need a small PM4 interpreter. *Mitigation:* phase 0 should check how much of the frame goes through the D3D API vs raw packets before committing.
- **EDRAM-dependent effects** (MSAA resolves, predicated tiling, memexport) may need special handling to look right.
- **Physics determinism.** FM2's physics must stay at its original tick, or handling and AI change (this is why Forza Plus 2 ties `CarPhysicsUpdateHz` to refresh). Decouple render from sim; never speed up the sim.
- **Legal.** Ship no game data or copyrighted code. Users provide their own disc. Generated C++ derived from the XEX is a grey area: consider shipping only the recompiler plus an install-time build, or follow the community norm the reference project uses.
- **Scope creep.** The reference project got to "playable" by staying on a general SDK. A native renderer is several times the work, so plan for the long haul.

## 7. Cheaper alternative (if the full rewrite is too much)

Fork hl360-Forza2 and attack its bottlenecks directly, still on ReXGlue:

1. Profile first (phase 0 above).
2. Turn on async pipeline compilation with a shipped pipeline cache, instead of synchronous compiles.
3. Fix frame pacing: replace VSync-count pacing with a timer, which also fixes 144 Hz and half-rate menus.
4. Silence or fix the empty-resolve EDRAM copies at the source (hook the title code that issues them) rather than filtering logs.
5. Build the generated code with LTO/PGO.
6. Upstream GPU fixes to the ReXGlue SDK.

Expect a solid improvement (maybe 1.3–2× in GPU-bound scenes), not the step change a native renderer gives. It takes weeks, not months.

## 8. Immediate next steps

1. Clone and build the reference repo, then profile one race at 60 and 120 Hz (phase 0).
2. Disassemble the XEX and locate the XDK D3D entry points. Measure what fraction of draws go through them.
3. Decide between the full rewrite (section 3) and the fork (section 7) using those two numbers.
