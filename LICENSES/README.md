# Licence texts

- `MIT.txt` — the project's original port code (`LICENSE`).
- `GPL-2.0-or-later.txt` — `vendor/libntfs`, the read-only NTFS-3G port. The
  combined Wii binary is GPL-2.0-or-later because it links this library.
- `LGPL-2.1-or-later.txt` — the ATRAC3/ATRAC3+ decoders in
  `third_party/at3_standalone` and FFmpeg's H.264 decoder when built by
  `tools/build_ffmpeg.sh`.

Other third-party notices are kept next to their code:

- `LICENSE-tkdr-recomp.txt` — tkdr-recomp (MIT)
- `third_party/at3_standalone/LICENSE.md` — FFmpeg/PPSSPP ATRAC decoders
- `third_party/stb/` — stb headers (public domain / MIT)
- `vendor/libntfs/LICENSE` — NTFS-3G/libntfs (GPL-2.0-or-later)

See `THIRD_PARTY.md` for the component map and `CREDITS.md` for the
human-readable credits.
