#!/usr/bin/env python3
"""Print one file from a raw FAT32 SD image (Dolphin's WiiSD.raw) without mounting it.

    python tools/perf/readsd.py C:\\tkdr_raw_user\\Load\\WiiSD.raw apps/tkdr/debug.log > debug.log

Reads the short (8.3) directory entries only, which is enough for the files the port writes
(debug.log, crash.txt).  The image is only read; nothing is written.
"""
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img, path = sys.argv[1], sys.argv[2]
    f = open(img, "rb")
    boot = f.read(512)
    bps, spc, rsvd, nfats = struct.unpack_from("<HBHB", boot, 11)
    spf = struct.unpack_from("<I", boot, 36)[0]
    root = struct.unpack_from("<I", boot, 44)[0]
    fat_off = rsvd * bps
    data_off = (rsvd + nfats * spf) * bps
    csize = spc * bps

    def fat_next(c):
        f.seek(fat_off + c * 4)
        return struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF

    def chain(c):
        while 2 <= c < 0x0FFFFFF8:
            yield c
            c = fat_next(c)

    def read_clusters(c, limit=None):
        out = bytearray()
        for k in chain(c):
            f.seek(data_off + (k - 2) * csize)
            out += f.read(csize)
            if limit is not None and len(out) >= limit:
                break
        return bytes(out if limit is None else out[:limit])

    def find(dir_cluster, name):
        want = name.upper()
        data = read_clusters(dir_cluster)
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:   # deleted, or a long-name part
                continue
            base, ext = e[0:8].decode("latin1").rstrip(), e[8:11].decode("latin1").rstrip()
            short = base + ("." + ext if ext else "")
            if short.upper() == want:
                hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
                return (hi << 16 | lo), struct.unpack_from("<I", e, 28)[0], bool(e[11] & 0x10)
        return None

    cluster, size, is_dir = root, 0, True
    for part in path.strip("/").split("/"):
        hit = find(cluster, part)
        if not hit:
            sys.exit("not found: " + part)
        cluster, size, is_dir = hit
    if is_dir:
        sys.exit("that is a directory")
    sys.stdout.buffer.write(read_clusters(cluster, size))


if __name__ == "__main__":
    main()
