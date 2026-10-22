# FM4 renderer port: translator status and change list

The renderer under `src/gpu` came from Forza-Motorsport-2-ReGPU (MIT, see THIRD_PARTY_NOTICES.md).
This file lists what has changed so far to run FM4 on it, and what still has to change.
The rules are the same as for the rest of the port. Output must match the 360 one to one: no skipped draws,
no sizes keyed to one title, and HLSL only (no shader bytecode).

## Step 0: dump FM4's shaders and survey them

We have no FM4 shader dump yet, so nobody has measured which Xenos features FM4 uses.
Before any renderer work, do this:

1. Run `tools/extractshaders.py` over the FM4 game files, plus DLC and the title update.
   It writes `<vs|ps>_<hash>.ucode`.
2. Run `tools/shadertest/run.sh survey <dump dir>`. It translates every shader and compiles each
   translation with dxc. It then prints a count for each feature used (loops, calls, aL addressing,
   memory export, fetch modes, vertex formats) and lists every shader that failed to translate or compile.

That report decides the order of everything below.

## Translator (src/gpu/ucode.*, src/gpu/hlsl.*): done

| Area | What is translated now | Checked by |
|---|---|---|
| Loops | `loop_start`/`loop_end` with loop constants `i0`-`i31` (count, start, signed step), `aL` clamped to [-256, 256], predicated break, `repeat`. Properly nested loops become `[loop] for`; anything else goes through a CF state machine | runs on lavapipe |
| Calls / jumps | `cond_call`/`return` with a 4-deep stack (a return with no caller does nothing), `cond_jmp` taken on a bool constant, predicate or unconditionally, and code after the first `end` (subroutines) | runs on lavapipe |
| Relative addressing | `aL`-relative temporaries on sources, vector and scalar destinations, and fetch src/dst; Xenia's rule for which constant operands are `a0`/`aL` relative; `abs_constants` and src3 negation on the scalar-constant ops (this replaces the `FM2_SC_NEG` switch) | runs on lavapipe |
| Texture fetch | explicit, register and computed LOD; register gradients; `setTextureLod`/`setTextureGradientsHorz/Vert`; `getTextureComputedLod`, `getGradients`, `getTextureWeights`; filter overrides; signed offsets; base-map mip mode. Computed LOD is only used in pixel shaders, as on the hardware | dxc; runs on lavapipe |
| Vertex fetch | signed 23-bit offsets, the no-zero signed mode, normalised 32-bit formats, rounding as on the hardware | runs on lavapipe |
| Memory export | `eA`/`eM0-4` with every stream format, endian mode and the red/blue swap. Exports are flushed at `alloc`, before `kill` and at the end. Writes go to a `u0` RWByteAddressBuffer | bytes compared on lavapipe |
| Depth output | pixel shaders that write `oDepth` get an `SV_Depth` output | dxc |

Tests are in `tools/shadertest` (`run.sh`; it needs a Vulkan device and dxc). They assemble microcode,
translate it, compile it with dxc to SPIR-V, run it and compare the results against values worked out by hand.

### Translator: still to do

- **Check with fxc on Windows.** The game compiles shaders with fxc (D3DCompile). The tests use dxc with `-HV 2016`.
  The risky case is a gradient instruction (SampleBias, computed LOD) inside a dynamic loop, which fxc can refuse.
  If the FM4 survey finds that case, emit SampleGrad with gradients taken outside the loop.
- **MRT.** `colorTargetsWritten` is recorded, but `main` still returns only `oC[0]`.
  The pixel shader output struct needs one SV_Target per colour target that is bound.
- **3D textures.** The dimension field is decoded but not bound yet. That needs a `gtex3d` space in the root signature.
- **Sampler model.** The 4 static samplers can't express everything a fetch constant can
  (per-axis clamp, border colour, anisotropy, the min/mag/mip combinations). Planned replacement:
  a sampler heap whose samplers are created from the fetch constant, keyed by its sampler fields.
- **Memory export binding.** Shaders that use memory export are marked as not translated
  until the renderer binds `u0` over guest memory and invalidates the matching textures and buffers.
- **FM2 regression.** The constant-relative rule now follows Xenia. Before merging, check it against
  an FM2 shader dump with `run.sh survey`, and compare frames if possible.

## Renderer (src/gpu/renderer.cpp and friends): to do, in order

The IDs refer to the FM2 hardcode audit. That audit found 58 grouped items; the ones listed here
change behaviour. Most of the remaining ~111 `FM2_*` names are debug switches that only need renaming.

1. **Remove draw skipping (D1, D5, D6, H3, D3).** No draw may be dropped while its pipeline or shader compiles.
   Instead:
   - translate the shader from the microcode in the draw's own shader-load packet, and wait for it;
   - load pipelines from a cache keyed by title ID;
   - wait for a pipeline that isn't ready yet.

   Also handle vertex fetch constant 96 or higher properly instead of skipping the draw, and grow the ring
   instead of dropping draws when it is full.

   *Status:* done, except the ring. Draws now wait for their shaders and pipeline; the blended/"settled"
   skip rules and the `FM2_ASYNC_*` switches are removed. Slot 96 (fetch index 31, select 3) has no
   fetch constant behind it. It now reads as zero and the draw goes ahead; before, the draw was skipped
   and the CPU wrote past `vf[96]`. Still to do: when the ring is full, draws are dropped (logged).
   The fix is to chain a second ring buffer or submit and recycle the ring; it needs a Windows run to check.
2. **Pass and surface descriptor (S1-S5, S13, E1-E5).** Replace the 1280x720 / pitch-1280 model and the
   tall/big pass detection with a descriptor built from `RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`,
   the window scissor and offset, and the resolve rect. Host surfaces get their size from that descriptor.
   EDRAM aliasing is handled by explicit ownership transfers, not by guessing the native MSAA layout.
   FM4 renders its frame as 3 predicated tiles with MSAA, and aliases EDRAM between formats. This is the biggest item.

   *Design:* host render targets in EDRAM space, as Xenia's render target cache does,
   rather than ReGPU's screen-space surfaces sized to the output.
   - Each layout (base tile, pitch, MSAA, format, colour or depth) gets a host render target.
     Its size is `pitch x HeightPixels` times the resolution scale.
   - A draw claims the tiles it can reach. Tiles another layout wrote last are copied across first
     (an ownership transfer).
   - Predicated tiles then need no special handling: each strip is ordinary EDRAM rows at its window offset.
     That removes the tall/big pass detection, the native-MSAA guess, the clear extension and `PassScale`.

   *Status:* the platform-independent core is in `src/gpu/edram.{h,cpp}` and is tested by `tools/edramtest`.
   It covers register decode, pitch and height in tiles, the window/screen scissor, the rows a draw can reach
   (scissor and viewport), the per-target tile ranges for MRT and depth, and the ownership map that
   produces transfers. The wiring steps:
   1. *Written, not yet run on Windows.* A host render-target cache keyed by `SurfaceKey`
      (`src/gpu/edram_targets.{h,cpp}`).
      - Each target is one typeless resource at guest MSAA and the resolution scale, with a typed view
        for draws and a `_UINT` view for transfers.
      - Host formats: 8888 as RGBA8, 2_10_10_10 as RGB10A2, 7e3 as RGBA16F (exact), 16_16(_16_16) as snorm16,
        the float formats as themselves, depth as D32_FLOAT_S8X24.
   2. *Written, not yet run on Windows.* The transfer shader (`src/gpu/edram_transfer_hlsl.h`). Each destination
      sample finds its EDRAM dwords and the source sample holding each one. It converts source host value ->
      guest bits -> destination host value. That covers any pair of colour/depth formats, 32/64 bpp and MSAA
      modes, and the resolution scale.
      - Stencil goes through `SV_StencilRef` where the device supports it, otherwise one pass per bit.
      - Transfers run only for tiles that another layout wrote last, so a layout redrawing its own tiles
        costs nothing.
      - `tools/edramtest` runs it on lavapipe and checks the sample mapping against an EDRAM filled forwards
        (5 layout pairs, including wrap and scale 2). It also checks that every format keeps its bits: all 2^24
        depths in both encodings, every 7e3 value, and the raw formats. All 60 shader variants compile with dxc;
        fxc is still to be checked on Windows.
      - With `REGPU_EDRAM_TARGETS=1` the renderer runs the cache beside its screen-space surfaces. Draws still
        go to the old surfaces; each draw's targets are created and claimed and its transfers recorded.
        Statistics are logged as `edram: ...`. The default path is unchanged. Draw workers are off while it runs,
        because ownership follows draw order.
      - Known gaps: depth reinterpreted between D24S8 and D24FS8 loses precision (Xenia keeps a second host depth
        for that). Host sample positions are the standard D3D ones, not the Xenos ones.
   3. Draws bound to the pass's targets, with the viewport and scissor in EDRAM space.
   4. Resolves that read the owner of the source tiles.
   5. Clears that claim tiles.
   6. Present, the upscalers and ultrawide, re-based on the descriptor. The "main view" becomes the pass
      whose resolve feeds the front buffer, not pitch 1280.
3. **Resolve/copy decode and texture formats (G1, G2, G3, G4, G9, S6).**
   - Decode all of `RB_COPY_CONTROL` / `RB_COPY_DEST_INFO`: format, number format, endian, swap,
     exponent bias, sample select, and constant fill.
   - Take bytes per texel from the destination format.
   - Add the texture formats FM4 needs: DXN to BC5, fp16/fp32, 2_10_10_10, 10_11_11 and 11_11_10,
     16_16 signed with exponent bias (FM4's 512x512 2x MSAA k_16_16 buffer), 24_8 sampled as a texture, CTX1.
   - Log an unknown format once by its ID instead of sampling white.
   - Map blend factors with a full table.
4. **CPU readback tracking instead of size-keyed writebacks (S7, S8, E7, E12).** Write a resolve back to guest memory
   when the CPU reads its range: page protection or dirty-range tracking on the destination. Do not decide it
   by destination size or 4x MSAA. On ReXGlue, FM4 broke when resolves were skipped by a "nobody reads it" guess,
   so no resolve may be skipped. This also covers garage thumbnails and photo mode.
5. **Move FM2's title code into a `titles/fm2/` module (H1, F4, A2-A5, K1, K2, F6).** This covers:
   - `ApplyDirtSeamFix`, the dealer easter egg and the camera-aspect hook;
   - the D3D device global and the 0x82000000 scan range;
   - the `.rmb` container finder;
   - the cache paths and branding.

   Put them behind a `TitleHooks` interface and give FM4 its own module. The cache keys include the title ID.
   The renderer's text edits on generated HLSL (MotionVariant and the debug hooks) depend on exact strings
   from hlsl.cpp. Those strings are kept, and the `SV_Depth` signature is already handled.
6. **Rename `FM2_*` switches** to `REGPU_*` through one helper that still accepts the old names; drop the dead A/B switches.
7. **Occlusion and predication (G8).** Support several queries at once, keyed by `RB_SAMPLE_COUNT_ADDR`,
   that return real counts. Handle COND_EXEC and bin predication in general.
8. **PM4 and EDRAM modes (G7, D2, G5).** Log each unhandled opcode once and implement the ones FM4 uses.
   Support EDRAM modes other than 4, 5 and 6.
9. **Never drop a draw when the upload ring is full (D3).** Today `RingAlloc` failing drops the draw (logged as
   `ring full`), which breaks 1:1. Grow the frame's ring: allocate the next buffer and continue there; the views
   that address the ring (`gmem`, root CBVs) are per draw. The alternative is to submit the list and wait for
   the GPU before reusing the ring. Either needs a Windows run.
10. **Display mode and present (S9-S11).** Take the front buffer size and format from the swap packet,
   and the display mode from the title config.
