#!/usr/bin/env python3
"""Check the static VFPU translation (recomp/vfpu_emit.py) against the interpreter.

Every distinct VFPU instruction that occurs in the game is taken from a
generated tree built without the translator (RECOMP_VFPU=0), translated with
several prefix states, and run next to runtime/vfpu.c on random register files
and memory.  The registers, the control registers and the memory touched must
agree.  Floating point is compiled without contraction on the host so that the
two sides round identically.

    RECOMP_VFPU=0 python -m recomp.emit test/ULUS10139_EBOOT.BIN <base-gen>
    python tools/test_vfpu_emit.py --base-gen <base-gen> --work <scratch dir>
"""
import argparse
import glob
import os
import random
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from recomp import vfpu_emit as ve  # noqa: E402

SITE = re.compile(r"vfpu_exec\(c, 0x([0-9a-f]{8})u, 0x([0-9a-f]{8})u\)")

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"

uint8_t *g_mem;
unsigned g_trap;
void recomp_trap(Ctx *c, uint32_t pc, const char *why) { (void)c; (void)pc; (void)why; g_trap++; }
void hle_call(Ctx *c, uint32_t i) { (void)c; (void)i; }
void call_indirect(Ctx *c, uint32_t a) { (void)c; (void)a; }

typedef struct { uint32_t raw, addr, pfx[3], base_reg, base_val, strict; void (*fn)(Ctx *); int rounds; int dflt; int lax_zero; } Case;
#include "cases.inc"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static uint32_t rand_float_bits(void) {
    static const uint32_t special[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u,
        0x7FC00000u, 0x40000000u, 0x3F000000u, 0x00000001u, 0x7F7FFFFFu, 0x3EAAAAABu, 0x4B000000u, 0xCF000000u,
        0x4F000000u, 0x3F333333u, 0xC0490FDBu, 0x40490FDBu};
    switch (rnd() % 6) {
    case 0: return special[rnd() % (sizeof special / sizeof *special)];
    case 1: return rnd();
    case 2: return (rnd() & 0x80000000u) | (0x3F000000u + (rnd() & 0x00FFFFFFu));          // around 1
    case 3: { float f = (float)((int)(rnd() % 2001) - 1000) / 8.f; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 4: return (uint32_t)((int32_t)(rnd() % 400001) - 200000);                          // an integer
    default: return rnd() & 0xBFFFFFFFu;
    }
}

static int same(uint32_t a, uint32_t b, int strict, int lax_zero) {
    if (a == b) return 1;
    if (lax_zero && ((a | b) & 0x7FFFFFFFu) == 0) return 1;   /* +0 and -0: which fminf/fmaxf returns is the library's choice */
    if (strict) return 0;
    return (a & 0x7FFFFFFFu) > 0x7F800000u && (b & 0x7FFFFFFFu) > 0x7F800000u;   // both NaN
}

#define WIN 0x09000000u
int main(void) {
    g_mem = calloc(1, GUEST_MEM_SIZE);
    if (!g_mem) { fprintf(stderr, "no memory\n"); return 2; }
    unsigned bad = 0, total = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        const Case *cs = &cases[k];
        for (int round = 0; round < cs->rounds; round++) {
            static Ctx a, b;
            for (int i = 0; i < 128; i++) a.v[i].u = rand_float_bits();
            for (int i = 0; i < 32; i++) a.r[i] = rnd();
            a.r[0] = 0;
            for (int i = 0; i < 16; i++) a.vfpu_ctrl[i] = rnd();
            a.vfpu_ctrl[0] = cs->pfx[0]; a.vfpu_ctrl[1] = cs->pfx[1]; a.vfpu_ctrl[2] = cs->pfx[2];
            if (cs->dflt == 2) {   // the translator knew nothing: run it from the other state too
                if (round & 1) { a.vfpu_ctrl[0] = 0xE4; a.vfpu_ctrl[1] = 0xE4; a.vfpu_ctrl[2] = 0; }
            }
            a.r[cs->base_reg] = cs->base_val;
            for (uint32_t o = 0; o < 0x100; o++) g_mem[(WIN & 0x1FFFFFFFu) + o] = (uint8_t)rnd();
            uint8_t mem0[0x100];
            memcpy(mem0, g_mem + (WIN & 0x1FFFFFFFu), sizeof mem0);
            b = a;
            vfpu_exec(&a, cs->addr, cs->raw);
            uint8_t mema[0x100];
            memcpy(mema, g_mem + (WIN & 0x1FFFFFFFu), sizeof mema);
            memcpy(g_mem + (WIN & 0x1FFFFFFFu), mem0, sizeof mem0);
            cs->fn(&b);
            total++;
            int ok = !memcmp(mema, g_mem + (WIN & 0x1FFFFFFFu), sizeof mema);
            const char *what = ok ? "" : "memory";
            for (int i = 0; i < 128 && ok; i++)
                if (!same(a.v[i].u, b.v[i].u, cs->strict, cs->lax_zero)) { ok = 0; what = "v"; }
            for (int i = 0; i < 16 && ok; i++)
                if (a.vfpu_ctrl[i] != b.vfpu_ctrl[i]) { ok = 0; what = "ctrl"; }
            for (int i = 0; i < 32 && ok; i++)
                if (a.r[i] != b.r[i]) { ok = 0; what = "r"; }
            if (!ok) {
                bad++;
                if (bad <= 25) {
                    printf("MISMATCH raw=%08X pfx=%X,%X,%X dflt=%d (%s)\n", cs->raw, cs->pfx[0], cs->pfx[1], cs->pfx[2],
                           cs->dflt, what);
                    for (int i = 0; i < 128; i++)
                        if (!same(a.v[i].u, b.v[i].u, cs->strict, cs->lax_zero) )
                            printf("   v[%d] interp=%08X translated=%08X (was %08X)\n", i, a.v[i].u, b.v[i].u, 0u);
                    for (int i = 0; i < 16; i++)
                        if (a.vfpu_ctrl[i] != b.vfpu_ctrl[i])
                            printf("   ctrl[%d] interp=%08X translated=%08X\n", i, a.vfpu_ctrl[i], b.vfpu_ctrl[i]);
                }
                break;
            }
        }
    }
    printf("%u runs, %u cases, %u failed cases\n", total, (unsigned)(sizeof cases / sizeof *cases), bad);
    return bad != 0;
}
'''


def harvest(base_gen):
    ops = {}
    for f in sorted(glob.glob(os.path.join(base_gen, "funcs_*.c"))):
        text = open(f, errors="replace").read()
        for m in SITE.finditer(text):
            ops[int(m.group(2), 16)] = int(m.group(1), 16)
    return ops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-gen", required=True, help="generated tree built with RECOMP_VFPU=0")
    ap.add_argument("--work", required=True)
    ap.add_argument("--rounds", type=int, default=12)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc", default="gcc")
    args = ap.parse_args()
    random.seed(args.seed)
    os.makedirs(args.work, exist_ok=True)

    ops = harvest(args.base_gen)
    pfx_vals = [set(), set(), set()]
    for raw in ops:
        if raw >> 26 == 0x37 and ((raw >> 23) & 7) < 6:
            which = ((raw >> 23) & 7) >> 1
            pfx_vals[which].add(raw & (0xFFF if which == 2 else 0xFFFFF))
    pfx_vals = [sorted(v) for v in pfx_vals]
    print(f"{len(ops)} distinct VFPU instructions; prefixes: {[len(v) for v in pfx_vals]}")

    cases, funcs = [], []
    skipped = 0

    def add(raw, addr, st, run_state, dflt):
        nonlocal skipped
        r = ve.translate(raw, addr, st)
        if r is None:
            return
        lines, _ = r
        k = len(funcs)
        funcs.append(f"static void k{k}(Ctx *c) {{\n    " + "\n    ".join(lines) + "\n}\n")
        base = (raw >> 21) & 31
        off = ((raw & 0xFFFC) ^ 0x8000) - 0x8000
        major = raw >> 26
        base_val = 0
        if major in (0x32, 0x3A, 0x36, 0x3E):
            addr_target = 0x09000000 + 0x40 + (random.randrange(4) * 16)
            base_val = (addr_target - off) & 0xFFFFFFFF
        strict = 1 if (major in (0x32, 0x3A, 0x36, 0x3E, 0x12) or (major == 0x34 and (16 <= ((raw >> 21) & 31) <= 19 or ((raw >> 21) & 31) == 1))) else 0
        cases.append((raw, addr, run_state, base if major in (0x32, 0x3A, 0x36, 0x3E) else 0, base_val, strict, k, dflt))

    nproc = 0
    for raw, addr in sorted(ops.items()):
        major = raw >> 26
        if major == 0x37 and ((raw >> 23) & 7) < 6:
            continue   # the prefix instructions themselves are checked by their effect on the next operation
        nproc += 1
        # 1. no prefix, known
        add(raw, addr, ve.DEFAULTS, ve.DEFAULTS, 0)
        # 2. a known prefix state from the game's own prefix values
        for _ in range(3):
            st = (random.choice(pfx_vals[0]) if random.random() < 0.7 else ve.NONE_ST,
                  random.choice(pfx_vals[1]) if random.random() < 0.7 else ve.NONE_ST,
                  random.choice(pfx_vals[2]) if random.random() < 0.7 else ve.NONE_D)
            add(raw, addr, st, st, 1)
        # 3. a random (possibly odd) prefix state
        st = (random.getrandbits(20), random.getrandbits(20), random.getrandbits(12))
        add(raw, addr, st, st, 1)
        # 4. nothing known: the guard
        add(raw, addr, ve.UNKNOWN, ve.DEFAULTS, 2)
    print(f"{nproc} instructions -> {len(cases)} cases")

    # instructions that were left to the interpreter
    left = {}
    for raw, addr in ops.items():
        if raw >> 26 == 0x37 and ((raw >> 23) & 7) < 6:
            continue
        if ve.translate(raw, addr, ve.DEFAULTS) is None:
            key = (raw >> 26, (raw >> 21) & 31 if raw >> 26 in (0x34, 0x3C, 0x12) else (raw >> 23) & 7)
            left[key] = left.get(key, 0) + 1
    print("left to the interpreter (major, sub): count", {f"{k[0]:#x}/{k[1]:#x}": v for k, v in sorted(left.items())})

    with open(os.path.join(args.work, "cases.inc"), "w") as fh:
        fh.write("".join(funcs))
        fh.write("static const Case cases[] = {\n")
        for raw, addr, st, br, bv, strict, k, dflt in cases:
            fh.write(f"  {{{raw:#x}u, {addr:#x}u, {{{st[0] if st[0] is not None else 0xE4:#x}u, "
                     f"{st[1] if st[1] is not None else 0xE4:#x}u, {st[2] if st[2] is not None else 0:#x}u}}, "
                     f"{br}, {bv:#x}u, {strict}, k{k}, {args.rounds}, {dflt}, {1 if raw >> 26 == 0x1B else 0}}},\n")
        fh.write("};\n")
    with open(os.path.join(args.work, "harness.c"), "w") as fh:
        fh.write(HARNESS)
    exe = os.path.join(args.work, "vfpu_test.exe")
    def posix(path):   # C:\a\b -> /c/a/b, for the msys2 shell
        path = os.path.abspath(path).replace("\\", "/")
        return "/" + path[0].lower() + path[2:] if len(path) > 1 and path[1] == ":" else path

    flags = "-pipe -O1 -ffp-contract=off -fno-fast-math -w"
    srcs = [os.path.join(args.work, "harness.c"), os.path.join(ROOT, "runtime", "vfpu.c")]
    compile_cmd = (f"{args.cc} {flags} -I {posix(os.path.join(ROOT, 'runtime'))} -I {posix(args.work)} "
                   + " ".join(posix(x) for x in srcs) + f" -o {posix(exe)} -lm")
    print("compiling...")
    msys_bash = r"C:\devkitPro\msys2\usr\bin\bash.exe"
    if os.name == "nt" and os.path.exists(msys_bash):
        # devkitPro's gcc only finds its headers from inside its own msys2 shell
        subprocess.run([msys_bash, "-lc", compile_cmd], check=True)
    else:
        subprocess.run(compile_cmd, shell=True, check=True)
    print("running...")
    return subprocess.run([exe]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
