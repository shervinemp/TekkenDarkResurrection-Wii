# Contributing

Thanks for helping. This is an experimental recompilation port, so small,
measured changes are more useful than large rewrites.

## Licence of contributions

By submitting a change, you agree that it may be distributed under the
project's licence:

- Original port code: **MIT** (see `LICENSE`).
- Third-party components keep their own licences (see `THIRD_PARTY.md` and
  `LICENSES/`).
- The combined Wii binary is **GPL-2.0-or-later** because it links
  `vendor/libntfs` (GPL-2.0-or-later). Do not add code that would be
  incompatible with that combination.

If you adapt third-party code, say so in the commit message and add the
attribution to `THIRD_PARTY.md` and `CREDITS.md`.

## Never commit game data

Do not add any of these to git:

- `EBOOT.BIN`, ISO/CSO/BIN/CUE images, or extracted game files
- `gen/` (`image.bin`, generated `funcs_*.c`, `func_table.c`, `image_base.h`)
- `build/` or any built `.dol`/`.elf`/`.map`
- `dist/`, `dist-tkdr/`, `dist-tkdr-sd-full/`, or packaged SD images
- memory-card/save images or other game assets

Run this before every commit or push:

```sh
python -I tools/check_repo.py
```

The check scans the git index, so it catches staged files even if they are
already deleted from disk. CI runs the same check on every push.

This project is source-only. See [`RELEASE_POLICY.md`](RELEASE_POLICY.md).

## Building and testing

Follow `README.md`:

```sh
python -I tools/setup_assets.py --iso /path/to/TekkenDR.iso \
                               --eboot /path/to/ULUS10139_EBOOT.BIN
make clean && make
make dist-tkdr
```

If your change affects generated code, say which `config/` or `recomp/`
change produced it. Do not commit the generated output.

## Maintainer publishing

The private development repository is **TekkenDarkResurrection-Wii-private**;
the public source-only mirror is **TekkenDarkResurrection-Wii**. The historical
build target name `tekken6wii` is kept for compatibility.

Local remotes:

- `origin` — private repository (fetch and push)
- `private` — private repository only
- `public` — public repository only

The public mirror is a **single-commit snapshot**, not the full development
history.

```sh
git push origin main                      # private only
python -I tools/publish_public.py         # private first, then a fresh public snapshot
python -I tools/publish_public.py --skip-private   # re-publish public after a takedown
```

There is also a local alias:

```sh
git config alias.publish '!python -I tools/publish_public.py'
git publish
```

Before publishing, run `python -I tools/check_repo.py`. Never publish a tree
containing a game image, `gen/`, `dist-*`, or a built binary.
