#!/usr/bin/env python3
"""Prepare Tekken: Dark Resurrection for the Wii build.

    python -I tools/setup_assets.py --iso /path/to/TekkenDR.iso \
                                   --eboot /path/to/ULUS10139_EBOOT.BIN

The ISO is never copied into git. A CSO is converted to a plain ISO in the
generated directory first. The script then runs tools/prepare_tkdr.py, which
verifies the supported ULUS10139 v1.00 dump and writes gen/ (image.bin and
generated C). `gen/` and any converted ISO are git-ignored.

Use --verify-only to check the ISO fingerprint without an EBOOT.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str]) -> None:
    print("[setup] " + " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    ap = argparse.ArgumentParser(
        description="Prepare Tekken: Dark Resurrection for the Wii build.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python -I tools/setup_assets.py --iso game.iso --eboot EBOOT.BIN\n"
            "  python -I tools/setup_assets.py --verify-only --iso game.iso\n"
        ),
    )
    ap.add_argument("--iso", required=True, help="your own ULUS10139 ISO or CSO")
    ap.add_argument("--eboot", help="decrypted ULUS10139 EBOOT.BIN")
    ap.add_argument("--out", default=str(ROOT / "gen"), help="generated-code directory")
    ap.add_argument("--verify-only", action="store_true",
                    help="check the ISO fingerprint and stop")
    args = ap.parse_args()

    iso = Path(args.iso).expanduser().resolve()
    if not iso.is_file():
        sys.exit(f"[setup] error: ISO/CSO not found: {iso}")

    eboot = Path(args.eboot).expanduser().resolve() if args.eboot else None
    if not args.verify_only and eboot is None:
        candidates = [
            iso.parent / "ULUS10139_EBOOT.BIN",
            iso.parent / "EBOOT.BIN",
            ROOT / "test" / "ULUS10139_EBOOT.BIN",
            ROOT / "ULUS10139_EBOOT.BIN",
            ROOT / "EBOOT.BIN",
        ]
        candidates += sorted(iso.parent.glob("ULUS10139*EBOOT*.BIN"))
        for candidate in candidates:
            if candidate.is_file():
                eboot = candidate.resolve()
                print(f"[setup] using decrypted EBOOT: {eboot}")
                break
    if not args.verify_only and eboot is None:
        sys.exit(
            "[setup] error: no decrypted ULUS10139 EBOOT found.\n"
            "Dump it with PPSSPP (Settings -> Tools -> Developer tools ->\n"
            "'Dump decrypted EBOOT.BIN on game boot'), then pass it with --eboot."
        )
    if eboot is not None and not eboot.is_file():
        sys.exit(f"[setup] error: EBOOT not found: {eboot}")
    out = Path(args.out).expanduser().resolve()

    if iso.suffix.lower() == ".cso":
        out.mkdir(parents=True, exist_ok=True)
        converted = out / "tkdr.iso"
        print(f"[setup] converting {iso.name} to {converted} (this can take a while)")
        run([sys.executable, "-I", str(ROOT / "tools" / "cso2iso.py"), str(iso), str(converted)])
        iso = converted

    cmd = [
        sys.executable, "-I", str(ROOT / "tools" / "prepare_tkdr.py"),
        "--iso", str(iso), "--out", str(out),
    ]
    if eboot is not None:
        cmd += ["--eboot", str(eboot)]
    if args.verify_only:
        cmd += ["--verify-only"]
        run(cmd)
        return

    run(cmd)
    print("\n[setup] assets ready")
    print(f"  generated: {out}")
    print("  build:     make clean && make")
    print("  package:   make dist-tkdr")
    print("  install:   copy dist-tkdr/apps/tkdr to sd:/apps/tkdr")
    print("\nThe generated image and C are game-derived: keep them out of git.")


if __name__ == "__main__":
    main()
