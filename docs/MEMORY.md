# Memory map and budget (2026-10-28)

This is the current map of the two Wii memory regions, the large objects that
live in them, and the reductions made on 2026-10-28.  The dated evidence is in
`docs/STATUS.md`, "Memory pass and the intro movie's frames"; the movie work is
also summarized in `docs/HANDOVER.md` B1.

## MEM1 (24 MiB)

MEM1 holds the DOL (text/rodata/data), `.bss`, libogc's own stacks and pools,
and the newlib heap (`arena1`).  Guest RAM is **not** here.

| item | size | notes |
| --- | ---: | --- |
| DOL text + rodata + data | 11,892,112 B (11.34 MiB) | 11,492,352 text, 384,544 rodata, 13,648 data, 1,568 sdata/sbss |
| `.bss` | 4,143,240 B (3.95 MiB) | largest objects below |
| libogc main stack | 128 KiB | `s_mainStack` (the boot/`main` stack, separate from pthread stacks) |
| libogc exception stack | 16 KiB | `s_excptStack` |
| libogc/libbte Bluetooth pools | 205 KiB + 64 KiB | `memb_mem_pool_pbufs`, `ram_block`; linked with Wiimote input |
| newlib heap (`arena1`) | 6,160 KiB at boot | host thread stacks and small allocations draw here |

The line to compare across builds is the boot one:

```
WII memory: arena1 0x811fc000..0x81800000 (6160 KiB left in MEM1)
```

After the game has created its threads, the heap has grown and the *physical*
top of `arena1` is lower (2,876 KiB left in the verification run), but most of
that is the thread stacks plus free fragments.  `mallinfo` in the same run:
arena 6,022,240 B, in use 5,650,904 B, free 371,336 B.

### Largest `.bss` objects

These are the objects that survived `--gc-sections` in the production build.
The GL renderer's 522 KiB staging array is now a lazy `big_alloc`, and the
4 MiB `px.4` in `ge.o` is only in the replay path and is discarded.

| object | size | source | disposition |
| --- | ---: | --- | --- |
| `g_verts` | 1,441,792 B | `raster.c` | hot staging for a single general draw; keep in MEM1 |
| `g_ch` | 524,608 B | `hle/audio.c` | 8 output channels x 16,384 stereo frames; keep (see below) |
| `g_at` | 197,160 B | `hle/atrac.c` | 6 ATRAC contexts with 2 x 4096 float PCM; keep |
| `s_batch` | 49,152 B | `wii_gxscene.c` | GX draw batching; hot |
| `g_pk` | 43,032 B | `raster.c` | one packed/skinned draw; hot |
| `g_files` | 36,864 B | `hle/io.c` | libfat file table |
| Bluetooth pools | ~265 KiB | libogc/libbte | Wiimote support |
| `s_bounce` | 32,768 B | `wii_fatmap.c` | bounce buffer for unaligned sector reads |
| `g_queue` | 32,768 B | `wii_time.c` | log queue |
| `s_tex` | 22,528 B | `wii_gxscene.c` | converted-texture slots |
| `g_states` | 22,528 B | `raster.c` | cached pixel states |
| all others | < 20 KiB each | | |

`g_ch` is 512 KiB because each of the eight channels owns a 64 KiB ring
(16,384 stereo frames, 371 ms).  Halving the ring would return 256 KiB, but
the ring is the audio path's jitter margin; it was not changed without an
audio run.

## MEM2 (64 MiB)

The `arena2` window is 51 MiB (IOS keeps the top of MEM2).  It is carved once at
boot:

| region | address | size |
| --- | --- | ---: |
| guest RAM | `0x90002000` | 32 MiB |
| guest VRAM (EDRAM window) | `0x92002000` | 2 MiB |
| low/scratch window | `0x92202000` | 2 MiB |
| `big_alloc` pool | `0x92402000` | 14 MiB |
| arena2 remainder | after the pool | about 1.8 MiB |

The pool is the scarce region at runtime: a heavy fight has measured
**13,486 KiB of 14,336 KiB used**, shared by decoded textures, GX converted
textures, the draw cache (1 MiB arena + 512 KiB slots), FFmpeg's frames and
packets, movie FIFOs and ATRAC files.  A 255 KiB framebuffer (`s_pixels`) now
also comes from here, but only when the CPU/movie/menu fallback needs it.

## What was reduced on 2026-10-28

| change | before | after | saved |
| --- | ---: | ---: | ---: |
| `-fno-asynchronous-unwind-tables -fno-unwind-tables` in the Makefile and `tools/build_ffmpeg.sh`; rebuilt project and FFmpeg | `.eh_frame` 793,608 + hdr 146,660 | 68,384 + 10,804 | ~861 KiB DOL |
| GL renderer's 480x272 RGBA staging array (`px.0`, raster.c) | 522,240 B `.bss` | lazy `big_alloc`, never on Wii | 522 KiB MEM1 |
| FAT-map verification buffers (`a.1`, `b.2`) | 200,016 B `.bss` | `malloc` only during `fatmap_open(..., verify)` | 200 KiB MEM1 |
| frontend CPU framebuffer (`s_pixels`) | 261,120 B `.bss` | `big_alloc` in `frontend_run()` | 255 KiB MEM1 |
| host thread stacks | boot asked 4 MiB (fell back to 1 MiB), workers scaled to 2 MiB, pthread default 1 MiB | boot 512 KiB, every other thread 256 KiB | ~1.5 MiB MEM1 |
| **totals** | | | **DOL -860,672 B, `.bss` -983,392 B** |

Measured boot headroom went from **4,104 KiB** (`docs/HANDOVER.md` B1,
2026-10-28 morning) to **6,160 KiB**.  The deepest host stack measured with
`-DEXP_STACKPROBE` over menus, the intro movie and a fight is **40,168 B**
(the movie decoder workers); the boot thread used 1,848-5,064 B, the GE thread
5,064 B, and the rest 1.4-1.9 KiB.  256 KiB therefore leaves a 6x margin over
the deepest observed chain.

## Things deliberately not moved

- `g_verts` (1.4 MiB) is written and read for every non-fast draw.  The MEM2
  pool was at 94% in a heavy fight, and moving a hot sequential buffer there
  would trade MEM1 headroom for an unmeasured memory-latency cost.
- `g_ch` and `g_at` are on the audio path; their rings/PCM are jitter margins,
  not dead weight.
- The Bluetooth pools keep Wiimote input working; they are released only by
  dropping Wiimote support.
- `gen/` code, `g_funcs`, the FFmpeg VLC/tables and libogc are required.

## Reproducing the measurement

```
make TARGET=tekken6wii-mem BUILD=build-mem "EXTRA_CFLAGS=-DWII_OSREPORT -DEXP_STACKPROBE" -j16
.\tools\perf\run_dol.ps1 -Dol .\tekken6wii-mem.dol -Seconds 40 -Out mem.log
```

`EXP_STACKPROBE` fills every host stack with `0xA5` at creation and prints
`STACKPROBE ... size ... used ...` every 300 frontend frames (and when a
thread ends), plus `mallinfo` and the `arena1` range.  The production build has
neither flag.
