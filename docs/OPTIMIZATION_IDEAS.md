# TKDR Wii - high-level optimization and effect-simulation ideas

This document records the larger architectural options that are alternatives to
continuing to tune the current per-primitive GE-to-GX translator.  It is a
design document, not a claim that every item has been implemented.  The
current shipped renderer and the measured results remain authoritative; see
`docs/STATUS.md` and `docs/PLAN.md`.

Progress: the exact bulk texture decoder (3.3) is implemented and active.  The
CI4/CI8 + TLUT upload code exists behind `GX_NATIVE_TEXTURES`, but is disabled
in production: the menu uses 8888 palettes, and forcing those through RGB5A3
produced a black screen.  It needs a dedicated corpus/test before it can be
enabled.  The other items in this document are still design options.

## 1. What the measurements say

The latest debug autopilot run (60 s, character select/menu scene) produced:

- one GE display list per guest frame (`gxl = guest`);
- one GX display copy per list (`gxp = gxl`), about 52 GX batches/frame;
- `skip=0`, `rtt=0` and `feat=0987` (through, texture, alpha test, dither,
  write mask);
- the only classified imperfect feature is partial write masks;
- GE thread about 4.5 s of the 60 s run; guest compute about 28 s; vblank
  waiting about 32 s;
- vblank timer overshoot about 11 us, so missed vblinks are not caused by a
  coarse sleep.

A frame-interval histogram from the same run is more useful than the average
frame rate:

```text
FRAMETIME n=2400 <17.5=1904 17.5-25=63 25-34=210 34-50=135 >50=87
```

Most frames are within one 60 Hz interval, but roughly a fifth are late.  The
late frames are concentrated around scene transitions (the same counts appeared
with the Dolphin Hybrid/Ubershader setting), so the visible stutter is a
mixture of transition/asset work and occasional renderer/emulator hitches, not
a uniform 45 fps CPU ceiling.  Any optimization should be judged with the
histogram, not only the average guest-frame count.

The practical conclusion for Dolphin runs is:

- optimizing C-level arithmetic (for example single-precision `f2i`, or
  generated-code `-O2`) can help a real Wii but has little effect while Dolphin's
  PPC JIT is the host CPU;
- reducing guest instructions, GX state changes, texture conversions and
  full-frame readbacks matters more;
- the menu does not currently exercise RTT, colour test or complex stencil, so
  implementing those features speculatively will not improve the frames being
  measured.  Capture the scenes that need them first.

## 2. Design principle: layers and resources, not primitives

The current pipeline translates each GE primitive independently, then tries to
cache state around it.  The bigger alternative is:

> Treat the guest's output as a set of resources and layers.  Compile or bake
> each layer into one of a small number of GX representations, and only fall
> back to a more expensive representation at layer boundaries.

A possible frame flow:

```text
guest GE list
  -> list/state/content hash
  -> pass classifier
  -> frame graph of layers
       native GX layer
       TEV/mask layer
       render-target composite layer
       cached/baked layer
  -> GX command recorder / vertex arrays
  -> target shadow (GX texture + lazily materialised CPU tiles)
```

This replaces "choose a path for every primitive" with "choose a path for
every visible layer, then cache it."  It also gives the renderer a place to
reuse work across frames, which the current per-list execution does not.

## 3. Candidate overhauls

### 3.1 Lazy GX target shadow (recommended RTT alternative)

Instead of RTT option A's full EFB readback and guest-VRAM conversion for every
target pass:

- render a target pass to the GX EFB and copy it to a GX texture (`GX_CopyTex`);
- keep the target as a **GPU resource** with a guest-address key, format,
  stride, dimensions and generation;
- keep a CPU-visible shadow with **dirty tile** bits;
- when the guest or the software rasteriser reads the target, detile and
  convert only the dirty tiles;
- when the guest writes or a transfer touches the target, invalidate the
  corresponding GPU tiles and re-upload lazily;
- when a later GX pass samples the target, bind the GX texture directly.

This removes the common full-target readback and keeps the guest-visible bytes
correct.  It is the option B idea from `docs/PLAN.md`, but with tile-level
dirty tracking rather than a whole-target copy.

Open problems:

- GX has one EFB.  An offscreen pass in the middle of a display list still
  needs the display EFB preserved.  A frame graph can order independent
  offscreen passes before display composition; mid-list cases need a
  save/restore strategy or a documented software fallback.
- The target texture copy is still a serialisation point.  Batch several
  draws into one target pass before copying.
- The copy format, tiling, alpha precision (RGBA6) and guest target format
  must be validated against the corpus.

### 3.2 Static layer and frame caching

Menus and card screens repeat identical geometry, state and textures every
frame.  The guest still runs, but the renderer can avoid rebuilding the same
GX work:

- hash the display-list bytes plus the referenced state, vertex ranges, CLUT
  generation and texture serials;
- if the layer is unchanged, replay a recorded GX command batch or reuse the
  last composited texture/XFB;
- cache **parts** of a layer (static background, card frame, text) and redraw
  only animated elements;
- for a completely static screen, present the previous frame without executing
  the same GX submission again.

The difficult part is reliable change detection.  Hashing a few hundred KiB
per frame is cheap compared with decoding and submitting vertices, but the hash
must include every input that affects the output.  A page-write bitmap in the
guest memory path is an alternative, but it adds a cost to every guest store
and must also track DMA and guest transfers.

### 3.3 Native texture formats: CI4/CI8 + TLUT

Most PSP game textures are paletted (4-bit or 8-bit) with a small CLUT.  The
current GX path decodes the palette to RGBA, then expands every texel to an
RGBA8 GX texture.  A native path would:

- keep the raw 4-bit/8-bit indices;
- use `GX_TF_CI4`/`GX_TF_CI8` and a GX TLUT;
- update only the TLUT when the palette changes;
- use RGB565/RGB5A3 for direct-colour textures where the format allows it;
- fall back to RGBA8 only for formats GX cannot represent (for example a true
  8888 palette).

This cuts texture memory and conversion work by roughly 4x for the paletted
case and reduces the number of texture uploads.  It is a contained change in
the texture/GX layer and can be tested independently of GE effects.

The PSP 4-bit tiling, CLUT ordering and GX CI4 tiling are not the same; the
conversion must be proven with synthetic vectors and real texture dumps.  A
palette change must not require a complete texture re-upload.

### 3.4 Effect compiler and mask atlas

Replace the hand-written per-feature branches with a small compiler/cache:

- classify each layer into native GX, TEV/mask, target composite, cached/bake
  or temporary CPU;
- derive the TEV colour/alpha equations, blend factors, fog and texture
  function once per state hash;
- cache the resulting GX state and, where useful, a recorded command list;
- bake static masks (card frames, stencil masks, colour-test masks) into
  textures once and reuse them;
- keep a per-layer feature mask so a later change cannot silently mix a GX
  layer with software output.

The cache key must include target format/stride, CLUT generation, texture
serial, write masks and the blend/test state.  The classifier output should
record which representation was chosen for each layer.

### 3.5 Render-target composites instead of per-primitive fallbacks

For destination-dependent effects (colour test against the framebuffer,
fixed destination blend, absolute difference, doubled destination alpha and
complex stencil), render the **affected layer** to a target texture, then do
one full-screen TEV composite.  This is more work than a native blend but far
cheaper than a CPU fallback per primitive, and it avoids splitting one visible
layer between GX and software.

Possible mappings:

| GE feature | Cheap representation |
| --- | --- |
| Source colour key | TEV/alpha compare, if the classified semantics are source-side |
| Destination colour test | Layer mask or one target-copy composite |
| Simple stencil ALWAYS/REPLACE/KEEP | destination-alpha write, already native |
| 2D binary stencil | Z-buffer shadow stencil (no EFB read) |
| Complex stencil | baked mask texture or one RTT mask pass |
| Fixed source blend colour | existing TEV multiply |
| Fixed destination blend colour | one layer composite with the constant in TEV |
| Absolute difference | two-pass subtract/max composite at layer level |
| Doubled source alpha | TEV alpha scale, already native |
| Doubled destination alpha | alpha-copy/pre-pass, not a plain SRCALPHA approximation |
| Partial RGB write mask | baked mask + one TEV composite |
| Partial alpha write mask | alpha-only GX write where possible; mask otherwise |
| Fog | native GX fog (linear/exp/exp2) |
| Logic/dither | native GX |

Do not use a per-primitive CPU fallback in the middle of a GX frame.  The CPU
renderer remains the correctness reference and a tool for tiny isolated masks.

### 3.6 Command and vertex recording

Implemented (2026-10-24): vertices are produced in the chip's own form
(`GxVtx`), and non-skinned draws are cached in that form by address and a
hash of everything they depend on (`runtime/gpu/vcache.c`; see
`docs/STATUS.md`).  GX display lists and indexed arrays were not needed: with
the vertices cached, sending them costs 1 us a draw.

The GX path currently decodes vertices into `Vtx`, converts them to a float
array, then emits them in direct mode.  Alternatives:

- **Preformatted vertex arrays** (position/colour/UV) uploaded once per batch;
- indexed 16-bit vertex arrays to reduce FIFO traffic;
- recorded GX command lists replayed when a batch is unchanged;
- double-buffered vertex pools with a draw-sync token before reuse;
- static 2D batches cached separately from dynamic 3D batches.

This reduces CPU and Dolphin JIT/FIFO work.  It does not replace the guest
vertex/skinning work, so profile before making it the main effort.

### 3.7 Vertex pipeline: paired-single and fused decode

The Broadway has paired-single FPU support.  A specialized vertex path could:

- process two components/vertices at once in skinning and lighting;
- fuse decode, transform and GX-format packing into one pass;
- cache transformed vertices by bone generation where the source mesh is
  reused;
- skip normal fetches when lighting is off (already done).

Hardware GX lighting/skinning was previously a net loss because of extra
attributes and per-state light submission.  Do not repeat that without a
measured profile.

### 3.8 Hand-HLE of hot guest routines (last resort)

The hot menu function sampled in the current profile is guest UI/vertex-setup
code.  Dolphin must JIT every PPC instruction.  A hand-written C replacement
for a well-understood, high-frequency guest routine could skip a large amount
of emulated work, but it is game-specific and an accuracy risk.  It should
only be attempted after:

- a running-only PC histogram identifies the function;
- its inputs/outputs and side effects are documented;
- the corpus shows identical output;
- the gain is measured against the release build, not only the debug build.

## 4. Why RTT option A is not the first step

`docs/PLAN.md` describes RTT option A as "render to EFB, copy to a linear
staging buffer, convert to the GE target format, copy to guest VRAM."  That is
correct but expensive if used generally:

- a full 512x512 target is about 1 MiB per readback;
- the copy serialises the GPU and CPU;
- the guest often samples the target again, so it is uploaded back to GX;
- the display EFB may need to be preserved around an offscreen pass in the
  middle of a list.

The menu measurements show `rtt=0`, so this is not currently the bottleneck.
Implement it only for classified target scenes, preferably as the lazy target
shadow (3.1) or as a layer-level composite.  For static card effects, baking
the final mask/composite once is usually cheaper than any per-frame RTT path.

## 5. Dolphin-specific considerations

The production target is still a native Wii binary, but most testing (and the
user's current play path) is Dolphin.  For that path:

- Dolphin JITs the recompiled PPC code, so C-level micro-optimizations can be
  invisible; prefer fewer guest instructions and fewer GX pipeline states.
- A new TEV/blend/texture pipeline combination can trigger a new Dolphin
  shader/pipeline.  Bound the number of states, cache them, and pre-warm a
  deterministic corpus.  Ubershaders/Hybrid can remove compile stalls but did
  not change the measured frame-interval histogram in the test configuration,
  so verify with the shader log before treating it as the fix.
- Avoid full EFB readbacks in the normal path; they are especially costly in
  Dolphin.
- Native CI4/TLUT and recorded command lists reduce the amount of GX emulation
  work per frame.
- Keep the test user's audio muted and preserve the user's controller
  configuration; performance experiments must not change those defaults.

## 6. Recommended order

1. **Measure a frame-interval histogram and classify the actual scene state.**
   Capture the state for the card, select, HUD, fog and blend scenes; record
   which effects are really used.
2. **Native texture formats (CI4/CI8 + TLUT) and palette-aware caching.**  Low
   semantic risk, contained in the texture/GX layer.
3. **Static 2D menu/card layer cache.**  Detect unchanged display-list content
   and replay or reuse the composited result.
4. **Lazy target shadow / RTT option B.**  Only materialise target pixels on a
   guest read; keep GX texture binding for GX sampling.
5. **Effect compiler, mask atlas and Z-shadow stencil.**  Implement only the
   classified effects; use the corpus as the gate.
6. **Command/vertex recording and paired-single vertex work.**  Focus on the
   fight/attract scenes if they remain CPU-bound.
7. **Hand-HLE hot guest routines** only if the running-only profile and release
   measurements justify it.

Each step should have one Dolphin integration run against the frozen corpus,
not a full-game run.  Production must stay probe-free; diagnostics stay behind
`-DHLE_TRACE`.

## 7. Non-goals and pitfalls

- Do not add per-primitive software insertion into a GX frame.
- Do not implement all GE features speculatively; the menu does not use them.
- Do not use a full-screen CPU pass for a layer that GX can draw, or a
  per-frame RTT readback for a static effect that can be baked.
- Do not weaken validation checks or unit tests to make a shortcut pass.
- Do not commit game data, reference images or ISO files.
- Do not assume Dolphin's shader cache is the stutter cause without measuring
  the frame-interval histogram and checking the shader log.

## 8. Evidence to collect next

- A frame-time histogram for title, card, select, transition and fight scenes,
  separated from loading.
- The actual partial-mask values and their layer context.
- A state-classifier dump for the first frame that uses colour test, complex
  stencil, fixed-destination blend or a non-trivial RTT target.
- A PPSSPP/real-PSP reference image for each effect, stored outside the repo.
- GXPERF and shader-cache logs for the same scenes, to distinguish guest work,
  GX state churn, texture conversion and shader compilation.
