# Status and evidence

Date: 2026-10-22. This file records what was actually exercised on the build
machine, not what is planned.

## Build

- Toolchain: devkitPPC GCC 15.2.0, libogc (2026 devkitPro install),
  libfat, AESND, WiiUse/BTE, Windows + devkitPro msys2 make.
- Launcher-only build (`stub/`, no game code):
  - `make` produces `tekken6wii.dol`.
  - ELF sections: ~0.8 MiB text, ~0.2 MiB data, ~4.7 MiB BSS.
- Generated-code build (synthetic fixture in `tests/make_tk6_fixture.py`):
  - `python tests/make_tk6_fixture.py` + `recomp.tk6.generate()` produce
    `func_table.c`, `funcs_000.c`, `image_base.h`, `image.bin`.
  - The generated C compiles and links into the same DOL; `make` succeeds.
- Generator unit test: `python -m unittest discover -s tests -v` passes.

## Dolphin run (synthetic fixture)

Dolphin 2609, JIT64, D3D11, HLE audio, test user folder with a folder-backed
FAT SD image.

Observed in Dolphin's OSReport log:

```text
WII storage: sd=mounted
WII storage: data found in sd:/apps/tekken6wii
WII main: entering boot
WII boot: allocating guest memory
WII memory: guest RAM 0x90002000 (32 MiB), VRAM 0x92002000 (2 MiB), low 0x92202000
WII boot: loading sd:/apps/tekken6wii/image.bin
WII boot: opening ISO sd:/apps/tekken6wii/tekken6.iso
WII boot: initialising GX
WII boot: initialising input
WII boot: initialising audio
WII audio: 44.1 kHz -> 48 kHz, AESND callback
HLE: 0/1 imports implemented
WII boot: entry 08804124, image 08804000+4018, free 08808100..0A000000
WII boot: starting guest scheduler
WII thread: host entry 08804124
WII thread: 08804124 got baton
WII recomp: first dispatch 08804124 (ra=00000000 sp=09FFFF90)
all threads finished or deadlocked
```

This verifies the boot chain, 32 MiB MEM2 guest window, GX setup, input init,
AESND init, LWP pthread scheduling, guest dispatch and generated-code
execution. The fixture deliberately returns from its entry and ends with no
runnable guest threads.

The fixture's fake ISO is not a real ISO9660 image; it was only used to reach
the scheduler. No game code from Tekken 6 is present.

## Dolphin core-init fix and launcher run

The first DOL built after the TKDR path changes failed Dolphin with
`Failed to init core`. The Dolphin log showed unaligned DOL text/data section
sizes (devkitPPC's `elf2dol` copies ELF sizes verbatim). The linker cannot
force alignment at the DOL layer, so `tools/pad_dol.py --data` rewrites the
DOL with 32-byte-aligned text/data sections while keeping load addresses and
BSS fixed. The Makefile now runs this automatically after `elf2dol`. The
resulting DOL passes core init.

Launcher run in Dolphin (SD-backed folder with fake `image.bin`/`tekken_dr.iso`):

```text
Tekken Wii - PSP static recompilation (TKDR / Tekken 6)
WII storage: sd=mounted
WII storage: data found in sd:/apps/tkdr
WII storage: saves in sd:/apps/tkdr/ms0
WII main: install=sd:/apps/tkdr ...
WII boot: allocating guest memory
WII memory: guest RAM 0x90002000 (32 MiB), VRAM 0x92002000 ...
WII boot: loading sd:/apps/tkdr/image.bin
Boot failed: this DOL has no recompiled game code ...
```

That is the expected launcher-only result: the path/selection/memory/save
logic runs, and the DOL correctly refuses to run without a generated game
image. A real TKDR build still needs the decrypted EBOOT and generated
`image.bin`.

## TKDR dump and full build

PPSSPP 1.20.4 was used locally to dump the decrypted EBOOT from the verified
ISO. The dump is a valid ELF32 MIPS `tkpsp` PRX, 4,836,160 bytes. It remains a
local, git-ignored game-derived file and is not part of this repository.

`tools/prepare_tkdr.py` then generated:

```text
functions  13809
imports      280
image    20629396 bytes (0x13AC794)
base     0x08804000
entry    0x089F34A8
```

The full Wii DOL builds with the current runtime (text ~8.5 MiB, data ~0.9 MiB,
BSS ~4.95 MiB; DOL 9,428,032 bytes after section alignment). `make dist-tkdr`
stages it together with the 20.6 MiB `image.bin`.

A Dolphin run with this full generated image and a small fake ISO (only to
exercise boot; no game assets) reached:

```text
HLE: 213/280 imports implemented
WII boot: entry 089F34A8, image 08804000+13AC794
WII boot: starting guest scheduler
WII recomp: first dispatch 089F34A8 (ra=00000000 sp=09FFFF90)
```

The first real TKDR instruction stream executes. A full-game Dolphin run with
the 1.75 GB ISO was not completed on this machine because the emulated SD image
needs ~1.8 GB of contiguous free space and the available drives could not hold
both the test image and the source ISO. That is a test-harness storage limit,
not a runtime error.

## Dark Resurrection ISO verification

The ISO at `C:\Users\sherv\Downloads\a-tekken.iso` was inspected and hashed:

```text
size   1750237184
sha1   8193a65dee49685db49284408c89bdb35324bc32
crc32  3ffd7a9d
game   Tekken: Dark Resurrection (ULUS10139) v1.00
```

This matches tkdr-recomp's supported dump exactly. `tools/prepare_tkdr.py
--verify-only` prints `OK: ISO matches tkdr-recomp's supported dump.` The
ISO's `PSP_GAME/SYSDIR/EBOOT.BIN` is still encrypted (`~PSP`, 4,836,160
bytes); `prepare_tkdr.py` rejects it with instructions to dump a decrypted
EBOOT with PPSSPP. Full TKDR generation has therefore not run yet.

## Not yet exercised

- Real `ULUS10466` EBOOT/ISO: not present on the build machine. The generator's
  sectionless parsing, SHA-256 gate, import extraction and startup discovery
  are implemented and unit-tested on a synthetic module, but the full game has
  not been booted.
- Visual output of the GX scanout (the run reached GX init; no screenshot was
  captured).
- Audio output on hardware/Dolphin (AESND init only).
- Save/load round trip through the PSP utility HLE.
- NTFS USB on physical hardware (libntfs is the same read-only port used by
  reVC-Wii; only FAT SD was exercised here).
- Performance: the CPU GE rasterizer on Broadway is unmeasured. Expect this
  to be the limiting factor until a GX renderer exists.

## Rebuild notes

- `gen/` holds game-derived output and is git-ignored. `make clean` removes
  `build/` but not `gen/`.
- If switching between launcher-only and generated code, run
  `make clean` after creating or removing `gen/`; the include path is chosen
  when make starts, and stale `boot.o` dependencies can otherwise keep the
  stub `image_base.h`.
## Real-ISO Dolphin run and E: deployment (latest)

The complete `dist-tkdr/apps/tkdr` folder was deployed to
`E:\apps\tkdr` together with `tekken_dr.iso`:

```text
boot.dol       9,428,448
image.bin     20,629,396
tekken_dr.iso 1,750,237,184
```

A temporary 1.85 GB FAT32 SD image was built on E: and Dolphin booted the
full generated image against the real ISO. The run reached:

```text
WII storage: data found in sd:/apps/tkdr
WII boot: opening ISO sd:/apps/tkdr/tekken_dr.iso
HLE: 213/280 imports implemented
WII boot: entry 089F34A8, image 08804000+13AC794
WII recomp: first dispatch 089F34A8
```

It did not reach the title screen or gameplay within the test window, and no
trap or missing-import line followed the first dispatch. The next debugging
target is the guest scheduling/HLE path after module_start; the deployment
itself is complete.

## Latest Dolphin debugging (2026-10-20, scheduler/endianness fixes)

Three runtime defects were found and fixed while following the first real
TKDR startup:

1. **Per-thread `$k0` context.** The generated PSP libc stores each thread's
   reent pointer at `[$k0+4]`. `thread_prepare()` left `r26` zero, so
   `__getreent()` printed `libc:%s: no reent structure` and startup entered
   the invalid finalizer path. Each emulated thread now gets a zeroed 0x100-byte
   guest context and `r26` points at it.
2. **Little-endian guest memory.** The Wii PowerPC is big-endian; the raw
   `memcpy()` in `mem_r32/mem_r16/mem_w32/mem_w16` read the PSP image's
   little-endian pointers byte-swapped (for example `0x08A600C8` became
   `0xC800A608`). The four accessors now byte-swap only under `__wii__`.
3. **Scheduler TLS restoration.** `tls_self` is process-global because only
   one guest thread runs at a time. When a sleeping thread resumed, it could
   retain the other thread's identity. `reschedule()` now restores
   `tls_self = self` on every baton handoff/wake.

Verified result with the real ISO and full generated image:

```text
HLE: 213/280 imports implemented
HLE[300]: sceGeListEnQueue ra=08A252C8
WII frontend: frame 120, guest frames=48, HLE calls=1695, host threads=8
raster: 0 worker threads
```

No TRAP and no unimplemented-import line occurred. The run then stops producing
frontend frames immediately after the first GE list reaches the rasterizer.
The CPU thread was still attributed `RUNNING`; the next debugging target is
the GE list/rasterizer path (list contents, primitive count, and locks around
`raster_flush`/scanout), not guest module startup. The title screen and
gameplay have not been reached.

The temporary per-function probe ring and verbose scheduler/semaphore logs used
to find these defects have been removed. A clean production build (no
`WII_OSREPORT`) now succeeds and has been copied to both
`dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`; `tekken6wii-debug.dol` preserves the
matching OSReport build for the next GE/rasterizer investigation.

## Automated Dolphin test and screenshots (2026-10-20)

`tools/dolphin_autotest.ps1` launches the debug DOL, forces the Dolphin window
foreground with `AttachThreadInput`, clicks the client area, sends a scripted
sequence (Enter, Z, Down, Z), and captures client screenshots at 5, 12, 20, 28,
and 36 seconds.

Dolphin's controller ports were enabled in the test user folder:

```ini
[Core]
SIDevice0 = 6
WiimoteSource0 = 1
```

Input was verified end-to-end with the runtime debug build:

```text
INPUT: pad=0200 wpad=00000000 -> 4000
CTRL: buttons=4000
```

`pad=0200` is the GameCube B button; `0x4000` is PSP Cross. The game was also
observed making repeated `sceCtrlReadBufferPositive` HLE calls, so controller
input is reaching the guest loop.

The runtime remained alive:

```text
WII frontend: frame 60, guest frames=22, HLE calls=1160, host threads=8
HLE[1282]: sceKernelSignalSema
```

However, the screenshots are black because the scanout buffer itself is black:

```text
FRONT 60: nonblack=0 first=FF000000
```

The window screenshots and the guest framebuffer agree, so this is not a host
capture artifact. The next blocker is the rendered image: at frame 60 the
game is polling input and rendering 22 guest frames, but every scanned-out
pixel is black. Title-screen and gameplay screenshots therefore do not yet
exist.

The automated screenshot outputs were kept outside the repository at
`%TEMP%\opencode\tekken-auto-*`; the harness itself is in
`tools/dolphin_autotest.ps1`.

## Debug-loop result (2026-10-20, final pass)

The first real blocker was not the scanout. While list 16 was running, the
GPU thread reached `GE_END`, queued the finish callback, and then blocked in
`sched_wake_idle()` trying to acquire the guest scheduler mutex. A guest thread
held that mutex while waiting for the GPU list. `sched_wake_idle()` is now
lock-free (atomic wake flag plus condition broadcast), and the GPU list and
callback complete.

The next starvation was the PSP audio thread: `sceAudioOutputPannedBlocking`
returned before its delay because `RET()` exits the HLE function, so the
high-priority SasCore worker never yielded. The blocking path now waits before
returning, and the guest uses a safe memory-access scheduling tick plus
thread-local host identity so the frontend and GPU callbacks are not starved by
long guest loops.

Additional contract fix: `sceKernelStartModule()` now returns `0` on success,
as the PSP API specifies, instead of returning the module UID.

Current verified limits:

- GPU lists and finish callbacks complete.
- `user_main` can run; `sceCtrlReadBufferPositive` and audio HLE continue.
- The renderer is still receiving only full-screen black clears
  (`color=(0,0,0,0)`, `clear=1/1`), so the scanned framebuffer remains
  `nz=0`.
- After roughly 1.6k HLE calls, `user_main` enters a long guest-side section
  with no HLE calls and every frame is still a black clear. No title/menu or
  gameplay frame has been reached.
- The temporary broad priority clamp used to test the audio starvation was
  removed; normal PSP thread priorities are restored in the production build.

The production DOL was rebuilt after removing tracing and is staged in
`dist-tkdr` and `dist-tkdr-sd-full`:

## Title screen reached; colour and performance work (2026-10-21)

The game now **advances past the Bandai Namco logo and renders the TEKKEN
DARK RESURRECTION title screen** in Dolphin.  This is the first real
in-game frame on this port.  Getting there fixed four separate defects,
and the colour cast and the frame rate were addressed in the same pass.

### 1. Host stack overflow (the DSI)

The production build died with a DSI whose report showed `PC == LR ==
0x80b00b254` -- a corrupted return address, i.e. a stack overflow rather
than a wild pointer.  Every guest `CALL` becomes a C call in the
recompiled code, so a deep guest call chain consumes the *host* stack of
the thread running it.  Guest threads were given only a 192 KiB host
stack (`runtime/hie/thread.c`, `platform/wii/pthread_lwp.c`); the
overflow smashed the return address and the CPU branched to garbage.
Guest threads now get 2 MiB, and `pthread_attr_setstacksize` accepts up
to 8 MiB.

### 2. Module handles returned to the guest

`sceKernelGetBlockHeadAddr` returned `0x800200CC` -- a *host* address --
when the block id was unknown, and the guest dereferenced it as a PSP
pointer.  It now returns a real guest address or 0.
`sceKernelLoadModule` also used to hand back a bare counter; it now
allocates a real (zeroed) module TOC in guest RAM and records it, and
`sceKernelGetModuleIdByAddress` resolves against that table.

### 3. Guest memory outside the mapped windows

`host()` funnelled every address outside the user-RAM and VRAM windows
into a single 1 MiB scratch page, so two different guest addresses
aliased onto the same bytes: a table read through one address returned
another address' data, and the guest then called whatever it found.  The
scratch region now covers the PSP-3000's extended user RAM
(0x0A000000..0x0A1FFFFFF) one byte per guest byte, so distinct
addresses never alias.  `guest_span()` and `guest_host_ptr()` were
extended to match, so reads and writes there are no longer silently
dropped.  Out-of-range accesses are counted and reported in the debug
build.

### 4. Red and blue were swapped

`frontend_scanout()` writes a pixel as `R | G<<8 | B<<16 | A<<24` (R in
the *low* byte), but `upload_scanout()` in `platform/wii/wii_video.c`
unpacked RGB565 using the big-endian ARGB layout -- R from bit 19, B from
bit 3.  Red and blue were transposed, which is why the Bandai Namco logo
rendered purple instead of magenta.  The unpack now reads the bytes the
scanout actually produces.

### 5. Host thread stacks exhausted MEM1

Raising the host thread stack to 2 MiB fixed the DSI, but it created a
second failure.  MEM1 on the Wii is 24 MiB, the game runs about a dozen
guest threads, and every one of them took a 2 MiB host stack from the
MEM1 heap.  Nine of them fitted; the tenth allocation failed, and
`pthread_create` failing called `exit(1)` -- so the emulator simply
stopped, part way through the intro, with no crash report:

```text
WII frontend: frame 2460, guest frames=976, HLE calls=38907, ...
pthread_create failed
```

Three changes:

- The default host stack is 1 MiB (the cap is 4 MiB), so a dozen threads
  fit in MEM1 with room to spare.
- `pthread_create` retries with a smaller stack if the allocation fails,
  down to a floor of 256 KiB.  A thread that cannot get the stack it asked
  for still runs.
- A thread that cannot be started at all no longer takes the emulator down
  with it: it fails that one thread and the game carries on.

Each host stack also carries a canary at its bottom now.  A guest call
chain that runs off the end of the stack used to smash the return address
and leave the CPU branching somewhere wild, which is very hard to
diagnose; the canary turns it into a message naming the thread.

### Performance

Measured with the debug build's `HLE_TRACE` counters (120 s runs):

```text
raster: px=56M texdec=4 prims=250098 tri_setup=54665ms
        pixstate=894ms verts=525ms vformat=88ms beginstate=75ms
  frontend present: 14569 ms
```

Two separate costs dominate a run, and finding them was the work:

**The frontend was converting every pixel twice.**  `frontend_scanout()`
turned the guest's 16-bit framebuffer into 32-bit ARGB (130k conversions a
frame), and then `upload_scanout()` turned that into RGB565 in the GX's
tiled layout (another 130k).  That cost **49.3 s of a 120 s run**.  The
frontend now converts the guest frame straight to RGB565 once and the
video layer only tiles it, which is a copy: **49.3 s -> 14.6 s**, a 3.4x
reduction in the frontend's share of the run.  The change also removed the
intermediate 32-bit buffer (520 KB) entirely.

**The rasterizer's per-pixel loop costs ~1.0 us/pixel.**  That is 56M
pixels in 54.7 s, and it is essentially all of `tri_setup`; the per-
primitive costs (vertex format, begin state, pixel state, vertex decode)
add up to under 2 s between them.

Two hypotheses for the per-pixel cost were tested and rejected:

- *Spilling in the pixel loop.*  `draw_triangle` compiled to 2821
  instructions with the whole shader inlined into it, well past what the
  Broadway's 32 GPRs can hold.  Splitting the setup out and making the
  shader a separate function brought it down to 851 instructions, and
  `fb_read`/`fb_write` were split into an inlinable fast path and an
  out-of-line general path -- but the measured time did not move at all.
  The register pressure was not the problem.
- *Uncached VRAM.*  The framebuffer and depth buffer live in VRAM, which
  is uncached on real Wii hardware, so the per-pixel traffic should
  stall.  Moving the depth buffer (which the guest never reads) to a
  host-side buffer changed nothing: still 1.0 us/pixel.  That is
  expected once you notice the measurements are taken *inside Dolphin*,
  where guest RAM and VRAM are ordinary host allocations -- the caching
  behaviour of a real Wii is not in play.

What the arithmetic actually costs is only ~90 float ops per pixel, so
~1.0 us/pixel is still several times more than the arithmetic explains.
The remaining suspects are the texture fetch (a bilinear sample is four
texel reads plus three `lerp8`s) and the per-pixel function call into
`shade()`.  Both would need the pixel pipeline rewritten around integer
fixed-point math to test, which was not attempted.

Other changes, all measured:

- The per-pixel texture-function switch divided by 255 for every channel
  (up to 12 float divides per pixel).  A float divide is a dozen-plus
  instructions on the Broadway's scalar FPU.  They are now multiplies by
  the constant 1/255.
- The texture cache's range invalidation set the entry's generation to 0,
  but the lookup's fast path required the generation to match, so *every*
  invalidated texture was re-hashed and re-resolved on *every* primitive
  (250k primitives per run).  A separate `stale` flag keeps the fast
  path and only re-validates when the source bytes really changed.
- The GX scanout texture was 512x512 while the picture is 480x272, so
  every frame converted twice as many pixels as it had to.
- The per-pixel perspective reciprocal is a Newton-Raphson refinement
  seeded once per scanline, instead of a float divide per pixel.
- `floorf`/`ceilf` in the per-pixel path were replaced with integer
  conversions.

The guest now renders about 5-6 frames per second where the PSP would do
30, so a full playthrough is slow, but the game is running and is
interactive.  The rasterizer is the remaining bottleneck: it holds the
emulated CPU for roughly three quarters of every run, which starves the
guest thread.

### 6. Where the time actually goes

The rasterizer is about 80% of a run, and the per-pixel loop is about
0.9 us a pixel.  Rather than guess where that goes, the pipeline was
measured by building it with one stage removed at a time (only raster.c
is recompiled, so each variant is quick).  Over 90 s runs:

```text
build        tri_setup     per Mpixel   guest frames
baseline     39706 ms      0.92 ms       649
no texture   46147 ms      0.96 ms       686
no blend     34396 ms      0.69 ms       704
no depth     38974 ms      0.83 ms       683
```

(The runs draw slightly different amounts, so the per-pixel column is
the one to compare, not the totals.)

- The **blend** is about a quarter of the per-pixel cost: a framebuffer
  read, four 8-bit channels converted to float, two blend factors and a
  mix, per pixel.
- The **depth test** is about a tenth.
- The **texture fetch** is negligible: every sample the game makes is a
  point sample (`samples: linear=0 point=12892487`) of a 32x16 palette
  texture, so there is no bilinear filtering and no wrapping to speak of.

What the rest of the per-pixel cost is: the colour interpolation in the
triangle loop (four channels plus u and v, perspective corrected), the
framebuffer read and write, and the call into the shader.

Two explanations were tested and rejected:

- *Register spilling in the pixel loop.*  `draw_triangle` compiled to
  2821 instructions with the shader inlined into it, well past what the
  Broadway's 32 GPRs can hold.  Splitting the setup out and making the
  shader a separate function brought it to 851 instructions, and
  `fb_read`/`fb_write` were made inlinable fast paths -- and the measured
  time did not move at all.
- *Uncached VRAM.*  The framebuffer and depth buffer live in VRAM, which
  is uncached on real hardware, so the per-pixel traffic should stall.
  Moving the depth buffer (which the guest never reads) to a host-side
  buffer changed nothing: still 1.0 us/pixel.  That is expected once you
  notice the measurements are taken *inside Dolphin*, where guest RAM and
  VRAM are ordinary host allocations.

### 7. The obvious wastes that were fixed

- **The clock was doing double-precision arithmetic.**  `hle_time_us()`
  is called several times per primitive, and it computed the elapsed time
  in `double`.  This CPU has no double-precision hardware, so every
  double operation became a call into libgcc's soft-float helpers, which
  are long integer routines.  The normal case is now plain integer
  arithmetic; the `TIME_SCALE` test hook keeps the double path.
- **The frontend converted every pixel twice.**  `frontend_scanout()`
  turned the guest's 16-bit framebuffer into RGB565 and then
  `upload_scanout()` turned that into the GX's tiled layout, writing two
  bytes at a time and testing the image bounds for every pixel -- 25 ns a
  pixel, 8.4 s of a 120 s run.  The tiling is a copy, and it is now a
  block copy: **8.4 s -> 1.3 s**.
- **The pixel format was tested inside the per-pixel loop.**  The format
  does not change inside a frame, so the test belongs outside the loop.
  Doing the widening directly also fixed a precision bug: the old code
  expanded a channel to 8 bits and packed it back down, and for green in
  the 5551 format that lost two bits -- green was being shown with only
  three bits of precision.
- **The frontend re-uploaded pictures that had not changed.**  It
  presents at about 21 Hz while the guest draws at about 7 Hz, so most
  presents converted the framebuffer, flushed the cache and invalidated
  the texture cache for a picture that was already on screen.  It now
  presents only when the guest or the GE has drawn something new.
- **The texture cache hashed the whole palette on every textured
  primitive** -- 119k times a run.  It is now hashed when the palette is
  reloaded.
- **`gpu_draw_prim` timed itself in the production build too**, for a
  counter nothing reads.

### 8. The harness was typing on the user's keyboard

The test harness dismissed the game's autosave dialog by sending a "B"
keystroke with `keybd_event`, which delivers the key to whatever window
the user has focused.  It now posts the key to Dolphin's own window with
`PostMessage`, so it cannot reach anything else, and it no longer raises
Dolphin's window over whatever the user is working in.  It also saves
and restores the two Dolphin settings it needs (`WiimoteSource0` and
the gamepad's B button), so the user's configuration is left alone.

### 9. What the per-pixel cost actually is

The table above measures the cost of the stages by building the pixel
pipeline with one of them removed and comparing the guest frame rate.
Taken one at a time, each stage is worth 5-8% of the frame rate, which
adds up to nowhere near the whole cost -- because removing one lets the
others do more work.  Removing all three at once is the honest measure:

```text
build                        guest frames / 90 s
baseline                            649
no colour interpolation             668
no texture fetch                    686
no depth test                       683
no blend                            704
no texture, blend or depth          830
no shader call at all              1281
```

So:

- The **shader** is about 90% of the per-pixel cost.  Skipping the call
  entirely nearly doubles the frame rate.
- Within the shader, the **texture fetch, the blend and the depth test**
  are about half of it.
- The **colour interpolation** is about 4%.
- The rest is the per-pixel function call: the shader sets up a
  472-byte stack frame and saves the registers the PowerPC ABI makes
  non-volatile, which is 36 memory operations a pixel before any
  drawing happens.

Two things were tried against that and did not work:

- **The blend in 8-bit integers.**  The float blend converts the
  destination colour from bytes to floats, and this CPU has no
  integer-to-float instruction, so each of those four conversions is a
  store to memory followed by a load.  Doing the whole blend in
  integers removes them -- and measured no faster (655 guest frames
  against 650).  The conversions were not what the blend was costing,
  so it was reverted rather than kept as extra complexity.
- **Making the shader a separate function** (as it was) versus inlining
  it into the pixel loop.  Inlining removes the per-pixel stack frame
  and the register saves; it measured between neutral and slightly
  better, and it is kept because the frame is per pixel rather than per
  triangle.

What is left is not a constant factor that a cleverer shader will
remove: a triangle with a texture, a depth test and a blend costs what
it costs.  Getting the game from 4-5 frames a second to the PSP's 30
means not running the pixel pipeline on the CPU at all -- mapping the
GE's display lists onto the Wii's own GX, which rasterises textured,
depth-tested and blended triangles in hardware.  That is a project of
its own: the GE's vertex formats, its texture formats and swizzling,
its 16-bit fixed-point depth and its blend modes all have to be
translated, and the textures the game draws from EDRAM have to be
uploaded to GX textures.  It is the remaining work, and it is not
started.

### 10. Half-resolution rendering (shipped)

The measurements say the rasterizer holds about three quarters of the
emulated CPU, so the guest gets what is left -- 4-6 frames a second
where the PSP does 30.  The only lever that gives a large factor without
rewriting the shader is to draw fewer pixels, so the scene is now drawn
at 240x136 and expanded into the guest's framebuffer at the end of each
display list.

How it works:

  * `target()` points the colour and depth buffers at a 256x136 host
    buffer instead of the guest's VRAM, halves the scissor, and sets
    the screen mapping to half scale.  The geometry therefore lands in
    the small buffer and the texture is sampled at half the rate, which
    is the same picture with half the samples.
  * `gpu_half_res_upsample()` expands the small buffer into the
    guest's framebuffer at the end of the display list, so the guest
    still sees a full-size framebuffer and the display path is
    unchanged.  The game double buffers, so both the buffer being drawn
    into and the buffer on display are written.
  * A file called "fullres" in the application directory turns it off
    and restores the full resolution rendering.

Measured over 90 s runs (debug build):

```text
                    raster px   rasterizer   frontend present   guest frames
full resolution        48 M        42.4 s        14.6 s            692
half resolution        18 M        18.5 s         1.9 s            783
```

And over 300 s of the production build at the title screen and into the
attract mode: 1512 guest frames at full resolution, 2009 at half.

The picture is correct: the title screen at half resolution matches the
full resolution one exactly, only softer, and the game gets far enough
into the attract mode to show the character art and the versus screen,
which the full resolution build does not reach in the same time.

The frontend's present cost fell from 14.6 s to 1.9 s as well, mostly
because it now presents an unchanged picture far less often.

### 11. The attract mode stalls, and it is not the rasterizer

At half resolution the game reaches the attract mode -- the demo fight,
the versus screen, the game over screen -- and then stops.  The thread
dump from the debug build says why:

```text
thread 4098 'user_main'      status=4 wait=vblank
thread 4105 'nuFileAsync'    status=1 wait=none      <- running
thread 4110 ''               status=2 wait=none
```

The main thread is waiting for a vblank and the file I/O thread is
running.  The last HLE calls before the stall are all
`sceKernelDcacheWritebackRange`, which is what the PSP's newlib does
before every file read: the game is streaming something from the disc
and the writebacks are being issued in a loop.

So the stall is the file I/O path in the attract mode (a video or a
streamed asset), not the rasterizer, and the full resolution build
reaches the same place, only later.  That is the next thing to look at.

### 12. What the GX would take (not started)

The obvious remaining idea is to let the Wii's own GX rasterise the
guest's triangles instead of the CPU doing it.  The measurements say
what it would buy: with the shader removed entirely the guest still
only reaches 1281 frames in 90 s, so the ceiling is about 14 frames a
second, and the GX path would land somewhere between the current 4-6
and that ceiling -- not 30.

The translation itself is mostly mechanical, and these are the pieces:

  * The GE applies the view and projection matrices itself, so the
    vertices are in clip space by the time the rasterizer sees them.
    The GX needs the object-space position and the combined P x V
    matrix, so the decoder would have to keep the world-space position
    and the matrix would be built from `g_ge.proj` and `g_ge.view`.
  * The GX's perspective correction divides by the clip w, so it can
    only reproduce the GE's if it is given the same w -- which the
    object-space plus matrix route does give it.
  * The viewport maps as: width 2 x XSCALE, left XCENTER - OFFSET -
    XSCALE, and the same for y; the z range is ZCENTER -/+ ZSCALE with
    a z scale of 0 to 0xFFFFFFFF.
  * The GE's features are simple: the game only ever uses modulate,
    src-alpha/one-minus-src-alpha blending, 4-bit palettised textures,
    point sampling and the depth test (measured: 119460 textured
    primitives, all of them 4-bit and all modulated, 125677 with the
    blend on and 125666 with the depth test on).
  * The texture would go up as RGB5A3, which has the same 4x4 block
    layout as the RGB565 the scanout already uses and keeps the alpha
    bit the 5551 palette has.
  * The comparison functions need a table: the GE's order is never,
    always, equal, not equal, less, less-equal, greater, greater-equal
    and the GX's is never, less, equal, less-equal, greater, not-equal,
    greater-equal, always.
  * The scene would be drawn into the EFB and copied to an external
    framebuffer, which is in the GX's tiled format, so the copy has to
    be un-tiled before it can be put into the guest's framebuffer.
  * The GX is not thread safe and the frontend uses it too, so the two
    would have to be serialised.

That is a few days of work, and the payoff is bounded by the 14 frames
a second ceiling above.

### 13. The GX scene renderer (written, nearly working)

The obvious way to make the game faster without making the picture
smaller is to let the Wii's own graphics chip do the drawing.  The GE
and the GX are both fixed function pipelines, so the guest's display
lists map across nearly one to one, and `platform/wii/wii_gxscene.c`
does that mapping:

  * The GE applies the view and projection matrices itself, so the
    vertices are in clip space by the time the rasteriser sees them.
    The GX has to be given the object space position and the combined
    matrix instead: only then does its perspective correction divide by
    the same clip w the GE would have used, and the picture comes out
    the same.

  * The "through" mode and the rectangles are already screen
    coordinates, so they are put back through the viewport's inverse.

  * The guest's textures are 4 bit palettised and the palette entries
    are 16 bit, so they go to the GX as RGB5A3 in the GX's 4x4 tiles:
    the same five bits of red and blue and the same six of green as
    the RGB565 the scanout already uses, with the palette's alpha bit
    kept.

  * The blend, the alpha test, the depth test, the scissor and the
    clear are all translated: the GE and the GX number their
    comparisons differently and their blend factors differently.

What works: the translation is correct.  The Bandai Namco logo comes
out of the chip pixel for pixel, with the right colours, the right
shapes and the right text, and the game's user interface (drawn in the
"through" mode) comes out in the right place.

What was wrong, and how it was found: the chip's command stream
desynchronised and Dolphin reported "FIFO: Unknown Opcode".  A test
triangle, drawn once a second by the frontend thread with exactly the
commands the scene uses, ran for the whole test without a single
error.  So the commands were right and the *thread* was the problem:
the GE thread is a host thread, so it runs at the same time as the
guest threads, and while the frontend was writing the chip's command
buffer the GE thread was writing to it as well.  The two sets of
commands interleaved and the chip read a command that was never meant
for it.  One lock around the command buffer fixed that, and the
desynchronisation is gone.

Two more things came out of it:

  * The chip's texture memory cannot simply be thrown away when a
    texture is replaced, because the chip runs behind the CPU and may
    still be reading it.  The memory is now kept and used again later.

  * The thread that draws must not wait for the chip.  `GX_DrawDone`
    spins, and the GE thread holds the scheduler's baton while it
    spins, so the whole game stops.  The copy out is queued behind the
    drawing in the command stream anyway, so it does not need the
    wait.

What is left: the game still stops with the chip drawing the scene.
The guest's threads are all waiting and the periodic dump of the
scheduler's threads does not run, which points at the GE thread being
stuck rather than at the guest.  The next thing to look at is where
exactly it stops; the frontend thread's own drawing (the scanout) has
always worked, so the difference between the two threads is the
interesting part.

The path is off by default.  A file called "gxscene" in the application
directory turns it on, and the build flag -DGXSCENE turns it on
without the file.  The build flag -DGXTEST draws the test triangle
instead, which is how the thread was found.

Even when it works, the gain is bounded.  The guest issues a quarter
of a million primitives a frame, and every one of them costs a
GX_Begin, a GX_End and the state changes that go with it.  That is the
same order of work as the software rasteriser's per-primitive cost, so
the win is in the heavy scenes, where there are many pixels per
primitive, and not in the light ones.  The measurement to make is the
attract mode, where the software rasteriser takes 87 seconds of a
90 second run.

### Reproducing

`%TEMP%\opencode\tekken-ingame-check\run_long.ps1` boots the DOL in
Dolphin, dismisses the autosave dialog with B (Cross) and captures a
screenshot every N seconds.  The key press is posted to Dolphin's own
window with `PostMessage`, not sent with `keybd_event`: the latter
delivers the keystroke to whatever window the user happens to have
focused, which is not something a test harness should be doing.  `bench.ps1` is the same without the
screenshots, for measuring a change.  Both were shortened during this
session: the production build has no `WII_OSREPORT`, so the boot check
now looks for a non-black screen instead of a log line, which cut a
fixed 305 s wait to a few seconds.

### Disk

`dist-tkdr-sd-full` is 1.7 GB because it holds the game ISO as the
prebuilt SD-card image; it is the deliverable, not build output.  The
two build trees (`build`, `build-debug`) are 85 MB and regenerable, and
`gen/` (56 MB) is the generated recompilation, which `make` needs.

## Resolution default and the chip renderer (2026-10-22)

### Full resolution is now the default

Half resolution (240x136 expanded into the guest framebuffer) was
reaching the screen, but it is visibly too soft: the picture is
magnified 480x272 -> 640x362 inside the 640x480 display, so halving it
again quarters the detail.  Production is now built **without**
`-DHALF_RES`, which means `gpu_half_res_set(0)` and the rasteriser draws
the guest's own 480x272.  `-DHALF_RES` still opts in for anyone who
needs the speed.

The scanout texture is also sampled with `GX_LINEAR` now instead of the
default nearest filter.  The picture is always magnified, so nearest
sampling was what made it look blocky even at full resolution.

### Command buffer desynchronisation: fixed

Four separate faults, all confirmed by fixing them one at a time:

1. **A self deadlock, not recursion.**  `wii_gxscene_frame_begin` locks
   the command buffer and `wii_gxscene_present` locked it again on the
   same thread.  libogc's `pthread_mutexattr_settype(PTHREAD_MUTEX_RECURSIVE)`
   does not take effect, so the mutex stayed NORMAL and the second lock
   blocked forever.  The pair is now balanced across the frame:
   `frame_begin` takes it, `present` gives it back.
2. **The colour attribute was two bytes, not four.**
   `GX_SetVtxAttrFmt` takes `(comptype, compsize)`.  For a colour the
   component type is `GX_CLR_RGBA` and the component size is `GX_RGBA8`.
   The code passed `GX_U8` for the size, which is `GX_RGB565` -- a 16 bit
   colour -- while `GX_Color4u8` wrote four bytes.  The chip sized a
   vertex two bytes short and read vertex data as commands.
3. **Vertex attributes are written in the chip's order.**  Position,
   then colour, then texture coordinate.  Writing the coordinate before
   the colour makes the chip read the wrong bytes for each.
4. **The border clear ran once per primitive.**  `clear_border` paints a
   full screen black quad; called for every non-clear primitive it wiped
   the frame each time, so only the last primitive survived.  It now runs
   once per frame, and the rasteriser drops its cached viewport, scissor
   and matrix afterwards because the clear changes them.

After these, the chip draws thousands of primitives with **zero FIFO
errors** and balanced list counts.

### The chip had no colour channel

`wii_video_game_begin` calls `GX_SetNumChans(0)` because the scanout quad
is drawn from its texture alone.  The scene shades with the vertex
colour, so with no channel the chip had no colour to shade with and
every primitive came out black.  The scene renderer now enables one
channel with `GX_SetChanCtrl(0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX,
GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE)` and routes it with
`GX_TEV_ORDER`'s colour argument set to `GX_COLOR0A0`.

The viewport also had a **negative height**: the GE's Y scale is
negative because it flips Y, and `ys * 2 * ky` was used directly.  It now
uses the magnitude and puts the top edge at `yc - oy - |ys|`.

### What still does not work

The chip draws and copies correctly -- reading the framebuffer back after
`GX_CopyDisp` shows the expected pixels -- but that picture never reaches
the display.  A solid green frame pushed through the frontend's own
scanout does appear, and copying out the chip's frame buffer from the
frontend produces black, which points at the frame buffer being cleared
(`GX_CopyDisp(..., GX_TRUE)`) or at the buffer being armed before the
copy lands.  Several arrangements were tried (copy on the drawing
thread, copy on the frontend, one frame of latency, `GX_DrawDone` before
arming, flushing only when arming) and none displayed the chip's frame.
The renderer therefore stays **off by default** (`gxscene` file or
`-DGXSCENE`).

### Measured

- CPU rasteriser, half resolution: 1507 guest frames in 100 s (~15 fps).
- Chip renderer: 1800 guest frames in 50 s (~36 fps) once the desync was
  fixed -- the chip path is much faster, but it is the path that does not
  display.
- Full resolution costs about twice the rasteriser pixels of half
  resolution; it is the default now because the picture at half
  resolution was judged too soft for the display.

### Known guest fault

After the guest reaches the `sceKernelDcacheWritebackRange` loop that
waits on `nuFileAsync` disc I/O, it has twice faulted with an ISI or DSI
soon after (a jump to `0xC3BBB5FC`, or a store to `DAR=130A0D03`).  The
same run on the CPU path has not faulted within 100 s.  The chip path
runs the guest faster, so it reaches this sooner; the fault is in the
guest/HLE interaction, not in the renderer, which is why it did not show
up before the desync was fixed.

### Build and deliverable

Production, probe free, full resolution:

    make TARGET=tekken6wii BUILD=build

The DOL is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`;
SHA256 `5FD69201E2D7DF88E16A8483F2BFB15180F155770A4A5920F9E83145C3FE0B5E`.

## The GX renderer becomes the default (2026-10-22, later)

This supersedes the "What still does not work" paragraph of the previous
section: the chip-rendered picture now reaches the display.

### What changed

- **The GX scene renderer is the default.**  A file called `cpu` in the
  application directory (`sd:/apps/tkdr/cpu`, or the `tekken6wii` directory)
  forces the software rasteriser.  The `gxscene` file and `-DGXSCENE` still
  force the chip on.

- **Draws are batched.**  Each guest primitive is expanded into triangles and
  appended to a 2048-vertex buffer inside `wii_gxscene.c`.  A state change
  (`wii_gxscene_set_state`, `set_scissor`, `set_world`, `set_screen`) or the
  end of a display list flushes the batch.  A run of primitives that share a
  texture, blend mode, scissor and matrix therefore costs one
  `GX_Begin`/`GX_End` instead of one per primitive.  The guest issues about a
  quarter of a million primitives in the opening scene, so this is the
  difference between the chip idling in command setup and the chip drawing.

- **Guest clears are not drawn by the GX path.**  They target the guest's own
  back buffer.  The GX has a single frame buffer, and its display copy is
  deferred, so drawing a clear into it wipes the picture before the copy
  runs.  The renderer clears the letterbox itself once per display list.

- **Logs no longer draw on the TV.**  `wii_log` writes only to the SD log.
  The boot console's buffer (`s_xfb[0]`) is cleared when the game takes the
  display.  This removes the purple console text screen and the cost of a
  console redraw for every line.  The log path list now starts with
  `sd:/apps/tkdr/debug.log`.

- The MEM2 big-block pool (`runtime/bigmem.h`,
  `platform/wii/wii_memory.c`), the decoded-texture LRU and retirement, and
  the big-block audio/movie buffers are part of the same change.

### Measured

- GX path, opening scene: the frontend runs at 60 Hz, the guest at roughly
  40-60 frames/s, with 126k-361k GX primitives over the run; the chip is
  idle between lists, the FIFO drains to zero and there are no FIFO errors.
- Software path, full resolution: about 15 frames/s.  This is the comparison
  point; the chip path is several times faster.

### What still blocks the intro

The guest stops submitting GE display lists after roughly 1900 guest frames,
at the end of the Namco opening.  The frontend keeps running; the chip is
idle; the guest's own loop continues with vblank, audio and display calls.
The intro movie is not drawn by the GX scene.  A framebuffer fallback (the
frontend scanning out the guest framebuffer when the GX went idle) was tried,
but the guest's movie path is the AVC stub, which writes black frames, and
the handoff needs more work before it can be shown accurately.  The software
path stops at the same screen, so this is a guest-side movie/loading problem,
not a rasteriser problem.

### Build workarounds

gcc 15.2's `try_forward_edges` internal compiler error also hit
`wii_input.c`, `runtime/hle/utility.c` and `raster.c`.  `wii_input_scan` and
`savedata_run` carry `optimize("O1")`; `raster.c` is built at `-O1` with the
RTL loop invariant/store passes disabled; `utility.c` is built at `-O1`.
The generated game code stays at `-Os`.

### Shipped

Production DOL (full resolution, GX default, probe-free, no on-screen logs):

    SHA256 9AE9300FEEC8C7612F30E85DC7325C034183088DB71B4D2089DA57B3B84B6AB9

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.

## The title and attract mode: wait semantics, thread stacks, render targets (2026-10-22, final)

This supersedes the "What still blocks the intro" paragraph above: the guest
now leaves the logo and reaches the title and the attract-mode fights.

### What fixed the stop

- **Wait semantics.** `sceKernelWaitThreadEnd(CB)` now returns 0 (the wait
  succeeded), not the target thread's exit status, and it treats a deleted
  (`ST_DEAD`) thread as ended.  The movie player polls its three worker
  threads with a zero timeout; a zero timeout is a poll, not "wait forever".
  An earlier experiment treated zero as infinite and blocked the player;
  that was reverted.
- **Host stack sizing.** A full megabyte of host stack per guest thread
  exhausted MEM1 when the movie player started its three workers, so two
  workers could not start and the game later executed a null callback.
  The host stack now scales with the guest's own stack (8x, clamped to
  256 KiB..2 MiB; the boot/main thread gets 4 MiB).  All workers start.
- **Optional null callback.** The EBOOT teardown calls a callback through a
  pointer that is null in this recompilation; `call_indirect(0)` is treated
  as an optional callback instead of branching to zero.
- **Render targets.** Only the double-buffered display surfaces go through
  GX.  A texture target (the movie player's decode buffer, the card effects)
  is drawn by the software rasteriser into guest VRAM, where the guest's
  later texture reads expect it.  The GX cannot do render-to-texture, so
  these passes are the remaining slow path.
- **Destination alpha.** The EFB is `GX_PF_RGBA6_Z24` rather than RGB8.  The
  GE framebuffer has alpha, and the game's card/glow effects use
  `DSTALPHA`/`INVDSTALPHA`; the GX rejects those blend factors on RGB8.  A
  clear writes alpha when the guest's clear mode asks for it; ordinary draws
  preserve it, matching the reference renderer's
  `glBlendFuncSeparate(..., GL_ZERO, GL_ONE)`.
- **Fixed source blend.** GE blend factors 10-13 use the fixed blend colour;
  the GX has no constant blend factor, so a second TEV stage multiplies the
  source by the fixed colour, mirroring the reference's `u_srcfix`/`u_fixa`
  shader path.  The fixed *destination* colour still falls back to ONE.
- **Texture cache.** The CLUT hash is part of the cache slot index and the
  GX texture cache has 64 slots.  The character cards use many palette
  variants of the same source textures; without this they re-decoded and
  re-converted every frame.
- **Start clears.** A clear before the list has drawn anything is this
  frame's background and is applied; this is what restored the logo
  fade-in/out.  A trailing clear (the guest clearing its other back buffer)
  is still skipped.
- **Console.** The boot console has its own buffer and is only shown for a
  fatal error.  The purple boot log no longer flashes on the TV.
- **CPU framebuffer fallback.** If the chip stops producing copies and the
  guest presents a new frame, the frontend can put the guest framebuffer on
  screen with the CPU (no GX commands, so it cannot race the GE thread).
  It only presents on a new guest frame, so it does not show stale green
  VRAM.

### Result

- The Bandai Namco fade, the Tekken title and the attract-mode "GAME OVER"
  fights are rendered; the 3D characters and stages are drawn by GX.  The
  guest no longer stops at the logo and no longer takes the null-callback
  crash.
- The intro movie video is still black: the AVC stub advances the clock but
  produces no picture.  The attract mode is the game content after it.
- The character-card/menu effects are not fully accurate yet.  GE colour
  test, fog, absolute-difference blending and render-to-texture are not
  implemented in the GX path, and the reference renderer (`tkdr-recomp`)
  lists the same gaps in its NOTES.  The card effects therefore use the
  software fallback (slow) and some of their colours are wrong; the fixed
  destination blend colour has no GX equivalent and is approximated by ONE.
- The menu is entered with the controller's **Start** button.  The automated
  harness only presses Cross (its autosave/OK button), so it remains in the
  attract loop.

### Reference checkouts

The two reference repositories were cloned outside this repository (they are
not vendored) to `%TEMP%/opencode/refs/`:

- `gasparegas/tkdr-recomp` - the upstream static recompiler/runtime and the
  GL renderer (`runtime/gpu/glr.c`).  Its `NOTES.md` explicitly lists
  "colour test, fog, doubled-alpha and absolute-difference blending" as not
  done, and its fixed-source blend is the shader multiply now mirrored in
  the GX path.
- `zxyandreay/azazel6` - the Tekken 6 research tree (no Dark Resurrection
  texture/GE reference).

### Shipped

Production DOL (GX default, full resolution, probe-free):

    SHA256 112960556956D0B507E680F110DF807F3670A1775F4122974415931E3FA3A36E

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.  The `cpu` marker file used during
testing was removed from the SD image; the production build is the fast GX
path again.

## Colour/format fixes, 3D visibility, fog, and the CPU profile (2026-10-22, late)

This is the result of the first implementation pass from `docs/PLAN.md`.

### What was wrong

- **CLUT byte order.** `LOADCLUT` copies the guest's little-endian palette bytes
  into `g_ge.clut`, but `clut_lookup()` read them as native 16/32-bit values.
  On the big-endian Wii every palette entry was swapped, which produced the
  wrong logo/card/character colours.  Palette entries are now byte-swapped on
  Wii before expansion.  CPU mode confirms the Bandai Namco logo is correct.
- **GE write masks.** `GE_MASKRGB`/`GE_MASKALPHA` are bits *not* to write, so
  zero means all channels are written.  The compatibility gate treated zero as
  "write nothing", which made all GX primitives vanish.  The software and GX
  paths now agree on the preserve-mask semantics.
- **Skipped primitives.** The first 2D-skip implementation returned without
  consuming the GE vertex/index pointer, so following geometry read the wrong
  vertices and appeared untextured/grey.  Skipped primitives now advance the
  same pointers as a normal draw.

### Render policy

- Transform-mode (3D) display primitives always go through GX.  GE tests GX
  cannot express (stencil, colour test) are left off rather than dropping the
  geometry into a software pass whose output the GX display never showed.
- Through-mode (2D) layers that GX cannot represent exactly are skipped in GX
  mode.  They were already invisible in the GX frame; this removes the
  per-frame software fill.  CPU mode still executes every layer exactly.
- GX texture slots increased from 64 to 128.  Global GX texture-cache
  invalidation happens on texture-buffer reuse/eviction, not on every upload.
- Vertex hot paths: a direct through-mode path, conditional normal/weight
  fetches, unit-normal/directional-light/specular fast paths, a decoded
  lighting-state cache, and `-Ofast` on the hot vertex/lighting helpers.
  These cut the measured vertex time from 53-59 s to about 33 s per 100 s.

### Fog

- The GE fog state is per-vertex `v = (viewZ + fog1) * fog2`.  GX takes
  positive eye-space distances, so the mapping is
  `start = fog1 - 1/fog2`, `end = fog1` for the usual positive slope.
  The earlier negative-Z mapping fogged the entire screen with the fog colour.
- The measured attract states use ranges such as `fog1=200000`, `fog2=7e-6`
  and a fog colour around `0x826450`; visible fighters are about 3,900 units
  from the camera, so the fog is intentionally far away and subtle.  Forcing
  the range closer would not match the game.

### Performance profile

A 100 s debug run (HLE instrumentation on) reaches the 3D attract stage:

- about 24 guest frames/s and 45 frontend frames/s;
- GE thread busy about 88-97% of the wall time;
- `draw_prim` CPU time about 82-90 s;
- after the fast 2D path, lighting-state cache and `-Ofast` vertex helpers,
  vertex decoding is about 33 s (down from 53-59 s); texture lookup/state
  setup is about 20 s; the remaining GE-thread time is primitive expansion
  and GX submission;
- 15-17 M decoded vertices per run, with roughly 6-7 M skinned and 6-7 M
  lit; many vertices are unique per primitive, so a decoded-vertex cache
  measured zero hits and was removed;
- texture decode is about 5-8 s; GX waits are near zero, so the chip is not
  the stall.

The next performance work should therefore be the vertex pipeline itself:
hardware skinning/lighting, indexed vertex/display-list submission with safe
buffer lifetime, or a cheaper PSP-compatible lighting path.  This is
CPU-side; more GX batching alone will not reach 60 fps.

### Known limitations

- GX has no stencil readback; 3D stencil tests are ignored and unsupported
  2D stencil/colour-test/fixed-destination layers are skipped in GX mode.
  A GX multipass/RTT mask is the next accuracy step.
- Render-to-texture is still not implemented; texture targets still use the
  software rasteriser.
- The intro movie remains black (AVC stub); attract mode is the content after
  the logo.
- The menu still needs the controller's Start button; the current harness
  sends Start, but the automated captures stop at GAME OVER.

### Shipped

Production DOL (GX default, full resolution, probe-free):

    SHA256 44A75316EDD70F0303095011AE60711654F034BE79859EAF9FD3498F0EF90C6D

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.  The temporary `cpu` marker used for
the software comparison was removed from the test SD image.

## Layer classifier and native GX cases (2026-10-22)

The section 4.8 classifier is now in the GX path.  Every through-mode
primitive gets a feature mask and one of three paths before vertex decode:

- **native** — the GX fixed-function state represents it;
- **approx** — the existing GX approximation is used (doubled source alpha,
  fixed source blend colour);
- **skip** — the layer is not drawn, with the first unsupported feature
  recorded (colour test, alpha-test mask, partial write mask, complex stencil,
  logic-with-blend, unsupported blend equation/factor, doubled-alpha/alpha-test
  interaction).

The native cases now include:

- GE logic ops when no blend is active, mapped to `GX_BM_LOGIC` with the
  matching `GX_LO_*` operation;
- GE dither through `GX_SetDither`;
- whole-RGB and whole-alpha write masks through `GX_SetColorUpdate` /
  `GX_SetAlphaUpdate`, including alpha-only passes;
- simple stencil `ALWAYS` + `REPLACE` (`KEEP` on both failure paths) through
  `GX_SetDstAlpha`; complex stencil still skips;
- the existing fixed-source blend TEV stage and doubled-source-alpha TEV
  scale.

Debug (`-DHLE_TRACE`) builds now expose `plan=native,approx,skip`,
`feat=<bitmask>` and `wm=full,alpha-preserved,alpha-only,partial` in
`GXPERF`, plus a one-shot classifier summary at shutdown.  Production builds
contain none of these strings.

### Evidence

- Debug run, 180 s, reached Bandai Namco logo, title, VS, battle and GAME OVER.
  In the attract/gameplay sample the only through-mode primitives were partial
  write-mask states; they were skipped by the first classifier pass.
- The whole-channel approximation for partial write masks changed that result.
  A debug autopilot (`EXTRA_CFLAGS=-DTKDR_AUTOPILOT`) reached the profile
  setup; the name-entry keyboard, `SELECT YOUR AREA`, the tutorial prompt and
  the `Choose Your Fighter` character-select screen are now visible instead of
  black.  The character portraits, card frames and player/status UI render in
  the correct palette.  The character-select sample reports `skip=0`,
  `plan=0,3188,0` and `wm=0,0,0,3188`, i.e. every through-mode primitive took
  the approximation path rather than being discarded.
- Production run, 90 s, reached the title/VS/game-over path.  The 3D stages
  kept their correct textures, palette colours, fog and character rendering
  after the partial-mask change.
- Production DOL is probe-free for `GXPERF`, `GXREJECT`, `gxr plan`,
  `gxr features`, `GXSTATE` and `HLE_TRACE`; the unit test suite passes.

### Remaining limits

- Colour test, complex stencil and the general fixed-destination/absolute-
  difference cases still skip in GX mode.  RTT is not implemented yet.
- Partial write masks are approximated with whole-channel writes.  The menu UI
  is visible, but a preserved alpha/stencil bit is not preserved exactly; the
  exact form needs a mask/RTT pass.
- The 180 s debug sample did not exercise the simple-stencil path; it is
  implemented and classified, but not yet observed in a captured card frame.
- The automated Dolphin harness still cannot deliver Start with focused
  `PostMessage`: the guest received a GameCube `C` press (PSP Triangle), but
  Start sent as Return, P, V or the Wiimote Plus key E never changed
  `g_buttons`.  The debug autopilot is a test-only workaround under
  `-DHLE_TRACE`/`-DTKDR_AUTOPILOT`; the real controller path remains the
  intended production input.
- The character-select screen is captured through the autopilot, but the
  profile/tutorial path is timing-sensitive and not yet a reusable
  user-facing harness.  The screen is also the first card/menu frame available
  for a PPSSPP comparison; no frozen PPSSPP corpus exists yet.
- Production performance was not remeasured in this pass because the
  probe-free build emits no `GXPERF`.

### Shipped

Production DOL (GX default, full resolution, probe-free):

    SHA256 27AA3AFDAAAE4614776310EE6A0CE348F23FDD266D2745BD4174720A6055EAAF
    length 9,702,432 bytes

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.

## Runtime, texture and audio checkpoint (2026-10-23)

The checkpoint commit `12d6697` integrated a second group of runtime fixes and
performance changes on top of the layer classifier:

- `__thread` was not per-thread under libogc.  `pthread_self()` and the
  re-entrant storage lock shared one word, so host file access was effectively
  unlocked and could wake the wrong host thread.  A small LWP registry now
  identifies threads, and the Makefile rejects any ELF that still has
  `.tdata`/`.tbss`.
- Storage I/O is serialised again, including save-data, stat, close, DMA and
  the frontend's marker-file checks.  `sceDmacMemcpy` invalidates the decoded
  texture range.
- Audio now uses one AESND stream voice at 44.1 kHz and lets the DSP resample
  to 48 kHz; the old CPU linear resampler and ring are gone.  Heartbeat output
  reports `audio_underruns`.
- The texture cache is four-way set associative, validates by a 32-bit-lane
  hash, uses the display-list generation to avoid re-hashing the same texture
  inside one list, and has a larger GX texture-slot table.  Texture state and
  pixel state are rebuilt only when the relevant generation/serial changes.
- GX triangle strips and fans are sent as strips/fans instead of being
  expanded into triangle lists; 8888 scanout red/blue order and the slow
  scanout path were corrected.  The GE rejects out-of-window list PCs and only
  raises callbacks for lists that actually have a callback id.
- The HOME check moved before the GX present path, so HOME works while GX is
  the active renderer.
- `GPU_SELFTEST` (`EXTRA_CFLAGS=-DGPU_SELFTEST`) runs synthetic 565, 5551,
  4444, 8888, swizzled, CLUT4/CLUT8, shift/mask/offset vectors through the
  real big-endian reader.  The debug boot reported:

      SELFTEST textures: 20/20 passed

- The debug autopilot reached `Choose Your Fighter` and then a fight; the
  character-select sample reported `skip=0`, `plan=0,3188,0`,
  `wm=0,0,0,3188` and `rtt=0`.  Production runs reached VS, battle and GAME
  OVER with the expected palettes and character rendering.
- The Dolphin test user folder is muted (`C:\tkdr_raw_user\Config\Dolphin.ini`,
  `[DSP] Volume = 0`) so automated demos do not play audio.
- A release-speed autopilot build is available without HLE probes:
  `make TARGET=tekken6wii-demo BUILD=build-demo EXTRA_CFLAGS=-DTKDR_AUTOPILOT`.
  The resulting `tekken6wii-demo.dol` has none of the `GXPERF`, `GXREJECT`,
  `gxr`, `GXSTATE`, `GPU_SELFTEST` or `SELFTEST` strings and reached
  `Choose Your Fighter` and BATTLE 1 in 60 s muted.  Use it for demos when the
  debug build's instrumentation is too heavy.
- I/O is not the loading bottleneck in the measured menu run: over 60 s the
  debug sample spent `isot=2070 ms` and `iot=2053 ms`, while the same run was
  already around 46 guest frames/s at character select.  The remaining weight
  is guest/renderer CPU work, not the ISO cache.
- Generated-code optimization was measured, not assumed.  `GEN_OPT=-O2`
  produced a 17.6 MiB DOL that could not start the next host thread (memory
  exhausted); `GEN_OPT="-O2 -fno-inline"` fitted at 10.3 MiB but did not
  improve the measured menu frame rate.  The shipped default stays `-Os`; the
  `GEN_OPT` make variable remains for future experiments.
- The guest memory-scheduling counter no longer has a per-access volatile
  store.  It is only updated by the guest thread; the measured menu rate did
  not change materially, but the access path is cheaper.
- The debug menu trace showed `rtt=0`; render-to-texture remains unimplemented.
  The profile indicates the game spends much of its wall time waiting for
  vblank, so further gains need renderer work rather than generated-code size.
  A frozen PPSSPP corpus is still open.

### Shipped

Production DOL (GX default, full resolution, probe-free; checkpoint `8E5C1F03`):

    SHA256 8E5C1F03CAEC6B87FEA7C3AC579359F206AE715E86023D2468295B7C61BD5025
    length 9,706,144 bytes

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.

## Frame-time diagnosis, f2i and optimization alternatives (2026-10-23, later)

The next pass separated the average menu rate from frame pacing and recorded
the larger architectural alternatives in `docs/OPTIMIZATION_IDEAS.md`.

### Frame pacing

A debug autopilot run (60 s) added a guest frame-interval histogram.  At the
character-select sample:

```text
FRAMETIME n=2400 <17.5=1904 17.5-25=63 25-34=210 34-50=135 >50=87
```

Most frames are within one 60 Hz interval; the late frames are concentrated
around scene transitions and loading.  The same histogram was produced after
switching the Dolphin test profile to Hybrid shaders, so shader compilation was
not responsible for those counts in that configuration.  The Hybrid setting
was restored to the existing test profile.  The run confirms that the average
"45 fps" is a mixture of good frames and transition hitches, not a uniform CPU
ceiling.

The GX scene still reports one display list and one present per guest frame,
about 52 GX batches/frame, `skip=0`, `rtt=0`, and only partial write masks in
the classified state.  RTT, colour test and complex stencil are therefore not
exercised by this menu sample and were not implemented speculatively.

### Small guest-code optimization

`f2i` now builds all four MIPS float-to-int rounding modes from the single
`fctiwz` cast instead of calling double/float libm routines.  This removes
soft-float double operations from generated code.  It is behaviour-equivalent
for the finite and saturated cases (including round-to-nearest-even ties), but
it did not change the measured menu frame rate because that scene is not
uniformly arithmetic-bound.  The debug PC sampler now also excludes time spent
blocked in HLE waits, so future profiles show running guest code rather than
the vblank loop.

### Documentation

`docs/OPTIMIZATION_IDEAS.md` records the higher-level options: lazy GX target
shadow, static layer/frame caching, native CI4/CI8 + TLUT textures, an effect
compiler/mask atlas, command recording, paired-single vertex work, and
Dolphin-specific trade-offs.  These are design options, not shipped behaviour.

### Shipped

Production DOL (GX default, full resolution, probe-free; checkpoint `695A341E`):

    SHA256 695A341EF6B129DBD7C95FE4425AFFB35AC35FF7368A21B676622C75D48EA1C0
    length 9,704,928 bytes

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.  It contains none of `GXPERF`,
`GXREJECT`, `gxr plan`, `gxr features`, `GXSTATE`, `HLE_TRACE`, `GPU_SELFTEST`,
`SELFTEST` or `FRAMETIME`; the unit tests pass.

## Fast texture decode, exact CI4/CI8 groundwork, and transition spikes (2026-10-23, later)

This pass implemented the first two texture-side ideas from
`docs/OPTIMIZATION_IDEAS.md` and used the frame-time diagnostics to locate the
remaining menu stutter.

### Bulk texture decode (shipped)

`tex_cache_lookup()` used to call `fetch()` once per texel.  `fetch()` makes
guarded guest reads (and a scheduler tick) and, for a paletted texture, another
guarded CLUT read and channel expansion for every texel.  The decoder now has a
bulk path for GE formats 0-5: it maps the contiguous texture range once, builds
the 256-entry palette once, and indexes it directly.  The generic `fetch()`
remains the fallback for DXT, CLUT16/CLUT32, non-contiguous sources and texture
pack replacements.

The 60 s debug autopilot sample dropped texture decode time from about 488 ms
to 264 ms (`texdec` count unchanged at roughly 530, so this is decode cost, not
a cache-policy change).  A debug-only sample check compares the bulk result with
`fetch()`; no mismatches appeared in the 20 s run.  The output in the character
select/card frames is unchanged.

### Exact CI4/CI8 + TLUT path (gated, disabled in production)

The GX path can upload a paletted texture as `GX_TF_CI4`/`GX_TF_CI8` with a
GX TLUT instead of expanding every texel to RGBA8.  The source swizzle/stride
and GE CLUT shift/mask/offset are handled when building the GX texture and
TLUT.  The captured menu palettes are all CLUTFORMAT 3 (8888), so this menu
still takes the exact RGBA8 path.

The native path is now behind `GX_NATIVE_TEXTURES` and is **not defined in
production**.  A forced experiment that sent 8888 palettes through RGB5A3
produced a black screen; that approximation is rejected.  The exact RGB565
variant also remains gated until a corpus scene exercises it.  The bulk
decoder above is independent, exact, and stays enabled.

### Transition spikes

`SPIKE` diagnostics now print the guest frame index, ISO/file I/O counters,
texture decodes and GX conversions for frames over 25 ms.  Around the
character-select transition the frame intervals are a sustained 32-35 ms, but:

- ISO/file I/O counters are flat during the spikes;
- GX texture conversion is only about 106 ms over the whole 60 s run;
- texture decode is about 264 ms over the whole run;
- the spike frames continue while the guest is executing its asset/state
  update code (`f_088048B0`, `f_088EF354` are visible in the running-only PC
  histogram).

The remaining hitch is therefore guest transition/asset work, not the ISO
cache or the GX texture upload.  The fast decode removes a real cost but cannot
remove that guest-side work.

### Shipped

Production DOL (GX default, full resolution, probe-free; checkpoint `FD99235F`):

    SHA256 FD99235F1DA67B6A6EB4F054270CAEF069E7E39476520B899060B9D3EA8CFF60
    length 9,708,160 bytes

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.  It contains none of `GXPERF`,
`GXREJECT`, `gxr plan`, `gxr features`, `GXSTATE`, `HLE_TRACE`, `GPU_SELFTEST`,
`SELFTEST`, `FRAMETIME`, `SPIKE` or `TEXNATIVE`; the unit tests pass.

## Fight frame rate: VFPU translation, draw cache, whole-draw skinning (2026-10-24)

Fights ran at 12-15 guest fps at the start of this pass.  In six different
fights of 17-22k vertices a frame (below) they now run at 27.6-30.2, mostly
29-30 (the game's own cap is 30).  Everything here was
measured with the release-speed autopilot demo (`EXTRA_CFLAGS="-DWII_OSREPORT
-DTKDR_AUTOPILOT"`), whose cheap `mftb` counters are printed every 300
frontend frames as `DEMOFPS` and its companion lines; the instrumented debug
build's per-primitive probes cost as much as the work they time (24 ms a
frame) and were misleading.

### How the frame rate is quantised

The guest waits for the vertical blank, so the guest rate is 60, 30, 20, 15,
12 ...: a frame that takes 34 ms of CPU shows as 20 fps and one that takes
33 ms as 30.  Compare `busy` (CPU use) and the two times in `DEMOFPS`
(`ge`: the GE thread; `other`: everything else) rather than the rate.  A run
with every 3D draw skipped (`-DEXP_NODRAW`) holds exactly 30.0 with `busy`
at 58%: the game's cap, and 19 ms of CPU a frame for everything but drawing.

Dolphin is deterministic, so one build always plays the same fight; a build
with different timing plays a different stage and fighters.  The scene is
the larger variable (14k to 39k vertices a frame), so `AUTOPILOT_SHIFT_MS`
moves the autopilot script to sample others.

### Where the time went (per guest frame, a 21-28k vertex fight)

| | before | after |
| --- | --- | --- |
| GE thread (`ge`) | 29-31 ms | 16-18 ms |
| everything else (`other`) | 24-27 ms | 13 ms |
| fight rate | 12-15 fps | 27.6-30.2 fps in the six scenes below |

The six scenes, from the final build (two reports each, `AUTOPILOT_SHIFT_MS`
= 0, 1700, 3300, 5100, 8900 and 12300; guest fps / `ge` / `other`, vertices
and draws a frame):

| shift | guest fps | ge ms | other ms | vertices | draws |
| --- | --- | --- | --- | --- | --- |
| 0 | 28.2, 29.3 | 17.8, 17.5 | 14.7, 14.6 | 22.3k, 22.1k | 4308, 4252 |
| 1700 | 29.1, 30.0 | 18.2, 17.6 | 13.6, 13.2 | 22.2k, 21.9k | 4186, 4117 |
| 3300 | 29.8, 30.2 | 16.9, 16.6 | 13.9, 13.6 | 20.1k, 19.8k | 3454, 3406 |
| 5100 | 29.1, 28.4 | 17.5, 17.8 | 13.8, 14.2 | 21.8k, 21.9k | 4075, 4103 |
| 8900 | 29.0, 27.6 | 18.5, 19.1 | 13.4, 13.5 | 20.1k, 20.4k | 3848, 3908 |
| 12300 | 29.7, 29.8 | 15.6, 15.4 | 13.1, 13.1 | 17.6k, 17.3k | 3227, 3179 |

With the build before the last round of changes (below), three other fights
of 27-28k vertices and 5.4k draws a frame ran at 20 fps (GE 20.6-22 ms,
other 14-16 ms) and one of 39k vertices at 15; the final build was not
sampled on one of those, because a different build plays a different fight.

### What was changed, in the order of measured effect

- **VFPU instructions are translated** (`recomp/vfpu_emit.py`, emitted by
  `recomp/emit.py`).  The interpreter was called 36,000 times a frame for
  12.5 ms.  14,544 of the game's 14,802 VFPU sites are now straight-line C
  with the register numbers and the prefixes folded in (the prefix state is
  tracked through each block and kept correct in memory at every
  instruction boundary; lv.q/sv.q use `vfpu_lvq`/`vfpu_svq`).  The remaining
  258 sites (vcmp, vcmov, the integer conversions) still call the
  interpreter, which now costs 0.5 ms.  `tools/test_vfpu_emit.py` checks
  every distinct VFPU instruction in the game (2,976, with the game's own
  prefix values, random ones and the unknown-state guard) against the
  interpreter on random registers and memory: 195,996 runs, 0 differences
  (only the sign of a zero from fminf/fmaxf is allowed to differ).  The
  generator can be switched back with `RECOMP_VFPU=0`.
- **Draw cache** (`runtime/gpu/vcache.c`).  92% of the draws that are not
  skinned repeat an earlier frame's vertex bytes, and the vertices a
  draw makes depend on nothing else that changes: not the camera (the chip
  applies the view), not the world matrix when the vertices are kept in
  object space (the chip is given a model-view matrix in its second position
  slot), not a scrolling texture (the texture coordinates are cached raw and
  scaled when sent).  It is four-way set associative, keyed by the address
  and hashes of the bytes, the world matrix, the lighting signature and the
  material (`ge_light_sig_flat()` ignores the directions and diffuse colours
  of directional lights, which a vertex without a normal does not feel; the
  full `ge_light_sig()` is used for draws with normals); blocks are recycled
  per draw size so it does not leak.  It takes 1.6 MB of the texture pool,
  which the textures coped with (`texture decodes 0`, no allocation
  failures).  Hit rates are 84-93% once a scene has settled.
- **Skinned fighters are decoded as a whole into the chip's vertices**
  (`decode_skinned_packed`).  The invariants are read once per draw, there
  is no call per vertex, and the colour is packed to bytes where it is made;
  a lighting-off or no-light draw is a constant colour without normals.
- **Lighting is flattened** (`LightFlat`, `light_flat()`).  The fighters are
  lit by one to three directional lights, diffuse only or with an
  integer-exponent specular term (the half vector is constant), so a vertex
  is three dot products and a few multiply-adds, inline.  The general
  lighting cost 0.47 us a vertex; this costs 0.15-0.2.
- **`draw_prim_fast()`** takes the common draws (skinned fighters and cache
  hits, including those that change the vertex format or the pixel state)
  without the general path's large frame and register saves.  Everything
  else still goes through `draw_prim()`.
- **The texture check hash has no multiplications.**  A multiply costs
  several cycles here and the old hash did two per eight bytes; the
  pixel-state rebuild that validates a texture fell from 13-17 us to 5 us
  (200 a frame).
- The chip is sent packed vertices (`GxVtx`: x y z u v and the colour bytes)
  instead of nine floats a vertex that were converted twice.
- Fog and the view-space position are not worked out for vertices drawn by the
  chip, which does fog itself.
- The SAS mixer keeps a voice's state in locals for a grain, and skips the
  envelope step of a held note (sustain with a zero rate leaves the level
  alone).  It cost 7 ms a frame in a scene with many voices and 0.5-3.5 ms in
  the fights measured since.
  The voice loops, the output loop, the VAG block decode and the feeder's `audio_pull`
  were then rewritten to give the same bits at half the cost (the sound work in a
  heavy fight 1.08 -> 0.53 ms a frame); `EXP_SASCHK` and `tools/test_sas.c` compare
  the output with the old code.
- Generated functions take their register file as `restrict`, with a compiler
  barrier before the cold path of `guest_mem_tick()` (which can run a guest
  interrupt handler on the same context).  The saving is small (0.5 ms), and
  `-O2` for the generated code saved about as much (0.6 ms, +0.6 MB), so
  the default stays `-Os`.
- The model-view matrix and the "are the lights directional" test are looked
  up by generation (`g_ge_world_gen` is new) instead of compared every draw.
- Found and fixed along the way: `light_state_op()` / `vertex_state_op()`
  stopped at the second light's colour registers, so a change to the colours
  of lights 1-3 did not invalidate the light cache.

### Things measured that were not the problem

Guest scheduling (61 reschedules, 6 thread switches and 51 HLE calls a
frame), controller scanning (0.04 ms), the audio stream (0.5 ms), the GE
command parse (40,000 commands a frame, 2.5 ms; only 8 opcodes matter) and
the scalar generated code (`-O2` made no difference).

### Compiler bug

GCC 15.2's `try_forward_edges` bug (`cfgcleanup.cc:580`) moves between
functions of `raster.c` as the file changes, and it is intermittent: the same
source failed once in four builds.  The unit now builds with
`-fno-thread-jumps -fno-crossjumping -fno-guess-branch-probability`, the
`optimize("Ofast")` functions add `no-thread-jumps`, `decode_vertex_generic`
(where it hit most often) is `O1`, and the Makefile compiles `raster.c` up to
three times before giving up.

### Remaining

Fights of 27k vertices and more (a stage with many props or effects) ran at
20 fps before the last round and need about 4 ms more.  The heaviest costs
left are the skinned decode (about 0.65 us a vertex, 9.4 ms a frame), cache
hits (2 us a draw, 5-6 ms at 2,800 hits a frame) and the audio mixer in
scenes with many voices.  Merging runs of cached draws into one chip
submission, and trusting a cache entry for a few frames once its bytes have
been stable for a while, are the next candidates; neither was tried.

### Debug tools added (all under `TKDR_AUTOPILOT`, none in production)

`DEMOFPS`, `SCENE`, `DRAWCATS` (time by kind of draw), `FASTPATH`,
`REBUILDS`, `GEOPS`/`GECMDS`, `HLEPROF`, `VCLASS` (VFPU instruction classes),
`VBENCH` (a vertex-decoder micro-benchmark at boot), `OTHER3D`,
`SKINREFUSED`, and the experiment switches `EXP_NODRAW`, `EXP_DECODEONLY`,
`EXP_NOFAST`, `EXP_NOCACHE`, `EXP_NOSKINPK` and `EXP_AB` (alternate the fast
path every report).

### Shipped

Production DOL (GX default, full resolution, probe-free; checkpoint `1F8289C7`):

    SHA256 1F8289C765DBAD5B51ABB6189A70E468D482295B3102CEDCA128D5F86A064145
    length 9,951,552 bytes

It is copied to `dist-tkdr/apps/tkdr/boot.dol` and
`dist-tkdr-sd-full/apps/tkdr/boot.dol`.  It contains none of `GXPERF`,
`GXREJECT`, `gxr plan`, `gxr features`, `GXSTATE`, `HLE_TRACE`,
`GPU_SELFTEST`, `SELFTEST`, `FRAMETIME`, `SPIKE`, `TEXNATIVE`, `DEMOFPS`,
`VBENCH`, `DRAWCATS`, `VCACHE`, `HLEPROF` or `AUTOPILOT`; the unit tests
pass; it boots to the attract mode in Dolphin, which cycles through
different stages (the intro movie slot is still black, as before).

The generated code has to be regenerated for the VFPU translation
(`python tools/prepare_tkdr.py ...`, or `python -m recomp.emit <eboot> gen`);
a `gen/` made by the earlier generator still builds and runs, only slower.

## Fight frame rate, second pass: guest memory accesses, the scheduler tick, draw state (2026-10-24, later)

The first pass left fights of 27k vertices and more at 20-24 fps.  This pass
found most of the remaining time in the guest thread, not in the draws.

All numbers are from the release-speed autopilot demo under Dolphin, whose
time base counts instructions (it has no cache or memory latency model); a
real Wii will add its cache misses on top.  Each build plays a different fight
(`AUTOPILOT_SHIFT_MS` samples others), so compare `busy` and the ms figures
rather than the scene.

| | before | after |
| --- | --- | --- |
| guest thread (sampled by `GUESTPROF`) | 12.5 ms a frame | 6.0 ms |
| `other` (everything but the GE thread) | 12-20 ms | 7.4-9.9 ms |
| three heavy shifts (2500, 18000, 10000; 17-26k vertices) | 20-29.4 fps, 74-93% busy | 28.8-30 fps, 68-81% busy |

### What was found and changed

- **The guest memory accessors were not inlined.**  At `-Os` a guest load or
  store (`mem_r32`, `mem_w32`...) was a call, and the helpers inside it
  (`guest_mem_tick`, `host_fast`) were calls too: some 45 instructions per
  access, with a frame each.  The helpers are now `always_inline`; the
  accessor itself is a frameless leaf (`mem_r32` is 8 instructions on the
  common path) that the generated code still calls, to keep the code small.
- **The scheduler tick moved out of the accessors** (`guest_loop_tick()` in
  `runtime/recomp.h`, emitted by `recomp/emit.py`): every function that calls
  something or contains a loop ticks once on entry, and every loop head ticks;
  a leaf without a loop does not.  Anything that runs long passes one of those
  places, so the cooperative scheduler sees the same cadence (a mask of 1024
  ticks instead of 2048 accesses).
- **Accessors are expanded in place inside tight loops** (a backward branch
  of at most 16 instructions, `RECOMP_LOOPINLINE_MAX`; `RECOMP_LOOPINLINE=0`
  turns it off): a call's argument setup and the guest register reloads after
  it cost more than the access there.  Expanding at all 210k sites would have
  added 4.6 MB to the DOL (MEM1 is 24 MB; text, data and bss are about 17 MB
  of it, leaving 5.5 MB at start); this adds 1.1 MB.  The loop tick and the
  inlining together grew the DOL from 9.95 to 11.64 MB.
- **`draw_prim_fast` keeps a state cache** (`s_hot`): the vertex format, the
  pixel state, the draw cache's register hashes, the model-view matrix and the
  skinned decode's invariants are worked out again only when the sum of the
  five register generation counters, the vertex type, the texture pack epoch
  or the display buffers moved.  The chip's setup is skipped while its own
  epoch (`g_gx_epoch`) has not moved either.  An A/B inside one run (`EXP_AB`)
  measured 1.3 ms less GE time a frame (skinned 9.6 -> 8.9 ms, cache hits 2.8
  -> 2.2 ms).
- **A draw that misses the cache is decoded in the fast path** (`plain_miss`)
  instead of through the general path; the draw cache's slots are 32 bytes in
  128-byte aligned sets of four; the GE interpreter reads guest RAM as one
  window and loads the stall address without an `isync`; and the list reader
  prefetches the vertices and the cache set of the draw that follows a
  `VADDR` and the first lines of a called list.  The last three do nothing in
  Dolphin and are meant for the real console; they are unmeasured.

### What was tried and did not work

- **Write tracking** (a stamp per 1 KiB of guest RAM, set by every store and
  read to tell static vertex data from vertices rebuilt every frame, with the
  idea of validating a cached or replayed draw by stamp instead of by hash).
  It cost 0.25 ms a frame, and it classified every plain draw as "written
  since the last frame", which switched the draw cache off.  A debug store
  probe (`-DTKDR_WATCH`: 256-byte stamps and the writing function) showed the
  reason: the guest rewrites the vertices of *all* plain draws every frame
  (a handful of guest functions, `f_08A27A98`, `f_08A0B200`, `f_08A0AD84`,
  `f_08A0AE84`, `f_08A0B110`, `f_08A1ACA8`, build them), and 55-70% of those
  draws then find their bytes unchanged.  So the hash of the bytes is the only
  valid check, and a stamp can never prove a draw unchanged.  Write tracking
  was removed again.
- **Replaying whole runs of draws as GX display lists** was not built.  Every
  3D draw is inside a CALL sub-list (about one draw per CALL), the game
  rebuilds its main list each frame by copying static lists with patched CALL
  targets (`f_08A189D8`, about 1 ms), and the vertices a replay would depend
  on are rewritten every frame, so a replay would still hash every draw and
  save only the interpreter and set-up work: 1.3-2.5 ms for a lot of risk
  (display lists have not been tried on a real console).
- **Per-function profile of the guest thread** is flat: after the changes
  above the heaviest function (`f_08A189D8`, the list copy) is 1.1 ms of a
  6 ms thread and nothing else is above 0.3 ms.

### Debug tools added

`GUESTPROF` (a sampling profile of the generated code: the guest memory
counter's roll-over charges the time since the last one to the calling
function; resolve the addresses with `powerpc-eabi-nm -n` on the ELF),
`PRIMDEPTH` (draws and commands at the top of a list and inside calls),
`FASTSPLIT`, `GUESTSTORES`/hit-run histogram, `FEPARTS`.  The per-draw and
per-command probes (`FASTSPLIT`, `GEOPS`, `DRAWPROF`) are only built with
`-DTKDR_FINEPROF`, so the plain `-DTKDR_AUTOPILOT` demo is closer to the
production build.  `-DTKDR_WATCH` is the store probe above (`WATCH`, `WRITER`
lines).

### Remaining

Skinned fighters are the largest single cost (about 8.5 ms for 2,500 draws, 0.43
us a vertex under Dolphin's instruction counts); the draws change every
frame, so only cheaper arithmetic helps (done in the third pass below).
Cache-hit draws are 2 ms, the audio mixer up to 2.4 ms in scenes with many
voices.  Whatever real-hardware cache misses add to the vertex and cache-slot
reads is unknown until it is run on a console; the prefetch hints and the
128-byte aligned sets are the guesses made for it.

## Fight frame rate, third pass: paired-single skinning (2026-10-24, evening)

`runtime/gpu/skin_ps.c` is the skinned fighter decode in Broadway assembly
(this devkitPPC GCC has no `-mpaired`, so it is hand written; `skin_ps.h`
describes the parameter block it reads by offset, checked by a
`_Static_assert`).  One call does a whole draw.  Per vertex it
- loads the 16-bit positions (staged big endian) and the 8-bit normals, weights
  and texture coordinates with quantised loads (no integer-to-float code);
- applies each weighted bone to the position and normal together, two floats
  per instruction, straight from the matrices as the guest laid them out, then
  the world matrix over the sum;
- lights it (up to three directional lights, with or without a specular term,
  or one constant colour), and stores the colour as four saturated bytes with a
  quantised store, which replaces the clamps and conversions.
It uses f0-f13 and r0, r3-r12 only (all volatile) and sets quantisation
registers 1, 6 and 7 on entry; libogc's context switch saves the paired
halves and all eight registers.  Nine variants (colour, diffuse, specular x
no, 8-bit or float texture coordinates) come from one macro.  Draws with
other texture coordinate types, or more than eight weights, use the scalar
code (`skin_run_c`).

| per frame, 21-39k vertex fights under Dolphin | scalar | paired singles |
| --- | --- | --- |
| skinned decode, a draw | 2.94 us | 1.71-1.75 us |
| the skinned category (2,500 draws) | 10.1 ms | 7.2-7.7 ms |

Both heavy shifts (2500, 18000) hold 30 fps at 75-88% busy.  `-DEXP_NOSKINPS`
switches the kernel off.  `-DEXP_SKINCHK` runs both paths on every skinned draw
and compares them: 20 million vertices, no bad ones, colour within one level,
texture coordinates identical, largest position difference 0.0024 on
coordinates up to 10,416 (that is float resolution at that size: the two paths
sum in a different order).

### A camera shake report, and what was measured

Someone saw camera jitter in a recent build, not knowing which.  In
autopilot fights under Dolphin nothing in the data shows it:
- the camera's world position and viewing direction (`CAMERA`, `CAMDIR`: the
  view matrix at the first skinned draw of each presented frame) have second
  differences of 0.1-0.2 of the step, the same in the last pushed commit, the
  current build and the current build with the kernel off; the windows where
  the ratio rises (direction, up to 0.9) are in the old build as well and peak
  at the same 0.0246 rad, which looks like the game's own hit shake;
- the geometry from the draw cache matches a fresh decode (`-DEXP_HITCHK`: 375
  thousand re-decoded hits, 0 bad; 3.4 million model-view checks, 0 stale;
  the chip's matrix slot always held the matrix the draw asked for);
- the guest frames last two display refreshes 147-149 times in 150 in a fight
  (`PACING`), so the pace is even.

### Debug tools added

`CAMERA`/`CAMDIR`, `PACING`, `SKINSHAPE`/`SKINLIGHT` (`-DTKDR_FINEPROF`: the
weights, light mode and texture coordinate kind of the skinned vertices),
`HOTREFRESH` (how often and how dearly `draw_prim_fast` redoes its cached
state: 81 plain refreshes at 6 us and 728 skinned ones at 1.5 us a frame), and
the `EXP_SKINCHK` and `EXP_HITCHK` checkers above.

## Correction: 30 fps is half speed (2026-10-24, night)

Every "30 fps" above, and "the game's cap of 30" in the earlier passes, was
wrong.  The fights are frame-locked at 60 logic frames a second: the round timer
in a fight counted 15 game seconds in 30 real seconds (54 at 110 s into a run, 39
at 140 s) while the guest ran at 30 frames a second, so the game was at half
speed.  A run that draws nothing reaches 57-60 guest frames a second.  The guest
rate is quantised by the vblank wait (60, 30, 20, 15...), which is why the fights
stuck at 30: a frame that takes more than 16.7 ms is shown for two refreshes and
the game's clock slows to match.

So the real target is 60 guest frames a second, a budget of 16.7 ms a frame for
everything (guest thread, GE thread, sound), and a console needs headroom on top
of what Dolphin shows.  Today a heavy fight costs 21-24 ms a frame under
Dolphin (GE thread 12-13.5, everything else 8.5-10), so it is 5-8 ms over, and
more over once cache and memory latency are counted.  Use the round timer, or
guest frames a second divided by 60, as the speed measure, not "busy" alone.

## The floor: guest thread and list work (2026-10-24, night)

Measured against the correction above: the draw-nothing build is 85% busy at 60
frames a second, so the guest thread, the sound and the list work have to come
down as well as the drawing.  Three changes, measured on three fights (the
`other` figure is everything but the GE thread, per guest frame):

| | before | after |
| --- | --- | --- |
| everything but the GE thread | 9.8-10.8 ms | 8.4-8.7 ms |
| guest thread (`GUESTPROF` total) | 7.8-8.1 ms | 6.3-6.8 ms |
| DOL | 11.67 MB | 11.95 MB |

- **Native overrides** (`profiles/tekken6/config/overrides.txt`,
  `runtime/hle/overrides.c`, `recomp/emit.py`).  A guest function can be replaced by a
  native one: the generated function keeps its name and every call site and
  calls the native version, and keeps its translated body as `f_XXXX_orig`.
  The first override is `0x08A189D8`, the copy of the game's static display
  lists into each frame's list (CALLs turned into BASE + CALL pairs, 0.9 ms a
  frame as translated code, walking host pointers now).  `-DEXP_OVRCHK` runs the
  original beside it on every call and compares all 32 registers and every word
  written: 590,000 calls, 0 different.
- **Hot functions at -O2** (`profiles/tekken6/config/hot_functions.txt`).  The guest
  profile is flat (19 functions are half of it, 189 are 95%), so a function-level
  fix does not exist, but the 189 hottest are generated into `funcs_hot.c`, which the
  Makefile builds with -O2 while the rest stays -Os.  Regenerate the list from
  the `GUESTPROF` lines of an autopilot run (`GUESTPROF` now prints 700 entries).
- **A SHA-1 in the guest** (`f_08A36278`, `f_08A363E0`) is 4-6 ms a frame while the
  game loads and nothing in a fight: a loading cost, a native version would speed up
  stage loads.
- **Merged skinned submit was not built.**  Triangle lists already go through a
  batch (`s_batch`); a strip or fan is one `GX_Begin` per draw, but the per-vertex
  stores are most of the submit (about 130 instructions a draw of which `GX_Begin`
  and `GX_End` are about 15), so merging draws saves under 0.3 ms.  Sending the
  kernel's output straight into the write-gather pipe would save about 0.7 ms but
  needs a store sequence of its own for each of the nine kernel variants.
- **"Frontend self" is idle time**: the frontend thread spins on `wii_yield()` until
  the next refresh, and the report counted that spin as work.

## The intermittent black 3D (2026-10-24, night)

Two autopilot runs (of about 40 since the first sighting) showed the whole 3D scene black from the start
of the second round, with the HUD intact (timer 54, health bars drawn).  What the probes found:

- **Both black runs had a round end in them**: the picture faded out at the end of round one (3,500-4,000
  chip draws a frame, mean luma 31 then 8 then 0 over ten seconds) and never faded back in.  Every run
  without a round end inside the 128 s window was fine, which is why the rate looked like 25-37% on the
  first builds (the fights were ending sooner) and why it vanished later (fights that never reach a
  round end); the baseline at half speed never got there.
- **It is not the clears**: every frame, dark or bright, starts with one colour+alpha+depth clear that is
  applied, never skipped (`CLEARS` lines).
- **It is not the GE state**: the first world-space draw of a dark frame has the same depth test, depth
  range, scissor, frame and depth buffer pointers, blend and lighting registers as bright frames with the
  same vertex type (`SNAP`).  The view and projection are valid.
- **It is not the cached chip state**: `EXP_COLD` (no hot state cache) was black once in five.
- **Not reproduced since**: 0 of 10 long fights (215 s, every one reaching round two), 0 of 28 short ones.
  The `-DEXP_DARKDBG=n` build (depth test off, white vertex colours, no texture once a run has been dark
  for 100 frames) was never triggered.

The remaining reading is a fade that the game's own logic left at full black (the HUD is drawn after the
scene), not a rasteriser fault; it is not proved.  The probes stay in the autopilot builds: `CLEARS`,
`SNAP` (all 256 registers every 120 copies), `BIGQUADS` (screen-covering draws and their colour, every ten
dark frames), `DARKDBG`.  If it shows again, look at the `BIGQUADS` line of a dark frame first: a full
screen black quad that is still there says the fade is the game's.


## Steve's hair: the chip never culled back faces (2026-10-25)

**Symptom** (character intros, hard to see in fights): the top of Steve's head was a dark, hard-edged cap
(dark green on a green stage, dark brown on others) showing over and between the blond strands, in patches
that followed the polygons.  The PSP shows blond hair all over the top.

**Cause.** The GX path drew every triangle with `GX_CULL_NONE`.  The GE rejects the triangles that wind
the wrong way when `GE_CULLFACEENABLE` is set (the software path's `cull_reject()` always did).  The hair
is a shell of strips drawn twice (an opaque pass with a 32x64 strand texture, then the same vertices
alpha-tested and blended); its back faces face away from every light (`base` 38 only), so they come out
dark, and the layered strips let them show.  A trace of the 1,000+ small strips a head is made of (the
`DT`/`DV`/`DN` lines under `-DEXP_DRAWTRACE`) showed the light colours were right for those normals
(world normal against the triangle's face normal: cosine 0.95-0.99), which ruled out the lighting.

**Wrong first guess, removed.** A 24-bit-depth tie between the strand layer and the scalp (a +/-2 and
+/-300 step depth bias on the world viewport) changed nothing in the same scene; it is not in the tree.

**Fix.**
- `GxrState.cull` (0 off, 1 and 2 the two windings, from `GE_CULLFACEENABLE` and `GE_CULL`; off for
  through mode and clears), set in `gxr_state()`; `apply_pixel()` calls `GX_SetCullMode` and
  `pixel_differs()` compares it.
- `GE_CULLFACEENABLE` / `GE_CULL` left `vertex_only_op()` in `ge.c`, so a write bumps the pixel
  generation (the pixel state is rebuilt, and `gxr_state()` runs again).
- The chip's front face is the GE's positive screen area, so the mapping is `{none, FRONT, BACK}`.  The
  other way round deletes the stages (they are single sided), which is how it was found.
- `-DEXP_NOCULL` puts the old behaviour back for A/B runs.

**Measured.** Four stages (ice, forest, palace, scrapyard) and a Lili-Dragunov fight: no dark cap, no
missing faces.  The same fight in the demo build: GE 12.5-13.5 ms a guest frame with culling, 14.5-15.4
ms without (other 7.9-8.7 ms either way), so culling saves about 1.7 ms of the GE thread as well.  The
speed line is still 49% (29.6 guest fps): this is a correctness fix that happened to be cheap.

**Test aids added** (autopilot builds only): `-DAUTOPILOT_STEVE` (holds the cursor on Steve for the
intro), `-DTKDR_FIXTIME` (the guest's wall clock fixed; the stage still varies between builds), and the
`EXP_DRAWTRACE` additions (hair texture dump `TX`, texture function, uv and raw weights/normals in `DV`,
world normal against face normal and the lights in `DN`).

## Elastic vblank, the SHA-1 override, and where the skinned draws stand (2026-10-25)

**Elastic vblank (now the default; `-DTKDR_FIXED_VBLANK` brings the fixed grid back).**  The guest's frame
loop waits for the next tick of a 16.7 ms grid, so a frame that took 17-33 ms was held to 33 ms: 30 guest
fps, half speed, however little it overran.  Now a vblank wait that starts after a tick went by with
nobody waiting (the frame is late) returns at once and moves the grid to that moment
(`hle_wait_vblank()` in `thread.c`; `sceDisplayGetVcount` reads the same count through
`hle_vblank_count()`, which stays continuous).  The guest's speed is then 1 / (frame time x 60), down to
60 Hz, and frames the game is early for wait as before.  The game logic is the same, it just runs at the
rate the frames take.

| same fight, Dolphin demo build | fixed grid | elastic |
|---|---|---|
| guest fps (SPEED) | 29.6 (49%) | 50.7-51.0 (84-85%) |
| GE / other ms a guest frame | 12.5 / 8.0 | 11.8-12.1 / 7.1-7.6 |
| a heavier stage (GE 17-18 ms) | 49% | 66-70% |

Looked at: title, menu, character select, a loading screen, fights, a K.O.; the round timer runs at the
measured rate.  Not looked at: movies (their pacing reads the same vblank count).  The elastic build
sums to the frame budget: one CPU runs the GE thread and the rest, so a frame is the GE and "other" times
added (about 19.4 ms in the first fight), which is what is still above 16.7.

**SHA-1 block transform, native (`ovr_08A36278`).**  The game hashes the data it loads (4-6 ms a frame
while loading).  Native versus the translated original on the same blocks (Dolphin's time base, 300,000
blocks): 4.6 us against 17.2 us a 64-byte block, 3.75 times faster; over a 110 s boot that is about 3.8 s
less loading.  `-DEXP_OVRCHK` compared all 32 registers and the rewritten schedule on about 2.5 million
blocks: no mismatch.  (The A/B itself was a one-off edit of the generated stub; `emit.py` keeps the
original body as `f_XXXX_orig` for exactly that.)

**The skinned draws, measured to see what is left (`TKDR_FINEPROF`, a heavy fight, 2,700-2,900 draws and
14.8-15.7k vertices a frame).**  `skin_prof()` now counts for the paired-single kernel too (it counted
only the scalar code before, which is why the old lines read zero).
- Lights: three on 89-90% of draws (284 draws have lighting off, none have one or two), a specular term
  on 17-21%.  Texture coordinates: 8-bit on 905 draws, float on 1,786, none on the rest.
- Weights: one bone with the full weight on 57% of the vertices, two on 39%, three or four on 4%; the
  vertex formats carry 3.6 weight slots on average, so about 2.2 zero weights are stepped over a vertex.
- Cost a draw: kernel 2.12 us (about 0.385 us a vertex, roughly 180 instructions: lighting 68, the world
  matrix 25, each bone 30 and each zero weight 7, the position and normal setup 30), checks 0.45 (0.30 of
  that is the state refresh spread over the draws: 293 skinned refreshes a frame at 2.95 us, 0.87 ms; format
  0.20 us on 279, pixel state 1.57 us on 77, vertex state 0.87 us), setup 0.19, submit 0.25.
- Specialising the kernel by light count would help 10% of the draws: not done.  Combining the world
  matrix into each bone (saves the second matrix apply, about 20 instructions a vertex, 0.65 ms) needs the
  product recomputed at every bone upload, and the vertex generation (bone uploads and texture scale and
  offset writes) moves about 800 times a frame, 3-4 draws apart: the products would cost about what they
  save (an estimate, not measured), so not done.  The skin-state
  reuse that is in saves 0.12 ms a frame (3.37 to 2.95 us on 295 refreshes).
- GX rigid skinning (single-bone vertices through the chip's position matrix slots): sized, not built.
  57% of the vertices would skip about 130 of their 180 instructions, 2.3 ms a frame at most, less the
  matrix uploads and the view x world x bone products (an estimate: about 0.9 ms at 800 uploads a frame), leaving
  1.4 ms (6%).  It would also need the chip's own lighting (which does not renormalise the normals the way
  the flattened lighting does, so colours move by a few levels, and spec draws would fall back) and a
  second vertex format with a state switch between the two.  Not worth it for that; the numbers above are
  what to revisit if the budget gets that close.

**A build fix.**  `0da9b74` did not compile without `TKDR_AUTOPILOT`: two `DP_T()` probes in
`hot_refresh()` were outside their `TKDR_FINEPROF` guard.  They are inside it now, and the plain `make`
DOL boots to the attract mode in Dolphin.

## The cache-hit draws and the texture lookup, measured (2026-10-25, later)

Started as P1 of the handover (merge runs of cache-hit draws into one chip submission); the measurements say
that is not where the time is, and found a smaller win in the texture lookup.

**Baseline** (demo build at `fce03a6`, 150 s runs, one DOL per shift): shift 2500 74-76% speed (ge 14.2,
other 7.4-7.8 ms); shift 18000 71-76% (ge 13.7-15.6, other 6.9-7.7).  The scenes differ from the ones in the
handover table because a rebuilt binary plays another fight.

**A cache-hit draw, `TKDR_FINEPROF`** (about 1,420-1,510 a frame, 2.1 ms): checks 0.46 us (0.35 of it the state
refresh spread over the draws), lookup 0.58 (hash 0.19, set search 0.07, the rest call and bookkeeping),
chip setup 0.24 (mostly the setup that follows each refresh), submit 0.28.  Hits come in 54 runs a frame (average 26
draws), and 29 runs of 16 or more hold 1,241 of the 1,418 hits.  The plain state is refreshed 82-98 times a frame
(1.5-1.8 a run), and those changes, not the runs, carry the fixed costs: **merging a run into one `GX_Begin` could save only
the begin itself (about 15 of ~130 instructions a draw, under 0.1 ms)**, as with the merged skinned submit.  Not built.

**Where the plain refresh goes** (`HOTREFRESH`: plain 49-98 x 5.4 us, skinned 295-326 x 2.6-3.0 us, 1.1-1.4 ms a
frame): mostly the pixel state, which is mostly the texture lookup (`TEXLOOKUP`: 145-190 lookups, 0.7-1.05 ms).
New probes (`TKDR_FINEPROF`): `TEXDECODE` (decodes: none in steady state; every stale lookup found its bytes
unchanged) and `TEXLOOKUP parts`.  The parts: a lookup that finds a clean entry costs 2.12 us, and the
preamble of a stale one is the same 2.12 us, of which almost all is `hash_bytes()` over the **whole 1 KB CLUT**
(every paletted texture is preceded by a LOADCLUT, which marks the hash dirty).  The re-hash of the texture
source itself (120-130 a frame, 250-320 KB, 0.42-0.58 ms) is already at one add per word, and cannot go without
guest write tracking (a store hook costs about what it saves).

**Change: a 4-bit texture hashes only the 16 palette entries it can reach.**  `((i >> shift) & mask) | off`
with `i < 16` and `off` a multiple of 16 stays inside `[off, off+16)`, so the other 240 entries cannot change
the texels (`tex_cache_lookup()`; 8 words for a 16-bit palette, 16 for 32-bit; CI8/CI16/CI32 still hash all of
it).  The preamble fell from 2.12 to 1.17 us (the rest is the CI8 textures), about 0.15 ms a frame at ~165
lookups.  It also stops a texture being decoded again when only palette entries it never uses change.
`-DEXP_FULLCLUTHASH` restores the whole-palette hash.  **Checked** with the new `-DEXP_TEXCHK` (every eighth
lookup of a 4-bit texture is decoded again from guest memory and compared with the cached texels, a sample of
the texels): 37,633 textures in a run through menus, loading screens and a 19k-vertex fight, 0 different (the
first, fuller version of the check, 215,000 and then 795,000 in menus and loads, 0 different, made the game
so slow that the scripted menus ran ahead of it).  The speed A/B of old against new hash did not give a
number: the two builds played different fights (14k and 25k vertices at shift 18000), so the evidence is the
per-lookup probe above, not a SPEED line.

**Tooling note.**  `build_shifts.ps1` after a flag-only change leaves `frontend.o` from the previous flags
and the link then fails with `undefined reference to g_sk_*`: delete the `.o` of every source that
uses the flag (here `frontend.c`, `raster.c`, `ge.c`, `vcache.c` for `TKDR_FINEPROF`).  The scratch DOLs
(`tekken6wii-<tag><shift>.*`) are deleted again.  The production DOL (11,931,616 bytes) has none of the
probe strings and boots to the attract mode in Dolphin.

## A vertex type switch no longer refreshes the whole hot state (2026-10-25, later)

**What was measured.**  `HOTWHY` counted why the fast path refreshed its state (`hot_refresh()`, about 375
times a frame in a fight, 1.1-1.4 ms): the vertex type had changed in 330 of them.  A fighter is drawn in parts
with their own bone count and stride, and the draws alternate between a few vertex types, so nearly every
skinned refresh was only a switch of format, with the lights, pixel state and view as they were.  (The three sub-probes of `HOTPARTS`, format 0.20, pixel state 1.58 and vertex state 0.90 us
per call, add up to the 2.6-3.0 us measured for a skinned refresh: nothing in it is unexplained.)

**Change.**  `hot_switch()` in `draw_prim_fast()` (after `hot_quick()` and before `hot_refresh()`): when only the
vertex type moved, and the state is a prepared skinned one that is still current (the sum of the generations
without the vertex one, the light generation, the texture-replace epoch, the guest frame and the through flag
are as they were, and the new format is one the paired-single kernel takes), it redoes the format
(`vformat()`), the kernel's layout (`skin_reformat()`) and the texture scale (`hot_skin_tex()`) and nothing else.
It is the same state the long way round reached through `skin_reformat()` already, minus the pixel state, display
target and bookkeeping work.  Anything else falls through to `hot_refresh()` as before.  `-DEXP_NOSWITCH` turns it
off.

**Measured** (`TKDR_FINEPROF`, shift 2500, one run): 217 switches a frame at 0.42 us each (0.09 ms); skinned
refreshes fell from 295-326 a frame (0.85-0.9 ms) to 78 (7.8 us each, the ones that really change the pixel
state, 0.61 ms), so about 0.17 ms a frame saved.  **Checked** with `-DEXP_HITCHK` over the same run: 940,590
skinned states rebuilt and compared with the hot one (every eighth skinned draw), 1,881,179 pixel states, 0
stale; the plain demo build plays both heavy shifts (79-80% and 63-69%; those are other fights than the
baseline's, so not a speed comparison), a fight screenshot shows the fighters lit and textured as before.
`HOTSWITCH` is the new `TKDR_FINEPROF` line.

## The chip lights the fighters (2026-10-25, later)

**What it is.**  A skinned draw whose lights are all diffuse (three directional lights on ~77% of the fighters'
draws; the rest have a specular term and stay on the CPU) is now lit by the GX chip.  The paired-single kernel
gets three more variants (`tkdr_skin_ps_hn/hu/hf`, for no, 8-bit and float texture coordinates) that
normalise the world normal as before and store it as three 16-bit numbers (14 fraction bits, the form the chip
reads, through quantisation register 6) in a 28-byte `GxVtxL` instead of working out the colour.
`wii_gxscene_draw_lit()` sends those vertices with the vertex description position + normal + texture
coordinate and a colour channel with the lights on: each light is a light object whose *position* is its unit
direction x 2^20 (a `GX_AF_NONE` light takes its diffuse direction from its position), its colour the diffuse
colour x material rounded to 8 bits; the ambient colour is the flattened lighting's `base`, the material is
white, the alpha comes from the material register, and the normal matrix of position matrix 0 is the identity
so that normals and light positions stay in world space.  The lights are loaded when their content changes
(8 times a frame), the vertex description and channel when the draws change kind (13 times a frame); every
other draw leaves lit mode first.  **Default on; `-DEXP_NOGXLIGHT` lights on the CPU as before** (that build is
byte for byte the size of the previous production DOL, 11,933,824 bytes; the default one is 11,937,792).

**A mistake on the way.**  The first version switched back with `GX_SetVtxDesc(GX_VA_NRM, GX_NONE)`.  libogc
keeps a counter of "normals on" that only `GX_ClearVtxDesc()` resets, so the transform unit went on expecting
normals and Dolphin printed "Mismatched configuration between CP and XF stages" for every draw (a flood of
pop-ups).  Both switches now set the whole description again after a `GX_ClearVtxDesc()`.  A run started while
the next build was compiling reached only the menus: never build while Dolphin runs.

**Measured.**  `-DEXP_ABLIT` alternates chip and CPU lighting at every report inside one run, so both see the
same fight (windows of 300 frontend frames, demo build, adjacent windows of the same scene size paired):

| fight | windows | chip - CPU, ms a frame | where |
| --- | --- | --- | --- |
| shift 2500, 19k vertices | 20 pairs | -0.58 (sd 0.99) | ge -0.62, other +0.04 |
| shift 18000, 21k vertices | 6 pairs | -0.57 (sd 0.56) | ge -0.55, other -0.02 |
| shift 18000, windows from 12k vertices | 18 pairs | -0.37 (sd 0.69) | ge -0.27 |
| shift 2500, windows from 3k vertices | 24 pairs | -0.50 (sd 0.95) | ge -0.55 |

Means over the 19-21k vertex windows: 20.75 vs 21.02 ms (47.2 vs 46.5 guest fps) and 19.92 vs 20.88 ms (49.0 vs
46.7).  Per op (`TKDR_FINEPROF`, shift 2500, two builds: other fights, so the per-op rows only): skinned decode
2.06 -> 1.85 us a draw (all skinned draws; 1,965 of 2,561 a frame are lit), submit 0.24 -> 0.29 us (two more
stores a vertex), checks and setup as before.  That is 0.5-0.6 ms of a ~20 ms frame, 2-3%, not the 0.9-1.6 ms
the handover estimated.

**Checked against the CPU path.**
- `-DEXP_LITCHK`: the lit kernel's positions and texture coordinates against the scalar code (as `EXP_SKINCHK`
  does), the normals' length, and the colour the chip would work out (each light's term rounded, the sum
  clamped, times the white material) against the CPU colour.  34.4M vertices, 0 bad; of 103M channel values
  the chip's arithmetic is 0 levels off on 55.8%, 1 on 42.8%, 2 on 1.4%, 3 on 43; mean -0.30 level.
- `-DEXP_LITTEST`: the real GX lighting under Dolphin.  At the start of a display list, when the lights have
  changed, 64 small squares with normals spread over the sphere are drawn through `wii_gxscene_draw_lit()` with
  the game's own lights (colour straight from the channel, no texture, blend or test), the frame buffer is read
  back at their middles and compared with the CPU's float result.  12 runs (two light setups: ambient 24 27 28
  and 38 38 38, three lights): all 192 values of every run within one 6-bit step (the frame buffer is RGBA6, a
  step is 4 levels), none two apart; the chip about 1 level darker on average (mean step -0.08 to -0.29; the
  chip multiplies the light sum by 255/256 and rounds each light's term).
- Fight screenshots (production DOL, attract mode, ~145-160 s): the fighters shaded as before.  The grey and
  black frames around 115-145 s are loading screens of the attract mode, present in the `-DEXP_NOGXLIGHT` build
  too.  Dolphin logs of all the runs: no `Mismatched` or other GX warnings.

**Not done / not known.**  Nothing is measured on a console (the chip's 8-bit light arithmetic and the 2^20 light
position are conventions of the GX SDK and Dolphin follows them; hardware is unverified).  The specular draws
could use a second channel.  `tools/perf/shots.ps1` ignored its `-OutDir` (sourcing `env.ps1` overwrote it, and
two runs wrote to the same files): fixed.  The scratch builds and DOLs are deleted again.

## The draw cache evicted only way 0 (2026-10-25, night)

The "slowdown that only shows up in player gameplay" of the handover was a bug in the draw cache's replacement,
present since the commit that added it.  `vc_store()` chose the least recently used way of a full set with
`for (k = 0; k < VC_WAYS && !v; k++) if (!v || age < v->age) v = &set[k];`: `!v` ended the loop after the first
pass, so a full set always gave up way 0.  Ways 1-3 kept whatever arrived first and were never replaced, every set
was direct-mapped, entries of earlier scenes stayed pinned, and each later scene (the second fight, a new stage)
thrashed in one way: cold misses a frame 120 -> 600 -> 1,800 from fight to fight, ge 12 -> 15-17 ms at the same
vertex count.  A player changes scenes far more often than the autopilot script, hence "only in player gameplay".

How it was found (the first readings, arena growth and the store policy, were wrong): `-DEXP_VCDUMP` keeps a ring of
the last 16,384 lookups with the real outcome and the slot each store gave up; `tools/perf/vcsim.py` replays it
against a model of the cache.  The replay of the real shape (true LRU) did not give the real miss rate: sets with 3-4 live
draws missed on every lookup and each miss evicted the previous draw of the cycle, which is most-recently-used
eviction, not LRU.  Fixed (a plain scan for the oldest way); the cache now has 4,096 sets (the replay showed 2,048
losing 3-9% of the lookups of the heaviest scenes to sets with more than four draws; 512 KB of slots) and a collector
(`vc_collect()`, run when the 1 MB arena is full) that gives up the draws not used for a while and slides the rest
together, so a fight after a fight does not keep the first one's blocks.  Pooled later-fight windows
(autopilot with `-DAUTOPILOT_MASH`, ~21k vertices): lookups missing 24.8% -> 4.3%, ge 14.8 -> 13.2 ms, frame
22.4 -> 20.7 ms.

**The collector, checked.**  `-DEXP_HITCHK -DVC_ARENA_BYTES=262144` (a 256 KB arena; the autopilot fights need
up to ~570 KB, so with 600 KB it never filled and ran 0 collections), 230 s of Dolphin: 26 report windows, 50-108
collections a window, 591,000 cached draws re-decoded and compared by the last window, **0 bad**, 0 stale model-view
checks.  (The `VCARENA` report line printed a fixed "of 1024" until then; it now prints the build's arena size.)  The
production arena is 1 MB; with it the collector ran once in about 40 logs.

**Not built: the draw cache misses that come from the ambient alpha.**  Some stages (a fading group of small
flat-lit draws, vertex colour as the ambient material, lighting on, every light off) change the *global ambient
alpha* (`GE_AMBIENTALPHA`) every frame; it is in the lighting signature, so those draws miss on every frame
although their bytes do not change (`-DEXP_VCWHY` counts them: the same address, the same bytes and the same
state but the alpha).  Measured with the `PLAINMISS` probe (`TKDR_FINEPROF`): a miss costs 5.6-6.2 us (decode
2.7-3.3 us a draw, 0.60-0.63 us a vertex; store 0.11; pack 0.64-0.79), and the worst window of a 190 s run had 130
such draws a frame (694 vertices): 0.80 ms of a 12 ms ge; windows with 40-60 were 0.23-0.38 ms; the stage that first
showed 150-300 a frame cost 1.1-1.8 ms.  An in-place update of the alpha (re-writing the colour alpha of the
cached vertices from the vertex's own alpha and the new ambient alpha) needs the vertex alpha kept beside the
cached vertices (the product is quantised) and a second index without the alpha in it; a faster lit no-normal
decode (lights off: `me + ma * ga / 255`, no per-light loop) would save about 0.1 us a vertex (est.).  Both are
under 0.2 ms on average over a fight (est. from the windows above), so neither is built.  An attempt to leave the registers of *disabled* lights out of
`ge_light_sig()` / `ge_light_sig_flat()` (logically sound: a light that is off adds nothing) was alternated with the
old signatures inside one run (`EXP_ABSIG`, 300-frame windows): no difference in the misses or the frame time in
the fight tested, so it was removed again.

## The black 3D, solved: the game blits its own display buffer (2026-10-25, night)

The user saw the whole 3D scene black in play under Dolphin, HUD intact.  The "intermittent black 3D" of two
sections up is this, and its reading there (the game's fade stayed black) was wrong.  It showed in about one
autopilot run in six or seven (170 s runs with `-DAUTOPILOT_MASH`, onset at the cut from the intro camera to the
fight camera), 4 of ~45 runs before it was understood.

What the probes showed, in order (all in `TKDR_AUTOPILOT` builds):
- `DARKRUN` (a dark picture, mean luma < 12 with at least 1,000 draws, for 100 frames): every guest thread was
  waiting normally (`sched_dump_threads`, now compiled into the autopilot builds), asynchronous reads complete
  synchronously, the texture pool was at 10 of 14 MB with no failed allocation, the camera and the GE state were
  the same as in bright frames.  (The old detector, luma < 6, flickered: the black state reads 0-8.)
- `EXP_DARKDBG` stages on a run that stayed dark for 100 frames: depth test off (1) black, white vertex
  colours (3) black, **a white texture in place of every texture (11) bright**, no textures (7) bright.
  So only the texel contents made it black.
- `TEXPROBE` scanned the converted textures: good fights have none that is all black, the dark state had exactly
  two, 512x512, alpha and colour zero, never converted again.  `TEXENTRY` read their cache entries: the guest
  source bytes were genuinely zero (0 nonzero bytes at decode, hash unchanged), not a stale decode.  Their
  addresses were 04000000 and 04044000, the guest's two display buffers.  `ZEROBIND` printed the state of the
  draws that bound them: through mode, decal, blend off, z test off, covering the screen, drawn after the 3D and
  before the HUD, into the buffer they sample (so on a PSP the blit changes nothing).
- The chip never writes guest VRAM; the display buffers stay zero; the blit painted the finished 3D black.

**Fix** (production code, in `platform/wii/wii_gxscene.c`): `is_display_buffer()` recognises that draw (a
through-mode texture at a presented address, at least 480x272, fmt <= 3), and `bind_display_copy()` copies the
picture so far out of the EFB (`GX_CopyTex`, RGB565, the letterbox rectangle; a 462 KB buffer from the pool) and
binds that instead.  In `raster.c`, `pix_state()` passes chip = 2 for such a draw and `tex_cache_lookup()` reuses
the entry without hashing: the 512 KB source costs about 0.85 ms a frame to hash at the rate measured for the
texture re-hashes (250-320 KB in 0.42-0.58 ms), once a frame for as long as the mode lasts (a fight after the
cut, 50 s in the run below).  `gpu_is_presented_buffer()` (display.c) is the address test.
The software path is unchanged (its VRAM is real).

**Checked in Dolphin** (`-DAUTOPILOT_MASH -DEXP_DARKDBG`, shift 2500, 170 s, `vq_6`): the game entered the blit mode
at about 80 s (`DISPCOPY 1`, draw 1,522,907) and kept it to the end of the run, 4,800+ blits (`DISPCOPY` prints the first
twelve and then every 600th).  The 21 reports after the entry show mean luma 71-155; 19 of them have 0 dark frames of
~250, one has 1, and one has 78 of 294 where the `BIGQUADS` lines show frames of a single full-screen black draw
(ge 3.3 ms: a scene change, not the black-3D state, which has thousands of draws).  Earlier runs that entered the mode
without the fix went black for ~40 s or until the round changed.  Hash skip: the 512 KB source of the display-buffer texture is
no longer hashed (about 0.85 ms a frame by the rate of the texture re-hashes); matched windows before and after
(different runs, one each) show ge 12.69 -> 11.45 ms, which is an indication, not an A/B.

**Not known:** why the game enters that blit mode in some runs and holds it for tens of seconds (a hold-last-frame
or a dissolve?).  The copy is the current EFB, not the previous frame; with the blit drawing a buffer onto itself
that is the same picture, and the screenshots show the right scene with the HUD.

## Matrix words in the fast list loop (2026-10-25, night)

The fast command loop of `run_list()` (the host-pointer loop of `bb9d43f`) handed every matrix command to the
general code: leave the loop, decode the word again, take the `switch`, re-enter the loop (set up the pointers
and the stall address again).  `GECMDS` of a fight window (FINEPROF, 36,346 commands a guest frame) shows the
bone matrix data as 1,772 runs a frame (12 words each, 0.83 ms in the probe build, 0.47 us a run) and the world and
view matrix words as 775 each, with their number commands; a hand-off costs about 0.2 us (0.4 ms for the 1,772
bone runs in the first A/B below).  Now `matrix_run()` takes
a whole run of bone, world, view or projection data words on the host pointer (the same conversion and store as
`load_matrix_word()`, the first word of another command or the stall address ends it) and the four number
commands are a store; the world and view generations move once a run instead of once a word (they are only
compared).  `-DEXP_OLDGEN` keeps the old path for the world, view and projection commands.

*Checked* with `-DEXP_MATCHK`: every run is also replayed word by word through `load_matrix_word()` on a copy and
the copy compared with the matrix and the word count: 150 s of Dolphin, 6.55 million runs (all four kinds),
0 mismatches.  *Measured* by alternating inside one run (`-DEXP_ABBONE`, `-DEXP_ABMAT`; 300-frontend-frame
windows, adjacent windows of the same size compared, shift 2500, 22-26k vertex fights): bone runs in the loop,
28 pairs, ge **-0.40 ms** (sd 0.72, so +-0.14), frame -0.54 ms (sd 0.94); world, view and projection words and
number commands, 15 pairs, ge -0.21 ms (sd 0.66, +-0.17), frame -0.21 ms: the second is within the noise of
its own run but is what the first one's cost per hand-off predicts (about 1,400 hand-offs a frame).

## Load hitches: the FAT chain walk, SHA-1 and the LZ decoder (2026-10-26)

The owner reported hitches while the game loads and changes scene.  A new probe (`TKDR_AUTOPILOT` builds only) finds
them: at each `sceDisplaySetFrameBuf`, a guest frame of 50 ms or more prints a `HITCH` line with the guest frame
number, the frame time, how much of it was guest code, the GE thread's list / present / finish times, the ISO bytes read
in that frame and how long the reads took (and how much of that was seeks), the three HLE calls that cost most since the
last report and the four generated functions with the most `guest_loop_tick()` calls in the frame (`addr<caller`; map an
address with `powerpc-eabi-addr2line -f -e <build>.elf`).  The function ranking counts ticks, not time, so it points at
a function rather than measuring it.  The every-300-frame hitches with "guest code 0 ms" are the report printing itself.
First run: the `AUTOPILOT_MASH` shift-2500 build for 110 s printed 79 `HITCH` lines.

**1. ISO reads: libfat walks the FAT chain.**  `fseek` on the 1.75 GB ISO follows the file's cluster chain from the start
for a backward seek, and Dolphin's SD image has 4 KB clusters: a far seek cost 40-81 ms against 2 ms for the read itself
(the `iso ... took` and `seeks` fields).  `platform/wii/wii_fatmap.c` reads the card's partition table, boot sector,
directory entries (long names with their checksum, so a stale long-name entry cannot be taken for the file) and the FAT
chain once at boot and keeps the file as a list of runs of clusters (at most 1,024; a more fragmented file, a volume that
is not FAT32 with 512-byte sectors or a path that does not resolve is not mapped and keeps using libfat).  `io.c` then reads
the sectors straight from the device into the 32-byte-aligned cache window.  The map is checked at boot against stdio:
a few ranges read both ways must be identical, or the raw path stays off; a device error later switches it off for the rest
of the run.  The log says `ISO: reads go straight to the card's sectors (0, 1 runs)`; `-DEXP_NOFATMAP` leaves it on libfat.
`tools/test_fatmap.c` (host) reads Dolphin's `WiiSD.raw` and compares 300 random ranges with the file on the PC, and with
`--synthetic` builds a card with a file in 50 runs that are not in order, long names, a deleted entry, look-alike names and a
stale long name, then reads and compares 600 ranges at three paths: 0 bad in both.

**2. SHA-1 while loading.**  The loader hashes each asset.  `08A36278` (the block transform) is now unrolled five rounds
at a time with the schedule in a 16-word circular buffer: 4.6 us a block as a loop, about a third of that unrolled
(`runtime/hle/sha1_core.h`).  The whole one-shot hash `08A363E0` is a new override (reads the words with two aligned loads
at any alignment, keeps the guest's 32-bit bit count, calls `guest_loop_tick()` once a block).  Frames that hashed an asset
went from 50-184 ms to 28-39 ms.  `-DEXP_OVRCHK` ran 311,000 transforms and 64 one-shots against the original with 0
differences in registers or memory; `tools/test_sha1.c` compares the transform with the old loop (state, schedule, the last
round's temporaries) on 200,000 random blocks and the digest with a textbook SHA-1 for 2,949 lengths and alignments, and the
published test vectors: 0 bad.

**3. The LZ decoder.**  `08805CB8` (the LZ decoder the loader runs) is a native override too.  It
decompresses in place, which the checker has to know: the original reads the source again after the override wrote over it,
so `EXP_OVRCHK` copies the source first (prescan) and restores it before running the original; 8 checks, 0 differences.  Not
timed on its own.  (The first check build stalled at boot: the 4 MB scratch buffer for that was a static array in MEM1,
see the MEM1 note; it is `big_alloc` now.)

**4. The ISO cache window.**  It went from 256 KB to 64 KB (192 KB of MEM1 back).  Three probe runs of the same fight
(256 KB, 256 KB, 64 KB) printed 35 / 40 / 39 `HITCH` lines; the ones with ISO reads were 1 / 1 / 5, and each of those
spent 1 ms in the reads (58-128 KB in one read), so the window is not what the frame time went into.

**Result:** 79 `HITCH` lines in the first 110 s run, 35-40 after (of which about 10-14 have more than 9 ms of guest code
and a real cause; the rest are the report print).  Not done: the boot's second SHA-1 `f_08A2D490` (185-199 ms once), the VFPU
math `f_089B667C` / `f_089B5D8C` (349 ms once, guest frame ~4,170) and `f_08A157B0` / `f_08A161E8` / `f_08A1633C` (100-170 ms in a
few frames), none of them repeating enough to pay for an exact override yet.  Run-to-run the count of `HITCH` lines varies by
about 5.  Checked: production DOL (no `HITCH`, `OVRCHK` or other probe string, 11,957,536 bytes) boots to the attract mode in
Dolphin with the card read path on, `python -m unittest tests.test_tk6_fixture`, `tools/test_sas.c`, `test_fatmap`, `test_sha1`.

## Memory pass and the intro movie's frames (2026-10-28)

The task was to map MEM1/MEM2, remove waste that does not cost speed, and verify in Dolphin; the owner also
reported that the intro movie "only plays the first frame and cuts off".  Both were done in one pass.

**MEM1 map.**  The production DOL was 12,831,552 B: 11,492,352 B text, 384,544 rodata, 13,648 data and
5,126,632 B `.bss`.  The largest live statics were `g_verts` 1,441,792 B (a staging buffer for one general
draw), `g_ch` 524,608 B (8 audio-channel rings), `g_at` 197,160 B (ATRAC contexts), the 200,016 B FAT-map
verification pair, a 261,120 B frontend framebuffer, a 522,240 B RGBA staging array in the GL renderer
(never used on the Wii) and the 940,268 B `.eh_frame`/`.eh_frame_hdr`.  The host thread stacks were the
other large item: the boot thread asked for 4 MiB and fell back to 1 MiB, the others scaled up to 2 MiB,
and `pthread_create(NULL, ...)` defaulted to 1 MiB.  `-DEXP_STACKPROBE` (every host stack filled with
0xA5, `STACKPROBE ... used` every 300 frontend frames and at thread end) measured the deepest use over
menus, the movie and a fight at **40,168 B** (a movie decoder worker); the boot thread used 1,848-5,064 B,
the GE thread 5,064 B, the rest 1.4-1.9 KiB.

**What changed.**
1. `-fno-asynchronous-unwind-tables -fno-unwind-tables` in `CFLAGS`/`CXXFLAGS` and in
   `tools/build_ffmpeg.sh`; the project and FFmpeg were rebuilt.  `.eh_frame` + hdr went from 793,608 +
   146,660 to 68,384 + 10,804 and the DOL from 12,831,552 to **11,970,880 B** (-860,672).
2. The GL renderer's `px[480*272]` is a lazy `big_alloc` instead of 522,240 B of `.bss`; the FAT-map
   verification buffers (`a.1`, `b.2`, 200,016 B) are `malloc`ed only while `fatmap_open(..., verify)`
   runs; the frontend's 480x272 `s_pixels` (261,120 B) comes from the MEM2 pool when `frontend_run()`
   starts.  `.bss` went from 5,126,632 to **4,143,240 B** (-983,392).
3. Host stacks: boot 512 KiB, every other thread 256 KiB (the `pthread_attr_init` and `NULL`
   `pthread_create` defaults too), down from the 4 MiB request / 2 MiB scale / 1 MiB default.  The floor
   in the allocation retry is 256 KiB.
4. The boot `arena1` line went from **4104 KiB** to **6160 KiB** left.  In the verification run `mallinfo`
   reported arena 6,022,240 B, in use 5,650,904 B, free 371,336 B, and the physical `arena1` range ended at
   `0x81531000..0x81800000` (2,876 KiB) with the stacks in place.  `docs/MEMORY.md` has the full map and
   the remaining statics.

**The movie.**  The decode always produced frames (`AVCPROBE ffmpeg frame 211` in a run), but the player
copies each picture into the framebuffer it already presented and does not call `sceDisplaySetFrameBuf`
again.  The GX-idle CPU fallback required a new `g_frames_presented`, so it stayed on the chip's stale
picture; the screen was black except when a block transfer happened to refresh one frame.  Now
`texrep_note_video()` calls `gpu_tex_invalidate_range()` for the decoded range, and the frontend also
enters the CPU fallback when `g_gpu_write_tick` (bumped by `gpu_tex_invalidate_range` for block transfers,
DMA and decoder writes) changes.  Production-DOL screenshots at 33-35 s of a run show the fire,
"GET READY FOR THE NEXT BATTLE" and VS frames in sequence; before the change the same window was black and
then the title.  The player still renders into a texture target (B3); this fix presents the guest
framebuffer, it does not add render-to-texture.

**Checks.**  Production DOL 11,970,880 B, SHA-256
`5F048F9881CA8643F3C6916B4BA6CD3403DD0EB1F51300CEFA4781C30A044EB7`, no probe strings; boots to the title
and the attract movie in Dolphin.  `tools/test_fatmap.c` synthetic card (50 fragmented runs, long names,
deleted/look-alike entries): 0 bad.  The stack probe build (WII_OSREPORT + EXP_STACKPROBE) reported the
sizes above.  Not run: the console (P0), a full A/B of frame time (this pass changes no draw path), and
`-O2` FFmpeg (the headroom now allows it; the failing configuration was 3860 KiB with 1 MiB stacks).

## Movie: the whole stream plays, not the first segment

**Symptom.** The boot intro stopped after ~9 s (271 frames) with only ~1.05 MB of the 17,182,720-byte
PSMF consumed; the guest then called `sceMpegAvcDecodeStop`.  Cutting the ring semantics differently
moved the stop to 297/301 frames, so the cut was flow-dependent, not a scripted intro length.

**Cause.** Three scheduler/HLE defects, none of them in FFmpeg or the presenter:

1. `hle_wait_vblank()` has a shortcut for a late frame (`cnt != g_vb_served`) that returns without
   waiting and advances `g_last_vblank`.  During a movie several guest threads wait on the same
   vblank, and a shortcut taken by one of them consumed the tick the others were asleep on.  The
   player's ringbuffer feeder (one `sceDisplayWaitVblankStart` per loop) was measured sleeping **2.5 s
   inside a single wait**, so the decoder drained the 700-packet ring.  The game's own feeder treats
   "ring empty while the stream is not fully read" (`S_B0 >= 2 && avail == 700 && read != size`) as the
   end of the film and sets the player's stop event.
2. `sceKernelWaitThreadEnd()` passed arg 1 -- the **status output pointer** -- to `wait_on()` as a
   timeout pointer.  It read the status word (usually 0) as a zero timeout, so every join returned
   `WAIT_TIMEOUT` immediately.  The movie state machine polls its three workers with this call and read
   the timeout as "workers finished": it marked playback done and tore the movie down.
3. `sceKernelClearEventFlag()` kept only the bits passed in (`pattern &= bits`) instead of clearing
   them (`pattern &= ~bits`), wiping flags down to the mask.

**What changed.**
- `hle_wait_vblank()` keeps the late-frame shortcut only when no movie is active (`hle_movie_active()`);
  while a movie plays every vblank wait really waits.  Outside movies the frame-rate behaviour is
  unchanged.
- `wait_thread_end()` blocks for the player's three worker threads (entries `088EEEFC`, `088EF1C4`,
  `088EF354`) once the decoder has produced a picture (`hle_movie_started()`), and keeps the old
  non-blocking behaviour otherwise.  Blocking every join hangs the boot; blocking the pre-playback join
  deadlocks the start of the movie.
- `sceKernelClearEventFlag()` clears the bits.
- `sceMpegAvcDecode()` yields to equal-priority peers after a decode (`hle_yield_peer()`): a decode is
  ~20 ms of native code and the feeder otherwise never got the CPU.

**Screen fill.**  The decoder's 480x272 picture used to be copied 1:1 into the
centre of the 640x480 XFB, so the movie sat in a black frame while the menus
and 3D filled the width.  It is now resampled into the same 640x362 rectangle
the GX path uses.  Nearest-neighbour is deliberate: a bilinear scaler cost
31.7 ms a frame, starved the decoder and the player stopped after 16 frames;
nearest costs 1.9 ms and the movie ran at 30 fps through the 50 s test.  The
per-pixel nearest must take U and V from one source pair: taking the nearest
word for each pixel can put a `(Y1,V)` word first, swapping U and V and drawing
coloured vertical bars (seen and fixed).  A GX RGBA8 texture path was tried and
rejected: tiling alone cost 3.56 ms and the player stopped after 162 frames.

**Evidence.** Probe run (WII_OSREPORT + `EXP_MOVIEPROBE`): `MOVIE frame 3301` at t = 127.3 s; the player
then stops itself through its own end-of-film path (`ra=088EEFB8`) with `C4=17,182,720 == C8`: the whole
PSMF stream read, 333 packets left in the ring.  Before the fix the same probe reported `C4=1,053,691`
of 17,182,720 and 271 frames.  Playback is 29.94 fps (3150 frames in 105.2 s), i.e. real time.  The
nearest presentation was separately measured at 1500 frames / 50 s (30 fps) with the full-width picture.

**Checks.** Production DOL 12,203,008 B, SHA-256
`18F5D4EE263EC2ABA8CCB3275FC77A6FDD24EDBCA13D6A1EB8A0A323A775E50C`, no probe strings; boots, plays
the intro and returns to the game in Dolphin.  Not verified on the console.

## Movie: fused converter, probe-free full intro (2026-10-28)

**Symptom after the full-screen scaler.** The decoder itself was fine, but the intro stopped early:
`MOVIE stop frames=1911 read=11077632 size=17182720` (63.8 s), and with the first fused build but
`WII_SDLOG + EXP_AVCPROBE` still enabled it stopped at **2,333 frames / 13,225,984 bytes** (78.0 s).
Both are short of the **3,301 frames / 17,182,720 bytes** the same stream plays in the earlier probe.
The SD-log writes and A/V probe prints are the remaining difference: `WII_SDLOG` fsyncs the log after
each movie report, and `EXP_AVCPROBE` prints every 30 frames.

**Change.** `wii_video_convert_movie()` now converts full-range YUV420 to limited-range YUV422 and
scales 480x272 -> 640x362 nearest in one pass on the **decoder thread**, into a 640x362 MEM2 buffer.
`wii_video_present_movie()` only copies rows into the XFB and flips; the frontend 1.9 ms per-pixel
scaler and the 480x272 intermediate are gone.  The old split measured 2.44 ms conversion + 1.9 ms
scale; the fused loop measures 4.16 ms but runs on the decoder thread, and the frontend copy is about
0.3 ms.  The in-loop deblocking filter stays on; there is no `skip_loop_filter` or other lossy decode
shortcut.

**Evidence.** The final probe-free production DOL (`WII_SDLOG`, `EXP_AVCPROBE`, the autopilot and the
code-side skip experiment all compiled out) was screenshotted at 110, 125 and 135 s: the intro is still
advancing at 110 s, the title fades in at 125 s and the title is up at 135 s.  The earlier fused
production build also showed the intro at 50, 70, 90 and 110 s.  The same intro stopped at 2,333 frames
when the SD logging was enabled, so the logging was the remaining perturbation.  Production DOL
**12,200,064 B**, SHA-256
`51BC7E22102E89E8E765B5D71868CB1D3242AE48B0E198F736370E8C1A9D0922`; `AVCPROBE`, `STATESORT`, `MOVIE`,
`AUTOPILOT` and `WII_SDLOG` strings are all absent.  The 110/125/135 s screenshots were checked.
The FFmpeg library is the `-O2` build; `-O3` is not used.  The Plus/Start movie skip is in the replay
script, not the emulator.  Not verified on the console.
