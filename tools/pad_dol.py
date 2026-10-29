#!/usr/bin/env python3
"""Align DOL text sections to 32 bytes.

devkitPPC's elf2dol copies ELF section sizes verbatim.  Dolphin warns about
unaligned text sections and, with MMU enabled, can fail core init.  This
rewrites the DOL with zero padding after text sections and updates file
offsets/sizes.  Load addresses and BSS are unchanged.

By default only text is padded; `--data` also pads data sections (and moves
the BSS start if needed).
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

ALIGN = 32


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--data", action="store_true", help="also pad data sections")
    args = ap.parse_args()

    src, dst = Path(args.src), Path(args.dst)
    data = src.read_bytes()
    if len(data) < 0x100:
        raise SystemExit("not a DOL")
    to = list(struct.unpack_from(">7I", data, 0x00))
    do = list(struct.unpack_from(">11I", data, 0x1C))
    ta = list(struct.unpack_from(">7I", data, 0x48))
    da = list(struct.unpack_from(">11I", data, 0x64))
    ts = list(struct.unpack_from(">7I", data, 0x90))
    ds = list(struct.unpack_from(">11I", data, 0xAC))
    bss_addr, bss_size = struct.unpack_from(">II", data, 0xD8)
    entry = struct.unpack_from(">I", data, 0xE0)[0]

    out = bytearray(0x100)
    pos = 0x100

    def add_section(off, size, pad):
        nonlocal pos
        payload = data[off:off + size]
        if len(payload) != size:
            raise SystemExit("truncated DOL section")
        new_off = pos
        out.extend(payload)
        extra = ((-size) % ALIGN) if pad else 0
        out.extend(b"\0" * extra)
        pos += size + extra
        return new_off, size + extra

    for i in range(7):
        if ts[i]:
            to[i], ts[i] = add_section(to[i], ts[i], True)
    for i in range(11):
        if ds[i]:
            do[i], ds[i] = add_section(do[i], ds[i], args.data)

    # Do not move BSS.  The extra data padding is zero and lies between the
    # real data end and the BSS start (or is re-zeroed by BSS clearing), so
    # keeping the linker's BSS address preserves the heap/arena layout.

    struct.pack_into(">7I", out, 0x00, *to)
    struct.pack_into(">11I", out, 0x1C, *do)
    struct.pack_into(">7I", out, 0x48, *ta)
    struct.pack_into(">11I", out, 0x64, *da)
    struct.pack_into(">7I", out, 0x90, *ts)
    struct.pack_into(">11I", out, 0xAC, *ds)
    struct.pack_into(">II", out, 0xD8, bss_addr, bss_size)
    struct.pack_into(">I", out, 0xE0, entry)

    dst.write_bytes(out)
    print(f"{src} -> {dst}: {len(data)} -> {len(out)} bytes, "
          f"text {ts[0]:#x}, data {ds[0]:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
