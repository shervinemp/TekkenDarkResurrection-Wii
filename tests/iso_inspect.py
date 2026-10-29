#!/usr/bin/env python3
"""Minimal ISO9660 inspector: find PSP_GAME files and PARAM.SFO strings."""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
with path.open("rb") as f:
    f.seek(16 * 2048)
    pvd = f.read(2048)
    root = pvd[156:156 + 34]
    root_lba = struct.unpack_from("<I", root, 2)[0]
    root_size = struct.unpack_from("<I", root, 10)[0]

    def read_dir(lba, size):
        f.seek(lba * 2048)
        data = f.read(size)
        out = {}
        o = 0
        while o < len(data):
            rl = data[o]
            if rl == 0:
                o = (o // 2048 + 1) * 2048
                continue
            nl = data[o + 32]
            name = data[o + 33:o + 33 + nl].decode("latin1")
            if name.endswith(";1"):
                name = name[:-2]
            lba = struct.unpack_from("<I", data, o + 2)[0]
            sz = struct.unpack_from("<I", data, o + 10)[0]
            flags = data[o + 25]
            out[name] = (lba, sz, flags)
            o += rl
        return out

    root_dir = read_dir(root_lba, root_size)
    print("ROOT:", sorted(root_dir))
    for d in ("PSP_GAME", "PSP_GAME/SYSDIR", "PSP_GAME/USRDIR"):
        if d == "PSP_GAME":
            ent = root_dir.get("PSP_GAME")
            if not ent:
                continue
            files = read_dir(ent[0], ent[1])
            print("PSP_GAME:", sorted(files)[:40])
        elif d == "PSP_GAME/SYSDIR" and "PSP_GAME" in root_dir:
            ent = root_dir["PSP_GAME"]
            pg = read_dir(ent[0], ent[1])
            if "SYSDIR" in pg:
                files = read_dir(pg["SYSDIR"][0], pg["SYSDIR"][1])
                print("SYSDIR:", sorted(files))
                if "EBOOT.BIN" in files:
                    lba, sz, _ = files["EBOOT.BIN"]
                    f.seek(lba * 2048)
                    head = f.read(16)
                    print("EBOOT size", sz, "head", head.hex(), head[:4])
            if "PARAM.SFO" in pg:
                lba, sz, _ = pg["PARAM.SFO"]
                f.seek(lba * 2048)
                sfo = f.read(sz)
                strings = []
                for enc in ("utf-8", "latin1"):
                    try:
                        strings = [s for s in sfo.decode(enc, "ignore").split("\0") if len(s) > 2]
                        break
                    except Exception:
                        pass
                print("PARAM.SFO strings:", strings[:30])
        elif d == "PSP_GAME/USRDIR" and "PSP_GAME" in root_dir:
            ent = root_dir["PSP_GAME"]
            pg = read_dir(ent[0], ent[1])
            if "USRDIR" in pg:
                files = read_dir(pg["USRDIR"][0], pg["USRDIR"][1])
                print("USRDIR entries:", sorted(files)[:60])
