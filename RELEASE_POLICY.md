# Release policy

This repository is **source-only**. There are no binary releases, and there
should not be any.

Do **not** publish or attach any of these to a release, GitHub Actions
artifact, package, website, or mirror:

- `EBOOT.BIN`, an ISO/CSO, or any extracted game files
- `gen/image.bin`, generated `funcs_*.c`, `func_table.c`, or `image_base.h`
- `build/` or any built `.dol`/`.elf`/`.map`
- `dist/`, `dist-tkdr/`, `dist-tkdr-sd-full/`, or a packaged SD image
- memory-card/save images, screenshots of game data, or other game assets

## Why

A built DOL contains recompiled game code derived from your own Tekken
executable. Distributing it raises copyright questions this project avoids,
and the GPL/LGPL obligations for libntfs and FFmpeg would come with it. The
source in this repository is the source; the game is yours to supply.

## If you build it

Build and run it locally from your own legally owned copy. Do not upload the
result. Share source changes and instructions, not the binary or game data.

## Forks and mirrors

Forks and mirrors should follow the same rule. The public mirror is a
single-commit source snapshot. Use `tools/check_repo.py` before publishing; CI
runs the same check.

## Takedown requests

The public mirror is separate from the private development repository. If a
copyright claim arrives, evaluate it with counsel before responding. A
counter-notice is an option for a mistaken claim, but it is not a substitute
for legal advice.

## Non-commercial

This is an unofficial, non-commercial fan project. Do not sell it, put it
behind a paywall, add monetised ads, or present it as an official Bandai
Namco/Sony product. Do not use official logos or cover art.

This policy is not legal advice.
