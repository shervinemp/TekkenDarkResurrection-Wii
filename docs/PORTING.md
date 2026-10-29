# Porting notes: Tekken 6 to Wii

This is a native recompilation port, not an emulator. The guest is the PSP
`ULUS10466` MIPS executable, translated ahead of time to C. The Wii supplies
the host CPU, GPU scanout, input and storage.

## Two profiles

`tools/prepare_tkdr.py` uses tkdr-recomp's original PRX path for
**ULUS10139 v1.00** (Tekken: Dark Resurrection). That path has full section
headers and relocation data, so function discovery and indirect-call targets
come from the executable rather than from heuristics. It is the recommended
first target for a playable build.

`tools/prepare_game.py` uses `recomp/tk6.py` for **ULUS10466** (Tekken 6).
Tekken 6 is an ET_EXEC with one RWX segment and no sections/relocations, so
that path is necessarily more heuristic and is still experimental.

Both generators write the same `gen/` layout (`image.bin`, `func_table.c`,
`image_base.h`, generated C), so the Wii runtime does not care which game it
was built for. The ISO stays a plain ISO in both cases; the runtime reads
ISO9660 sectors and raw LBA ranges. Extracting the disc or converting to CSO
would require a different I/O layer and add CPU cost on Wii.

## What was reused

| Layer | Source | Notes |
| --- | --- | --- |
| MIPS decoder / AOT emitter | tkdr-recomp | `recomp/mips.py`, `recomp/emit.py`; shared unchanged |
| PSP runtime | tkdr-recomp | guest RAM, threads, HLE, VFPU, GE software raster |
| Tekken 6 ELF/module knowledge | azazel6 | sectionless ELF, module record, import/stub layout, verified roots |
| SD/USB/NTFS behavior | reVC-Wii | `fatMountSimple` + `libntfs` mount policy, SD-preferred saves |
| NTFS driver | rhyskoedijk/libntfs-wii (vendored by reVC-Wii) | read-only, GPL-2+ |

The new code is the `platform/wii/` backend, `runtime/wii/` boot/frontend
glue, the LWP pthread layer, and `recomp/tk6.py`.

## Memory map

The PSP user partition is 32 MiB at `0x08000000`; EDRAM is 2 MiB at
`0x04000000`. tkdr's host build maps the whole 160 MiB PSP address space into
one buffer. The Wii has 88 MiB total (24 MiB MEM1 + 64 MiB MEM2), so the port
maps only the used windows:

| PSP address | Wii allocation | Size |
| --- | --- | --- |
| `0x04000000..0x04200000` EDRAM | `g_vram` in MEM2 | 2 MiB |
| `0x08000000..0x0A000000` user RAM | `g_mem` at `0x90002000` in MEM2 | 32 MiB |
| everything else | `g_low` scratch window | 2 MiB |

`runtime/recomp.h` special-cases `host()` on `__wii__`. The 32 MiB guest
window is exactly the PSP user partition, so the existing allocator
(`sysmem.c`) and stack placement work unchanged.

MEM1 holds the DOL text/data/BSS, the host thread stacks and the C heap.
Large buffers (decoded textures, movie frames, audio files, the draw cache
and the frontend's CPU framebuffer) come from the 14 MiB `big_alloc` pool in
MEM2 instead.  The production DOL is 11.97 MB on card, `.bss` is 3.95 MiB and
the boot `arena1` line reports 6160 KiB left in MEM1 (2026-10-28).  The full
map, the largest statics and the measured pool use are in `docs/MEMORY.md`.

## Threads

The runtime assumes POSIX threads. devkitPPC's libsysbase pthread layer uses
the newer KThread scheduler; in this environment a thread created from the
main loop was never scheduled. The port therefore provides
`platform/wii/pthread_lwp.c`, which implements the used pthread subset on
libogc's LWP threads/mutexes/condition variables. Types still come from
newlib's `<pthread.h>`; LWP handles live in the opaque fields.

- Guest PSP threads are host LWP threads. Only the thread holding tkdr's
  `g_big` baton executes guest code, preserving the PSP's single-core
  priority scheduler.
- `tls_self` is process-global rather than TLS, because the baton guarantees
  one guest thread at a time.
- The GE display-list thread is another LWP thread. With no raster workers
  (`RASTER_THREADS=0` on Wii), the GE thread draws inline and the main loop
  yields with `KThreadYield()` so it and guest threads make progress.
- The AESND feeder is a low-priority LWP thread.
- `-Wl,--allow-multiple-definition` is used only because libsysbase's
  pthread object is pulled in by libgcc's `pthread_once`; our LWP
  definitions come first and win. This should be replaced with a complete
  symbol set or a rebuilt libsysbase when convenient.

## Video

`platform/wii/wii_video.c`:

- Boot console on a normal XFB (install instructions, fatal messages).
- After data is found: GX with a 256 KiB FIFO, a 512×512 RGB565 texture, and
  an orthographic quad. Each frame the software-rendered 480×272 ABGR image
  is converted to GX's 4×4 tiled RGB565 and uploaded; the quad is letterboxed
  into the EFB.
- GX only does scanout. The PSP GE is still emulated by `runtime/gpu/raster.c`
  on the CPU. That is the main performance risk on a 729 MHz Broadway.

The next renderer step is a `gxr.c` analogous to tkdr's `glr.c`: record
transformed/lit vertices from the existing raster pipeline, then submit them
to GX with TEV stages for texture/vertex colour/alpha test/blend. The vertex
work (skinning, lighting, near clipping) stays on the CPU because GX has no
programmable vertex shader.

## Audio

PSP audio is 44.1 kHz; the Wii DSP runs at 48 kHz. A feeder LWP thread pulls
10 ms (441-frame) blocks from tkdr's `audio_pull()` mixer and linearly
resamples them to 480 frames into a lock-free ring. The AESND callback
(interrupt context) only drains the ring, so it never takes a mutex. SAS
(software voices) and ATRAC3/3+ decoding are compiled in; ATRAC performance on
Broadway is unmeasured.

## Files

- `platform/wii/wii_storage.c` mounts SD with `fatInitDefault()` and mounts
  USB lazily. FAT is tried first; NTFS is mounted read-only with
  `NTFS_FORCE | NTFS_READ_ONLY | NTFS_IGNORE_CASE` (the same flags reVC-Wii
  uses for dirty/hibernated sticks and case-insensitive lookups).
- The game's ISO is read by `runtime/hle/io.c`; no game bytes are copied into
  the DOL. Raw `disc0:` sectors and `ms0:` save files keep their PSP paths.
- Save data is written under `<app>/ms0/PSP/SAVEDATA`, so the tool can chdir
  to the install directory and the existing HLE paths work.
- `image.bin` is the relocated executable image generated from the user's
  EBOOT. It is game-derived and must stay out of git.

## Tekken 6 generation (`recomp/tk6.py`)

The Tekken 6 EBOOT is an `ET_EXEC` with one RWX `PT_LOAD`, no section
headers, and no relocations. The generator:

1. Validates the ELF header and loads `PT_LOAD` bytes at their guest VAs. The
   page containing `0x08804000` includes the ELF header, so file offset
   `0x1000` maps to the image base.
2. Locates the PSP module record at the first `PT_LOAD`'s `p_paddr` (a file
   offset, per the verified ULUS10466 convention) and reads the module GP,
   export/import tables.
3. Parses import descriptors and maps every `(library, NID)` to a stub
   address. `recomp/nids.json` supplies the PSP names; unknown NIDs get a
   deterministic fallback name and are reported as unimplemented HLE.
4. Decodes the executable range (`text_start` to `stub_top & ~0xFFF`) and
   seeds function discovery from:
   - the entry point and `profiles/tekken6/config/startup.json` roots,
   - every direct `jal` target,
   - profile pointer/constructor/immediate-call entries,
   - with `--coverage full`, every aligned data word that points into text.
5. Traces each function intraprocedurally to find its real end (returns,
   branch joins, jump-table targets) instead of assuming the next seed is the
   end.
6. Emits one C function per guest function with the shared tkdr emitter and
   writes `func_table.c`, `image_base.h` (base/size/GP) and `image.bin`.

Limitations:

- Without `.eh_frame` or relocations, function ends and indirect-call targets
  are inferred. `startup` coverage is intentionally conservative (boot and
  reachable slice); `full` is broad but includes false positives.
- A function reached only through a runtime-computed pointer that never
  appears in the loaded data will trap at `call_indirect` (the runtime logs
  the address). Extend the profile roots/pointer lists when that happens.
- The 4 MiB module region is inside the same RWX segment as code, so the
  `text_end` default (`stub_top & ~0xFFF`) must be adjusted if a game build
  places code above it.

## HLE coverage

The runtime's HLE table is shared with tkdr and already covers the common PSP
kernel, I/O, display, GE, audio, ATRAC, SAS, utility and RTC services. Tekken
6 imports 268 functions across 30 libraries (azazel6's inventory). The
startup NIDs (`sceKernelSetCompiledSdkVersion500_550`, `CreateThread`, etc.)
are aliased in `recomp/nids.json`; unsupported imports return 0 and log once.
The first full-game boot will identify the next missing services.

## Next steps

1. Boot a real ULUS10466 dump and triage the first trap/missing import; grow
   `startup.json` roots and NID aliases.
2. Replace the software GE path with a GX renderer for playable frame rates.
3. Add an ISO read-ahead cache and move raster/texture allocations to MEM2 if
   MEM1 runs short on a full generated build.
4. Add H.264 playback (FFmpeg/PPC).  Until then `runtime/hle/avc_stub.c`
   treats each supplied buffer as one AU and draws a black frame, so movie
   players advance instead of waiting forever for video.
5. Measure SAS/ATRAC audio on hardware; add a frame-time overlay.
