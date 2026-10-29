# Handover: Tekken: Dark Resurrection on Wii

Written 2026-10-25 at commit `4af3009`; brought up to date 2026-10-28 (branch `main`; local commits are not
pushed, section 14).  **Section 14 is what is open, in the order to take it.**
This is the entry point for whoever
continues the work: what the system is and how it is put together (sections 1-4), how to build, run and
measure it (5-6), where it stands (7), everything still open (8-10), what has been tried and must not be
tried again (11), and the rules the project has been run under (12).  `docs/STATUS.md` is the dated lab
notebook with the evidence for every claim here; `docs/PLAN.md` and `docs/OPTIMIZATION_IDEAS.md` are the
older plans (partly superseded, see section 13).

## 1. What this is, in one paragraph

A native Wii port of the PSP game *Tekken: Dark Resurrection* (`ULUS10139` v1.00), made by **static
recompilation**: the game's MIPS/Allegrex executable is translated ahead of time into C
(`recomp/emit.py`, 13.8k functions), compiled with devkitPPC together with a hand-written runtime that
stands in for the PSP's operating system (threads, display, file I/O, audio, save data) and its
graphics chip (the "GE"), and run on the Wii's Broadway CPU, with the Wii's own GPU ("GX") drawing the
frames.  It runs under Dolphin: the intro, title, menus, character select and fights are playable and
look right.  **It has never been run on a console.**  The one open question that decides everything is
speed: a fight needs 16.7 ms a frame for 60 game frames a second, and a heavy fight costs 19-25 ms under
Dolphin (section 7).

No game data is in the repository.  You need your own decrypted `EBOOT.BIN` (`test/ULUS10139_EBOOT.BIN`
is where this checkout keeps it, git-ignored) and your own ISO.

## 2. Pipeline from the disc to the screen

```
EBOOT.BIN --recomp/*.py--> gen/*.c + image.bin + func_table.c   (not in git: regenerate, ~8 s)
                                   |
gen/*.c + runtime/ + platform/wii/ --devkitPPC make--> tekken6wii.dol (11.9 MB)
                                   |
Wii/Dolphin:  guest threads (LWP) --HLE calls--> runtime/hle/*   (sysmem, thread, display, io, audio, sas, atrac ...)
              guest builds a display list --sceGeListEnQueue--> GE thread: runtime/gpu/ge.c (command interpreter)
              ge.c --draw_prim / draw_prim_fast--> runtime/gpu/raster.c (decode, skin, light) --> platform/wii/wii_gxscene.c --> GX
              frontend thread (runtime/wii/frontend.c): presents the copied frame at 60 Hz, reads the pad
```

- **Recompiler** (`recomp/`): `mips.py` decoder, `analyze.py` function discovery, `elf.py` / `tk6.py`
  loaders, `emit.py` C emitter, `vfpu_emit.py` (the PSP's vector unit translated to straight-line C, 14,544
  of 14,802 sites; the other 258 call the interpreter in `runtime/vfpu.c`).  `tools/prepare_tkdr.py` is
  the front door for this game (`tools/prepare_game.py` / `recomp/tk6.py` is the unfinished Tekken 6
  path).  Output: `gen/funcs_NNN.c` (35 files), `gen/funcs_hot.c` (the 189 hottest functions, built at
  `-O2`; the list is `profiles/tekken6/config/hot_functions.txt`), `func_table.c`, `image_base.h`,
  `image.bin` (the relocated executable image the runtime loads).  Regenerate with
  `python -m recomp.emit test/ULUS10139_EBOOT.BIN gen`.
- **Native overrides**: a guest function can be replaced by hand-written C.  List in
  `profiles/tekken6/config/overrides.txt` (`address symbol`), code in `runtime/hle/overrides.c`.  The
  generated function keeps its name (so every call site is unchanged) and its translated body becomes
  `f_XXXX_orig`.  `-DEXP_OVRCHK` runs the original beside the override on every call and compares all 32
  registers and the memory snapshot (`OVR_SNAP`); no override goes in without a clean run of that.
  Current overrides: `08A189D8` (the copy of the static display lists into each frame's list),
  `0896D7AC` (the animation decoder; gained almost nothing), `08A36278` (the SHA-1 block transform used
  while loading; 17.2 us a block translated, 4.6 us as a loop, about a third of that unrolled), `08A363E0`
  (the whole SHA-1 one-shot hash, `runtime/hle/sha1_core.h`) and `08805CB8` (the LZ decoder, which works in
  place: `EXP_OVRCHK` has to restore its source before it runs the original).
- **Runtime** (`runtime/recomp.h` is the guest machine: `Ctx` register file, `mem_r32` etc. as
  `always_inline` accessors, `guest_loop_tick()` at function entries and loop heads for the scheduler).
  `runtime/hle/thread.c` is the scheduler and the kernel objects; guest threads are host LWP threads and
  only the holder of the `g_big` baton runs guest code (the PSP is single core).  The rest of
  `runtime/hle/` implements the PSP modules the game imports (`hle_table.c`, `dispatch.c`).
- **GE emulation** (`runtime/gpu/ge.c`): interprets the PSP display-list commands into a register file
  `g_ge.cmd[]` and tracks *generation counters* (`g_ge_pix_gen`, `g_ge_vertex_gen`, `g_ge_light_gen`,
  `g_ge_view_gen`, `g_ge_world_gen`) so the rasteriser can cache what it derived from registers; which
  opcode bumps which counter is the table `g_op_class` (an opcode wrongly left out of the pixel class was
  the cause of the hair bug, section 7).  It runs on its own thread (`GE thread`).
- **Rasteriser / GX front end** (`runtime/gpu/raster.c`, 5.9k lines): `pix_state()` and `gxr_state()`
  derive the pixel state (`GxrState`, applied by `apply_pixel()` in `wii_gxscene.c`), `vformat()` the
  vertex format, `decode_*` the vertices.  Everything is drawn through `draw_prim()` (general) or
  `draw_prim_fast()` (the common cases, with a cached "hot state" `s_hot`).  A software rasteriser for the
  same state still exists (a file named `cpu` in the app directory forces it; it lacks fog, colour test,
  stencil, logic ops, so it is not a visual reference).
- **GX backend** (`platform/wii/wii_gxscene.c`): one vertex format (float x y z, RGBA8, float u v), one
  colour channel passing the vertex colour (the CPU lights everything except the diffuse-lit fighters, which
  take a second form with a normal and are lit by the chip, see section 3), the view matrix in position
  matrix 0, a model-view in 1 for cached draws, batching of triangle lists, strips and fans submitted as
  such.  `g_gx_epoch` is bumped whenever the chip state changes outside the draw path.
- **Frontend** (`runtime/wii/frontend.c`, `boot.c`): finds `sd:/apps/tkdr/` (or `usb:`), loads
  `image.bin` and the ISO, starts the guest, presents frames at 60 Hz (`FRAME_US`, non-blocking), reads the
  pad, and in autopilot builds scripts the menus (section 6).
- **Platform** (`platform/wii/`): `pthread_lwp.c` (pthread subset on libogc LWP; libogc has no thread-local
  storage, `__thread` data would silently be one shared word and the link step refuses it),
  `wii_audio.c` (AESND, 48 kHz, a feeder thread resamples the 44.1 kHz mixer), `wii_storage.c` (FAT SD,
  FAT/NTFS USB), `wii_memory.c`, `wii_input.c`, `wii_video.c`.

### 2.1 Memory

MEM1 is 24 MiB.  The production DOL is 11,970,880 bytes on card; its text/rodata/data are 11.34 MiB and
`.bss` 3.95 MiB.  At boot `arena1` reports **6160 KiB left** (it was 4104 KiB before the 2026-10-28 memory
pass); the host thread stacks now ask for 512 KiB (boot) / 256 KiB (everything else), and the deepest one
measured over menus, the movie and a fight is 40 KiB.  So **use `big_alloc` (the MEM2 pool), not `malloc`,
for anything large**, but note the pool is nearly full in a fight.  MEM2 is 64 MiB; the guest RAM is 32 MiB
(`g_mem` at `0x90002000`), VRAM 2 MiB, low window 2 MiB, the texture/draw-cache pool 14 MiB; about 1.8 MiB
of the 51.8 MiB arena is left.  A heavy fight has measured 13,486 KiB of the 14,336 KiB pool in use.  See
`docs/MEMORY.md` for the full map and the list of large statics.  The emitter's `RECOMP_LOOPINLINE_MAX`
(environment variable, default 16) limits memory-accessor inlining to loops of <= 16 instructions because
inlining everywhere costs +4.6 MB of MEM1.

### 2.2 Time

`hle_time_us()` is the guest's clock.  The **vblank** is elastic by default (`hle_wait_vblank`,
`thread.c`): a wait that starts after a tick went by with nobody waiting returns at once and re-bases the
grid, so the game's speed is 1 / (frame time x 60) instead of 60/30/20 fps steps.  `-DTKDR_FIXED_VBLANK`
restores the fixed 16.7 ms grid.  `sceDisplayGetVcount` reads the same count.  The game logic is locked to
60 frames a second, so "30 fps" is half speed; judge speed by the `SPEED` line (guest fps / 60) or the
round timer, never by `busy`.

## 3. How the fast fight path works (the part that took most of the effort)

A heavy fight frame is ~2,700-2,900 skinned draws (the fighters are drawn as ~5-vertex strips, ~15k
vertices) plus ~1,000-3,000 plain draws (the stage and props), and the GE list is ~38-40k commands (3.2-3.4k
CALL/RET pairs, 5-6k VADDR, ~5k PRIM).

- **VFPU translation** replaced a 12.5 ms interpreter (36k calls a frame).  `tools/test_vfpu_emit.py`
  checks every distinct VFPU instruction of the game against the interpreter (200,388 runs, 0 differences).  Since
  2026-10-27 it also translates `vcmp` (all sixteen conditions, into the CC register), `vcmov`, and the pack/unpack
  conversions `vuc2ifs`..`vi2s` (`vfpu7`); the interpreter was still taking 1,100-1,270 calls and 0.46-0.53 ms a frame in
  every fight (`vi2uc.q` 464, `vcmp` 480, `vcmov` 232 a frame), now 0 calls (22 sites left in the whole game: `vrnd*`,
  `vh2f`, one `vfpu9`).  The interpreter's `vcmp` conditions 9 and 10 now normalise `isnan` / `isinf` to 0 / 1 (`!!`), as the
  translation does, since a library `isinf` may return -1.
- **Draw cache** (`runtime/gpu/vcache.c`): 92% of the non-skinned draws repeat an earlier frame's vertex
  bytes.  4-way set associative (4096 sets, true LRU; the first version evicted only way 0 of a full set, which
  made every set direct-mapped and every later scene slower: pooled later-fight windows went from 24.8% to 4.3%
  of the lookups missing, ge 14.8 -> 13.2 ms and the frame 22.4 -> 20.7 ms at ~21k vertices; fixed 2026-10-25, the
  ring-and-replay tools for that are `EXP_VCDUMP` and `tools/perf/vcsim.py`), keyed by guest address + content
  hashes + world matrix + lighting signature + material; vertices are cached in object space and the chip applies
  a model-view matrix; texture coordinates are cached raw and scaled at submit.  1 MB arena + 512 KB of slots
  (16,384 x 32 bytes) from the MEM2 pool; when the arena is full the draws not used for a while are given up and the rest slid together
  (`vc_collect`).  A hit costs ~2 us (hash + lookup + submit).  `-DEXP_HITCHK` re-decodes hits and compares
  (375k hits, 0 bad).
  **Write tracking cannot replace the hash**: the guest rewrites the vertex bytes of every plain draw
  every frame (a few vertex-building functions) and 55-70% of those then hit; stamps cannot prove
  "unchanged" (section 11).
- **Skinned decode** (`decode_skinned_packed`, `skin_prepare`, `skin_run`): one call per draw, straight
  into `GxVtx` (x y z u v rgba8).  The common case runs in the **paired-single assembly kernel**
  `runtime/gpu/skin_ps.c` (nine variants: colour / diffuse / specular x no / 8-bit / float texture
  coordinates; parameter block `SkinPS` in `skin_ps.h`, offsets checked by `_Static_assert`; uses f0-f13
  and r0, r3-r12 only; devkitPPC's GCC has no `-mpaired`).  `-DEXP_SKINCHK` runs kernel and scalar code
  on every draw and compares (20M vertices, colour within 1, uv identical).  The scalar `skin_run_c` is
  the reference and the fallback.
  **Vertex-colour parts (2026-10-27):** skinned parts with a vertex colour (565 / 5551 / 8888; clothes and
  accessories of some fighters, 0x331 / 0x333 / 0x337 / 0x33D, 5-6 vertices a draw) used to fail `skin_format_ok()`
  (`col == 0`) and took the general `draw_prim` path, ~10 us a draw (1-6 ms a frame in the fights that have them: 4 of
  the 7 fights of one attract run; the `OTHER3D` line says `skinned N`, `SKINFMT` lists the vertex types).  They now
  go through `skin_prepare` mode 3 (lighting off: the vertex colour as it is) or mode 4 (lighting on), decoded by the
  scalar loop in `skin_run_c` (no kernel: it has no colour input; `skin_ps_format_ok` says `col == 0`), through
  `draw_prim_fast` like the rest.  Mode 4 lights with `col_lit()` (`ColLight`, built per draw by `col_light_build()`):
  `light_general()`'s sums regrouped (emissive + ma x (global + light ambient) + md x sum of diffuse x max(0, n.L) +
  specular), for all-directional lights without powered diffuse and without a specular colour from the vertex; any
  other setup runs `light_vertex()` itself, as before.  Checked by `-DEXP_SKINCOLCHK` against
  `decode_vertex_skinned()` (the path these vertices took), three demo runs of shift 2500 (attract mode after the
  fight): 3.8M, 3.6M and 6.8M vertices (all mode 4), positions and uv identical, colour bytes identical except 4
  vertices off by one level in the second run, 0 bad; the shift 18000 fight has no such parts (no `SKINCOLCHK` line in
  540 s, `OTHER3D skinned 0` throughout).  *Cost* (in-run A/B, Dolphin demo build, shift 2500): `-DEXP_ABSKINCOL`
  (whole-draw path against the general path), on the 7 window pairs that had such draws: **-0.94 ms a frame** (sd 1.29;
  ge -0.69, other -0.26), -1.4 to -1.7 ms in the fight with 551 such draws a frame; `-DEXP_ABCOLFLAT` (`col_lit()` against
  `light_vertex()` inside the whole-draw path), 45 pairs over the whole run: **-0.32 ms a frame** (sd 0.50; ge -0.33).
  Not covered (still the general path): 9-10 draws a frame stayed on the general path in the A/B's whole-draw windows.
  (The skinned parts with a colour and no normals, vertex type 0x431D with lighting on, were listed here as "not built"
  in earlier revisions of this document; they are on the whole-draw path since `b85ac30`, which is what mode 3 / 4
  means.  Their own A/B was not measurable in the fights played in the 2026-10-28 session: no fight had 0x431D draws.)
- **Flattened lighting** (`LightFlat`, `light_flat()`): the fighters are lit by 1-3 directional lights,
  diffuse or with an integer-exponent specular term, so a vertex is three dot products.  The semantics to
  keep: world normal, normalised unless its squared length is within 0.999..1.001, `max(0, n.L)` per
  light, colour = base + sum of diffuse x that, clamp, alpha constant.
- **Chip lighting** (default since 2026-10-25; `-DEXP_NOGXLIGHT` lights on the CPU as before): a skinned draw
  whose lights are all diffuse (`SkinPrep.hw`, `gx_lit_build()`: one to three unit directions) is made by the
  kernel's `h` variants (`skin_ps.c`) with the normalised world normal (s16, 14 fraction bits, `GxVtxL`, 28
  bytes) in place of the colour, and `wii_gxscene_draw_lit()` sends it with the channel lit: each light's
  position is its direction x 2^20 (the diffuse direction of a `GX_AF_NONE` light is the vector from the vertex
  to the light's position), its colour the diffuse colour x material rounded to 8 bits, the ambient colour the
  flattened `base`, the material white, the alpha from the material register; the normal matrix of position
  matrix 0 is the identity, so normals and light positions stay in world space.  Draws with a specular term
  (17-23%) stay on the CPU.  The vertex description is set again whole on every switch (`GX_ClearVtxDesc()`):
  libogc remembers that normals were ever on, and after `GX_SetVtxDesc(GX_VA_NRM, GX_NONE)` Dolphin warns
  "Mismatched configuration between CP and XF stages" on every draw.  Every other draw leaves lit mode first
  (`LIT_LEAVE()`).  A fight has ~1,900 lit draws, 13 switches and 8 light loads a frame.  The frame buffer is
  RGBA6, so the one level or two the chip's arithmetic differs by does not show (section 8, P7).
- **Hot state** (`s_hot`, `hot_refresh`, `hot_quick`): `draw_prim_fast()` keeps the format, pixel state,
  lights and (for skinned draws) the `SkinPrep` between draws, keyed by the sum of the generation counters,
  the vertex type, the texture-replace epoch and the frame number.  A skinned draw does not depend on the
  world matrix or bones (the kernel reads them live), so those are left out of its sum.  The next fighter
  part with a different vertex type reuses the lighting-derived state (`sk_light`, `sk_ok`).
- **Chip state skip**: `s_hot.setup_ep == g_gx_epoch` means the chip is already set up for this state.
- **Culling** (`GxrState.cull`, mapping `{none, FRONT, BACK}`): the chip's front face is the GE's positive
  screen area.  Before 2026-10-25 the chip never culled; that drew Steve's hair shell's back faces dark.
- **Guest thread**: memory accessors always inlined, scheduler tick at function entries and loop heads
  (12.5 -> 6.3-6.8 ms), the 189 hottest functions at `-O2`, `restrict` on the register file.
- **Sound**: `runtime/hle/sas.c` keeps a voice in locals for a grain and skips the envelope of a held note;
  a fast path for held notes saved ~0.3 ms.  The exact rewrite (P3: voice loops, output loop, VAG decode,
  `audio_pull` over the active channels) saved another 0.55 ms a guest frame at 47 fps, bit-identical
  (`EXP_SASCHK`, `tools/test_sas.c`).

## 4. Debugging and measuring tools in the tree

All of these are compile-time and **none is in the production DOL** (it contains none of `GXPERF`,
`DEMOFPS`, `DRAWCATS`, `AUTOPILOT` ...; check with `strings` before a release).

| flag | what it gives |
| --- | --- |
| `-DWII_SDLOG` | stderr goes to the SD-card `debug.log` as well, so a console run of a demo or profiling build leaves its numbers there (add `-DWII_OSREPORT` to keep Dolphin's mirror) |
| `-DWII_OSREPORT` | mirrors `wii_log` / stderr into Dolphin's log as `N[OSREPORT]: ...` |
| `-DTKDR_AUTOPILOT` | the release-speed **demo build**: scripts the menus to a fight, prints `DEMOFPS`, `SPEED`, `SCENE`, `DRAWCATS`, `FASTPATH`, `GEOPS`, `HLEPROF`, `GUESTPROF`, `HOTREFRESH`, `CAMERA`, `PACING`, `OTHER3D`, `SKINREFUSED` ... every 300 frontend frames |
| `-DTKDR_AUTOPILOT` (the `SOUND` lines) | the demo build also prints `SOUND sas` (calls, voices by kind, voice-samples, VAG blocks, output loop, ms per guest frame), `SOUND sas per voice-sample` (ns), `SOUND atrac` (decodes, decoder frames, us a frame) and `SOUND ring` (the ring mix and the feeder's stream), from `sound_prof_report()` in `sas.c`; the ticks are the calling thread's wall clock (a slice taken by the GE thread counts) |
| `-DTKDR_FINEPROF` | per-draw `mftb` probes: `FASTSPLIT` (checks / decode / setup / submit), `HOTPARTS`, `SKINSHAPE`, `SKINLIGHT`, `TEXLOOKUP` (+ `parts`, `TEXPRE`, `TEXDECODE`), `HOTREFRESH`, `HOTWHY`, `HOTSWITCH`, `HASHCOST` (the plain-draw vertex hash: lookups, KB, ms, cycles a word, hits apart), `PLAINMISS` (the draw-cache miss path: draws and vertices a frame, decode / store / pack us); costs 2-3 ms a frame, compare only with each other |
| `-DTKDR_AUTOPILOT` (the `HITCH` lines) | the demo build also prints one `HITCH guest=N dt=.. ms: guest code .. ms, ge list/present/finish .., iso NK in M reads took X ms (seeks Y) \| top-3 HLE calls \| top-4 generated functions (addr<caller)` line for each guest frame of 50 ms or more, from `display.c`; map an address with `powerpc-eabi-addr2line -f -e <build>.elf`.  The function ranking counts `guest_loop_tick()` calls, not time; the every-300-frame hitches with "guest code 0 ms" are the report print itself |
| `-DEXP_NOFATMAP` | ISO reads go through libfat again instead of straight to the card's sectors (`platform/wii/wii_fatmap.c`, on by default); the boot log says which |
| `-DAUTOPILOT_SHIFT_MS=n` | starts the autopilot script n ms later, so the build plays another fight (Dolphin is deterministic; one binary = one fight). Heavy ones: 2500 (25k vertices), 18000 (30k) |
| `-DAUTOPILOT_MASH` | after `AUTOPILOT_MASH_START_S` (30) s past the script's last menu press, alternates 10 s of the plain script with 10 s of a player-like one (walk in, strings of punches and kicks, crouch, block); the `MASH` line says in how many of the window's frames it pressed. Plays the later fights and scene changes the plain script never reaches (this is how the draw-cache bug showed) |
| `-DEXP_VCDUMP` | the draw cache keeps a ring of its last 16384 lookups (address, key, three hashes, real outcome, the victim of each store); on heavy windows `frontend.c` prints it with the slot table (`VCDUMP`, `VCR`, `VCV`, `VCT` lines). Replay it against other cache shapes and policies with `python tools/perf/vcsim.py <log> [policy=lru,rand,bip] [dump numbers]`: when the replay of the real shape does not give the real miss rate, the policy is wrong |
| `-DVC_ARENA_BYTES=n` | the draw cache's vertex arena (default 1 MB, at most 1 MB); a small one makes `vc_collect()` run, for the `EXP_HITCHK` test of the collector.  The autopilot fights need up to ~570 KB, so use `262144`: 50-108 collections a report window, 0 bad (`VCARENA` prints the size, the KB handed out, the draws that found no block and the collections) |
| `-DEXP_VCWHY` | the draw cache says *why* it missed on bytes it had cached: `VCWHYSTAT` counts the misses where only the global ambient alpha (`GE_AMBIENTALPHA`) differed from the cached draw's state, against any other lighting difference (`ge_light_sig_flat_noalpha()` in `ge.c`).  Together with `VCACHE` / `VCMISS` (no slot for the address / other bytes / same bytes under other state) of the autopilot report |
| `-DEXP_ABREFRESH` | alternates at each report between the cache keeping a draw whose only change is the global ambient alpha as a hit (its alpha bytes made again: `VCREFRESH n`) and the old hash (a miss): `ABREFRESH the window above had the alpha-only cache differences as hits / misses`, pair with `abpairs.py <log> ABREFRESH "as hits"` (2026-10-28) |
| `-DEXP_ABPIPE` | alternates at each report between the skinning kernel writing the chip-lit fighters' vertices straight into the GX write-gather pipe (P5) and into the staging buffer as before: `ABPIPE the window above had the lit skinned draws written by the kernel to the pipe / to the staging buffer`, pair with `abpairs.py <log> ABPIPE "to the pipe"` (2026-10-28) |
| `-DEXP_PIPECHK` | every chip-lit skinned draw: the pipe kernel's stream (the same stores into a buffer, `g_skin_ps_dense`) against the staging kernel's `GxVtxL`s, `PIPECHK n draws, m bad` every 4096 draws; turns the pipe path off for the run (the draw goes through the staging buffer) |
| `-DEXP_NOPIPE` | the kernel writes the chip-lit fighters to the staging buffer and `wii_gxscene_draw_lit()` copies them to the pipe, as before P5 (the pipe path is also off under `EXP_RIGID`, `TKDR_FINEPROF`, `EXP_LITCHK`, `EXP_CULLCENSUS`, `EXP_DRAWTRACE`, `EXP_PIPECHK`, which read `g_pk`) |
| `-DEXP_AVCPROBE` | the intro movie's decoder inputs: `AVCPROBE au n len .. nals ..` (the NAL units of each buffer `avc_next_au()` gets), `AVCPROBE nal7` / `nal8` (the SPS and PPS bytes) and `AVCPROBE decode n dst .. stride .. mode ..` (2026-10-28; see B1) |
| `EXP_NOFFMPEG=1` (make variable) | builds the black-frame movie stub even when `third_party/ffmpeg-ppc` exists; `-DTKDR_FFMPEG` is what the Makefile defines when FFmpeg is linked (2026-10-28; see B1) |
| `-DAUTOPILOT_STEVE` | holds the cursor on Steve for the character intro |
| `-DTKDR_FIXTIME` | the guest's wall clock fixed (the stage still varies between builds) |
| `-DAUTOPILOT_KEYS -I<dir>` | (with `-DTKDR_AUTOPILOT`) the autopilot presses the buttons of `<dir>/ap_keys.inc` instead of its fixed menu script: rows `{ start_ms, length_ms, buttons }` from the first frontend frame, in the guest's clock (about 0.65 of wall time under Dolphin); `tools/perf/autopilot_keys.ps1 -Out <dir>/ap_keys.inc -Keys '30000 Start 5000','47000 Cross'` writes it.  Route learned 2026-10-27: Start at the title, Cross twice (Quick Battle, VS CPU), fighter grid, Cross per fighter, VS screen, fight; **that route has no stage select, the stage is random** |
| `-DEXP_NODRAW` `-DEXP_NOFAST` | draw nothing / always the general path (the "floor" build: 57-60 guest fps, ~14 ms before the later cuts) |
| `-DEXP_NOCACHE` `-DEXP_NOSKINPK` `-DEXP_NOSKINPS` `-DEXP_NOSKINKEEP` `-DEXP_NOQUICK` `-DEXP_NOSETUPSKIP` `-DEXP_OLDGEN` `-DEXP_COLD` `-DEXP_NOCULL` | turn one optimisation off for an A/B |
| `-DEXP_AB` | alternates the fast path on and off each report, inside one run |
| `-DEXP_SKINCHK` `-DEXP_HITCHK` `-DEXP_OVRCHK` | equality checkers: kernel vs scalar, cache hit vs fresh decode, override vs original |
| `-DEXP_SKINCOLCHK` `-DEXP_ABSKINCOL` `-DEXP_ABCOLFLAT` | the vertex-colour skinned parts (section 3): SKINCOLCHK decodes every such vertex again with `decode_vertex_skinned()` and compares position, uv and colour bytes (`SKINCOLCHK calls n (mode3 n mode4 n) verts n bad k exact n; max ... colour err d`, every 1024 calls; the first 12 mismatches; one `SKINCOLCHK format` line per vertex type); ABSKINCOL alternates, at each report, the whole-draw path and the general path for them; ABCOLFLAT alternates `col_lit()` and `light_vertex()` for the lit ones |
| `-DEXP_TEXCHK` | every 8th lookup of a 4-bit paletted texture is decoded again from guest memory and compared with the cached texels (`TEXCHK n textures checked, k bad`); slows the game, use it with `build_shifts.ps1`, 200 s |
| `-DEXP_NOGXLIGHT` | the CPU lights the fighters (the chip does by default) |
| `-DEXP_LITCHK` | every lit draw's kernel output against the scalar code, and the chip's arithmetic (modelled) against the CPU colour: `LITCHK ... colour channel diffs 0:n 1:n ...`; slow (halves the speed) |
| `-DEXP_LITTEST` | at the start of a display list, when the lights changed, draws 64 patches with known normals through the lit path and reads the frame buffer back: `LITTEST run n: ... 6-bit steps got - expected ...`; up to 12 runs, the patches flash in a corner for a frame |
| `-DEXP_ABLIT` | alternates chip and CPU lighting at each report (`ABLIT` line, `LITSTAT` counts) for an A/B inside one fight |
| `-DEXP_RIGID` | the single-bone skinned draws go to the chip (P6): **off by default, a loss in Dolphin**.  Companions: `-DEXP_RIGIDCHK` (the chip's inputs against the skinning kernel's output for every such draw: `RIGIDCHK calls n verts n bad k; view position err .. normal err .. uv err ..`, the first 12 mismatches and the first 10 draws refused for a normal matrix that is not a rotation are printed; slows the game), `-DEXP_ABRIGID` (alternates chip and CPU at each report, `ABRIGID` line, `RIGID per frame` counts; needs `EXP_RIGID`) |
| `-DEXP_SASCHK` | every `sceSasCore` call mixes a copy of the voices with the old algorithm (`runtime/hle/sas_ref.h`) and compares the output grain and every voice field with the fast mixer's, and every `audio_pull` compares its output and the channels' read positions with the old loop: `SASCHK calls n bad k (...)`, `AUDCHK pulls n bad k` (k must stay 0); roughly halves the speed. The host version is `tools/test_sas.c` |
| `-DEXP_ABBONE` `-DEXP_ABMAT` | alternate, at each report, taking the bone matrix runs (`ABBONE`) / the world, view and projection matrix words and number commands (`ABMAT`) in the fast list loop and in the general code, for an A/B inside one run; pair the windows with the `ABxxx` lines (the line closes the window above it) |
| `-DEXP_MATCHK` | every matrix run of the fast list loop is replayed word by word through `load_matrix_word()` on a copy and compared: `MATCHK n runs, k bad` every 131k runs, `MATCHK MISMATCH` on a difference |
| `-DEXP_ABSND` | alternates the fast and the old sound mixer (SAS voice loops, output loop, `audio_pull`) at each report (`ABSND` line) for an A/B inside one fight; read the `SOUND` lines per window |
| `-DEXP_NOSWITCH` | no `hot_switch()`: every vertex type change refreshes the hot state (the old behaviour) |
| `-DEXP_FULLCLUTHASH` | a 4-bit texture hashes the whole 256-entry palette again (the old key) |
| `-DEXP_DRAWTRACE` | `DT` / `DV` / `DN` / `TX` lines for the draws in a time window (`DT_FROM_S`..`DT_TO_S`, default 75-80 s) |
| `-DEXP_DARKDBG`, `CLEARS`, `SNAP`, `BIGQUADS`, `DISPCOPY`, `DARKRUN`, `TEXPROBE`, `TEXENTRY`, `ZEROBIND` | probes for the intermittent black 3D (section 9, B6); all but `EXP_DARKDBG` come with `TKDR_AUTOPILOT`.  `DISPCOPY` says the display-buffer blit was answered with a copy of the picture (the fix), `DARKRUN` that a dark picture persisted (thread dump and `TEXPROBE`: the converted textures that are all black, with `TEXENTRY` for what the cache knows of each, `ZEROBIND` for the first draws that bind one).  `EXP_DARKDBG` also steps a run that has been dark for 100 frames through debug stages (1 depth test off, 3 white vertex colours, 11 and a white texture for every texture, 7 no textures), 100 frames each, and prints the mean luma of each |
| `-DEXP_CULLCENSUS` | `CULLCENSUS` (skinned draws with every vertex outside one clip plane, or whose whole box is under one pixel) and `RIGIDCENSUS` (how many draws and vertices are single-bone) lines over each 300 frames; the cull census reads the draw buffer of the CPU path, so it is wrong for draws taken by `EXP_RIGID` |
| `-DEXP_RECTCENSUS` `-DEXP_ABRECT` | the sprites (rectangles, `prim 6`): `RECTCENSUS` over each 300 presented frames (rectangles in the transform and in the through mode; of the transform-mode ones how many have a corner behind the eye or far outside the picture, how many come with blend, depth test, fog and a texture, the depth range, how many had the GE's face culling on, how many have their corners the wrong way round, and how many the chip's culling would have dropped before the fix below) and `RECTODD` (the first 12 odd ones, with their world position).  `EXP_ABRECT` alternates, at each report, the chip's face culling for rectangles on and off (`ABRECT` line) |
| `-DEXP_XFORMCENSUS` | the transform-mode (3D) draws over each 300 presented frames, by what the chip can do with their pixel state: `XFCENSUS` (draws, blended, translucent = blended without a depth write; `gxr_classify()`'s verdict native / approximated / "skip", which for geometry means drawn with the missing part left out; the features used; draws with a texture-map mode other than 0) and `XFSTATE` (the twelve most used translucent, non-native, texture-matrix or sprite states in full: prim, cull, texmap mode, blend, fixed colours, alpha test, depth, write mask, texture format / function / size, stencil, colour test, fog).  `-DEXP_RECTCENSUS` also prints `RECTSPR`, the first ten sprites of a window (screen corners, depth, colour, uv, texture, state) |
| `-DEXP_ZBIAS` `-DEXP_ABZBIAS` `-DEXP_ZBIAS_Q=n` | **depth offset for the blended layers** (2026-10-27, item 2 of section 14, candidate 4): the chip's viewport depth range is moved toward the eye by n steps of the PSP's 16-bit depth (default 1.5) for the draws that blend, test depth and do not write it, not in the through mode (`GxrState.zbias`, set in `gxr_state()`; `zbias_set()` and `load_viewport()` in `wii_gxscene.c` send the viewport again when the direction changes; the direction follows `z_func`: LESS / LEQUAL toward smaller depths, GREATER / GEQUAL toward larger, the other comparisons none).  `-DEXP_ABZBIAS` (demo build) alternates it on and off every report (`ABZBIAS` line; pair the windows with `abpairs.py` and the screenshots).  Off by default; compiled in three variants, never run |
| `-DEXP_ABSTENCIL` `-DEXP_STENCILSEQ` `-DEXP_TEXALPHA` | probes for the billboard-fog search (2026-10-27, section 14 item 2): ABSTENCIL alternates every report between drawing and dropping the 3D draws whose stencil test is NOTEQUAL (`ABSTENCIL the window above dropped/kept ...`); STENCILSEQ prints, for one frame in 300, the runs of stencil / blend state in draw order (`STSEQ #n xN ...`; needs `-DEXP_XFORMCENSUS -DEXP_NOSETUPSKIP`, or the fast path skips the census); TEXALPHA prints the alpha histogram of every texture a translucent draw uses, once (`TEXA ...`; needs `-DEXP_XFORMCENSUS`). |
| `-DEXP_SLOW2D` | (with `TKDR_AUTOPILOT`) times the 2D / software draws (`DRAWCATS` category 5): `SLOW2D f=.. N us (state .., chip ..) ...` for each over 2 ms (`state` = texture lookup and decode, `chip` = GX upload), `S2D` per texture every 300 frames (2026-10-28) |
| `-DEXP_TEXDECCHK` `-DEXP_CICHK` | every decoded texel against the per-texel addressing it replaced (`TEXDECCHK n textures, k bad` for CLUT, `TEXDECCHK16` for 16/32-bit), and every GX RGBA8 conversion against the byte-wise one (`RGBACHK n textures, k bad`); slow, use with `build_shifts.ps1` |
| `-DEXP_OLDPALDECODE` `-DEXP_OLDBLIT` `-DEXP_NOC16TAB` | the old per-texel decode loops / decode and hash the display-buffer blit as before / the per-texel 16-bit expansion instead of the two byte tables (for an A/B build, or if the new code is suspected) |
| `-DEXP_TEXDECBENCH` | (with `-DGPU_SELFTEST`) `TEXDECBENCH <format and size>: n us` at boot, the time of `tex_decode_fast()`; with `-DEXP_CICHK` also `RGBABENCH` (`convert_rgba8` new against old) |
| `-DTKDR_WATCH` | guest store probe (256-byte stamps + writer PC) |
| `-DGPU_SELFTEST` | self-test at boot: texture formats through `fetch()` (20), `tex_decode_fast()` against `fetch()` for all formats (840 textures, 94,488 texels), the GX paletted converters and RGBA8 tiles (`SELFTEST textures: 860/860`, `SELFTEST gx paletted: 841/841`) |
| `-DEXP_ABCOLNONRM` | alternates, at each report, the whole-draw path and the general path for skinned parts with a colour and no normals (0x431D; not yet measured: no fight had them) |
| target name containing `debug` | defines `HLE_TRACE` (HLE trace ring; per-primitive probes cost ~24 ms a frame: never time with it) |
| `-DTKDR_FIXED_VBLANK` | the old fixed vblank grid |
| `-DGX_NATIVE_TEXTURES` `-DGX_FASTTEX` | paletted CI4/CI8 uploads (exact RGB565 only / lossy), both off |

Reading a `DEMOFPS` line: `guest` is the guest frame count, `guest/s` the guest rate, `ge` the GE
thread's ms per guest frame, `other` everything else (guest thread, sound, frontend); there is one CPU, so
**frame time = ge + other**.  `SPEED` = guest/s over 60.  The `DRAWCATS` line splits the GE time by kind of
draw (fast skinned / cache hit / cache miss / skinned slow / other 3D / 2D).

## 5. Build

From **PowerShell** (the msys2 make in Git Bash cannot create temp files):

```powershell
$env:DEVKITPRO='C:/devkitPro'; $env:DEVKITPPC='C:/devkitPro/devkitPPC'
$env:PATH='C:\devkitPro\devkitPPC\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin;'+$env:PATH
python -m recomp.emit test/ULUS10139_EBOOT.BIN gen     # when recomp/ or overrides.txt changed
make -j16                                               # production tekken6wii.dol
make TARGET=tekken6wii-audit-demo BUILD=build-audit-demo "EXTRA_CFLAGS=-DWII_OSREPORT -DTKDR_AUTOPILOT" -j16   # demo
```

`tools/perf/env.ps1` sets this up for the scripts below.  Notes that cost time before:

- A full rebuild with `-j16` is ~20 s.  Touching `runtime/recomp.h` rebuilds everything.
- **A flag-only change does not rebuild objects.**  Delete the `.o` of every source that uses the flag
  (and all of `runtime/gpu`, `platform/wii` after a `gpu.h` change; `thread.o` for the vblank flag).
- GCC 15.2 intermittently dies in `try_forward_edges` on `raster.c` (the Makefile builds that unit at `-O1`
  with extra `-fno-...` flags, retries it three times, and `optimize("Ofast", "no-thread-jumps")` is used
  on hot functions).  A single "internal compiler error" or `ld returned 1` after it is a retry, not a bug.
- A link error `undefined reference to g_sk_*` (or another probe counter) after `build_shifts.ps1` means an
  object kept the previous flags: delete the `.o` of `raster.c`, `ge.c`, `vcache.c` and `frontend.c`.
- **Always run the plain `make -j16` after touching `raster.c`** and boot the result once
  (`tools/perf/shots.ps1 -Dol tekken6wii.dol -OutDir ... -At 50,100`): commit `0da9b74` broke it because
  every test build was an autopilot one (`DP_T` only exists under `TKDR_AUTOPILOT`).  Fixed in `4af3009`.
- Do not run `git` from a PowerShell with the devkitPro PATH (its msys2 git shows every CRLF file as
  modified); use the Bash tool for git.  Tracked files are LF in the index and CRLF in the working tree
  (`core.autocrlf=true`).
- The C: drive on the development machine is ~100% full: never build another SD image, and delete the
  scratch `tekken6wii-<tag><shift>.dol/.elf/.map` files that the perf scripts leave in the repository root
  (all git-ignored; keep `tekken6wii.*`, `tekken6wii-audit-demo.*`, `tekken6wii-audit-debug.*`).

## 6. Running and measuring in Dolphin

Dolphin 2609a at `C:\Users\sherv\Desktop\Projects\Ballanced\build\tools\dolphin\Dolphin.exe`, user dir
`C:\tkdr_raw_user`, whose `Load\WiiSD.raw` (1.85 GB FAT32) already holds the game ISO and `image.bin`; only
the DOL needs to change (`Dolphin.exe -u C:\tkdr_raw_user -b -e <dol> -l`).  Boot is slow: ~30 s autosave
dialog, ~90 s logo, the autopilot reaches a fight at ~100-120 s.  Automated runs are muted
(`[DSP] Volume = 0` in the test user).  Override the paths with `TKDR_DOLPHIN`, `TKDR_DOLPHIN_USER`,
`TKDR_OUT`.

Scripts in `tools/perf/` (PowerShell; run them from the PowerShell tool, a comma list of shifts breaks
under `powershell -File`):

```powershell
.\tools\perf\build_shifts.ps1 -Tag ex -Shifts 2500,18000 -Extra '-DTKDR_FINEPROF'   # demo build + one DOL per fight
.\tools\perf\play_shifts.ps1  -Tag ex -Shifts 2500,18000 -Seconds 150                # plays them, prints DEMOFPS/SPEED
.\tools\perf\run_dol.ps1 -Dol .\tekken6wii-ex2500.dol -Seconds 150 -Out ex.log      # one run, the whole log
.\tools\perf\shots.ps1 -Dol .\tekken6wii.dol -OutDir shots -At 50,100 -Total 105    # window screenshots (PrintWindow)
.\tools\perf\sheet.ps1 -Out sheet.png -Files shots\shot_50.png,shots\shot_100.png    # contact sheet
python tools\perf\readsd.py C:\tkdr_raw_user\Load\WiiSD.raw apps/tkdr/debug.log > debug.log   # a file off the SD image (-DWII_SDLOG runs)
python tools\perf\abpairs.py ex.log ABBONE "in the fast"                                 # pairs the windows of an in-run A/B (EXP_ABxxx), prints mean and sd of new - old
```

Method that has held up: compare variants **inside one fight** (`EXP_AB`, `EXP_ABBONE`, ... read with `abpairs.py`; a
different binary plays a different fight, so the logs of two builds are not an A/B) or on the same shift numbers;
look at `ge` and `other` ms, then `SPEED`; sample at least the two heavy shifts; take a screenshot after
anything that touches draws (3D can go black while the HUD still draws; a missing `raster_init()` did that).
Dolphin's emulated time is roughly instruction counts with **no cache or memory-latency model**: it hides
what a console will charge for cache misses, so prefer changes that cut instructions *and* memory traffic.

## 7. Where it stands

**Playable under Dolphin:** boot, logos, title, mode select, character select, loading screens, fights
(rounds, K.O.), replays, attract mode.  The production DOL (the commit "Fix the black 3D and the draw cache's eviction ...", 2026-10-25 night: 11,944,992 bytes, SHA-256
`B1146431F6F64EA960049C4CBC6B1C599C04B81A556AFFEBD5CE66F387C75DB6`, no probe string in it) boots to the attract
mode in Dolphin and plays its fights with lit fighters and the 3D visible in the blit mode (B6); with
`-DEXP_NOGXLIGHT` the lighting is on the CPU again.  The commit after it (`12482f0`, 2026-10-26, the load-hitch work:
card-sector ISO reads, native SHA-1 and LZ) and the work of 2026-10-27 then uncommitted (the rigid-skinning code, off by
default, and the census probes; all of it is in `b85ac30` now) built to 11,957,696 bytes, SHA-256
`1355FA4650550A31468D9F5348D302D1141BEA8C9DF144F9723E483C4AAEA841`; with the rectangle-culling change of the same
evening (section 14, item 2) it is 11,957,792 bytes, SHA-256
`EF819A69AF34786D0529DD7C689477C067E79251F0387B555465ED7792954B9B`, no probe string, boots to the attract mode in Dolphin (a fight at 60 s, the rankings at 100 s, the title at 140 s).  The tree after the later `GxrState.zbias` field and the `EXP_ZBIAS` code (off) builds to 11,957,920 bytes, SHA-256 `BA5BDA888201BF40CBBD398DF4B0C2054F0BDE38AB7F762958388DAC4700A488` (no probe string; built in a scratch directory and not run, `tekken6wii.dol` in the root is still the 11,957,792-byte one, which behaves the same).  With the VFPU translations (`vcmp`, `vcmov`, `vfpu7`; `gen/` regenerated) and the vertex-colour skinned parts (section 3), both in the production path (the tree of commit `b85ac30`), it is 11,973,120 bytes, SHA-256 `A0B08352EBAEA5C05A5F4A7C2E082683E3F62B8F3086F3E58153014B0353327C` (built 2026-10-27 as `tekken6wii-prodchk.dol` in `build-prodchk`, no probe string), and boots to the attract mode in Dolphin: lit fighters and the 3D at 60 s, another stage at 150 s.  After
the 2026-10-28 memory pass and the movie-display fix the production DOL was **11,970,880 bytes**, SHA-256
`5F048F9881CA8643F3C6916B4BA6CD3403DD0EB1F51300CEFA4781C30A044EB7` (no probe strings).  After the fused movie
converter (section 9, B1) the production DOL is **12,200,064 bytes**, SHA-256
`51BC7E22102E89E8E765B5D71868CB1D3242AE48B0E198F736370E8C1A9D0922` (no probe strings); it boots to the
title and the attract movie, plays the full intro and shows the title at 125 s (screenshots at 110, 125 and 135 s).
The DOL in `dist-tkdr/apps/tkdr/boot.dol` and `dist-tkdr-sd-full/apps/tkdr/boot.dol` is the **2026-10-24
morning checkpoint** (`1F8289C7...`, 9,951,552 bytes), deployed before the second and third fight passes:
nothing has been deployed since, on instruction.

**Speed** (Dolphin, autopilot demo build, per guest frame):

| fight | ge | other | frame | speed |
| --- | --- | --- | --- | --- |
| shift 2500, fixed grid (before the elastic vblank) | 12.5 | 8.0 | 20.5 (grid: 2 ticks) | 49% |
| shift 2500, elastic | 11.8-12.1 | 7.1-7.6 | ~19.4 | 84-85% |
| shift 18000, elastic (30k vertices) | 17-19 | 6-7.8 | ~24-25 | 66-70% |
| draw-nothing build (`EXP_NODRAW`) | 4.5-5.2 | ~8.5 | ~14 | 57-60 fps |
| console demo, a light fight, Dolphin CPU at 1.5x cost (P9, 2026-10-27) | 17.2-19.4 | 8.7-11.6 | ~27-30 | 55-62% (33-37 guest fps) |

So a light fight is **2.7 ms** over the 16.7 ms budget and a heavy one **8 ms**, under Dolphin; a console is
expected to be worse (no measurement exists).  History: 12-15 guest fps at the start of 2026-10-24, "30 fps"
after the VFPU translation / draw cache / skinning passes (that was half speed, see section 2.2), then the
floor cuts and the elastic vblank.

**Correct as far as checked:** logo fade, title, menus, fighter lighting and textures (Steve's hair fixed
2026-10-25 by culling), fog (GX fog from the GE's `fog1/fog2`), alpha test/blend, logic ops/dither/write
masks/simple stencil (the section 4.8 classifier in `docs/PLAN.md`), CI4/CI8 exact decode, AT3 and SAS
audio in Dolphin (not on hardware).

## 8. Remaining work: performance (the critical path)

Ordered by expected value for the least risk.  Estimates are Dolphin milliseconds a frame in the
heavy/light fights; "(measured)" means a number from this repository's logs, "(est.)" an unmeasured guess.

**P0. Measure on a real console (tooling done 2026-10-25, not yet run on a console).**  Nothing about cache
behaviour, MEM1/MEM2 contention, the 128-byte draw-cache sets, prefetch (`dcbt`) or the 11.9 MB DOL on real memory
has been verified.  Run the two heavy fights on a console and re-plan from the real `ge`/`other`.  Everything
below is sized in Dolphin terms until then.  Also confirm the elastic vblank and MEM1 headroom on the console.
A build with `-DWII_SDLOG` sends stderr (where `DEMOFPS`, `SPEED`, `DRAWCATS` ... go) into the SD-card log
`sd:/apps/tkdr/debug.log` beside the usual `wii_log` lines, written and `fsync`ed after each report;
`GUESTPROF` is cut to 16 lines there.  Recipe:

```powershell
make TARGET=tekken6wii-console-demo BUILD=build-console-demo "EXTRA_CFLAGS=-DWII_SDLOG -DTKDR_AUTOPILOT" -j16
```

keep the production DOL as `boot.dol.bak`, copy the new DOL over `boot.dol`, start it, and read `debug.log` after a few
minutes in a fight (the scripted menus run on the wall clock and may lose their place on a slow SD card: play to a
fight by hand then, the lines print every 300 frontend frames regardless).  Dolphin check: the SD log of a 120 s run
held 671 of the 672 report lines the OSReport log had (the last report was still being written when Dolphin
closed).  `python tools/perf/readsd.py <WiiSD.raw> apps/tkdr/debug.log` reads a file off Dolphin's raw SD image.

**P1. Cache-hit draws: 1.0-2.9k a frame at ~1.5 us = 2.1 ms (measured 2026-10-25, `docs/STATUS.md` "The
cache-hit draws and the texture lookup").**  Merging runs of hits into one chip submission was looked at and
**not built**: a hit is checks 0.46 (0.35 of that is the state refresh), lookup 0.58 (content hash 0.19, set search
0.07), setup 0.24 (mostly after a refresh), submit 0.28; the per-vertex stores are the submit, so a merged
`GX_Begin` saves under 0.1 ms, and the fixed costs belong to the 82-98 state refreshes a frame (1.5-1.8 a run),
not to the runs.  What it did find is the texture lookup inside those refreshes (0.7-1.05 ms of the 1.1-1.4 ms of refresh): the
CLUT hash over the whole palette (done for 4-bit textures, ~0.15 ms; CI8 still hashes all 256 entries) and
the source re-hash of 120-130 textures a frame (0.42-0.58 ms, unavoidable without write tracking).  Related and new: the six guest functions that rewrite the plain
draws' vertex bytes every frame (`f_08A27A98, f_08A0B200, f_08A0AD84, f_08A0AE84, f_08A0B110,
f_08A1ACA8`) could be overridden natively to produce a content hash (or an "unchanged" flag) while they
write, which would make a hit cost a lookup instead of a hash of the bytes: est. 1-2 ms but needs
`EXP_OVRCHK` plus a `EXP_HITCHK` run; the older stamp experiment says do not trust anything weaker than
the content (section 11).

**P1, re-measured 2026-10-25 (`HASHCOST`, `GUESTPROF` mapped to functions with `nm`, two fights of 21-31k vertices, 40 windows):**
the six vertex-writing guest functions cost **0.02 ms a frame** (max 0.05, self plus callees), so there is no speed in
overriding them natively; the content hash of the plain draws' vertices is 1,937 lookups x 74 bytes x 0.22 us =
**0.42 ms a frame** (0.35 of it on hits), 8.5 cycles a word.  That hash is all an override of the writers could remove,
and computing it in the writers costs the same adds (it saves only the loads), so the earlier "est. 1-2 ms" has
nothing behind it: <= 0.2 ms for the risk the stamp experiment warns about.  What is exact is the hash
loop itself (one word a loop with ~6 instructions; unrolled by four with the loads ahead it is 2-3 cycles a word,
same hash value): **measured on a scratch build 2026-10-25: ~0.04 ms a frame, not the 0.1-0.2 estimated; not built.**  The `lookup` column of `FASTSPLIT hit` (0.80 us) is the
average over hits *and* misses: 162 misses a frame at 6.2 us (1.0 ms) are in it; a hit is ~1.45 us (2.57 ms for
1,775 hits with the probes on): checks 0.45, hash 0.22 + search 0.09 + ~0.1, setup 0.23, submit 0.29.

**P2. Guest thread, 6.3-6.8 ms (measured).**  The profile is flat (19 functions are half of it, 189 are
95%), so there is no single function to fix: more of the profile at `-O2` (`HOT_OPT`; costs DOL size and
MEM1 headroom, ~0.6 ms per whole-generated-code `-O2` for +0.6 MB, measured), a handful of further native
overrides chosen from `GUESTPROF` (each needs `EXP_OVRCHK`; the two done so far gave 0.9 ms and ~0), and
checking what `guest_mem_tick()` still costs at the loop heads.  Looked at again 2026-10-25 night: the biggest
single `GUESTPROF` entry of a fight window is 0.15 ms of the 6.5 (the next ones 0.13, 0.12, 0.12, then 0.04), so the
profile is as flat as before, the 189 hot functions are already at `-O2`, and nothing was built.

**P3. Sound: measured 2026-10-25 (`SOUND` lines, two fights of 21-31k vertices, 40 windows): 0.87 ms per guest frame
at the speed they ran (40 fps), 0.59 ms at 60 fps -- 3.5% of the CPU.**  Sound work follows *real time* (the audio thread
paces itself at 44.1 kHz), so per guest frame it grows as the game slows down.  Per real second: SAS mixer 23 ms (2.3%),
the feeder's stream (`audio_pull`, 8 channels per sample) 9.4 ms (0.9%), the ring mix 2.6 ms.  Per guest frame:
`sceSasCore` 1.4 calls (grain 768) = 0.58 ms (0.17-0.95), of which the voice loops 0.40 (5.6 voices, 4,300
voice-samples a frame, all pitched/resampled: the `pitch == 1.0` fast path never runs), the VAG decoder 0.09 (1.2-1.4 us a
28-sample block, 40-76 blocks), the output loop 0.09 (a guarded `mem_w16` per sample); feeder 0.23, ring 0.06.  A voice-sample costs
~99 ns with the VAG decode in it (~55 without, ~40 cycles), 160-190 ns for a voice whose envelope moves.
**AT3+ is not decoded in the fight windows** (the autopilot's fights play no streamed music); it appears only in two
loading windows a run: 0.73 ms per decoder frame (2048 samples), so music, when it plays, costs 21.5 frames/s x 0.73 =
15.7 ms/s (1.6% of the CPU, 0.26 ms per 60 fps frame; Dolphin's cycle model, so a console will be slower).
**Built 2026-10-25, bit-identical (committed as `204fcb6`): the exact sound rewrite.**  `sas.c`: a voice's source
samples come straight out of the block buffer (`cur < 28` is the whole fast-fetch test; the block end, loop jump and
end of sample still go through `next_sample()`), a send that adds zero is skipped (`g_send_dirty` says whether any
voice added to the send buffer, so the wet pass and the clear of `g_send` are skipped when none did), held notes,
moving envelopes and noise have their own loops, the dry-only output is written as packed 32-bit words straight
into the guest window (byte-swapped on the Wii), a VAG block is read as 16 bytes at once (`guest_host_ptr`, byte-wise
gather when it does not lie in one window) and a block with no prediction filter skips the clamps.  `audio.c`:
`audio_pull` walks the channels once per block of 256 frames and takes only the channels that have data.
*Measured with `-DEXP_ABSND`* (the two mixers alternate at each report inside one run; two fights, 65 windows each; the
heavy windows pooled, >= 16k vertices, 29 fast + 28 old windows, ~4,700 voice-samples a guest frame, ~47 guest fps):

| per guest frame | old | new |
| --- | --- | --- |
| `sceSasCore` (all of it) | 0.82 ms | 0.43 ms (VAG 0.06, output loop 0.04) |
| ns per voice-sample (VAG decode in it) | 195 | 95 |
| feeder's stream (`audio_pull`) | 0.205 ms | 0.038 ms |
| ring mix (`mix_into_ring`, not touched) | 0.056 | 0.056 |
| **sound total** | **1.08 ms** | **0.53 ms** |

That is **-0.55 ms a guest frame at 47 fps; sound follows real time, so at 60 fps it is -0.42 ms** (50 -> 25 ms per
real second, 2.5% of the CPU).  The fight with 5-6k voice-samples a frame alone: `sceSasCore` 1.21 -> 0.64, stream
0.20 -> 0.04.  Frame time (ge + other) in the same windows 21.26 -> 20.80 ms (the windows alternate, so scene noise
is of that size).  An idle grain (no voice playing) costs 0.065 -> 0.028 ms.  The estimate before building was
-0.45 ms (-0.3 at 60 fps); the stream saved more than assumed.  The ticks are the calling thread's wall clock and
Dolphin's cycle model has no cache misses, so a console will differ.
*How it is checked:* `-DEXP_SASCHK` mixes a copy of the voices with the old algorithm (`runtime/hle/sas_ref.h`) on every
call and compares the whole output grain and every voice field, and compares every `audio_pull` output and read position
with the old loop.  A 330 s Dolphin run (shift 2500): 18,945 `sceSasCore` calls and 12,993 pulls, **0 bad**.  The
game never used noise, `WithMix` or the dry-off switch in that run, so `tools/test_sas.c` (host build, same sources with
`-DEXP_SASCHK`) feeds random voices (every ADSR curve, held notes, noise, pitches to 8x, odd grains, unaligned output,
negative and huge volumes and sends, `WithMix`, random rings with wrapping counters): 8 seeds x 200 rounds, ~94k
`sceSasCore` calls and ~5.6k pulls, 0 bad.  A mutation run (28 one-line faults, 25 caught; the 3 left are equivalent, they only make the code slower)
showed the test notices real faults; the first version of the test had a wrong address and was blind, so rerun the
mutations after changing it.  Lossy options (not needed): nearest-neighbour instead of the linear interpolation
(audible aliasing), the envelope stepped every 2nd-4th sample (softer attacks and releases), 16-bit paired-single
mixing (+-1 LSB differences, inaudible, not bit-identical), dropping quiet voices (audible when many play).  Not built.

**P4. GE list interpretation: the loop is done (`bb9d43f`), the matrix words were the last piece (built 2026-10-25
night).**  A fight frame has ~36k commands (5.2k VADDR, 4.2k PRIM, 3.9k BASE, 3.4k CALL and RET each, 1.8k bone
matrix runs, 1k vertex type, 0.9k texture scale/offset, 0.8k world and 0.8k view matrix words); the host-pointer fast loop
of `run_list()` takes the state writes, VADDR, BASE, PRIM, CALL and RET.  Everything it left to the general code
cost a hand-off of ~0.2 us (leave the loop, decode again, `switch`, re-enter): `matrix_run()` now takes a whole
run of bone, world, view or projection data words on the host pointer and the four number commands are a store.
**Bone runs: ge -0.40 ms (28 pairs, +-0.14); world, view and projection: ge -0.21 ms (15 pairs, +-0.17)**;
`-DEXP_MATCHK` replays every run through `load_matrix_word()`: 6.55M runs, 0 mismatches (`docs/STATUS.md`
"Matrix words in the fast list loop").  What is left on the general path is per-list (JUMP, END, SIGNAL, FINISH,
OFFSETADDR, ORIGIN), TEXFLUSH (~25 a frame), LOADCLUT and TRANSFERSTART: nothing worth a case.  The "40k commands,
2-2.5 ms" estimate of the earlier handover was the slow loop.

**P5. Skinned kernel and per-draw costs, ~8.6 ms for ~2.7k draws (measured: kernel 2.12 us a draw,
~0.385 us / ~180 instructions a vertex; checks 0.45; setup 0.19; submit 0.25).**  Measured on 2026-10-25;
the cheap changes are *small*:
- lighting was 68 of the ~180 instructions (33 of them are gone on the draws the chip lights now, P7), the world-matrix apply 25, each bone 30, each zero weight 7
  (about 2.2 are stepped over a vertex = 15 instructions, 8% of the kernel; **re-sized 2026-10-25 and not built**:
  finding the next non-zero weight with `cntlzw` costs ~6 instructions per non-zero bone (1.5 a vertex) plus a
  mask for formats of fewer than four slots, so the net is ~3 instructions a vertex, ~0.03 ms; testing a
  whole word of four weights only helps formats whose second four slots are all zero; leaving the loop once
  the weights sum to 255 is cheap but is not bit-identical when a vertex's weights do not sum to 255 or
  are not normalised, so it would need a data-wide check first.  The earlier "0.2-0.5 ms" had no instruction
  count behind it), setup 30;
- three lights are on 90% of draws, so light-count variants help 10% of the draws (skip);
- keeping the light constants in non-volatile FPRs saves ~12 loads a vertex but costs a save/restore per
  draw at ~5.5 vertices a draw: ~0.3 ms (est.);
- world x bone combined matrices save ~20 instructions a vertex (0.65 ms) but the vertex generation (bone
  uploads + texture scale/offset writes) moves ~800 times a frame, 3-4 draws apart, so the products cost what
  they save (est.);
- sending the kernel's output straight into the write-gather pipe saves ~0.7 ms (est.) but needs a store
  sequence per kernel variant (nine);
- `frsqrte` with one Newton step instead of two saves 3 instructions on re-normalised vertices but can move
  an 8-bit colour by one level: only with an `EXP_SKINCHK` tolerance decision.
- **The refresh** (`hot_refresh`): was 293 skinned + 86 plain a frame = 0.87 + 0.67 ms.  330 of ~375 refreshes
  were only a vertex type switch between fighter parts; `hot_switch()` (2026-10-25, `docs/STATUS.md` "A vertex type
  switch ...") now handles those at 0.42 us, so a skinned refresh happens ~78 times a frame (7.8 us each) and
  the saving is ~0.17 ms.  What is left is the pixel-state rebuild (texture lookup: the CI8 palette hash and the
  120-130 source re-hashes a frame).

- **Redundancy check, 2026-10-27 (`EXP_DRAWTRACE` `DV` lines, one fight, four traced frames six apart):** a frame has 2,704 skinned draws and 14,973 vertices (5.5 a draw) over ~1,900 distinct vertex blocks: mostly the two fighters cut into hundreds of small parts (x 1300-2300, z 2700-4500), and a far-away set of 213 draws (x up to 7500, z up to 10000) of which a 71-draw model is drawn three times in different poses.  No draw's output repeats from one traced frame to the next (0 of 2,704), so there is nothing to keep across frames; 186 draws (6.9%) repeat an earlier draw of the same frame with the identical output, worth at most ~0.4 ms before the cost of finding them.  Not built.

**P6. GX rigid skinning: BUILT and MEASURED 2026-10-27, a loss in Dolphin, so OFF (`-DEXP_RIGID` turns it on).**
A draw whose vertices all follow one bone at full weight goes to the chip as the model has it (16-bit positions, the
normal brought to unit length, the texture coordinate: vertex format 2, `GxVtxR`) with bone x world x view in the
second position-matrix slot and the bone x world rotation in the same slot's normal matrix; the chip does the
transform and the lighting the kernel would have (`rigid_scan`, `rigid_mats`, `rigid_decode`, `rigid_try` in
`raster.c`; `wii_gxscene_draw_rigid()` in `wii_gxscene.c`).  The chip does not renormalise, so a draw is refused
when the normal matrix is not a rotation: the world matrix of a fight carries one uniform scale (about 2,700; it is
divided out), and a bone that stretches (0-27 draws a frame) is skinned on the CPU as before.
*What a fight gives it* (`RIGID per frame`, shift 2500): 659-706 draws a frame (3,400-3,675 vertices, 5.2 a draw)
go to the chip with ~100 matrix loads; 1,215-1,294 are refused as not single-bone.  The census (`EXP_CULLCENSUS`,
`RIGIDCENSUS`) had counted 1,158 draws / 6,465 vertices a frame that are single-bone throughout.
*Correct:* `EXP_RIGIDCHK` replays every such draw through the skinning kernel and compares the chip's inputs with
its output: 10.5M vertices, texture coordinates exact, normals within 0.0018, positions within float noise
(absolute error under 5e-4 at view coordinates in the thousands; the checker's tolerance, 3e-4 relative to
max(|value|, 1), flags ~317 of them on components near zero, which is rounding and not a fault).  Paired fight
screenshots of the chip and CPU windows of one `EXP_ABRIGID` run (same stage, same fighters, matched by the time
of each shot against the `ABRIGID` lines) show the same shapes and lighting; nothing is warped or flipped.
*Cost:* `-DEXP_RIGID -DEXP_ABRIGID`, alternating windows of 300 frontend frames, Dolphin demo build, shift 2500:
**the chip path is slower**, +0.47 ms a frame at first (14 pairs, sd 1.00; ge +0.41), +0.24 ms after the four
per-draw `memcmp` calls (132 bytes a draw, a libc call each) became inline compares and a matrix version number
(19 pairs, sd 0.66; ge +0.22, other +0.02).  `DRAWCATS`: the same 2,207 fast-skinned draws took 5.63 ms with the
chip against 5.21 on the CPU.  The draws are small, so what the draw itself costs (the weight scan, the matrix
cache check, switching the current matrix between the view slot and the model slot, the second vertex format)
outweighs the multiply-adds it saves; the chip path writes the same number of FIFO words a vertex.
*Not measured:* the 1.5x clock stand-in (the loss scales with the clock) and a console.  Do not turn it on by default
without a measurement that flips the sign.  Ways it could still pay, none built: take whole runs of single-bone
draws of one bone with one matrix load and one `GX_Begin` (a merged submit was sized under 0.3 ms before), or keep
the bone slot with the draw-cache entry so the scan runs once per vertex block (under 0.3 ms).

**P7. Hardware lighting for the fighters: BUILT 2026-10-25, default on; 0.5-0.6 ms a frame in a fight
(estimated 0.9-1.6 ms).**  Design: section 3.  The kernel no longer does the three dot products and the colour
conversion (the 68 instructions of lighting become the normalisation it kept plus a normal store); the chip does
the lighting.  The same fight alternated with `-DEXP_ABLIT` (windows of 300 frontend frames, adjacent windows
of the same scene size compared, Dolphin, demo build): shift 2500 (19k vertices) chip - CPU = -0.58 ms a frame
(sd 0.99 over 20 pairs; ge -0.62, other +0.04); shift 18000 (21k vertices, 6 pairs) -0.57 ms (sd 0.56); the
lighter windows of the same runs -0.37 (18 pairs) and -0.50 (24 pairs).  Per op (`TKDR_FINEPROF`, shift 2500):
skinned decode 2.06 -> 1.85 us a draw averaged over all draws (the lit ones about 1.7), submit 0.24 -> 0.29
(two more stores a vertex), 1,965 of 2,561 skinned draws lit.  Checked three ways: `-DEXP_LITCHK` (34.4M
vertices, 0 bad; the chip's arithmetic as modelled is within 0 / 1 / 2 / 3 levels of the CPU colour on 55.8 /
42.8 / 1.4 / 0.00004% of 103M channel values, mean -0.30), `-DEXP_LITTEST` (the real GX lighting in Dolphin on
64 normals over the sphere with the game's lights, 12 runs, every channel within one 6-bit step, none two,
the chip about 1 level darker on average: -0.08 to -0.29 step), and fight screenshots.  The estimate was high
because the kernel's lighting is ~33 of ~250 instructions on the draws it applies to, a quarter of the draws
stay on the CPU, and the submit got dearer.  The first prototype (2026-10-23) lost because of per-state light
loads and extra attributes: here the lights load when they change (8 a frame) and 13 switches cost nothing
measurable.  **Not done:** the 17-23% specular draws (a second channel with specular lights, est. 0.2-0.3 ms
and more state switching), a kernel store straight into the FIFO (P5: **built** 2026-10-28, see the session notes), and any console measurement (the chip's
vertex rate is not a worry: 15k lit vertices a frame; the XF's 8-bit lighting arithmetic and the 2^20
light position are the SDK's own conventions but are unverified on hardware).

**P8. Display-list replay (GX display lists for CALL sub-lists): deprioritised, 1.3-2.5 ms (est.).**  Every 3D
draw sits inside a CALL sub-list (~1 draw per CALL) and the vertices change every frame, so a replay still
has to hash them.  Large on real hardware only if the cache hash goes away (see P1).  A hit's submit is 0.28 us of its ~1.5 us
(checks 0.4-0.5, lookup 0.5, setup 0.25), so a display list per cached draw would save at most ~0.2 us a hit
(~0.5 ms for 2.8k hits) and has to be rebuilt whenever the draw changes: not built.

**P9. Real-hardware-only items (unverified):** `dcbt` prefetch of the next vertices / draw-cache sets,
128-byte-aligned cache sets, `mftb`-cheap probes, 32-byte-aligned vertex staging, DOL size (11.9 MB; see
2.1), `-Os` vs `-O2` for the cold generated code.  Sized without hardware (2026-10-27): the 189 hot guest functions
(`hot_functions.txt`, built at `-O2`) are **636 KB of code** (the largest two 62 KB and 52 KB) against Broadway's
32 KB L1 instruction cache and a 256 KB L2 that data shares, so the guest thread's instruction fetches will miss far
more on a console than Dolphin, which models no cache, can show; at 1-2 ms a frame (est.: 8-15k line fills at 12-80
cycles) it is real but not the whole gap.  Whether `-Os` for part of the hot set pays can only be told on hardware.
Dolphin charges about one cycle an instruction, so a real CPI of 1.3-1.8 on this code would make every CPU-bound
millisecond above 1.3-1.8x as long (est.); that factor, not any single function, is what P0 has to measure.
A stand-in for that factor without hardware (2026-10-27): Dolphin's CPU clock override.  `OverclockEnable = True` and `Overclock = 0.666667` under `[Core]` in the user directory's `Dolphin.ini` (restore it after) make every CPU cost 1.5x as long in the guest's timebase.  The console demo (`-DWII_SDLOG -DTKDR_AUTOPILOT`) in a light fight then ran at **33-37 guest frames a second (55-62%)** with ge 17.2-19.4 ms and other 8.7-11.6 ms (against 51-54 and 12 / 7 at 1.0x), the screen still presenting 50-58 frames a second.  The draw-nothing floor (ge ~5, other ~8.5 at 1.0x) is ~20 ms at 1.5x, so no cheaper *drawing* alone reaches 60 there; the floor (guest logic 5.6 ms, list handling ~5 ms, sound, frontend) has to shrink too.

**P10. If the budget cannot be reached:** the options are frame skipping (render every other frame while
the logic runs at 60: **the project owner has rejected this**; "full-speed and correct logic means no
skipping"), lowering the cost of the fighters (fewer bones/parts drawn at distance), or shipping at the
elastic speed (66-85% on Dolphin).  Do not choose among these without asking.

**P12. Culling skinned draws that cannot be seen: measured 2026-10-27, dropped.**  `EXP_CULLCENSUS` tested every skinned draw
of two fights against the clip volume (every vertex outside one plane) and against a one-pixel box: 1-3% of the draws,
at most ~0.1 ms a frame before the cost of the test itself.  The stages and the fighters are in view nearly all
the time.  Not built.

**P11. Draw-cache misses from the global ambient alpha (measured 2026-10-25 night; BUILT 2026-10-28, see the end of section 14's session notes).**  Some stages fade
a group of small flat-lit draws by changing `GE_AMBIENTALPHA` every frame; it is part of the lighting signature,
so each of those draws misses every frame although its bytes are unchanged (`-DEXP_VCWHY`).  A miss costs 5.6-6.2 us
(`PLAINMISS`: decode 0.62 us a vertex, store 0.11, pack 0.64-0.79 us a draw); the worst window of a 190 s run had 130
of them a frame (0.80 ms), typical heavy windows 15-60 (0.1-0.4 ms), the stage that first showed it 150-300
(1.1-1.8 ms).  Options: an in-place alpha rewrite of the cached vertices (needs the vertex alpha kept beside them
and a second index without the alpha in it) or a faster lit no-normal decode (~0.1 us a vertex): both under 0.2 ms on
average.  Leaving the registers of switched-off lights out of the lighting signatures was tried and measured
(`EXP_ABSIG`, alternating in one run) with no effect, and removed (`docs/STATUS.md`, "Not built: the draw cache
misses that come from the ambient alpha").

## 9. Remaining work: correctness and features

**B1. Intro movie.  BUILT 2026-10-28 (Dolphin only; the console's speed is not known).**  `avc.c` is the decoder: FFmpeg 7.1.1's
H.264 decoder and parser alone (`--disable-everything --enable-decoder=h264 --enable-parser=h264 --disable-asm`, LGPL-2.1+,
single-threaded; the build script defaults to `-Os`, the shipped library is `-O2`), built by `tools/build_ffmpeg.sh` (downloads the official tarball, checks its SHA-256, configures
out of tree, installs `third_party/ffmpeg-ppc/`, none of it committed; the header of the script has the Windows recipe: run
it from PowerShell with devkitPro's msys2 bash, `MAKE` set to its make and `TMPDIR` to a POSIX path).  The `Makefile` links
it when `third_party/ffmpeg-ppc/lib/libavcodec.a` exists (`-DTKDR_FFMPEG`, `avc_stub.c` dropped); `make EXP_NOFFMPEG=1` (or
no such directory) builds the black-frame stub as before.  The make has no dependency on the flag: after switching, delete
`tekken6wii.elf` to relink.  `avc_mem.c` answers FFmpeg's allocator calls from the MEM2 pool (the script renames the libraries'
`malloc/free/realloc/memalign` to `tkdr_ff_*` with `objcopy --redefine-sym`).  The decoder runs with `thread_count 1` and
`AV_CODEC_FLAG_LOW_DELAY` (the stream has no VUI reordering limit, so FFmpeg would otherwise hold pictures back).  The
pictures are `YUVJ420P` (full range) and `convert_8888()` turns them into the guest's pixel format straight into host memory,
three table loads and ors a pixel (the 16-bit modes and a destination that is not one block of host memory take the
slower `convert_frame()`).  Checked in Dolphin: the intro plays with the right colours (fire and the "GET READY FOR THE NEXT
BATTLE" card), the title and the attract fights follow, the production DOL does the same.
Measured with `-DEXP_AVCPROBE` (Dolphin's timebase, old split path): decode 19.3 ms mean and 32.4 ms worst, 8888 conversion
5.6 ms; the current fused path measures decode 17.8 ms plus 4.16 ms for the combined convert/scale on the decoder thread, and
about 0.3 ms for the frontend row copy, for a 33.4 ms frame period.  **Unknown: Broadway's speed** (the decode has big tables
and Dolphin does not model the caches; the in-loop deblocking filter stays on, no `skip_loop_filter`).
**2026-10-28, later: the movie showed only its first frame.**  The player decodes into a guest buffer and copies it into the
framebuffer it has **already** presented; it does not call `sceDisplaySetFrameBuf` again for every picture.  The frontend's
GX-idle CPU fallback used to run only when `g_frames_presented` changed, so it kept showing the chip's old (black) picture
while the decoded frames went into the guest buffer unseen; a `TEXFLUSH`/block transfer would occasionally update one frame,
which is the "first frame, then it cuts off" symptom.  Two changes fix it: `texrep_note_video()` now marks the movie's texture
range stale (so a chip-side sampling of it re-decodes), and the frontend's fallback also triggers on `g_gpu_write_tick`, which
`gpu_tex_invalidate_range()` bumps for every block transfer, DMA or decoder write.  Screenshots of the production DOL now show
the fire, "GET READY FOR THE NEXT BATTLE" and VS frames advancing.
**The MEM1 trade-off is gone as it stood.**  FFmpeg grows the DOL by 0.85 MB at `-Os`; the old build compensated by
letting host thread stacks take 4 MB (boot) and up to 2 MB (others) from MEM1, leaving 4104 KiB and failing at `-O2`
(+1.1 MB) when a movie worker could not start.  The 2026-10-28 memory pass (`docs/MEMORY.md`) removes the unwind tables
(DOL -860,672 B), drops `.bss` by 983,392 B and reserves 512 KiB (boot) / 256 KiB (others) of host stack, so the boot line
now says **6160 KiB left** with room to spare; `-O2` has not been re-run, but it no longer competes with a 4 MB stack.
The movie player still renders into a texture target, which needs B3 (not looked at; the picture shows without it).
*Measured 2026-10-28 with `-DEXP_AVCPROBE`:* the stream is **H.264 Main profile, level 2.1, CABAC** (PPS entropy_coding_mode_flag 1), 480x272, 29.97 fps (VUI timing 4004 / 240000), `num_ref_frames` 1, POC type 2 (no B-frames; output order is decode order), 6-bit frame_num; the player hands the stub one 521,050-byte buffer about 12-15 s into a run (AUD, SPS, PPS, SEI, an IDR, then P slices; the first ones are 65-310 bytes, probably a fade from black, not checked) and `sceMpegAvcDecode` writes to guest RAM `0x09754140`, stride 512, pixel mode 3 (8888).  The whole MPEG HLE (`mpeg.c`: ring buffer, PS demux, AU splitting, ATRAC3+ audio) is done; the decoder that was missing here is the one built above, and the work list that stood in this place was carried out.

**2026-10-28, fused converter and probe-free playback.** The full-screen nearest scaler was added after the initial
full-stream evidence, and the intro then stopped at `MOVIE stop frames=1911 read=11077632 size=17182720` (63.8 s).  A first
fused build still stopped at **2,333 frames / 13,225,984 bytes** while `WII_SDLOG` and `EXP_AVCPROBE` were enabled; the SD
log fsyncs and the A/V probes were the remaining perturbation.  `wii_video_convert_movie()` now converts full-range YUV420
to limited-range YUV422 and does the 480x272 -> 640x362 nearest scale in one pass on the **decoder thread**, into a 640x362
MEM2 buffer; `wii_video_present_movie()` only row-copies that into the XFB and flips.  The frontend 1.9 ms scaler and the
480x272 intermediate are gone.  The final probe-free production DOL was screenshotted at 110, 125 and 135 s: the intro is still advancing at 110 s, the
title fades in at 125 s and is up at 135 s.  Production DOL **12,200,064 B**, SHA-256
`51BC7E22102E89E8E765B5D71868CB1D3242AE48B0E198F736370E8C1A9D0922`; `AVCPROBE`, `STATESORT`, `MOVIE`, `AUTOPILOT` and
`WII_SDLOG` strings are absent (the probes remain opt-in in the source).  Screenshots at 110, 125 and 135 s were checked;
the FFmpeg library is the `-O2` build.  The Plus/Start skip is in the replay script, not in the emulator input mapping.

**B2. GE effects the GX cannot express are skipped, not emulated:** colour test, complex stencil,
absolute-difference blend (GE equation 5), doubled-alpha factors (6-9), fixed destination blend colours
(10-13).  The design is the layer classifier of `docs/PLAN.md` section 4.8 (native/cheap GX first, TEV or a
Z-shadow mask second, RTT multipass for the general full-screen case; per-primitive CPU insertion into a GX
frame is **forbidden**: software-only sublayers are invisible in the GX frame).  Do not implement a feature
that no captured scene uses (`rtt=0` and `skip=0` in the menu and fight states measured so far).

**B3. Render-to-texture.**  Texture targets (the movie player's decode buffer, card/menu effects) fall back
to the software rasteriser.  Design in `docs/OPTIMIZATION_IDEAS.md` 3.1 (lazy GX target shadow + dirty CPU
tiles) and PLAN section 5.  Build only against a captured scene.

**B4. Native paletted textures.**  `GX_NATIVE_TEXTURES` (exact RGB565 CI4/CI8 + TLUT; four defects fixed
2026-10-23, `GPU_SELFTEST` 265/265) and `GX_FASTTEX` (lossy RGB5A3, 3-9% of a menu frame differs by > 24
levels) are off.  Turning the exact one on would cut texture upload 4-8x on transitions; it needs a
same-frame on/off screenshot comparison of the characters' faces first (PLAN section A).  2026-10-25 night: no
window of 83 autopilot logs shows a texture decode above 0.5 ms a frame (`TEXDECODE`; the fights decode nothing),
so there is nothing to win in a fight; the gain would be in menus and loading, which the 300-frame windows
average away and nothing has measured.

**B5. Transition stalls.**  A 578 KB asset read followed by `sceGeDrawSync` (the guest waits for the GE list
that uploads the new data).  Returning early from a display-only `sceGeDrawSync` was tried and caused
missing character art (the guest reused buffers the GE thread still read).  The exact answer is a snapshot /
copy-on-write of the list, vertex and texture ranges at enqueue (PLAN section B); the cheap answer is to
accept the stutter (the elastic vblank softens it).
*Load hitches, 2026-10-26 (`docs/STATUS.md` "Load hitches ..."):* the stalls that were not that wait were mostly the ISO
seeks (libfat walks the FAT chain: 40-81 ms a far seek, now direct sector reads through `wii_fatmap.c`) and the loader's
SHA-1 and LZ decoding (now native, checked): `HITCH` lines in a 110 s run went from 79 to 35-40 and the frames that hash
an asset from 50-184 ms to 28-39 ms.  Left, one-off: the boot's second SHA-1 `f_08A2D490` (~190 ms), VFPU math
`f_089B667C` / `f_089B5D8C` (349 ms once), `f_08A157B0` / `f_08A161E8` / `f_08A1633C` (100-170 ms in a few frames).

**B6. The intermittent black 3D: fixed (2026-10-25), one question left.**  From a fight's first round the
whole 3D scene is black with the HUD intact, for ~40 s or until the round changes (1 in 6-7 autopilot runs of
130-170 s with `-DAUTOPILOT_MASH`).  After the 3D the game draws a full-screen blit (through mode, decal,
blend and z test off) whose texture is its own display buffer (04000000 or 04044000, 512x512, 5551) and
whose render target is that same buffer: on a PSP it redraws the picture onto itself, once a frame.  The GX
path draws into the EFB and never writes guest VRAM, so those buffers are zero in guest memory and the blit
painted the finished 3D black.  The fix is in production code: `bind_display_copy()` in `wii_gxscene.c`
copies the picture so far out of the EFB (`GX_CopyTex`, RGB565, the letterbox rectangle, a 462 KB pool
buffer) and binds that for any through-mode texture of a presented address that is at least 480x272
(`is_display_buffer()`); `pix_state()` in `raster.c` tells `tex_cache_lookup()` (chip = 2) that the texels
are not used, so the empty 512 KB is not hashed every frame.  The software path is unchanged (its VRAM is
real).  How it was found (the readings that were refuted are in section 11): the black state was not a stuck
thread, async IO, the pool, the draw cache, the GE state or the camera; the staged `EXP_DARKDBG` run showed
that only the texture contents made it black (stage 11, a white texture for every texture, was bright);
`TEXPROBE` found exactly two all-zero 512x512 converted textures and `TEXENTRY` that their guest source was
zero, at the display buffers.  Verified in Dolphin: runs that entered the mode (`DISPCOPY` lines, 2400+ in
50 s) stayed bright with the right picture and the HUD, 0 dark copies.
**Open:** why the game enters this blit mode in some runs and holds it for ~40 s (a hold-last-frame or a
dissolve?).  Our copy is the current EFB, not the previous frame; that is the same picture when the blit is
an identity, and was not found to differ.  To reproduce the state, build with `build_shifts.ps1 -Extra
'-DAUTOPILOT_MASH -DEXP_DARKDBG'` and loop `run_dol.ps1` 170 s runs until a log holds `DISPCOPY`.

**B7. Visual QA corpus.**  `docs/PLAN.md` section 3 (PPSSPP reference frames, per-layer state capture,
`tools/compare_frames.py`) and `docs/INGESTION_AUDIT.md` (texture/CLUT/framebuffer ingestion audit) were
planned and **not written**.  The culling fix changed every fighter and stage draw: a pass over all
characters and stages against PPSSPP is the right way to catch single-sided/double-sided or winding surprises
(the stages are single-sided; the wrong winding deletes them).  Hair/alpha layers are the sensitive ones.

**B8. Audio on hardware.**  AESND at 48 kHz fed by a 44.1 -> 48 kHz linear resampler on a low-priority LWP
thread; SAS and ATRAC3/3+ are compiled in and sound right in Dolphin; ATRAC performance on Broadway is
unmeasured.  With the elastic vblank the music streams in real time while the game logic may run at 66-85%:
check that sound effects and music stay acceptable in a heavy fight, and that nothing keys off the vblank
count for audio sync.

**B9. Saves, input, network.**  Save data goes under `<app>/ms0/PSP/SAVEDATA` through the utility HLE
(`runtime/hle/utility.c`); a full save/load round trip has not been exercised in the notes.  Input is Wii
Remote / classic / GameCube pad mapping in `wii_input.c`; only the autopilot scripts navigate in tests.
Ad-hoc/infrastructure network modes are HLE stubs.  NTFS USB (read-only libntfs) has only been built, never
run on hardware.

**B10. Movies and the elastic vblank.**  Not looked at: whether pacing that reads `sceDisplayGetVcount` /
`WaitVblankStart` runs the intro and cutscenes at the right speed (they read the same count, which stays
continuous, so they should).

## 10. Remaining work: housekeeping

- `README.md` "Status" and `docs/PORTING.md` "Next steps" are stale (they predate the GX renderer, the
  fight work and the real-ISO runs); `docs/PLAN.md` is correct for design but its "What is true today" is
  from 2026-10-22.  Fold this document's section 7 into the README when the owner is ready to publish.
- `libsysbase`'s pthread object is shadowed by `-Wl,--allow-multiple-definition`; replace with a complete
  symbol set.
- No CI: the checkers (`tools/test_vfpu_emit.py`, `tools/test_sas.c`, `tests/`, `GPU_SELFTEST`, `EXP_SKINCHK`, `EXP_HITCHK`,
  `EXP_OVRCHK`, `EXP_SASCHK`) are run by hand.  A script that builds the four checker variants and plays one fight in
  Dolphin would catch the "plain build broken" class of mistake.
- `build-*` directories in the repository root are git-ignored build trees of earlier experiments; most can
  be deleted.
- `profiles/tekken6/` (Tekken 6 `ULUS10466`) and `tools/prepare_game.py` are experimental; the
  Dark Resurrection path (`profiles/tkdr`, `tools/prepare_tkdr.py`) is the one that works.

## 11. Tried and refuted (do not repeat without new evidence)

| idea | result |
| --- | --- |
| depth bias / z-fighting for Steve's hair | wrong: +/-2 and +/-300 steps changed nothing; the cause was missing back-face culling |
| per-1KiB guest store stamps to prove draw vertices unchanged | every plain draw is rewritten every frame, so hits went to 0 and the GE thread got 4-8 ms slower; content hashing is the only valid check |
| a four-entry display-list/frame cache | hashing cost cancelled the saved draws |
| returning early from display-only `sceGeDrawSync` | no frame-time gain, missing character art |
| hardware lighting, first prototype (2026-10-23) | net loss (extra normal/colour attributes and a light-object submission per state cost more than the CPU lighting saved), reverted; rebuilt around the paired-single kernel on 2026-10-25 and kept: -0.5 to -0.6 ms (P7) |
| `-O2` for all generated code | +0.6 MB for ~0.6 ms; only the 189 hot functions are `-O2` |
| `raster.c` at `-O2` instead of `-O1` (2026-10-25) | the object compiled 4/4 times with the GCC 15.2 ICE flags, but per-op probes gave: skinned decode 2.06 -> 2.05 us (already `Ofast` by attribute), vertex-type switch 0.42 -> 0.33 us, clean texture lookup 0.84 -> 0.62 us: about 0.1 ms a frame for a unit that ICEs at `-O2` |
| merging runs of cache-hit draws into one `GX_Begin` (P1) | under 0.1 ms: a hit costs ~1.5 us and the cost sits in the state refreshes between runs, not in the runs |
| native override of `f_0896D7AC` (animation decoder) | correct (checked) but gained almost nothing |
| light-count specialised skin kernels | 90% of draws use three lights |
| combined world x bone matrices | recompute per bone upload costs what it saves (est.) |
| merged skinned submit (one `GX_Begin` for several draws) | under 0.3 ms: the per-vertex stores are the cost |
| native CI4/CI8 textures with a forced RGB5A3 approximation | black screen / lossy; exact RGB565 only |
| skipping GX frames (frame skip) | rejected by the owner |
| trusting a cache entry for a few frames once stable | unsound for the same reason as the stamps |
| the hash loop of the draw cache unrolled by four (P1, 2026-10-25) | measured on a scratch build: ~0.04 ms a frame, not the 0.1-0.2 estimated; not built |
| "the later fights are slower because the draw cache's arena only grows" (2026-10-25) | the arena was real but not the cause: the cache evicted only way 0 of a full set (`vc_store`, `k < VC_WAYS && !v`), so every set was direct-mapped; a variant-aware store policy written for it was not the fix either; found by replaying the logged lookup stream (`EXP_VCDUMP`, `vcsim.py`) against the real hit/miss per lookup |
| GX rigid skinning (P6, built 2026-10-27) | correct (checked, same picture) and 0.2-0.5 ms a frame slower in Dolphin: 5-vertex draws, the per-draw work exceeds the skinning saved; off, `-DEXP_RIGID` |
| culling off-screen / sub-pixel skinned draws (P12) | 1-3% of the draws: not worth the test |
| B6 (black 3D): the game's own fade stayed at full black (2026-10-24 reading) | no: the black draw was the display-buffer blit of zero texels (section 9, B6) |
| B6: a stuck guest thread, an asynchronous read that never finished, MEM2 pool exhaustion, a stale chip state (depth, lighting, cull, texture unit), the camera, the draw cache | each excluded by a probe: `sched_dump_threads` shows every thread waiting normally, `sceIoReadAsync` completes synchronously, the pool was at 10 of 14 MB with no failed texture allocation, debug stage 11 (white texture for all textures) was bright, `CAMERA` x = z = 0 also in bright windows, `HITCHK` 0 bad |
| B6: the two zero textures are stale decodes of stage textures (hash/flush not seen) | no: `TEXENTRY` read the guest source and found it zero (nonzero bytes at decode: 0, hash unchanged): they were the empty display buffers |

## 12. Working agreements (from the project owner, 2026-10)

- **Do not deploy** the DOL to `dist-tkdr/apps/tkdr/boot.dol` or `dist-tkdr-sd-full/apps/tkdr/boot.dol`
  (`make dist-tkdr` copies it) unless explicitly asked.  **Commit and push only when asked.**  Commit
  messages end with the `Co-Authored-By` line the tooling asks for.
- **Real speed, correct logic**: the target is 60 guest frames a second and a frame time the console can
  hold; no frame skipping, no accuracy traded for speed without saying so (the owner notices lighting and
  hair artifacts).  "Any optimization is worth pursuing": small wins count, but measure each.
- No game data, `gen/`, `image.bin`, ISOs or reference checkouts in git; no GPL code copied
  (`tkdr-recomp` and `azazel6` are MIT-clean references, PPSSPP is a behaviour reference only).
- Production DOL stays probe-free; instrumentation under `TKDR_AUTOPILOT`, `TKDR_FINEPROF`, `EXP_*`,
  `HLE_TRACE`.
- Keep the unit checkers green (`tools/test_vfpu_emit.py`; `python -m unittest tests.test_tk6_fixture`; `tools/test_sas.c` after any change to `sas.c` / `audio.c`; `tools/test_sha1.c` after `sha1_core.h`; `tools/test_fatmap.c`, image and `--synthetic`, after `wii_fatmap.c`).
- Elastic vblank stays on by default (decided 2026-10-25).

## 13. Where things are documented

| topic | where |
| --- | --- |
| dated evidence for every number above | `docs/STATUS.md` (sections "Fight frame rate" x3, "Correction: 30 fps is half speed", "The floor", "The intermittent black 3D", "Steve's hair", "Elastic vblank ...") |
| accuracy plan: GE effects, RTT, corpus, movie | `docs/PLAN.md` (sections 3-8, 11-13 and "Remaining avenues"; partly outdated) |
| layers/resources design, candidate overhauls | `docs/OPTIMIZATION_IDEAS.md` |
| MEM1/MEM2 budget, large statics, pool use, 2026-10-28 reductions | `docs/MEMORY.md` |
| memory map, threads, audio, files | `docs/PORTING.md` |
| install layout for the SD card | `dist-tkdr/apps/tkdr/README.txt`, `wii/apps/tkdr/` |
| scripts | `tools/perf/` (build / play / screenshot), `tools/dolphin_autotest.ps1`, `tools/prepare_tkdr.py` |
| the owner's per-session notes | Claude memory notes (`tekken6wii-*`), not part of the repository; their content is folded in here |

## 14. Handoff: what is open, in the order to take it (2026-10-27)

**State of the tree.**  `main` is at `b85ac30` (plus the commit that updates this paragraph and the texture-matrix
commit of the evening of 2026-10-27, item 2 below), with only the `EXP_ABCOLNONRM` A/B switch (`frontend.c`, `raster.c`)
uncommitted.  Eight commits are local and **not pushed** (`512d0d1`, `83ed638`, `fa0c6a9`, `e50b9d2`, `204fcb6`,
`8c5c7db`, `12482f0`, `b85ac30`); nothing has been deployed since the 2026-10-24 checkpoint (section 7).  `b85ac30`
(2026-10-27) holds, in the production path: the VFPU translations of `vcmp`, `vcmov` and the pack/unpack conversions
(`recomp/vfpu_emit.py`, `runtime/vfpu.c`; **`gen/` must be regenerated** after a checkout, it is not in git; the
interpreter's 400-700 calls a frame, 0.2-0.5 ms, are gone), the vertex-colour skinned parts on the whole-draw path
(`raster.c`, section 3: -0.94 ms a frame where the fight has them, plus -0.32 ms for `col_lit()`), and the chip no
longer face-culling rectangles (`gxr_state()` has a `rect` argument; it only matters where the game draws sprites with
culling on, which no scene looked at did).  Off by default in it: the P6 rigid-skinning path (`EXP_RIGID`; a no-op in
the production build, `rigid_try()` returns 0 at once) and the probes of 2026-10-27 (flags table).  Checked at that
state: `tools/test_vfpu_emit.py` (200,388 runs, 0 failed), `python -m unittest tests.test_tk6_fixture`,
`EXP_SKINCOLCHK` (14.2M vertices, 0 bad), `EXP_RIGIDCHK` (P6), the production build (section 7: no probe string,
boots, fights drawn).  Untracked scratch in the root (git-ignored): `tekken6wii-console-demo.dol/.elf/.map` is the
**console demo for P0, keep it** (it predates `b85ac30`: rebuild it before taking it to a console); the other
`tekken6wii-*.dol` and `build-*` trees are experiment leftovers.

**Session of 2026-10-28: first-use texture costs (built, checked, uncommitted, not deployed).**  The task was "continue with
the remaining performance optimizations"; the classic levers (P5, P7 specular, P11, P1) were not started, because a look at
the 2D / software category found something larger.  `-DEXP_SLOW2D` (`raster.c`, with `TKDR_AUTOPILOT`) times every draw of
category 5 ("2D/sw") and prints the ones over 2 ms as `SLOW2D f=.. N us (state .., chip ..) prim n vt .. WxH fmt addr serial`
(`state` = `pix_state()`, i.e. texture lookup and decode; `chip` = `gxr_prim()`, i.e. the GX upload) and, every 300 frames,
`S2D` per texture.  Three real costs came out of it:
1. **Texture decode** (`tex_decode_fast()`): every first use of a texture paid 40-50 cycles a texel in `texel_addr()`'s
   multiplications and divisions (3.6-8.5 ms for a 256x256 CLUT4 card, 20-25 ms for a 512x512 16-bit one: the 30-50 ms
   frames when a menu loads a card).  Now a row (linear) or a 16-byte piece (swizzled) at a time, for all six formats the
   function takes (565, 5551, 4444, 8888, CLUT4, CLUT8).  The old loops stay behind `-DEXP_OLDPALDECODE`.
2. **The display-buffer blit** (B6's `chip == 2`: the game draws a presented 512x512 buffer as a texture): `tex_cache_lookup()`
   hashed 512 KB and decoded 262,144 texels (~27 ms) every time a block transfer touched the buffer, although the chip
   answers that draw with a copy of its own picture (`bind_display_copy`) and never reads the texels (the guest's memory does not
   hold the picture anyway).  A window of 22.9 ms / guest frame in a heavy fight is explained by it.  Now `chip == 2` returns
   early with `TexState.blit` set (`gxr_state()` counts it as a texture); the old behaviour is `-DEXP_OLDBLIT`.
   **Not verified by a live episode**: the game only enters the blit mode now and then (a scene or timing lottery), and
   six 150 s hunts with screenshots (shifts 2500, 10000, 18000, `EXP_SLOW2D`) did not hit one.  The GX side is unchanged
   (`is_display_buffer()` has the same criteria as the raster side's `chip == 2`; only if the copy's allocation failed does the
   bind now fail, where it used to bind blank texels).  **When an episode shows up (`DISPCOPY` in the log), look at the
   screenshots, and check that `SLOW2D` shows no 512x512 draw.**
3. **`convert_rgba8()`** (`wii_gxscene.c`, the GX upload of every decoded texture; CLUT textures take this path too, the native
   CI4/CI8 upload is off in production): halfword stores and a whole-tile path with no edge clamping.  The byte-wise copy
   stays as `convert_rgba8_ref()` under `-DEXP_CICHK`.
**Numbers** (Dolphin; `GPU_SELFTEST` build with `-DEXP_TEXDECBENCH -DEXP_CICHK` printing `TEXDECBENCH` / `RGBABENCH` at boot,
built twice, once with `-DEXP_OLDPALDECODE`; old -> new, us a texture): CLUT4 256x256 linear 4330 -> 703, swizzled 4833 -> 719;
CLUT8 256x256 swizzled 4519 -> 826; 565 512x512 linear 20,917 -> 8320; 5551 512x512 swizzled 24,533 -> 10,126; 8888 256x256
swizzled 3972 -> 824; CLUT4 128x128 swizzled 1215 -> 183; `convert_rgba8` 128x128 470 -> 196, 256x256 1880 -> 778, 512x512
7523 -> 3111.  In a fight run (shift 2500) the first use of a 256x256 CLUT4 card: `state` 1.2-1.6 ms (5.1 before), `chip` 0.9-2.0
ms (2.0-3.2 before).  **Steady-state fight speed did not change** (these are first-use costs): 76-78% in the heavy window of
that run (ge 14.7 + other 6.6 ms); that is not an A/B.  A 512x512 565 decode was still 8.3 ms (~19 cycles a texel): the
same day it got two 256-entry tables (below), 5.1 ms.
**Checks:** `-DEXP_TEXDECCHK` (every decoded texel against the per-texel addressing, CLUT: `TEXDECCHK`, 16/32-bit:
`TEXDECCHK16`): 1345 textures in a fight run, 0 bad; `-DEXP_CICHK` (`RGBACHK`: every conversion against the byte-wise one):
1345 textures, 0 bad.  `-DGPU_SELFTEST` now also compares `tex_decode_fast()` with `fetch()` for 6 formats x linear and
swizzled x widths 1-64 x heights 1-16 x narrow and wide strides on pseudo-random bytes (`SELFTEST decode_fast: 840 textures,
94,488 texels checked`, all equal; `SELFTEST textures: 862/862`), and `convert_rgba8()` against the GX tile rule written out
(2x2, 6x5 padded, 8x8; `SELFTEST gx paletted: 841/841`).  The fixture unit test is green; the other standing checkers
(`test_vfpu_emit.py`, `test_sas.c`, `test_sha1.c`, `test_fatmap.c`) test files this change does not touch and were not rerun.
**16-bit expansion by two tables (same day, built and checked, not deployed).**  `c16_to_32()` turns a 565 / 5551 / 4444 texel into RGBA8888 with shifts and masks; every output bit is a copy of one input bit or a constant, so the result for a value is the OR of the results for its low and its high byte (the green of a 565 straddles them and is no exception).  `tex_decode_fast()` now looks the two bytes up in `s_c16_tab` (3 formats x 2 x 256 words, 6 KB, built on first use from `c16_to_32()` itself): `-DGPU_SELFTEST` compares the tables with `c16_to_32()` for all 65,536 values of all three formats (the 861st check), and the 840 shape checks and `TEXDECCHK16` read the same loops.  A/B on the boot bench (`-DEXP_TEXDECBENCH`, one build with `-DEXP_NOC16TAB`, same machine, same minute): 565 512x512 linear 8320 -> 5077 us, 5551 512x512 swizzled 10,124 -> 6515 us, the other formats unchanged.  The fight run with `-DEXP_TEXDECCHK` (shift 2500, 160 s) decoded 1153 paletted textures, 0 bad, and **no 16-bit texture** (no `TEXDECCHK16` line), so the 16-bit path is covered by the exhaustive and the shape checks and by no real-data run.  Production DOL after this: 11,976,544 bytes, SHA-256 F0D53178A4976C75..., no probe string (the same twelve words, all 0).
**P5, P7, P11 and the blit, the rest of that list (same day, later).**  **P11 built**: the draw cache keeps a lit draw without normals (`vc_regs()`: `normals` 0, lighting on, format colour 0 or 4-7) under the lighting signature *without* the global ambient alpha (`ge_light_sig_flat_noalpha()`, now cached like the other two) and keeps `0x100 | alpha` beside it in the slot (`VcSlot.gax`, the old `pad`); a lookup that matches in everything but `gax` is a hit whose alpha bytes the caller makes again (`vc_alpha_refresh()` in `raster.c`, `tok.refresh`; `vc_lookup2()` stores the new alpha in the slot), a few instructions a vertex instead of a miss's decode, store and pack (5-6 us).  The byte is `(2 * a * ga + 255) / 510` with `a` the vertex's alpha when the format has a colour and the material update's bit 0 is set, else the material alpha register; `GPU_SELFTEST` compares it with `light_registers()` and `light_general()` for all 65,536 pairs and `colour_alpha()` with `rd_color_fast()` for every colour value (the 862nd check).  **The first version hashed the alpha into `hr` with `mix()` and `-DEXP_HITCHK` found it wrong (bad 1403 of 1.1M re-decoded draws, alpha off by 1-2):** `mix(h, w)` is `rotl(h, 5) + h + 2 * (w & ~h)`, so a lone small `w` counts only in the bits where `h` has zeros and different alphas collided; hence `gax` is compared whole.  The same collision applies to any single small value mixed last; the long chains of the other signatures have not shown it (0 bad over 6.7M checks before and 1.0M after).  **Checks:** HITCHK demo, 175 s, ~980,000 cached draws re-decoded, **0 bad**, with 2,500-5,900 refreshed hits a 300-frame window; `-DEXP_ABREFRESH` A/B over 19 pairs of that run's heavy fight: cache misses a frame **-6.4** (sd 6.3), frame **-0.26 ms** (sd 0.58, about two standard errors), ge -0.15; the fight is not the stage that had 150-300 such draws a frame (the stage is unnamed and cannot be chosen), where the estimate is ~1 ms.  Draws with normals and the odd colour kinds 1-3 are not refreshed (the old hash, alpha included); with lighting off the alpha is not in the hash at all now (it never reached the vertices).  `-DEXP_VCWHY`'s "flat" / "full" guess no longer names the signature (it compares with the old `hr`).  **The display-buffer blit, checked by reading** (no live episode: six hunts and one more run found none): the raster side's `chip == 2` is `chip && through && fmt <= 3 && w >= 480 && h >= 272 && gpu_is_presented_buffer(addr)`; the chip side's `is_display_buffer()` is `!tex_native && fmt <= 3 && through && w >= PSP_W && h >= PSP_H` and the same address list (`gpu_disp_info()` and `gpu_is_presented_buffer()` read one `s_disp_addrs` with the same mask), so the two agree; a blit draw is always `use_gx` (it needs `chip`), so the software rasteriser, which would read `tex.pix == NULL`, never sees it; `tex_get()` returns NULL for a NULL `tex_pix` (the draw is then drawn untextured), `tex_touch()` takes a NULL entry; `gxr_state()` counts `blit` as a texture.  What the new code changes at run time is only that the chip copies the picture once per lookup (a new serial) where the old cached entry kept its serial until it was decoded again: one lookup a frame in the one scene that does it, so the same.  Still worth a look when a `DISPCOPY` shows up.  **P7's specular draws: not built, and not buildable exactly**: the GE's term is `(n . h)^e` with a whole exponent (8 in the fights); the chip's specular channel is a ratio of two quadratics in `n . h` (`GX_AF_SPEC` with `GX_InitLightShininess`), which cannot follow a power of eight, so the highlight would change shape: a visible loss for ~0.2 ms.  **Production DOL** (plain `make`): 11,976,384 bytes, SHA-256 12BE680C22934AE2...; no probe string (`GXPERF`, `DEMOFPS`,
`DRAWCATS`, `AUTOPILOT`, `SLOW2D`, `TEXDECCHK`, `RGBACHK`, `SELFTEST`, `DISPCOPY`, `HITCH`, ... all 0); boots to the attract mode
with the 2D textures right, and the same code in a demo build draws the fights (ice-field stage, shots at 130 and 160 s looked at: fighters, penguins, HUD and lighting right).
**The earlier uncommitted work** (the opt-in TEV fog `EXP_TEVFOG`, the stage lock `EXP_STAGELOCK` in `overrides.c`, the part log
`EXP_PARTLOG` and the A/B switch `EXP_ABCOLNONRM`) **went in with `f3a57ca`**.  The "0x431D not
built" text of earlier revisions was wrong: those parts have been on the whole-draw path since `b85ac30`; no fight played this
session had any, so their A/B (`EXP_ABCOLNONRM`) was not measured.
**P5 built (2026-10-28, later), on by default.**  The skinning kernel's `h` variants (`skin_ps.c`: `tkdr_skin_ps_h{n,u,f}_pipe`, `g_skin_ps_pipe[3]`) store the chip-lit fighters' vertices straight to the write-gather pipe at `0xCC008000` (`psq_st` with the offset 0 and the stride 0, the stream order of a vertex: position 12 bytes, normal 3 x s16 = 6, texture coordinate 8 = 26 bytes), instead of the 28-byte `GxVtxL`s of the staging buffer that `wii_gxscene_draw_lit()` then copied a vertex at a time.  `wii_gxscene_draw_lit()` is now `wii_gxscene_lit_begin()` (all the state, the lights, the `GX_Begin`; it returns the vertex count, a multiple of three for triangles, or 0 if nothing was begun) + the copy loop + `wii_gxscene_lit_end()`; the hot path in `draw_prim_fast()` calls begin, the pipe kernel and end itself (`SKIN_PIPE`, draws of 3 vertices or more; the kernel runs after the setup, so nothing between the GX_Begin and the kernel may touch the pipe).  **Checks:** `-DEXP_PIPECHK` demo (shift 2500, 200 s): the dense twin of the pipe kernel (the same stores, to a buffer) against the staging kernel on **11.1 million chip-lit draws, 0 bad**; that proves the bytes and their order, not the stores to the pipe itself, which only a render shows: the production DOL's fight screenshots (ice stage: fighters, penguins, HUD, lighting) looked right.  **Measured** with `-DEXP_ABPIPE`, two heavy fights (shift 2500: 13 pairs, ge -0.40, other +0.36, frame -0.04; shift 18000: 30 pairs, ge -0.33 sd 0.62, other -0.01, frame -0.34 sd 0.91): pooled ge **-0.35 ms/frame** (about 3.6 standard errors), frame -0.25 (about 1.7).  It is what the estimate said (the loop and the staging stores are gone; ~4k lit vertices a frame), and small; the cache effect on the real Wii is unmeasured.  `-DEXP_NOPIPE` turns it off.  **Production DOL:** 11,980,256 bytes, SHA-256 C1B39FF932AAF94D...; no probe string (`GXPERF`, `DEMOFPS`, `DRAWCATS`, `AUTOPILOT`, `SLOW2D`, `TEXDECCHK`, `RGBACHK`, `SELFTEST`, `DISPCOPY`, `HITCH`, `PIPECHK`, `ABPIPE`, `VCREFRESH`, `ABREFRESH`: all 0).
**Items 3 and 6 of this section looked at (2026-10-28, nothing built except the `EXP_AVCPROBE` probe).**  *Item 3, the floor:* a fresh budget of a heavy fight (plain autopilot demo, shift 2500, 18 windows, 17.5k vertices and 3200 draws a guest frame, 96% speed): frame 16.9 ms = GE thread 10.3 (`run_list` 9.9: fast-skinned draws 5.49 ms for 2491 draws, 2.2 us each at 5.6 vertices a draw; cache hits 1.28 ms for 694; the other ~2.9 ms is list interpretation and the remaining draws) + the rest 6.6 (guest thread 6.0 sampled, sound 0.3, frontend 0.34).  The guest profile (mean 5.7 ms, `GUESTPROF` resolved to functions with `nm` on the demo ELF) is as flat as P2 said: the largest single entries are `vfpu_lvq` 0.32 ms and `vfpu_svq` 0.15 (the out-of-line VFPU quad load and store helpers, together 8% of the guest thread), `f_0896B1F4` 0.25 (one 423-line function with 21 VFPU calls) and a cluster of about 1.0 ms at `f_08A163BC` to `f_08A1D990` (`f_08A18A64` is read: it writes the BASE and CALL words of each model into the game's display list and calls the native `ovr_08A189D8`; `f_08A1959C` and `f_08A16814` are 50-60 lines of loads, stores and calls, the rest of the cluster was not read); only about 3.8 of the 5.7 ms are in the 700 sampled entries.  Candidates, none built: the VFPU quad helpers expanded in place at the hot call sites (~0.2 ms, grows the instruction-cache footprint, which the notes above chose against), a native override of the list-building functions, `f_0896B1F4`.  Each is 0.1-0.3 ms against a per-pair noise of ~1 ms in Dolphin, so none can be shown without the console run (item 1).  *Item 6, correctness:* **B1 was the one with a concrete next step: the movie's stream is now known and the work is "get an H.264 decoder onto PowerPC" (section 9, B1).**  B2 and B3 stay "against a captured scene only" (the earlier measurements had `rtt=0` and `skip=0`; this run's demo log does not print those counters, so nothing new was learned).  B4, B7, B8 and B9 need either a comparison against PPSSPP the owner can supply, or the console.  B6's open question (why the game enters the display-buffer blit) needs a reproduction (1 run in 6-7 of 130-170 s) and was not attempted.  B10 (movie pacing) cannot be judged until the movie plays.

**Why speed is still short, in one paragraph.**  Dolphin, a light fight, 1.0x: ge ~12 ms + other ~7 ms = ~19 ms against
the 16.7 ms budget (85% speed); a heavy stage ~25 ms.  With the CPU at 1.5x cost (the stand-in for a real Broadway, P9)
a light fight is 55-62%.  The draw-nothing floor is ~14 ms at 1.0x (~20 ms at 1.5x): game logic 5.6, list handling
~5, sound, frontend.  Every drawing lever left is under ~1 ms, and the three biggest guesses of 2026-10-25 / 27
(P6 rigid skinning, culling, a redundancy cache) measured as a loss or nothing.  The next real gains are in the
**floor** and in knowing the real CPI factor (item 1), not in more draw cuts.

**Open items, in order:**

1. **P0, run the console demo on a Wii** (the only measurement that can change the plan).  `tekken6wii-console-demo.dol`
   (`make TARGET=tekken6wii-console-demo BUILD=build-console-demo "EXTRA_CFLAGS=-DWII_SDLOG -DTKDR_AUTOPILOT" -j16`;
   built before the P6 code, rebuild after any commit); the SD-card recipe is in P0.  Read `sd:/apps/tkdr/debug.log`
   (`DEMOFPS` every 300 frames, `HITCH`, `GUESTPROF`; `tools/perf/readsd.py` reads it off a Dolphin SD image).  Compare
   ge / other with the 1.5x stand-in (ge 17-19, other 9-12) to learn the real CPI factor, then re-plan.  The owner had no
   console to hand on 2026-10-27.
2. **Billboard fog report (the owner, 2026-10-26; not investigated).**  **2026-10-28: the owner believes the Ominous Mist fog is fixed** ("I believe the mist is fixed"; not checked by a run here: the changes that went in for it are the foot-wisp stopwatch fix `0e920e0`, the texture-matrix UV `30aad88` and the opt-in TEV fog `EXP_TEVFOG` in `f3a57ca`); treat this item as closed unless the owner reports it again, and read what follows as the history.  The fog sprites (the billboards, not the distance
   fog) flicker, sit in the wrong place and disappear; **stage specific, stage not named**: ask which stage first, and
   whether the message was meant for this project (it came in a burst of messages, one of which was retracted).  Starting
   points, in order: the draw cache (the true LRU of `8c5c7db` makes far more hits, so a draw whose output depends on state
   the cache key misses would now go stale: `EXP_HITCHK`, `EXP_VCWHY`), then the fast list loop's matrix runs (`EXP_ABMAT`,
   `EXP_ABBONE`, `EXP_MATCHK`), then the GX culling and depth state (alpha layers are the sensitive ones; billboards are
   camera-facing quads, so a wrong view matrix or a culling flip would misplace or delete them).  Bisect with the
   `EXP_*` switches before reading code.
   *Looked at on 2026-10-27, not reproduced and not found (the owner named no stage when asked):* a 12 minute run
   through the attract mode (about eight stages: neon alley, pool, Arctic, cherry blossoms, waterfall, cobbles, pagoda, a
   night hill) with `EXP_HITCHK` and `EXP_MATCHK` was clean (6.7M cached draws re-decoded, 0 bad; 53M model-view and chip
   slot checks, 0 stale; 20M matrix runs, 0 bad), so the draw cache and the fast list loop's matrix words are not it, at
   least on those stages.  `EXP_RECTCENSUS` found sprites (rectangles in the transform mode, all with blend, depth test
   and a texture, none with fog) on the cathedral, the night hill and a few other stages, 1-120 a frame, with depth values
   up to ~100,000 (past the GE's 65535, clamped on both the chip and the software path), **none behind the eye, none with
   face culling on, none with corners the wrong way round**: the culling theory (the chip culled rectangles by winding, the
   GE does not) has no case in the scenes seen.  The chip path no longer culls rectangles anyway (`gxr_state()` takes a
   `rect` flag, as `draw_rect()` of the software path has no culling): correct and unobserved.  The stage that looks most like
   it has fog layers is the ruins stage with the bone skeleton (grey mist bands, in the picture `rgab/sheetA` of 2026-10-27's
   scratch) and the grey moor with a lake and bare trees: neither came up with the census running.  The attract mode shows
   a different set of stages on every run, even of one DOL (the autopilot's stage, too, changes with the binary), so a stage
   cannot be summoned by timing: **ask the owner for the stage and what is wrong (a screenshot of the PSP or Dolphin
   would do), then run a long attract-mode run with `EXP_RECTCENSUS -DEXP_ABRECT` and screenshots every 5 s
   (`tools/perf/shots.ps1`, `sheet.ps1`) and look for that stage in it**.  Dolphin's `dolphin.log` in the user directory is
   appended to by every run: read from the last `CreateSysDirectoryPath` line.
   *Second look, 2026-10-27, from the logic only (the owner: "there's a stage where it has fog cards floating on the ground", then "just look into the logic": no Dolphin run after that).*  No stage name again.  A 15 minute attract run of the production DOL (141 shots, one every 6 s) showed seven stages: the pink Valentine hall, the meadow with ruins, the dark ruins with bones and a dragon, the golden treasure cave, the **moonlit forest with wolves** (grey mist bands over the ground in the fight, shot 672), the autumn temple, the cathedral.  The forest and the dark ruins are the candidates.  The owner then guessed "possibly the mystical forest stage? idk" (unsure; the game's own stage name was not checked against the forest above, but it is the one that fits).  On one rankings screen over the forest (shot 726) a black hard-edged rectangle lies across the ground; it is most likely a ranking row sliding in, not proven to be a fog card.  What the chip path does not do that a ground mist layer could need, from reading the code, in the order I would test them: (1) **texture mapping modes**: `GE_TEXMAPMODE` (0xC0) and the texture matrix (`g_ge.tgen`, loaded in `ge.c`) are never read by either renderer (`raster.c`, "texmapmode 0; others not yet"); a layer whose coordinates come from the matrix or an environment map takes the vertex's own (zero when the format has none): one texel, a flat card, and neither the draw cache key nor the hot path knows the mode.  (2) **transform-mode draws are drawn with what the chip lacks left out** (`gxr_classify()` is only consulted for through-mode layers): a non-simple stencil test, the colour test, blend equations 1, 3, 4, 5 (drawn as add), factors 8 and 9, a fixed destination colour other than black or white (drawn as ONE), partial write masks.  (3) **reverse fog** (`GE_FOG2` < 0): `apply_fog()` only keeps GX's start and end ordered, so the fog amount rises with distance where the GE's is full fog nearer than `FOG1`; the chip's fog also assumes near = 1 and far = max(100000, 2 end) whatever the game's projection.  (4) **depth**: the software reference compares 16-bit integers (`shade()`: `zi = (int)clamp(z)`, a tie passes for LEQUAL / GEQUAL) while the chip compares 24 bits, so a layer lying at or a hair under the ground, which passes on the PSP by a tie, z-fights or vanishes on the chip (flicker, wrong place, disappearing); no bias exists anywhere.  A constant offset toward the eye for blended, non-depth-writing draws of about 1.5/65535 of the depth range is the candidate: **built behind `-DEXP_ZBIAS`** (flags table; off by default, compiled, never run in Dolphin; the owner asked not to run it).  For the next stage run: `-DEXP_XFORMCENSUS` (with `-DEXP_RECTCENSUS`) prints which of these a stage's draws use.  Both probes are off by default and built once with `-DTKDR_FIXTIME` without errors.  **Run 2026-10-27 (demo build with `-DEXP_ZBIAS -DEXP_ABZBIAS -DEXP_XFORMCENSUS`, 20 min attract, shots every 4 s, the offset alternating every report):** the moonlit forest did **not** come up (ten other stages did: tiki/waterfall, dark ruins, autumn temple, hangar, Halloween garden, treasure cave, willow field, ruined meadow, pink hall, ice with aurora), so the owner's stage is still untested.  What it did show: (1) `XFCENSUS` over 130 windows: every 3D draw is native or approx on the chip; the skip reasons are **only** non-simple stencil (0-5k a window, the fighters' and floor shadow layers), never colour test, blend equation, factor, logic op or partial mask, so candidate 2 is mostly closed; (2) the only draws with a real texgen (`texmap=301`, texture matrix from the normal) are opaque, depth-writing 32x16 stencil-replace draws (the fighters' own ramp), never a blended card, so candidate 1 is not a fog card (it may still flatten a fighter's shading: unlooked-at); (3) the depth offset: in the ruined-meadow fight (flower and grass cards, on/off shots 4 s apart, `cmpH.png`) no difference could be seen, and no fog card was visible there either.  **Then the forest was reproduced on purpose (same day):** a menu-exploration with `-DAUTOPILOT_KEYS` found no stage select anywhere (Quick Battle/VS CPU picks the stage at random; Practice > Freestyle only shows a "STAGE WALL ON" popup and a training grid), but the **first attract demo after boot is repeatable if the input history is**: with a single Cross at 5 s (`-Keys '5000 Cross 2000'`, nothing else) three different builds all showed the moonlit forest (kangaroo Roger Jr. vs a white-suited fighter, wolves, moon) from about 35 to 65 s of wall time; with no Cross at all the same build showed the Buddha-statue cliff instead.  In that forest, 26 frames with the offset alternating every report (shots 40-44 ON, 45-49 off, 50-54 ON, 55-59 off, 60-64 ON): **no fog card, ground mist, flicker or vanishing layer was visible with the offset on or off, and no on/off difference**.  The blended, depth-testing, non-writing draws there are 128x128 and 128x64 CLUT4 alpha-tested cards (about 26 and 15 a frame: the grass and ferns) plus fighter shadows; nothing with texgen, a blend factor or colour the chip drops.  So either the owner's stage is another one, or the artifact needs a camera or moment the demo does not show, or it only appears on the real console (Dolphin's depth and fog may differ).  **Open: ask the owner for a screenshot or the stage's name.**  Not done: a pixel diff (the scene moves; the shots are not frame-aligned).  **Stage sweep and the stencil test (later the same day; the owner's guesses were the cemetery, the hidden retreat, All Hallow's Eve):** the stage of the first demo is NOT repeatable from the input history alone (the same key list gave the neon market in one run and a rooftop pool in the next two, and All Hallow's Eve in another: a lottery, perhaps on the wall clock), so ten Cross times (3000-10000 ms) were swept and each boot was told apart by its log (`stencil=1 f3` marks the two stages that use the NOTEQUAL stencil).  Stages seen: Valentine hall, Buddha-statue cliff, a neon Chinese market with an oni mask (King vs Lili), treasure cave, aurora ice field with penguins, dark ruins with bones, ruined meadow, temple courtyard, **All Hallow's Eve** (pumpkin garden), rooftop pool; no cemetery and no hidden retreat came up.  Two stages show something that fits the owner's words: (a) the neon market has hard-edged lighter green rectangles and vertical columns on the floor and over the fighters' legs, in both depth-offset windows; (b) All Hallow's Eve has small flat white ellipses lying on the ground, some with a faint cone.  Both are additive glow cards (blend source alpha, destination fixed white).  The shaft cards' textures (64x64 CLUT4, serials 228/229) have a hard-edged alpha profile by design (`00 00 32 FF..FF 32 00 00`) and the floor pools are 64x64 opaque-alpha animated frames (serials 10-30) shaped by the vertex alpha, so the hard edges are probably the game's own light shafts; the owner, shown the picture, also read the rectangle as a light shaft.  `-DEXP_ABSTENCIL` (A/B, every report) drops the transform-mode draws whose stencil test is NOTEQUAL, which the chip has no test for and otherwise draws without it: 32x16 opaque-alpha cards, standard blend, no depth write, double-sided (30-200 draws a frame on All Hallow's Eve, a few on the neon market), with `REPLACE 0` after drawing, so on the PSP each pixel is blended once and only over ground the game stamped with stencil 255 (opaque geometry with `f1 r255 ops0/0/2`; other geometry stamps 0).  Dropping them changed nothing visible on either stage (three on/off pairs each, by eye), so the missing stencil is not what makes the rectangles or the ellipses; whether it matters for mist density or for mist over the fighters is not shown.  Not built: a stencil emulation (the chip has no destination-alpha test; a copy of the EFB alpha as a mask, or destination alpha as a blend factor for the additive case, were thought through and are not needed yet).  New probes, off by default and compiled out (the default DOL is byte-identical: 11,957,920 bytes, SHA-256 BA5BDA88...): `-DEXP_ABSTENCIL`, `-DEXP_STENCILSEQ` and `-DEXP_TEXALPHA` (flag table).  **Still open: which stage, and what is wrong in it (the neon market's green rectangles? All Hallow's Eve's white ellipses?), ideally with a picture of how it should look: there is no reference render here (no PPSSPP).**  **Closed by the owner (same day, shown the neon market and All Hallow's Eve shots): "I think both look okay rn".  Neither stage is the bug stage; the green shafts and the white ellipses are as intended.  The bug stage is still unfound: not the moonlit forest, the neon market or All Hallow's Eve; the cemetery and the hidden retreat were guessed and never came up in the sweep.**  **Then the owner (2026-10-27, after that): "I think might have been fixed anyway. continue for performance optimizations".  Treated as closed; reopen only if the owner reports it again, with a stage name or a picture.**
   **Reopened 2026-10-27 (same day, on a build of `b85ac30`): the owner on the stage "Ominous Mist": "I see the fog and now it's not sliding but resetting every few secs"** (the owner's hint: "timing bug perhaps?"; no Dolphin runs wanted for visual confirmation, read the code).  Read from the code, no run: (a) **timing is not it**: `hle_time_us()`, the elastic vblank grid and `sceKernelSysClock2USecWide` / `clock_split` were checked against PPSSPP and are right, the game's UV scroll is driven by its own frame counter (`f_08A1771C`, +1 a frame) and not by a clock, and the offsets, scale, hot state, draw cache and texture cache all carry the register values through (a changed `GE_TEXOFFSETU/V` bumps `g_ge_vertex_gen`; the cache holds raw UVs and applies scale/offset at submit); (b) **the gap that fits "not sliding"**: the game's material UV animation (`f_08A198D4`, record types none / linear scroll with a 20-bit frame×speed wrap, table-driven, and a **texture matrix**: `TEXMAPMODE` 0xC0000101, also 0xC0000301 and 0xC0000001 elsewhere) writes the scroll into the texture matrix (`f_08A19AFC`: `TGENMATRIX` words from VFPU registers, slots 2 and 5 zero) in the matrix types, and the renderers ignored `TEXMAPMODE` and `g_ge.tgen` altogether (raster.c "texmapmode 0; others not yet"), so a matrix-scrolled layer did not slide.  **Built 2026-10-27 (uncommitted): `vtx_state_refresh()` folds the texture matrix into the existing scale/offset when `TEXMAPMODE & 0x303 == 0x101` (UVs through the texture matrix) and the matrix only scales and moves (slots 1, 2, 3, 5 zero, slot 11 non-zero): `su*m0/m11`, `sv*m4/m11`, offsets `su*m9/m11`, `sv*m10/m11` (PPSSPP: scale applied after the matrix, offset replaced by it), so it costs nothing per vertex; `GE_TEXMAPMODE` joined `vertex_state_op()` (a changed value bumps `g_ge_vertex_gen`; the pixel gen moved with it anyway); the general decoder reads `vtx_state()` instead of the raw registers.**  Not done: rotating/sheared matrices, source 0x001 (position) / 0x201 / 0x301 (normals, the fighters' ramp), projective ones.  **Unproven**: that Ominous Mist's fog uses a matrix record at all (the stage cannot be chosen; the "resetting every few seconds" half is not explained by this: it may be the game's own fade or the type-1 20-bit sawtooth); a run showing the stage with the old and new DOL is the only check, and `EXP_XFORMCENSUS` (`texmap=101` in `XFSTATE`) says whether the stage uses it.
3. **Shrink the floor** (P2, P4: ~8-10 ms at 1.0x that no draw change touches): the guest thread is flat (19 functions are
   half of it, `GUESTPROF`), the GE list handling is ~5 ms (38k commands, 3.3k CALL/RET pairs a frame), sound ~0.9 ms (after
   the SAS rewrite), the frontend ~1 ms.  The exact candidates left are each small (0.1-0.4 ms): the 6.9% of skinned draws that
   repeat within a frame (P5), the ambient-alpha misses (P11: **built** 2026-10-28; 1-1.8 ms on the one stage that had them), the pixel-state rebuild of the texture
   lookup (P1, 0.17 ms), the specular lit draws (P7, 0.2-0.3 ms).  One drawing case left that is larger where it occurs:
   the skinned parts with a vertex colour and **no normals** (vertex type 0x431D, lighting on): **built** (they are on the
   whole-draw path, mode 3 / 4 of section 3, since `b85ac30`; this item used to say "not built").
4. **P9 on hardware** (needs a console): `-Os` vs `-O2` for the 636 KB hot code, `dcbt`, 128-byte cache sets.
5. **P10, the owner's call**: if item 1 shows the budget cannot be reached, the options are the elastic speed (66-85% in
   Dolphin), fewer parts drawn at distance, or frame skipping (rejected so far).  Do not choose without asking.
6. **Correctness and features (section 9):** B1 the intro movie (built; only its console speed is unknown, see B1), B2 / B3 GE effects and render-to-texture (build
   only against a captured scene), B4 native paletted textures, B7 the visual QA corpus against PPSSPP, B8 audio on
   hardware, B9 saves, input and network modes on hardware, B6's open question (why the game enters the display-buffer blit).
7. **Housekeeping (section 10):** the stale README and PORTING, the checkers that no CI runs, the `build-*` trees.

**Recipes behind all of the above (also section 6).**  Build a measurable fight DOL with `tools/perf/build_shifts.ps1 -Tag X
-Shifts 2500 -Extra '-DAUTOPILOT_MASH ...'` (a change of `-D` flags alone does not rebuild the objects: delete the `.o` of the
sources that use the flag under `build-audit-demo\obj`), run it with `tools/perf/run_dol.ps1 -Dol .. -Seconds 170 -Out log`, pair
an in-run A/B with `python tools/perf/abpairs.py <log> ABxxx "<text of the new state>"` (two builds' logs are not an A/B: each
plays another fight and often another stage), take screenshots with `tools/perf/shots.ps1` (the files' times map a shot to
its `ABxxx` window through the `mm:ss:ms` stamps of the Dolphin log).  Never build while your Dolphin runs; use this project's
own Dolphin user directory (`C:\tkdr_raw_user` on the owner's machine) and leave other Dolphin instances alone.  Section 12
holds the working agreements (no deploy, commit and push only on request, probe-free production DOL, no frame skipping).
