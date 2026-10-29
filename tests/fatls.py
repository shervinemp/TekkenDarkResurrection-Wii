#!/usr/bin/env python3
"""List a FAT32 image's root/known directories (read-only, for test diagnostics)."""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
bps, spc, rsv, nfats, rootents, totsec16, media, fatsz16 = struct.unpack_from("<HBHBHHBH", data, 11)
if fatsz16 == 0:
    fatsz = struct.unpack_from("<I", data, 36)[0]
    root_cluster = struct.unpack_from("<I", data, 44)[0]
else:
    raise SystemExit("not FAT32")
root_sec = rsv + nfats * fatsz
cluster_bytes = bps * spc


def sec(n):
    return data[n * bps:(n + 1) * bps]


def cluster(n):
    return data[root_sec * bps + (n - 2) * cluster_bytes:
                root_sec * bps + (n - 1) * cluster_bytes]


def entries(buf):
    out = []
    lfn = []
    for off in range(0, len(buf), 32):
        e = buf[off:off + 32]
        if len(e) < 32 or e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        attr = e[11]
        if attr == 0x0F:
            lfn.insert(0, e[1:11] + e[14:26] + e[28:32])
            continue
        name = e[0:11].decode("latin1").strip()
        if lfn:
            name = b"".join(lfn).decode("utf-16le", "ignore").split("\0")[0]
            lfn = []
        if name in (".", ".."):
            continue
        cl = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        out.append((name, attr, cl, size))
    return out


def list_dir(cl, prefix=""):
    buf = cluster(cl)
    for name, attr, cl, size in entries(buf):
        print(f"{prefix}{name} {'<DIR>' if attr & 0x10 else size}")
        if attr & 0x10 and name not in (".", ".."):
            list_dir(cl, prefix + name + "/")


print("root cluster", root_cluster)
list_dir(root_cluster)
