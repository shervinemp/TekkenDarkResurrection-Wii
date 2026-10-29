#!/usr/bin/env python3
"""Prepare Tekken: Dark Resurrection (ULUS10139 v1.00) for the Wii build.

This uses tkdr-recomp's complete PRX loader/analyzer/emitter: full
section/relocation information and the known-good function set. The user
supplies the ISO and a *decrypted* EBOOT.BIN.

The ISO is not copied or embedded: the Wii reads it from SD or a FAT/NTFS USB
stick at runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from recomp import analyze, elf, emit  # noqa: E402

PROFILE = ROOT / "profiles/tkdr/profile.json"


def hash_iso(path: Path):
    h = hashlib.sha1()
    crc = 0
    size = 0
    with path.open("rb") as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
            crc = zlib.crc32(b, crc)
            size += len(b)
    return size, h.hexdigest(), f"{crc & 0xFFFFFFFF:08x}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", required=True, help="Tekken: Dark Resurrection ISO")
    ap.add_argument("--eboot", help="decrypted ULUS10139 EBOOT.BIN")
    ap.add_argument("--profile", default=str(PROFILE))
    ap.add_argument("--out", default=str(ROOT / "gen"))
    ap.add_argument("--verify-only", action="store_true",
                    help="check the ISO fingerprint and stop")
    args = ap.parse_args()

    profile = json.loads(Path(args.profile).read_text(encoding="utf-8"))
    iso = Path(args.iso)
    if not iso.is_file():
        print(f"error: ISO not found: {iso}", file=sys.stderr)
        return 2

    size, sha1, crc32 = hash_iso(iso)
    want = profile["iso"]
    print(f"ISO:    {iso}")
    print(f"size:   {size}")
    print(f"sha1:   {sha1}")
    print(f"crc32:  {crc32}")
    if size != want["size"] or sha1 != want["sha1"] or crc32 != want["crc32"]:
        print("error: not the supported ULUS10139 v1.00 dump", file=sys.stderr)
        print(f"  expected size {want['size']}, sha1 {want['sha1']}, crc32 {want['crc32']}",
              file=sys.stderr)
        return 2
    print(f"game:   {profile['title']} ({profile['game_id']}) v{profile['disc_version']}")
    if args.verify_only:
        print("OK: ISO matches tkdr-recomp's supported dump.")
        return 0

    if not args.eboot:
        print("error: --eboot is required unless --verify-only is used", file=sys.stderr)
        return 2
    eboot = Path(args.eboot)
    if not eboot.is_file():
        print(f"error: EBOOT not found: {eboot}", file=sys.stderr)
        return 2
    head = eboot.read_bytes()[:16]
    if head[:4] == b"~PSP":
        print("error: the EBOOT is still encrypted (~PSP).", file=sys.stderr)
        print("Dump the decrypted EBOOT with PPSSPP:", file=sys.stderr)
        print("  Settings -> Tools -> Developer tools ->", file=sys.stderr)
        print("  'Dump decrypted EBOOT.BIN on game boot'", file=sys.stderr)
        print("It appears under PSP/SYSTEM/DUMP/ULUS10139_EBOOT.BIN.", file=sys.stderr)
        return 3
    if head[:4] != b"\x7fELF":
        print("error: not an ELF EBOOT (expected a decrypted ELF32 MIPS image)",
              file=sys.stderr)
        return 3

    try:
        image = elf.load(str(eboot))
        if ".text" not in image.sections or ".lib.stub" not in image.sections:
            raise ValueError("missing .text/.lib.stub; is this the decrypted tkpsp PRX?")
        if image.entry != int(profile["executable"]["entry"], 16):
            print(f"warning: entry {image.entry:08X} differs from profile "
                  f"{profile['executable']['entry']}", file=sys.stderr)
    except Exception as exc:
        print(f"error: cannot load the EBOOT as a tkpsp PRX: {exc}", file=sys.stderr)
        return 3

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("funcs_*.c"):
        old.unlink()

    print("translating (this can take a few minutes)...")
    try:
        p, stats = emit.generate(str(eboot), str(out))
    except Exception as exc:
        print(f"error: generation failed: {exc}", file=sys.stderr)
        return 4

    manifest = {
        "game_id": profile["game_id"],
        "title": profile["title"],
        "iso_sha1": sha1,
        "iso_crc32": crc32,
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "functions": len(p.functions),
        "imports": len(p.stubs),
        "image_bytes": len(p.img.mem),
        "base": f"0x{p.img.base:08X}",
        "entry": f"0x{p.img.entry:08X}",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                      encoding="utf-8")

    print(f"\nTKDR: {len(p.functions)} functions, {len(p.stubs)} imports, "
          f"{len(p.img.mem)} byte image")
    print("\nNext:")
    print("  make clean && make")
    print("  mkdir -p sd:/apps/tkdr")
    print("  cp tekken6wii.dol sd:/apps/tkdr/boot.dol")
    print(f"  cp {out / 'image.bin'} sd:/apps/tkdr/image.bin")
    print(f"  cp {iso} sd:/apps/tkdr/tekken_dr.iso")
    print("\nThe generated C and image.bin are game-derived: keep them out of git.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
