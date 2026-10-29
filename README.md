# Tekken: Dark Resurrection on Wii

A work-in-progress Wii port of *Tekken: Dark Resurrection* (PSP, USA
`ULUS10139`). The PSP executable is statically recompiled to C with
[tkdr-recomp](https://github.com/gasparegas/tkdr-recomp); the Wii side is a new
backend built on libogc.

This is a source-only fan project. No game data, ISO, EBOOT, generated code or
binary is included — you bring your own copy of the game. It is unofficial and
not affiliated with Bandai Namco or Sony.

## Where it stands

- The TKDR generator produces 13,809 functions and 280 imports, and a 20.6 MiB
  relocated image.
- The DOL builds, boots in Dolphin, and reaches the first guest dispatch
  (`0x089F34A8`).
- A full ISO playthrough on real hardware is still pending. The GX renderer,
  audio, input, saves and FAT/NTFS storage are in place but still need
  hardware testing.
- [docs/STATUS.md](docs/STATUS.md) has the test evidence, and
  [docs/PORTING.md](docs/PORTING.md) explains the architecture and open work.

## What you need

- devkitPro with devkitPPC, libogc, libfat, AESND (`-laesnd`), WiiUse/BTE.
  The project is built with devkitPPC GCC 15.2.
- Python 3.10 or newer.
- A legally owned `ULUS10139` ISO (or CSO).
- A decrypted `EBOOT.BIN` from that ISO. PPSSPP can dump it through
  *Settings → Tools → Developer tools → Dump decrypted EBOOT.BIN on game boot*.

## Build and install

Clone the public mirror:

```sh
git clone https://github.com/shervinemp/TekkenDarkResurrection-Wii.git
cd TekkenDarkResurrection-Wii
```

Prepare the game:

```sh
python -I tools/setup_assets.py --iso /path/to/TekkenDR.iso \
                               --eboot /path/to/ULUS10139_EBOOT.BIN
```

The script checks that the ISO is the supported `ULUS10139` v1.00 dump,
converts a CSO to a plain ISO if needed, and generates the C and `image.bin`
under `gen/`. The ISO is not copied into the repository or embedded in the
DOL; the Wii reads it from SD or USB at runtime. If you leave out `--eboot`,
the script looks for a decrypted `ULUS10139_EBOOT.BIN` next to the ISO and in a
few common places.

Build and package:

```sh
make clean
make
make dist-tkdr
```

`make clean` matters if you built the launcher-only DOL first. `gen/` is not
deleted by `make clean`; remove it yourself if you want a fresh generation.

The packaged app is in `dist-tkdr/apps/tkdr/`. Copy it to the SD card:

```text
sd:/apps/tkdr/boot.dol
sd:/apps/tkdr/image.bin
sd:/apps/tkdr/tekken_dr.iso   (or tekken_dr.bin)
sd:/apps/tkdr/ms0/            (created for saves)
```

or use `usb:/apps/tkdr/`. The app also accepts `Tekken DR.iso`,
`ULUS10139.iso`, `a-tekken.iso` and `game.iso`. The `.bin` spelling is useful
when a file manager tries to mount every `.iso`; the contents are the same
ISO9660 image.

`image.bin` is not the game disc. It is the relocated executable image
generated from the EBOOT, and you need both files.

## Build notes

- To build only the launcher, run `make` without generating `gen/` first. It
  uses `stub/` and explains where the game files belong.
- To check the ISO without generating code:
  `python -I tools/prepare_tkdr.py --verify-only --iso ...`.
- If `make` cannot find Python in the devkitPro shell, pass it explicitly:
  `make PYTHON=/c/Path/To/Python313/python.exe`.
- The generated code and `image.bin` are game-derived; keep them out of git.

## Controls

| PSP | GameCube | Wii Remote | Classic Controller |
| --- | --- | --- | --- |
| D-pad | D-pad / stick | D-pad | D-pad / left stick |
| Cross | B | 2 | B |
| Circle | A | A | A |
| Square | Y | 1 | Y |
| Triangle | X | B | X |
| L / R | L / R | Nunchuk Z / C | ZL+L / ZR+R |
| Start / Select | Start / Z | Plus / Minus | Plus / Minus |
| Analog | main stick | Nunchuk stick | left stick |

`HOME` on the Wii Remote or Classic Controller, or the console power button,
exits back to the loader.

## Storage and saves

- SD is mounted as `sd:` (FAT). Saves go to `sd:/apps/tkdr/ms0`.
- USB is mounted only when the SD card does not have the game. libfat is tried
  first; if that fails, the read-only NTFS-3G port is used. NTFS is for game
  data; saves go to SD or a writable FAT stick.
- The ISO is read through the tkdr ISO9660 layer in `runtime/hle/io.c`, with a
  64 KiB read-ahead cache and direct FAT sector reads.
- `debug.log` is written next to the DOL. Boot messages also appear in
  Dolphin's OSReport log.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `no image.bin + ISO` | Put `image.bin` and `tekken_dr.iso`/`.bin` in `sd:/apps/tkdr/` and check the names. |
| `not the supported ULUS10139 v1.00 dump` | The ISO is the wrong region/version or damaged. Use the exact dump listed above. |
| `EBOOT is still encrypted (~PSP)` | Dump the decrypted EBOOT with PPSSPP and pass that file. |
| CSO is not read at runtime | Convert it with `python tools/cso2iso.py in.cso out.iso`, or let `setup_assets.py` do it. |
| `python: No such file or directory` | Run `make PYTHON=/path/to/python.exe`. |
| Dolphin rejects the DOL | Rebuild with the current Makefile; `tools/pad_dol.py --data` aligns the DOL sections. |

## Repository layout

```text
platform/wii/        libogc platform: GX scanout, input, AESND, storage, LWP
runtime/             tkdr PSP runtime (HLE, GE software raster, VFPU, audio)
runtime/wii/         Wii boot glue and frame loop
recomp/              tkdr recompiler
profiles/tkdr/       ULUS10139 v1.00 ISO fingerprint
vendor/libntfs/      read-only NTFS-3G port (GPL-2+)
third_party/         ATRAC3+ decoders (LGPL-2.1+) and stb (public domain)
stub/                no-game launcher function table
tools/               asset setup, generator, packaging and repo checks
wii/apps/tkdr/       Homebrew meta.xml and README for the app folder
tests/               synthetic fixture and generator tests
docs/                porting notes and status
```

## Credits and licences

The port is built on [tkdr-recomp](https://github.com/gasparegas/tkdr-recomp)
(MIT). It also uses libntfs/NTFS-3G (GPL-2.0-or-later), FFmpeg's ATRAC and
H.264 decoders (LGPL-2.1-or-later), stb (public domain/MIT), and the devkitPro
Wii libraries. [CREDITS.md](CREDITS.md) has the full acknowledgements and
[THIRD_PARTY.md](THIRD_PARTY.md) has the licence details.

The original port code is MIT. A built DOL is GPL-2.0-or-later because it
links libntfs; FFmpeg components add LGPL-2.1-or-later obligations. See
[LICENSE](LICENSE) and `LICENSES/`.

## Releases and contributing

This is a source-only project: no ISO, EBOOT, `gen/`, `dist/` or binary
releases. See [RELEASE_POLICY.md](RELEASE_POLICY.md).

Contributions are welcome under the project's licences; see
[CONTRIBUTING.md](CONTRIBUTING.md). Run `python -I tools/check_repo.py` before
pushing.

*Tekken: Dark Resurrection* is a trademark of Bandai Namco Entertainment Inc.
This is an unofficial, non-commercial fan project, not affiliated with or
endorsed by Bandai Namco or Sony.
