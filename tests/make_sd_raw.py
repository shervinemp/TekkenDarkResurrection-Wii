#!/usr/bin/env python3
"""Build a FAT32 Wii SD image for Dolphin (test-only).

Dolphin's folder-sync cannot handle a 1.75 GB ISO on this machine, so this
creates the raw image directly with pyfatfs and streams the files in.
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

ROOT = Path(__file__).resolve().parent.parent
IMAGE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"E:\tkdr_raw_user\Load\WiiSD.raw")
SIZE = 1_850_000_000

FILES = [
    (ROOT / "tekken6wii.dol", "apps/tkdr/boot.dol"),
    (ROOT / "gen/image.bin", "apps/tkdr/image.bin"),
    (Path(r"C:\Users\sherv\Downloads\a-tekken.iso"), "apps/tkdr/tekken_dr.iso"),
]


def main() -> int:
    IMAGE.parent.mkdir(parents=True, exist_ok=True)
    if IMAGE.exists():
        IMAGE.unlink()
    with open(IMAGE, "wb") as f:
        f.truncate(SIZE)
    pf = PyFat()
    pf.mkfs(str(IMAGE), PyFat.FAT_TYPE_FAT32, size=SIZE)
    pf.close()

    fs = PyFatFS(str(IMAGE))
    fs.makedirs("apps/tkdr")
    fs.makedirs("apps/tkdr/ms0")
    for src, dst in FILES:
        t0 = time.time()
        total = src.stat().st_size
        done = 0
        with src.open("rb") as inp, fs.openbin(dst, "wb") as out:
            while True:
                chunk = inp.read(4 << 20)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if done % (256 << 20) < (4 << 20):
                    print(f"  {dst}: {done / (1 << 20):.0f}/{total / (1 << 20):.0f} MiB",
                          flush=True)
        print(f"{dst}: {total} bytes in {time.time() - t0:.1f}s", flush=True)
    fs.close()
    print(f"image: {IMAGE} ({IMAGE.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
