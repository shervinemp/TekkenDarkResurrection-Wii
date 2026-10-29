# Credits and thanks

This port exists because of a lot of prior work. The list below says what each
project/person contributed and where to find them. Licence details are in
`THIRD_PARTY.md`; this file is the human-readable “thank you”.

## The original game

- **Tekken: Dark Resurrection** — developed and published by Bandai Namco
  Entertainment. PSP, USA `ULUS10139` v1.00.
- No game code, executable, ISO/CSO, image, audio or other game asset is
  included in this repository. The game and its trademarks belong to Bandai
  Namco and its licensors.

## Recompilation and runtime

- **tkdr-recomp** by **gasparegas** and contributors —
  <https://github.com/gasparegas/tkdr-recomp>
  - The static MIPS/Allegrex recompiler and the PSP runtime: threads, HLE,
    GE software rasteriser, audio, save data, ISO9660 and the sectioned PRX
    path used for Dark Resurrection.
  - MIT licence; see `LICENSE-tkdr-recomp.txt`.
- **Dolphin** — <https://github.com/dolphin-emu/dolphin>
  - Used for testing and as an emulation reference. Not vendored or linked
    into the Wii DOL.

## Libraries and audio/video

- **libntfs / NTFS-3G** — <https://github.com/extremscorner/libntfs> and
  <https://github.com/tuxera/ntfs-3g>
  - The read-only NTFS port in `vendor/libntfs`, by **extrems** and the
    NTFS-3G contributors. GPL-2.0-or-later; see `vendor/libntfs/LICENSE` and
    `LICENSES/GPL-2.0-or-later.txt`.
- **FFmpeg** — <https://ffmpeg.org>
  - The ATRAC3/ATRAC3+ decoders in `third_party/at3_standalone` and the
    optional H.264 decoder built by `tools/build_ffmpeg.sh`.
    LGPL-2.1-or-later; see `third_party/at3_standalone/LICENSE.md`,
    `LICENSES/LGPL-2.1-or-later.txt`, and the FFmpeg source tarball/recipe.
- **PPSSPP** — <https://www.ppsspp.org>
  - The source of the standalone ATRAC decoder extraction in
    `third_party/at3_standalone`, and the recommended way to dump a decrypted
    EBOOT for your own game. PPSSPP is GPL-2.0-or-later; its code is not
    vendored here.
- **stb** — <https://github.com/nothings/stb>
  - `stb_image` / `stb_image_write` headers. Public domain / MIT; see the
    headers.
- **devkitPro / libogc / libfat / AESND / WiiUse / BTE** —
  <https://devkitpro.org>, <https://github.com/devkitPro/libogc>
  - The Wii toolchain and runtime libraries. Their licences are their own;
    see the devkitPro packages. libogc is not vendored here.
- **reVC-Wii** — the author's earlier Wii port, used as a reference for the
  SD/USB/NTFS storage and mounting patterns. GPL-3.0 upstream; its code is not
  copied wholesale.

## This project

- **Shervin Naseri (`sherv`)** — the Wii backend, platform glue, GX renderer,
  build/packaging and documentation.
- **Contributors** — everyone who has supplied code, measurements, tests, logs
  or feedback. See the git history for the full list.

## A note on attribution

If you believe a source is missing from this file, or that a piece of code is
closer to another project than the notes say, please open an issue. The goal is
to give credit accurately and to fix any licensing mistake promptly.

The original port code is MIT; the combined Wii binary is GPL-2.0-or-later
because it links libntfs. See `LICENSE`, `THIRD_PARTY.md` and `LICENSES/`.
