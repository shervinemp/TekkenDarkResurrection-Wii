# Third-party code and licences

This repository contains no Tekken: Dark Resurrection game code, executable,
assets, ISO/CSO, or generated translation. Game-derived files
(`gen/image.bin`, generated `funcs_*.c`, `func_table.c`, `image_base.h`, and any
trimmed ISO) are produced locally from the user's own dump and must not be
committed.

The original port code is **MIT** (`LICENSE`, `LICENSES/MIT.txt`). The combined
Wii binary is **GPL-2.0-or-later** because it links `vendor/libntfs`; see
"Licence notes" below. Human-readable credits are in `CREDITS.md`.

| Component | Location | Licence |
| --- | --- | --- |
| tkdr-recomp (recompiler, PSP runtime) | `recomp/`, `runtime/` | MIT — `LICENSE-tkdr-recomp.txt` |
| libntfs-wii / NTFS-3G port (extrems / Tuxera) | `vendor/libntfs/` | GPL-2.0-or-later — `vendor/libntfs/LICENSE`, `LICENSES/GPL-2.0-or-later.txt` |
| ATRAC3/ATRAC3+ standalone decoder (FFmpeg via PPSSPP) | `third_party/at3_standalone/` | LGPL-2.1-or-later — `third_party/at3_standalone/LICENSE.md`, `LICENSES/LGPL-2.1-or-later.txt` |
| FFmpeg 7.1.1, H.264 decoder only (intro movie) | built by `tools/build_ffmpeg.sh` into `third_party/ffmpeg-ppc/` (not committed) | LGPL-2.1-or-later (configured `--disable-gpl --disable-nonfree`; the script checks `config.h`) |
| PPSSPP | decrypted-EBOOT dump instructions and the ATRAC decoder extraction | GPL-2.0-or-later upstream; not vendored |
| stb_image / stb_image_write | `third_party/stb/` | Public domain / MIT (see headers) |
| devkitPro libogc, libfat, AESND, WiiUse/BTE | external toolchain | libogc's permissive licence; other packages as noted by devkitPro |
| Dolphin | testing/emulation reference only | GPL-2.0-or-later upstream; not vendored |
| reVC-Wii storage patterns (not code copied wholesale) | reference only | GPL-3.0 upstream |

## Licence notes

- **MIT:** the original port code. See `LICENSE` and `LICENSES/MIT.txt`.
- **GPL-2.0-or-later:** `vendor/libntfs` is GPL-2+. If you redistribute a Wii
  DOL linked with it, the GPL applies to the combined binary. The original
  port code remains MIT, but the binary as a whole is subject to the GPL
  because of libntfs. Keep the licence texts with any redistribution.
- **LGPL-2.1-or-later:** the ATRAC3/ATRAC3+ decoders and FFmpeg's H.264
  decoder. If you distribute a DOL that includes them, keep the LGPL text,
  provide the corresponding source and build recipe, and allow relinking as
  the LGPL requires. `make EXP_NOFFMPEG=1` builds without the FFmpeg H.264
  decoder.
- **No game data:** the game, executable, ISO/CSO, generated image and
  recompiled code are not covered by any licence here.

## A note on FFmpeg and patents

The H.264 decoder is built from the unmodified official FFmpeg 7.1.1 release
tarball (`https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`, SHA-256
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`) by
`tools/build_ffmpeg.sh`, with only the H.264 decoder and parser enabled, no
GPL or non-free parts, and no assembly. It is linked statically into the DOL,
so a DOL that includes it carries the LGPL-2.1+ obligations. FFmpeg's licence
does not grant patent rights; H.264 is covered by patents in some places, a
matter for whoever distributes a build (this is not legal advice).

## Game ownership and trademarks

*Tekken: Dark Resurrection*, and all related characters, assets and trademarks,
belong to Bandai Namco Entertainment Inc. This project is unofficial and is
not affiliated with or endorsed by Bandai Namco or Sony. Users must supply
their own legally obtained game files. Do not distribute game data or a built
binary; see `RELEASE_POLICY.md`.

## Reporting a licensing problem

If you believe a file in this repository infringes your rights, please open an
issue or contact the maintainer. The file will be removed or the licence
corrected promptly.
