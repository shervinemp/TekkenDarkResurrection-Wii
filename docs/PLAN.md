# TKDR Wii — accuracy and performance completion plan

> **2026-10-25: start with `docs/HANDOVER.md`.**  It is the current map of the design, the state and
> every open item; this plan is still the design reference for the GE effects (section 4), render-to-texture
> (5), the movie (7) and the corpus (3), but its "What is true today" is from 2026-10-22.

Status: **in progress**.  The first accuracy/performance pass has shipped:
CLUT byte order, GE write-mask semantics, GX 3D rendering, fog mapping and
vertex/lighting fast paths.  The layer-level emulation strategy for
features GX cannot express is section 4.8.  This document remains the gate
for the remaining accuracy/RTT/performance work; no full harness runs or
“spam testing” between phase gates.

Target: **Tekken: Dark Resurrection `ULUS10139` v1.00** on the native Wii
recomp port.  The Tekken 6 `ULUS10466` path must stay buildable, but is not
the visual target of this plan.

## 1. Goals

1. **Visual parity with the PSP** for the scenes that are currently wrong:
   the Bandai Namco fade, the Tekken title, the attract/menu character cards,
   character select, and the in-game HUD.
2. **Correct GE effects** everywhere: colour test, fog, absolute-difference
   blending, doubled-alpha factors, and fixed blend colours.
3. **No render-target fallback** for the card/menu effects.  The GX path must
   render GE texture targets itself, or hand them to the software path only
   for a measured, documented exception.
4. **Performance**: the attract/menu/cards and 3D scenes must stay at the
   game’s intended rate (60 Hz where the guest is 60 Hz), with a frame budget
   of 16.7 ms.  No CPU fallback may dominate a normal frame.
5. **No regressions**: the logo fade, title, attract fights, saves, audio and
   the existing tests must keep working.
6. **Production stays clean**: debug instrumentation remains under
   `-DHLE_TRACE`; the production DOL is probe-free; no game data or reference
   checkouts enter the repository.

## 2. What is true today

- The GX path is the default and draws the logo fade, title, 3D characters
  and stages.  The game leaves the logo, reaches the title and runs the
  attract-mode “GAME OVER” fights.  It no longer stops at the logo or crashes
  on the EBOOT’s null teardown callback.
- The AVC intro movie is still black; the stub advances the clock.
- The character-card/menu effects are visibly wrong (cyan where the original
  effect is red, wrong overall hue) and can be slow.
- Texture targets currently fall back to the software rasteriser.  The
  `GXPERF` counters show texture decodes/conversions in the card effect and
  the software renderer is the bottleneck there.
- The reference (`tkdr-recomp`) **does not implement** colour test, fog,
  doubled-alpha or absolute-difference blending in its GL renderer either.
  Its `NOTES.md` says so explicitly.  Its fixed-source blend is the only
  effect we could mirror directly (the `u_srcfix` / `u_fixa` shader path);
  its fixed-destination blend uses `GL_CONSTANT_COLOR`, which the GX does not
  have.
- The reference’s texture decode (`fetch`, CLUT lookup, swizzle,
  `c16_to_32`) is **identical** to ours.  A diff found no Wii-specific
  texture-reader difference.  The remaining cyan/red/UV symptoms therefore
  point at GE effects, render-target formats, or the missing RTT path, not at
  the basic texel fetch.
- **The reference is a secondary source, not proof.**  It can itself be
  wrong for a small percentage of cases, and our own ingestion could be
  wrong: ELF/PRX section extraction, relocations, `image.bin` construction,
  module/overlay loading, endianness, texture/CLUT/swizzle/stride handling,
  framebuffer formats and scanout.  The plan therefore adds an independent
  **ingestion and format audit** (section 3.5) and treats the reference as a
  hint to be checked, not as an oracle.
- The software rasteriser also lacks the same GE effects (fog, colour test,
  stencil, logic ops).  “CPU mode” is therefore not a complete visual
  reference; the PSP/PPSSPP output is.

## 3. Ground truth and test corpus (do this first)

The current loop of full Dolphin runs after every change is too coarse and
slow.  Freeze a corpus and make the work testable offline.

### 3.1 Reference frames

Capture, at the same guest scene, from **PPSSPP** (or a real PSP) and from
the current CPU and GX paths:

| ID | Scene |
| --- | --- |
| `fade-in` | first visible frame of the Bandai Namco logo |
| `fade-out` | last frame before the logo disappears |
| `title` | Tekken Dark Resurrection title |
| `attract-a`, `attract-b` | two different attract “GAME OVER” fights |
| `card-<n>` | each character card, including the effect behind it |
| `select` | character select / menu |
| `hud` | an in-game frame with 2D HUD over 3D |
| `fog` | a stage/frame that uses fog |
| `blend` | a frame that uses the problematic blend factor |

Store these **outside the repo** (game-derived images are not committed) in
`%TEMP%/opencode/tkdr-corpus/` with a `manifest.json` containing the guest
time/frame, input state, expected image hash and the PPSSPP build/version.

### 3.2 GE state capture

Use the existing `GE_CAPTURE` machinery, extended if needed, to save for one
representative display list:

- the full `GeState` (`g_ge.cmd[]`, matrices, viewport, scissor),
- VRAM and the relevant RAM,
- the target address/stride/format and display address,
- the texture sources, CLUT state and the display list bytes.

Add a **state classifier** (debug only) that prints, once per distinct
pipeline state, the fields relevant to this plan: colour test, fog, blend
equation/factors/fixed colours, alpha test, destination alpha, target
format/stride/swizzle, texture format and size.  This is how we identify
which effect is actually responsible for the card colour instead of guessing.

### 3.3 Offline comparison and ground-truth hierarchy

- CPU: replay the captured list with the software rasteriser and save a PPM.
- GX: render the same state on the Wii once and capture the screen; do not
  iterate the whole game.
- PPSSPP/real PSP: the reference image for the same scene.
- Add a small comparison script that reports per-pixel difference, the
  dominant colour histogram and the largest differing rectangles.  The gate
  is “GX matches CPU, and CPU matches PPSSPP within the documented
  threshold”, not “looks better”.

Ground truth, strongest first:

1. a real PSP capture of the same scene;
2. PPSSPP at the same guest state;
3. an independent GE specification or a hand-built test vector;
4. `tkdr-recomp` source/notes;
5. our current output.

If the reference and the PSP disagree, the PSP wins.  If our output differs
from both, the ingestion/format audit below decides whether we are reading the
game data or the GE state incorrectly.

### 3.4 Test policy from here

- No full game run while coding a feature.
- Offline/replay checks and unit tests during a phase.
- **One** Dolphin integration run per phase gate, against the frozen corpus.
- Record the run duration, frames, GXPERF counters and screenshot hashes.
- Only rebuild/deploy after a gate passes.

### 3.5 Ingestion and format audit (parallel, independent)

The reference can be wrong, and so can our own ingestion.  This audit is a
separate evidence path; it runs **in parallel** with the feature milestones
(M1–M4) so it does not delay the implementation, but no format-sensitive
scene is signed off until it has either cleared our ingestion or produced a
fix.

#### A. Executable and module ingestion

- `tools/prepare_tkdr.py`, the ELF/PRX parser, relocations, section/segment
  selection, entry point, GP, import/stub tables and module records.
- Construct `image.bin` and compare the extracted bytes and relocations with
  the decrypted EBOOT/PRX segments from the ISO.  Verify load addresses, file
  offsets, base/relocation values and **endianness**.
- Check guest RAM/VRAM/low-window initialization and the `host()` mappings.
- Module manager: `sceKernelLoadModule` / `sceKernelStartModule` are stubs.
  Determine whether the card/menu effect code or its exports live in the
  generated executable or in a PRX/overlay we never load.  If a module is
  needed, the audit must say so before we blame the renderer.

#### B. Texture and CLUT ingestion

- Re-verify `fetch()`, `clut_lookup()`, `texel_addr()`, `c16_to_32()`,
  `tc16`/`tex_bytes`, `TEXSIZE`/`TEXBUFWIDTH`/stride, swizzle, padding,
  `tex_w/tex_h` vs `tex_pw/tex_ph`, UV scale and the texture matrix.
- Check 4/8/16/32-bit indices, `clut_shift/mask/off`, CLUT entry endianness,
  the RGBA5650/5551/4444/8888 palette formats, DXT formats, and the
  texture-replacement path (`texreplace.c` / `texrep_lookup`), which may
  change the format or dimensions.
- Build **independent test vectors**: synthesize a texture for each format and
  palette, decode it with our reader, and compare against a hand-computed
  RGBA image.  Dump a real game texture and compare with a PPSSPP texture dump
  if available.  Do not use the reference’s output as the expected value for
  a format the reference may also get wrong.

#### C. Framebuffer and scanout ingestion

- GE target vs display format/stride: RGB565, RGB555, RGBA4444, RGBA8888,
  alpha, row order, stride and the `fb_decode16`/`fb_encode16` conversions.
- `gpu_scanout_scaled()` / `frontend_scanout()` and the XFB format; compare
  the CPU scanout and GX EFB output of the same guest frame.
- Render a synthetic colour-bar/checker framebuffer through both paths and
  compare; this isolates ingestion from shader/effect bugs.

#### D. ISO and data path

- ISO9660 sector/offset/lookup and the file ranges the card/menu code reads.
- Verify that the user’s ISO and `image.bin` are the supported revision and
  that no read returns a truncated or misaligned block.

#### E. Deliverable

Add `docs/INGESTION_AUDIT.md` with:

- the test vectors and their expected/actual hashes;
- byte-level diffs (executable segments, texture dumps, framebuffers);
- the module/overlay finding;
- a clean list of what is proven correct, what is fixed, and what remains
  uncertain.

Only then can a “the reference says X” conclusion be treated as evidence.

## 4. GE feature matrix

`runtime/gpu/gpu.h` already names the registers; the software and GX paths
must both implement the behaviour.  PPSSPP/the PSP GE specification is the
behavioural reference; do not copy GPL code into this MIT project.

### 4.1 Colour test

- GE: `GE_COLORTESTENABLE`, `GE_COLORTEST` (function), `GE_COLORREF`
  (reference), `GE_COLORTESTMASK`.
- Behaviour: compares the **destination framebuffer colour** (masked) with a
  reference and discards or keeps the fragment.  It is used for card/menu
  effects and masks.
- Current: not implemented in either path.
- GX problem: the GX pixel pipeline cannot read the EFB as a texture while
  drawing into it.
- Options:
  - **A. CPU fallback** for primitives with colour test.  Correct and
    straightforward; should be limited to the effect primitives.
  - **B. RTT + second pass**: render the primitive to a texture, then sample
    that texture together with the framebuffer copy in a second TEV pass.
    Accurate but requires the RTT work below.
  - **C. Stencil/alpha approximation**: reject as a general solution.
- Plan: do **not** make the CPU fallback the default for a whole GX frame.
  Implement the exact cheap form first (reject-black/alpha-test/source-colour
  mask), then use a mask texture or the RTT second pass for the general case.
  The CPU route remains the correctness reference and the fallback for tiny,
  isolated masks.  Section 4.8 defines the decision rule.
- Acceptance: the card effect matches PPSSPP, the fallback count is reported,
  and the card frame stays within the budget.

### 4.2 Fog

- GE: `GE_FOGENABLE`, `GE_FOG1`, `GE_FOG2`, `GE_FOGCOLOR`; linear,
  exponential and exponential-squared modes.
- Current: not implemented in the software or GX paths (verify with the state
  classifier before coding).
- GX: fixed-function fog (`GX_SetFog`) has linear/exp/exp2 and uses
  **eye-space z**.  The GX path loads the guest view matrix as the position
  matrix, so the eye-space z is available.  Map the GE near/far/density and
  colour to `GX_SetFog`; disable fog for through/2D prims.
- Software: implement the same formula from the reference behaviour so CPU
  and GX agree.
- Acceptance: a fog-heavy stage matches PPSSPP in colour and falloff; no
  global red/blue cast from a missing fog pass.

### 4.3 Absolute-difference blending (GE equation 5)

- GE: `out = |src - dst|`.
- Current GX: falls back to ADD; software implements the equation.
- GX has no absolute-difference blend.  Options:
  - **A. CPU fallback** for prims using equation 5.
  - **B. Two-pass subtract/max** with RTT (accurate but expensive).
- Plan: classify first.  If the effect is an isolated small mask, **A** is
  acceptable; for a full-screen card/menu layer, use **B** (or a mask
  texture) so the layer is never split between CPU and GX.  See 4.8.

### 4.4 Doubled-alpha factors (GE factors 6–9)

- GE: source/destination alpha multiplied by two, clamped.
- Current: approximated by plain `SRCALPHA`/`INVSRCALPHA`.
- GX: source 6/7 can be done exactly with the TEV alpha scale (`SCALE_2`).
  Destination 8/9 require a doubled EFB alpha; until a pre-pass exists, use
  the CPU fallback for those, or document the approximation if the game never
  uses them in a visible card effect.
- Acceptance: a known blend ramp matches the software reference; classify
  the real use first so we do not add work for unused factors.

### 4.5 Fixed destination blend colour (GE factors 10–13)

- GE: source fixed colour is already mirrored with a second TEV stage
  (`apply_fixed_source`).
- Destination fixed colour needs a constant blend factor.  The GX has none
  (no `GL_CONSTANT_COLOR` equivalent).
- Options: CPU fallback, or RTT/multipass with a constant-colour TEV stage.
- Plan: the fixed **source** factors (10/11) use the existing TEV multiply;
  the fixed **destination** factors (12/13) use the RTT constant-colour
  composite.  GE factors 0/1 are destination-colour/inverse-destination-
  colour, not zero/one; do not map them to `GX_BL_ZERO`/`GX_BL_ONE`.  Do not
  use a per-primitive CPU pass for a full-screen layer.  See 4.8.

### 4.6 Stencil, logic ops, dithering, alpha-test nuances

- The GE exposes stencil test/ops, logic ops and dithering.  The reference
  lists stencil/logic as not done.
- Logic ops and dithering are native GX features (`GX_SetLogicOp`,
  `GX_SetDither`) and should not go to the CPU.
- Stencil: `ALWAYS`/`REPLACE`/`KEEP` maps to a destination-alpha write.  A
  binary stencil mask can use the GX Z buffer as a shadow stencil for 2D
  layers.  Complex compare/incr/decr operations use a mask texture or RTT.
- Scope rule: implement only the features the classified card/menu/HUD state
  actually uses; do not expand scope without a corpus case.
- Alpha test/masks are already implemented; re-verify against the corpus after
  the blend changes.

### 4.7 Destination alpha

- Done in the last change: EFB `GX_PF_RGBA6_Z24`, clears write alpha when the
  GE clear mode asks, ordinary draws preserve it, matching the reference’s
  `glBlendFuncSeparate(..., GL_ZERO, GL_ONE)`.
- Add to the corpus: a card effect that uses destination alpha, and a
  regression check that the alpha is preserved across a list.

### 4.8 Emulating what the fixed-function GX cannot express

The target is visual parity, not cycle accuracy.  The GX is fixed-function but
its TEV, alpha/Z buffers, destination alpha, fog and copy hardware can imitate
most missing GE operations if the work is organised around **layers**, not
individual primitives.

The central rule:

> **Never mix per-primitive software output into a GX frame.**  A visible
> layer is either drawn natively on GX, approximated on GX, or rendered as a
> complete layer into a texture and composited.  The CPU fallback is a
> correctness reference and a tool for very small, isolated masks—not a
> per-primitive path in the middle of a GX scene.

This avoids the failure mode already observed: software-only stencil/colour
passes write guest VRAM that the GX display never shows, while the GX frame
keeps whatever it drew around them.

#### Per-feature tricks

| GE feature | Cheap Wii/GX representation |
| --- | --- |
| Ordinary blend, alpha test, depth, scissor, texture | Native GX |
| Logic ops | `GX_SetLogicOp` (native) |
| Dithering | `GX_SetDither` with the GE enable state |
| Fixed source blend colours (GE 10/11) | TEV constant multiply (already used) |
| Fixed destination blend colours (GE 12/13) | RTT constant-colour composite; there is no GX constant blend factor |
| Partial RGB/alpha write masks | Approximate with whole-channel GX writes; exact preserved bits need a mask/RTT pass |
| Stencil `ALWAYS` + `KEEP`/`REPLACE` | Write destination alpha with `GX_SetDstAlpha`; no test needed |
| Binary stencil mask | Use a shadow value in the GX Z buffer and map the stencil compare to a GX Z compare for 2D layers |
| Reject black / transparent | Texture/vertex alpha test, or a small mask texture |
| Arbitrary colour test | Mask texture or RTT second pass; a full-screen CPU pass is the last resort |
| Absolute difference | RTT two-pass (`src-dst` and `dst-src`, then max/composite) or an approximation where it is visually minor |
| Fixed destination colour | Exact for 0/white; otherwise RTT composite with the constant in a TEV register |
| Doubled destination alpha | RTT/alpha-copy pass; source-side doubled alpha is already exact |
| Complex stencil/stencil ops | Mask texture or RTT mask pass |
| Static card/menu mask or effect | Bake/cache as a texture keyed by state; reuse across frames |

#### Implementation patterns

1. **Native GX first.**  If the state maps to a GX fixed-function operation,
   use it directly.  Logic ops, dithering, alpha-only masks and the fixed
   source blend colours are in this class.
2. **TEV approximation.**  Use TEV stages/constants for constant colours,
   lerps, texture masks, source/destination alpha and fog.  Do not force a
   comparison the TEV cannot express; convert it to a mask instead.
3. **Z-buffer shadow stencil.**  For 2D effects where depth is not otherwise
   needed, write the stencil value into the Z buffer (via z writes), then draw
   the colour with the mapped GX depth comparison.  This gives a hardware
   mask without a render target.  It is not valid for layers that also need
   real depth; those use RTT.
4. **RTT mask/composite.**  Render the effect's mask or colour layer to an
   offscreen target, copy it to a GX texture (or staging buffer for guest
   reads), then perform one full-screen TEV composite.  This is the general
   solution for colour test, arbitrary fixed destination colours, absolute
   difference, doubled destination alpha and complex stencil.
5. **Bake / cache.**  Static card/menu masks and effect layers should be
   generated once and reused.  Cache key = source state + target format +
   palette/CLUT + generation.  Invalidate on target writes, CLUT changes or
   texture-pack changes.
6. **Layer-level fallback only.**  If none of the above is available yet, render
   the entire layer with the software path into guest VRAM and present it as a
   complete CPU frame; do not interleave it with GX draws.  This is a
   temporary correctness fallback, not the production plan for the cards.

#### Display lists and vertex submission

The GX command stream is already a display-list-like FIFO and the batching in
`wii_gxscene.c` collapses runs of equal state into one `GX_Begin`/`GX_End`.
The profile shows GX waits are near zero; ordinary display lists will not fix
the CPU vertex cost.  The useful forms are:

- **static batches** cached as pre-recorded GX command lists, reused only when
  the source state/vertices are unchanged;
- **indexed vertex arrays** with a double-buffered/limbo pool and a token
  before reuse, so the chip never reads a buffer that the CPU has refilled;
- never a shared mutable array without a token.

These are performance aids, not a substitute for the layer/mask rules above.

#### Order of work

1. Add the state classifier (already required by 3.2) and emit a per-layer
   feature mask.
2. Implement the native/cheap cases: logic ops, dither, fixed source blend
   colours, `ALWAYS`/`REPLACE` stencil, alpha-only write masks, and the
   whole-channel approximation for partial write masks.
3. Implement the Z-buffer shadow stencil for the classified 2D stencil layer.
4. Implement RTT option A (section 5) and use it for the general colour-test,
   fixed-destination and absolute-difference layers.
5. Bake static card/menu masks; only after that measure per-layer CPU time.
6. Keep the software renderer as the reference for PPSSPP comparisons and for
   tiny masks that are demonstrably cheaper in CPU.

#### Acceptance

- No visible GE layer is split between GX and software.
- Every classified missing feature has either an exact GX form, a TEV/mask
  approximation, an RTT composite, or an explicitly documented temporary
  layer fallback.
- The card/menu frame matches PPSSPP within the corpus threshold and does not
  regress the logo/title/attract scenes.
- The trick chosen for each feature is recorded in the classifier output so a
  later change cannot silently reintroduce a mixed layer.

## 5. Render-to-texture (RTT)

This is both the main card-colour risk and the main performance item.  It is
also the foundation for the general layer-level approximations in section 4.8:
colour test, arbitrary fixed-destination colours, absolute difference and
complex stencil masks all become “render the layer, then composite it” rather
than per-primitive CPU work.

### 5.1 Current behaviour

`gpu_is_display_target()` routes only double-buffered display surfaces through
GX.  Any other GE target is drawn by the software rasteriser into guest VRAM,
then decoded/converted again when the game samples it.  That is why the card
effect is slow and why target formats can look wrong.

### 5.2 Target model

For every GE target, record:

- guest address, stride, pixel format, width/height, scissor;
- whether it is a display buffer, a texture, or RAM;
- the guest texture format/swizzle the later sampling will use;
- the generation/serial that invalidates the texture cache.

Extend `gpu_is_display_target()` into a `GpuTarget` classification used by
both renderers.  The display-buffer tracking already exists in `display.c`.

### 5.3 Design options

- **A. EFB -> linear buffer -> guest target (CPU copy).**
  Render the list to the GX EFB, `GX_CopyDisp`/`GX_CopyTex` to a linear
  staging buffer, convert the pixels to the GE target format/stride, copy to
  guest VRAM, invalidate the target’s texture cache range.  Simple and
  accurate; cost is roughly one target readback per effect pass (0.5–1 MiB).
  Start here.
- **B. EFB -> GX tiled texture.**
  Use `GX_CopyTex` into a GX texture and let the GX scene sample it directly;
  the guest reads are served from the same target.  Fastest, but needs a
  mapping from guest target -> GX texture and careful token/limbo ownership.
  Promote after A if the profile says the copy is the bottleneck.
- **C. Hybrid.**
  A for targets the guest will CPU-read; B for GX-only targets.

### 5.4 Synchronization and correctness

- The GE thread is the only writer of the GX stream.  Perform the RTT copy at
  the list boundary, after the draws and before the list’s finish callback.
- Use the existing draw-sync token/limbo mechanism for the staging buffer and
  texture ownership; never reuse a buffer the chip may still read.
- On copy, bump the target’s **texture generation/serial** so the software
  `TexEntry` and GX `TexSlot` caches re-read/reconvert exactly once.
- Preserve the guest’s target **stride, format, swizzle and UV convention**;
  test a 480×272 display-sized target and a 512×512 texture target.
- Display lists that only render to a non-display target must not present a
  display frame; the frontend should keep the last display copy.
- The frontend’s CPU fallback must not show stale guest VRAM during an RTT
  frame.

### 5.5 Acceptance

- The card/menu frame has no CPU-decoded target and no cyan/red patches.
- `GXPERF` shows the texture decodes/conversions dropping to the texture
  atlas only, not per card frame.
- The RTT copy cost is measured and within the frame budget.
- The CPU, GX and PPSSPP images agree within the corpus threshold.
- No XFB/VI tearing, no use-after-free on the target, no regression in the
  logo fade/title/attract.

## 6. Card/menu effect completion

1. Capture the exact menu/card state with the state classifier.
2. Classify every active feature against section 4: colour test, fog, blend
   equation/factors, fixed colours, doubled alpha, target format.
3. Implement the missing behaviour in the order of the matrix and section 4.8:
   native GX first, then TEV/mask/Z-shadow approximations, then RTT
   composites.  Use the CPU path only as a layer-level/temporary fallback or
   for a tiny mask, never as a per-primitive insert into a GX frame.
4. Compare the card against the PPSSPP corpus at each gate.
5. Do not tune individual colours by eye.  If the effect is still wrong, the
   classifier must identify the state responsible before another change.

The cyan/red symptom is treated as evidence, not as a colour-space bug: it
may be a missing colour test, a destination-alpha/fixed-colour blend, a
render-target format, or a target sampled with the wrong UV/serial.  The
corpus and classifier decide which.

## 7. Intro movie (separate workstream)

The intro is a PSMF/H.264 movie.  The current AVC stub writes black and
advances the clock; the attract mode is the post-logo content.

Options:

- **A. Pre-decode on the PC into a Wii-friendly stream** (MJPEG/raw/I-frame
  sequence) during the user-side preparation, store it beside the user’s game
  data, and feed it through the `sceMpeg` HLE.  Best quality/perf trade-off;
  keeps the game data user-supplied; needs a prepare-tool step, storage budget
  and A/V sync.
- **B. Port a small H.264 decoder** (e.g. a baseline/Main-profile decoder) to
  Broadway.  Real-time 480×272 decode is a performance risk; license review
  required.
- **C. Full FFmpeg PPC**.  Large build; likely too slow for 30 fps.
- **D. Leave the stub black** and document; the attract/menu are reachable.

Decision: prototype **A** after the renderer work, measure size/quality/sync;
keep the stub as the fallback.  Do not hold the card/effect fixes hostage to
the movie.

## 8. Input, menu and harness

- The menu is entered with **Start**.  Map/hold it through the existing
  safe path (`PostMessage` to Dolphin’s own window; save/restore
  `WiimoteSource0` and GCPad buttons), never a system-wide key.
- Add a harness option for a single Cross (autosave/OK) and Start presses at
  the title; remove the four Cross presses that can drive the title.
- Corpus entries for the title and each card require the Start input state.
- Keep the harness’s save/restore guarantees.

## 9. Performance plan

Targets:

| Scene | Target |
| --- | --- |
| logo fade | 60 Hz frontend, no >16.7 ms frame |
| title | 60 Hz |
| attract/menu/cards | 60 Hz, no CPU target decode |
| character select | 60 Hz |
| 3D HUD/gameplay | 60 Hz where the guest is 60 Hz |

Instrumentation (debug only): texture decodes, GX conversions, cache hits/
misses, binds, draws, CPU-fallback prims, RTT copies, draw-sync waits, stage
time.  The existing `GXPERF` line is the starting point.

The larger architectural alternatives (lazy target shadow, static layer/frame
caching, native CI4/CI8 + TLUT textures, effect compiler/mask atlas, command
recording and the Dolphin-specific trade-offs) are written down in
`docs/OPTIMIZATION_IDEAS.md`.  They are options to evaluate after the current
corpus gate, not a replacement for the layer rules above.

Work:

1. Remove per-frame card target decodes via RTT (section 5).
2. Keep the CLUT/palette-keyed cache (already in) and size the GX/CPU caches
   from the corpus working set.
3. Handle colour-test/abs/fixed-dest by the section 4.8 layer rules: native/
   TEV/mask first, RTT composite for full layers.  Never fall back per
   primitive in the middle of a GX frame; use CPU only for a tiny mask or a
   temporary whole-layer path.
4. Cache static GX batches/display lists and use indexed vertex arrays with a
   double-buffered/limbo pool and token-before-reuse.  Do not use a mutable
   shared array without a token.
5. Revisit hardware lighting/skinning only after the layer work, and only if
   the profile shows a measured win.  A first hardware-lighting prototype was
   a net loss: the extra normal/colour attributes and per-state light-object
   submission cost more than the CPU lighting it removed.
6. Re-profile after each phase gate; do not add speculative optimisations.

## 10. Validation and delivery

### 10.1 Per-phase gate

- Build production and debug; the debug HLE trace/probes stay gated.
- Run the existing unit tests.
- Replay/corpus comparison (CPU vs GX vs PPSSPP) for the scene changed.
- One Dolphin integration run against the frozen corpus; record hashes,
  GXPERF and frame timing.
- Confirm no regressions in logo fade/title/attract/saves/audio.
- Production DOL probe check (`strings` for diagnostic markers = 0).
- Deploy to both dist folders; record the SHA-256.
- Update `docs/STATUS.md`; commit with a focused message.

### 10.2 Definition of done

- All corpus scenes match PPSSPP within the documented threshold.
- Card/menu effects show the correct red/cyan/alpha result; no missing
  colour-test/fog/abs/fixed-dest effect in the classified state.
- No per-frame software target decode in the card/menu scene.
- Frame times meet the table.
- The intro movie is either visibly decoded (option A/B) or explicitly
  documented as the remaining stub limitation, with the attract/menu playable.
- Production probe-free; tests pass; commit pushed; dist hashes recorded.

## 11. Execution order

| Milestone | Work | Depends on | Gate |
| --- | --- | --- | --- |
| M0 | corpus, PPSSPP captures, state classifier, offline compare | — | manifest frozen |
| M0.5 | ingestion/format audit (parallel with M1–M4) | M0 | audit cleared or fixes landed |
| M1 | target classification, RTT option A, target formats/UV | M0 | card target matches |
| M2 | native/cheap 4.8 cases + blend exactness: doubled alpha, fixed source colours, logic/dither, stencil alpha writes | M0/M1 | blend corpus matches |
| M3 | colour test, Z-shadow/mask, RTT composite for general 2D effects | M0/M1 | card mask matches |
| M4 | fog | M0 | fog corpus matches |
| M5 | card/menu integration + performance | M1–M4 | 60 Hz card/menu |
| M6 | RTT option B only if M5 is short of budget | M5 | budget met |
| M7 | intro movie prototype (option A) | M5 | video + A/V sync |
| M8 | full validation, docs, deploy | all | definition of done |

No phase starts before the previous gate passes; no “spam” full-game runs
between gates.

## 12. Risks and decisions needed

- **GX limitations**: colour test, absolute-difference and fixed destination
  blend have no direct fixed-function equivalent.  The plan uses the layer
  rules in 4.8: native/cheap GX first, TEV/mask or Z-shadow second, RTT
  multipass for the general full-screen case.  Per-primitive CPU insertion is
  forbidden; CPU is only a correctness reference, a tiny-mask tool or a
  temporary whole-layer fallback.
- **Mixed GX/software layers**: the current stencil/card experiments showed
  that software-only sublayers are invisible in the GX frame and corrupt the
  composition.  Every unsupported feature must be handled before the layer is
  submitted, or the whole layer must go through the temporary CPU path.
- **RTT formats**: the GE target’s stride/format/swizzle vs the GX copy
  format is the highest-risk part; the corpus includes 480×272 and 512×512
  targets and a UV-sensitive card.
- **Destination alpha precision**: RGBA6 has 6-bit alpha vs the GE’s 8-bit;
  validate the effect ramps against PPSSPP and document any residual.
- **Reference gap**: `tkdr-recomp` does not implement the effects we need;
  PPSSPP/PSP behaviour is the ground truth.  Do not copy GPL code; implement
  from the spec/behaviour and keep the MIT license clean.
- **Reference correctness / our ingestion**: the reference is a secondary
  source and may be wrong for a small percentage of cases; our ELF/PRX,
  `image.bin`, module, texture/CLUT and framebuffer ingestion may also be
  wrong.  Mitigation: the independent audit in 3.5, PPSSPP/PSP ground truth
  and synthetic test vectors.  A format-sensitive corpus gate does not pass
  on “the reference agrees”; it passes only when the independent evidence
  agrees too.
- **Movie**: predecoded assets are user-supplied data and must stay out of
  git; storage/quality choices need approval.
- **Input**: the menu requires Start; the harness must not send system-wide
  keys; the user’s controller configuration is saved/restored.
- **Scope**: the corpus decides which stencil/logic/dither features are
  actually needed.  Do not implement unused GE features speculatively.

## 13. Immediate next action

After this plan is approved:

1. Build the M0 corpus and state classifier, including the per-layer feature
   mask used by section 4.8 (native / TEV / Z-shadow / RTT / temporary CPU).
2. Start the M0.5 ingestion/format audit in parallel: executable/module
   extraction, synthetic texture/CLUT test vectors, and framebuffer/scanout
   checks.
3. Capture the card/menu state and implement the native/cheap 4.8 cases first:
   logic/dither, fixed source blend colours, alpha-only writes,
   `ALWAYS`/`REPLACE` stencil.
4. Implement M1 RTT option A as the general layer composite, then the Z-shadow
   mask and the remaining TEV/mask cases.  The audit runs alongside M1–M4 and
   is required before their format-sensitive gates are signed off.
5. Keep the CPU renderer as the correctness reference and for tiny masks; do
   not reintroduce per-primitive software inserts into a GX frame.

**Progress (2026-10-22/23):** the section 4.8 classifier and the native/cheap
cases (logic, dither, whole/alpha-only write masks, simple stencil) are
implemented and probe-free in the production DOL.  Partial write masks are
now drawn with whole-channel writes instead of being skipped; this makes the
profile/name keyboard, area selection and tutorial prompt visible.  A
debug-only autopilot (`EXTRA_CFLAGS=-DTKDR_AUTOPILOT`) reaches the profile
setup because Dolphin's focused `PostMessage` path does not deliver Start to
its DInput keyboard backend.  See `docs/STATUS.md` for the evidence and the
current shipped hash.  The autopilot now reaches `Choose Your Fighter` with
the character portraits/card frames and player UI visible; the remaining
profile/tutorial navigation or a reliable controller injection path is still
needed for a reusable harness.  The next renderer step is M1 RTT option A and
the general layer composite (and exact preservation of partial alpha/stencil
bits); the ingestion audit remains parallel work.  The 2026-10-23 runtime
checkpoint also fixed the libogc `__thread` aliasing, storage re-entrancy,
AESND streaming, texture-cache validation, GX strip/fan submission and 8888
scanout; its integration evidence is in `docs/STATUS.md`.  Automated Dolphin
demos are muted through the test user's `[DSP] Volume = 0`, and a release-speed
`tekken6wii-demo.dol` can be built with `EXTRA_CFLAGS=-DTKDR_AUTOPILOT` when
the debug probes make the autopilot run unnecessarily heavy.  A generated-code
speed experiment (`GEN_OPT=-O2 -fno-inline`) was rejected: it grew the DOL and
did not improve the measured menu rate; the default remains `-Os`.

The hardware-lighting prototype was reverted because it was a net loss.
(2026-10-25: it was rebuilt around the paired-single skinning kernel, with the lights loaded only when they
change, and is on by default: 0.5-0.6 ms a frame in a fight; see `docs/STATUS.md`, "The chip lights the
fighters".)

**Texture progress (2026-10-23, later):** the bulk texture decoder (exact
formats 0-5, with the generic fetch fallback) is implemented and active.  The
CI4/CI8 + TLUT GX upload path exists behind `GX_NATIVE_TEXTURES` but is disabled:
the menu uses 8888 palettes, and a forced RGB5A3 approximation produced a black
screen.  It needs a dedicated corpus/test before enabling.  The remaining
transition spikes are guest asset/state work, not ISO or GX texture upload; see
`docs/STATUS.md` for the frame-time evidence.

## Remaining avenues and potential (2026-10-23, later)

This section records the open work after the first texture and HLE experiments.
It is deliberately ordered by **evidence value**, not by how appealing the
implementation sounds.

### A. Character-specific black faces (current correctness blocker)

**Update (2026-10-23, evening):** the black regions in the native-path captures
are explained by the TLUT binding defect in section C (fixed).  The
`native-toggle` run's `native_off` frame is not evidence against the exact path
either way: its log shows only CLUTFORMAT 3 (8888) textures, which the exact-565
native path never took, so on and off rendered the same; whether that fighter's
face is a defect or dark skin and shading still needs a same-frame look at full
resolution.

Observed: Steve can render with a black face while his body is correctly lit;
Eddie was also reported with a black face; Anna was not. This character
specificity is important: it is not a global palette-order failure, which would
affect every texture. It may be a per-character face texture, palette/alpha
entry, vertex normal/material state, or a cache-slot collision.

The native RGB565 CI4/CI8 path is a suspect because the black-face screenshot
appeared in the build where that path was enabled, and the same capture with the
path disabled had a correct Steve face. That comparison was not a same-frame
comparison, so it is not proof yet.

Required isolation:

1. Capture the same guest frame with native textures on and off. Use a debug
   toggle or a Dolphin save state; do not compare frames captured seconds
   apart, because the pose/lighting changes.
2. Log the face texture state: `fmt`, `clut_fmt`, `clut_shift/mask/off`,
   address, stride, swizzle, dimensions, texture serial, and the first TLUT
   entries. Compare the native CI4/CI8 image with the exact RGBA8 path.
3. Compare the **software rasteriser** and GX at that same frame. If the CPU
   face is correct, the bug is in the GX upload/state path. If both are black,
   inspect ingestion, palette/alpha, or the model's material/normal data.
4. Check whether the face is a separate texture from the body and whether its
   cache slot is evicted/reused at the moment it is drawn.
5. Add a synthetic C4/C8 test vector for the exact tile/nibble/TLUT layout. The
   current bulk decoder is verified, but the native GX upload is not.
6. Do not re-enable the native path in production until the same-frame test
   passes. The exact RGB565 path can be validated independently of 8888.

Potential root causes, in order of likelihood:

- C4 nibble order / CI4 tile source mapping;
- CLUT index shift/mask/offset mapping or TLUT byte order;
- face texture misclassified as RGB565 when its palette is 5551/4444/8888;
- GX texture cache/slot eviction while the face is sampled;
- face material/normal/lighting state (would also affect the software path);
- missing/incorrect face texture ingestion.

### B. Exact transition smoothness without lossy palettes

The HLE trace for a transition spike showed a 578 KB asset read followed by
`sceGeDrawSync`, i.e. the guest waiting for the GE list that uploads/uses the
new asset data. Exact RGBA8 upload is the cost.

Tested and rejected: returning early from a display-only `sceGeDrawSync`. It
did not improve frame times and caused missing character art because the guest
reused buffers the GE thread was still reading.

The sound exact option is a **buffer snapshot / copy-on-write architecture**:

- on enqueue, snapshot the display-list, vertex/index and texture source ranges
  that the list may read;
- let the GE thread work from the snapshot;
- let the guest proceed after the snapshot is complete, not after the GPU draw;
- keep the last completed XFB on screen until a newer copy lands;
- retire snapshots with the existing draw-sync token/limbo machinery.

This is a larger memory/ownership project than one HLE return, but it is the
only way to make the exact path asynchronous without the race seen in the
experiment.

### C. Fast texture path (opt-in, lossy)

**Update (2026-10-23, evening):** the paletted path had four defects, all fixed:
the texture object was made with `GX_InitTexObj` + `GX_InitTexObjTlut`, which
leaves libogc's non-paletted flag set so `GX_LoadTexObj` never attaches the
TLUT (every CI texture sampled as black; this explains the all-black `ci4all`
run and the black regions in the native face crops); CI4 put the left texel in
the low nibble (the GX wants it high); the RGB5A3 TLUT had the opaque flag
inverted; and the buffer was not rounded to whole GX tiles, so a small texture's
last tile overran into a misaligned TLUT.  `GPU_SELFTEST` now checks the CI4/CI8
tile layout, a swizzled source and the RGB565/RGB5A3/8888 TLUT encodings
(265/265 pass).  The opt-in build flag `GX_FASTTEX` keeps every CI4/CI8 texture
paletted (8888 palettes in RGB5A3).  It renders the menus correctly, but it is
visibly lossy: 3-9% of the picture differs by more than 24 levels on static menu
frames (gradients and translucent overlays).  It stays off by default.

The GX can use CI4/CI8 with RGB5A3 for 8888 palettes, cutting upload size 4-8x.
It is not exact: 8-bit colour becomes 4-bit and alpha becomes 3-bit. It should
be an explicit `fasttex`/build option with a side-by-side corpus gate, not a
default. It could remove much of the transition upload stall if the quality
loss is acceptable.

### D. Render-to-texture and layer effects

RTT option A still has no corpus scene: the captured menu reports `rtt=0`,
`skip=0`, `feat=0987` and only partial write masks. Implement in this order:

1. exact RGB565/8888 texture correctness and face regression;
2. lazy target shadow (GX texture + dirty CPU tiles) for a real offscreen
   target;
3. one layer-level TEV composite for fixed-destination/absolute-difference/
   colour-test effects;
4. static mask atlas / Z-shadow stencil for cards;
5. only then consider per-primitive CPU as a tiny-mask fallback.

Do not add general RTT until a captured scene exercises it; otherwise it is
unmeasured complexity.

### E. Static layer/frame cache

A four-entry display-list cache was prototyped. It reached cache hits, but the
command/raw-vertex hashing cost roughly cancelled the saved GX draws, and the
transition frames did not benefit. Keep this as a low-priority idea; revisit
only with a cheap change detector (guest write generations or page dirty bits)
and a profile that shows repeated expensive layers.

### F. Command and vertex recording

Potential work for fights, not the current menu:

- preformatted GX vertex arrays and indexed 16-bit submission;
- recorded GX display lists keyed by state/content, replayed only when inputs
  are unchanged (measured 2026-10-24, not built: the guest rewrites every plain
  draw's vertices each frame, so a replay still has to hash them, and each
  CALL sub-list holds about one draw; see `docs/STATUS.md`, "Fight frame rate,
  second pass");
- double-buffered vertex pools with a draw-sync token before reuse;
- static 2D batches cached separately from dynamic 3D batches.

Requires a profile showing GX submission, not guest skinning/lighting, is the
next bottleneck.

### G. Vertex pipeline and hand-HLE

**Update (2026-10-24):** the fight profile found larger items than skinning
arithmetic: the VFPU interpreter (36,000 calls and 12.5 ms a frame; now
translated by `recomp/vfpu_emit.py`), general per-vertex lighting (now
flattened for the fighters' directional lights), and decoding the stage
every frame (now a draw cache, `runtime/gpu/vcache.c`, plus whole-draw
skinned decoding straight into the chip's vertex form).  Fights hold 30 fps
in the scenes measured; paired-single skinning was not needed.  See
`docs/STATUS.md`, "Fight frame rate".

- paired-single skinning/lighting and a fused decode-to-GX path;
- cache transformed vertices by bone generation only if a real scene shows
  reuse (the previous decoded-vertex cache measured zero hits);
- hand-HLE of hot guest routines only after a same-frame corpus test; the
  transition ring shows file/audio/GE state work, so a hasty HLE is risky.

### H. Ingestion and corpus

The independent ingestion/format audit remains valuable and is still missing
`docs/INGESTION_AUDIT.md`. Priorities:

- face texture raw/decoded/CLUT dumps for Steve/Eddie/Anna;
- synthetic C4/C8/TLUT GX vectors;
- framebuffer/scanout comparison for the same guest frame;
- frozen PPSSPP/PSP captures outside the repository;
- executable/module/overlay audit only if a missing module is suspected.

### I. Acceptance order

1. Face regression isolated and fixed; native texture path either proven exact
   or disabled.
2. Same-frame native off/on comparison recorded in `STATUS.md`.
3. Exact transition decision: snapshot architecture or accepted stutter.
4. Fast texture path, if desired, gated and visually compared.
5. RTT/effects only against a captured corpus scene.
6. Command/vertex/paired-single work only against a fight profile.

No speculative palette approximation or per-primitive software insertion should
reach production while the face bug is unresolved.
