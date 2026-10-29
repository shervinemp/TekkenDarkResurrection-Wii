"""Replay the draw cache's lookup stream (the VCDUMP blocks of an -DEXP_VCDUMP demo log) against other cache shapes.

A dump is the last 16384 lookups of a heavy window: draw address, vertex type + count, and the hashes of the vertex
bytes, the world matrix and the material/lights.  The stream is replayed through a set-associative cache with the
replacement rule of runtime/gpu/vcache.c (the slot of the same address, else an empty one, else the least recently
used); the first half warms it up, the second half is counted.

usage: python tools/perf/vcsim.py <log> [policy=lru,rand,bip] [dump numbers ...]
"""
import re
import sys
from collections import Counter

MASK = 0xFFFFFFFF


class Dump:
    def __init__(self, n, clock):
        self.n, self.clock = n, clock
        self.stream = []    # (vbase, key, hd, hw, hr) in lookup order
        self.res = []       # what the real cache made of each: 0 hit, 1 no slot for the address, 2 other bytes, 3 other state
        self.victim = {}    # position in the stream -> (vbase, key, age, kind) of the slot the store gave up
        self.table = {}     # set -> [(vbase, key, age) x 4] of the real slot table at the time of the dump


def read_dumps(path):
    dumps, cur = [], None
    for raw in open(path, errors='replace'):
        i = raw.find('VCDUMP begin')
        if i >= 0:
            f = raw[i:].split()
            cur = Dump(int(f[2]), int(f[4]) if len(f) > 4 else 0)
            dumps.append(cur)
            continue
        if 'VCDUMP end' in raw:
            cur = None
            continue
        if cur is None:
            continue
        i = raw.find('VCR ')
        if i >= 0:
            for rec in raw[i + 4:].strip().split('|'):
                f = rec.split()
                if len(f) >= 5:
                    r = tuple(int(x, 16) for x in f[:5])
                    if r[0]:
                        cur.stream.append(r)
                        cur.res.append(int(f[5], 16) if len(f) > 5 else 0)
            continue
        i = raw.find('VCV ')
        if i >= 0:
            f = raw[i + 4:].split()
            cur.victim[int(f[0])] = (int(f[1], 16), int(f[2], 16), int(f[3]), int(f[4]))
            continue
        i = raw.find('VCT ')
        if i >= 0:
            f = raw[i + 4:].split(None, 1)
            cur.table[int(f[0])] = [(int(a, 16), int(b, 16), int(c)) for a, b, c in (e.split() for e in f[1].strip().split('|'))]
    return dumps


def h_fib(vbase, key, bits):
    return (((vbase >> 2) * 2654435761) & MASK) >> (32 - bits)


def h_mix(vbase, key, bits):
    x = ((vbase >> 2) ^ (key * 0x9E3779B1)) & MASK
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & MASK
    x ^= x >> 13
    x = (x * 0xC2B2AE35) & MASK
    x ^= x >> 16
    return x >> (32 - bits)


def h_fib_key(vbase, key, bits):
    return ((((vbase >> 2) ^ (key >> 24 << 3)) * 2654435761) & MASK) >> (32 - bits)


def simulate(stream, sets, ways, hashfn, policy="lru"):
    bits = sets.bit_length() - 1
    table = [[] for _ in range(sets)]   # per set: [vbase, key, hd, hw, hr, age]
    clock = 0
    half = len(stream) // 2
    hits = misses = cold = 0
    rng = 12345
    for n, (vbase, key, hd, hw, hr) in enumerate(stream):
        st = table[hashfn(vbase, key, bits)]
        found = None
        for e in st:
            if e[0] == vbase and e[1] == key:
                found = e
                break
        hit = found is not None and found[2] == hd and found[3] == hw and found[4] == hr
        if n >= half:
            if hit:
                hits += 1
            else:
                misses += 1
                if found is None:
                    cold += 1
        clock += 1
        if hit:
            found[5] = clock
            continue
        if found is not None:
            found[2:5] = [hd, hw, hr]
            found[5] = clock
        elif len(st) < ways:
            st.append([vbase, key, hd, hw, hr, clock])
        elif policy == 'lru':
            v = min(st, key=lambda e: e[5])
            v[:] = [vbase, key, hd, hw, hr, clock]
        elif policy == 'rand':
            rng = (rng * 1103515245 + 12345) & MASK
            st[(rng >> 16) % ways][:] = [vbase, key, hd, hw, hr, clock]
        else:   # 'bip': a new entry goes in at the least recently used position, one time in eight at the most recent
            rng = (rng * 1103515245 + 12345) & MASK
            v = min(st, key=lambda e: e[5])
            oldest = v[5]
            v[:] = [vbase, key, hd, hw, hr, clock if (rng >> 16) % 8 == 0 else oldest - 1]
    return hits, misses, cold


def main():
    dumps = read_dumps(sys.argv[1])
    args = sys.argv[2:]
    policies = ['lru']
    if args and args[0].startswith('policy='):
        policies = args.pop(0)[7:].split(',')
    pick = [int(a) for a in args] or range(len(dumps))
    configs = [
        ('2048x4 fib   (now)', 2048, 4, h_fib),
        ('2048x4 mix', 2048, 4, h_mix),
        ('2048x4 fib+key', 2048, 4, h_fib_key),
        ('4096x4 fib', 4096, 4, h_fib),
        ('4096x4 mix', 4096, 4, h_mix),
        ('2048x8 fib', 2048, 8, h_fib),
        ('2048x8 mix', 2048, 8, h_mix),
        ('4096x8 mix', 4096, 8, h_mix),
        ('8192x4 mix', 8192, 4, h_mix),
    ]
    for d in pick:
        st = dumps[d].stream
        distinct = Counter((r[0], r[1]) for r in st)
        sizes = {}
        for r in st:
            sizes[(r[0], r[1])] = (r[1] >> 24 & 0x3F) * 24
        per_set = Counter(h_fib(k[0], k[1], 11) for k in distinct)
        over = sum(c for c in per_set.values() if c > 4)
        print('dump %d: %d lookups, %d distinct draws (%d KB of vertices), %d of them in sets of the current cache with more than 4 draws' % (
            d, len(st), len(distinct), sum(sizes.values()) // 1024, over))
        dd = dumps[d]
        if dd.res and any(dd.res):
            half = len(dd.res) // 2
            real = sum(1 for r in dd.res[half:] if r)
            print('   real cache, second half of the stream: %d misses of %d (%.1f%%), classes %s; whole stream %.1f%%' % (
                real, len(dd.res) - half, 100.0 * real / max(1, len(dd.res) - half), dict(Counter(dd.res[half:])), 100.0 * sum(1 for r in dd.res if r) / len(dd.res)))
            kinds = Counter(v[3] for v in dd.victim.values())
            print('   victims of the stores: 1 empty %d, 2 same address %d, 3 live %d, 4 old %d' % (kinds[1], kinds[2], kinds[3], kinds[4]))
        if dd.table:
            full = sum(1 for e in dd.table.values() if all(x[1] for x in e))
            live = Counter(sum(1 for x in e if x[1] and x[2] < 12000) for e in dd.table.values())
            print('   real slot table: %d sets in use, %d full; live (used in the last 12000 lookups) slots per set %s' % (len(dd.table), full, dict(sorted(live.items()))))
        for name, sets, ways, fn in configs:
          for policy in policies:
            h, m, c = simulate(st, sets, ways, fn, policy)
            print('   %-20s %-4s %6d slots (%4d KB)  miss %5.1f%%  (%d misses of %d, no slot for the address: %d)' % (
                name, policy, sets * ways, sets * ways * 32 // 1024, 100.0 * m / max(1, h + m), m, h + m, c))


if __name__ == '__main__':
    main()
