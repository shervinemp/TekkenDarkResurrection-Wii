"""Static translation of PSP VFPU instructions into straight-line C.

runtime/vfpu.c interprets every VFPU instruction: it decodes the register
fields into index lists, applies the prefixes with loops and writes the result
back through more loops, for each of the tens of thousands of VFPU
instructions a frame.  Everything it decodes is a constant at translation
time, and so is the prefix state in the common case (a vpfx instruction right
before the operation it applies to), so the translator emits the arithmetic
directly.

The translation mirrors the interpreter operation by operation (same lane
order, same float operations) and the interpreter stays the reference: an
instruction the translator does not handle, or one whose prefix state is not
known, is left to vfpu_exec().  tools/test_vfpu_emit.py checks every
instruction in the game against the interpreter.

The prefix registers (c->vfpu_ctrl[0..2]) are kept correct in memory at every
instruction boundary, so the interpreter can be entered at any point.  The
translator additionally tracks what it knows about them: `st` is a list of
three entries (source, target, destination prefix), each an int when the value
is known and None when it is not.
"""
import struct

NONE_ST = 0xE4          # the "no prefix" value of the source/target prefix
NONE_D = 0              # and of the destination prefix
DEFAULTS = (NONE_ST, NONE_ST, NONE_D)
UNKNOWN = (None, None, None)

# k_pfx_const in vfpu.c
PFX_CONST = ["0.f", "1.f", "2.f", "0.5f", "3.f", "(1.f / 3.f)", "0.25f", "(1.f / 6.f)"]
# k_cst in vfpu.c (the first 20 entries are the VFPU constants, the rest zero)
CST = ["0.f", "3.402823466e38f", "1.41421356f", "0.70710678f", "1.12837917f", "0.63661977f",
       "0.31830989f", "0.78539816f", "1.57079633f", "3.14159265f", "2.71828183f", "1.44269504f",
       "0.43429448f", "0.69314718f", "2.30258509f", "6.28318531f", "0.52359878f", "0.30103000f",
       "3.32192809f", "0.86602540f"] + ["0.f"] * 12


def vsize(op):
    return (((op >> 7) & 1) | (((op >> 15) & 1) << 1)) + 1


def VD(op):
    return op & 0x7F


def VS(op):
    return (op >> 8) & 0x7F


def VT(op):
    return (op >> 16) & 0x7F


def vec_regs(n, reg):
    mtx, col, tr = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if n == 1:
        tr, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    return [mtx * 4 + (((row + i) & 3) + col * 32 if tr else col + ((row + i) & 3) * 32) for i in range(n)]


def mtx_regs(n, reg):
    """m[j*4 + i]: column j, row i of the (possibly transposed) operand."""
    mtx, col, tr = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if n == 1:
        tr, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    out = [0] * 16
    for i in range(n):
        for j in range(n):
            out[j * 4 + i] = mtx * 4 + (((row + i) & 3) + ((col + j) & 3) * 32 if tr
                                         else ((col + j) & 3) + ((row + i) & 3) * 32)
    return out


def half_to_float_bits(h):
    s, e, m = (h >> 15) << 31, (h >> 10) & 31, h & 0x3FF
    if e == 31:
        return s | 0x7F800000 | (m << 13)
    if e == 0:
        if not m:
            return s
        f = struct.unpack("<f", struct.pack("<f", m * 2.0 ** -24))[0]
        return struct.unpack("<I", struct.pack("<f", f))[0] | s
    return s | ((e + 112) << 23) | (m << 13)


def float_lit(bits):
    """A C expression for the float with these bits."""
    f = struct.unpack("<f", struct.pack("<I", bits))[0]
    if f != f:
        return f"vfpu_bits({bits:#x}u)"
    if f in (float("inf"), float("-inf")):
        return "(INFINITY)" if f > 0 else "(-INFINITY)"
    return f"{f.hex()}f" if f == f and f != 0 else ("(-0.f)" if bits >> 31 else "0.f")


def eats_prefixes(raw):
    """Whether vfpu_exec() resets the prefix registers after this instruction."""
    major = raw >> 26
    if major in (0x12, 0x32, 0x35, 0x36, 0x3A, 0x3D, 0x3E, 0x3F):
        return False
    if major == 0x37:
        return ((raw >> 23) & 7) >= 6
    return True


class Gen:
    """Translation of one instruction."""

    def __init__(self, raw, addr, st):
        self.raw, self.addr, self.st = raw, addr, list(st)
        self.out = []
        self.used = set()   # prefix registers (0 s, 1 t, 2 d) the operation reads
        self.n_tmp = 0

    # -- operand access ------------------------------------------------------
    def emit(self, line):
        self.out.append(line)

    def tmp(self, base="x"):
        self.n_tmp += 1
        return f"{base}{self.n_tmp}"

    def pfx(self, which):
        """The known value of a prefix, or None; a guard is added for unknown."""
        self.used.add(which)
        v = self.st[which]
        return NONE_ST if v is None and which < 2 else (NONE_D if v is None else v)

    def load(self, n, reg, base):
        """Raw lanes of a vector register as C variables."""
        names = []
        for i, idx in enumerate(vec_regs(n, reg)):
            nm = f"{base}{i}"
            self.emit(f"float {nm} = c->v[{idx}].f;")
            names.append(nm)
        return names

    def apply_st(self, names, n, pfx):
        """The lanes after a source/target prefix: C expressions."""
        if pfx == NONE_ST:
            return list(names)
        orig = list(names) + ["0.f"] * (4 - n)
        out = []
        for i in range(n):
            sel, abs_ = (pfx >> (i * 2)) & 3, (pfx >> (8 + i)) & 1
            cst, neg = (pfx >> (12 + i)) & 1, (pfx >> (16 + i)) & 1
            if cst:
                x = PFX_CONST[sel + (abs_ << 2)]
            else:
                x = orig[sel]
                if abs_:
                    x = "0.f" if x == "0.f" else f"fabsf({x})"
            if neg:
                x = f"(-{x})"
            out.append(x)
        return out

    def src(self, n, reg, which, base):
        pfx = self.pfx(which)
        if pfx == NONE_ST:
            return self.load(n, reg, base)
        names = self.load(n, reg, base + "r")
        lanes = self.apply_st(names, n, pfx)
        out = []
        for i, e in enumerate(lanes):   # one variable per lane, so a lane used twice is evaluated once
            nm = f"{base}{i}"
            self.emit(f"float {nm} = {e};")
            out.append(nm)
        return out

    def write_d(self, vals, n, reg):
        """write_vec_d(): saturate per the destination prefix, honour its mask."""
        p = self.pfx(2)
        regs = vec_regs(n, reg)
        mask = (p >> 8) & 0xF
        for i in range(n):
            if mask & (1 << i):
                continue
            sat = (p >> (i * 2)) & 3
            v = vals[i]
            if sat == 1:
                v = f"vfpu_clampf({v}, 0.f, 1.f)"
            elif sat == 3:
                v = f"vfpu_clampf({v}, -1.f, 1.f)"
            self.emit(f"c->v[{regs[i]}].f = {v};")

    def write_i(self, vals, n, reg):
        """write_ivec_d(): integer results; only the write mask applies."""
        p = self.pfx(2)
        regs = vec_regs(n, reg)
        mask = (p >> 8) & 0xF
        for i in range(n):
            if not mask & (1 << i):
                self.emit(f"c->v[{regs[i]}].u = {vals[i]};")

    # -- the instruction classes --------------------------------------------
    def alu3(self):
        raw = self.raw
        n, group, sub = vsize(raw), raw >> 26, (raw >> 23) & 7
        if group == 0x19 and sub == 2:   # vscl: the scalar comes from t without a prefix
            s = self.src(n, VS(raw), 0, "s")
            k = vec_regs(1, VT(raw))[0]
            self.emit(f"float k = c->v[{k}].f;")
            self.write_d([f"{s[i]} * k" for i in range(n)], n, VD(raw))
            return True
        if group == 0x1B and sub == 0:
            return self.vcmp()
        if group == 0x18 and sub not in (0, 1, 7):
            return False
        if group == 0x19 and sub not in (0, 1, 4):
            return False
        if group == 0x1B and sub not in (2, 3, 5, 6, 7):
            return False
        s = self.src(n, VS(raw), 0, "s")
        t = self.src(n, VT(raw), 1, "t")
        if group == 0x18:
            op = {0: "+", 1: "-", 7: "/"}[sub]
            self.write_d([f"{s[i]} {op} {t[i]}" for i in range(n)], n, VD(raw))
        elif group == 0x19:
            if sub == 0:
                self.write_d([f"{s[i]} * {t[i]}" for i in range(n)], n, VD(raw))
            else:   # vdot / vhdp
                terms = [("1.f" if (sub == 4 and i == n - 1) else s[i]) for i in range(n)]
                self.emit("float sum = 0.f;")
                for i in range(n):
                    self.emit(f"sum += {terms[i]} * {t[i]};")
                self.write_d(["sum"], 1, VD(raw))
        else:
            f = {2: lambda a, b: f"fminf({a}, {b})", 3: lambda a, b: f"fmaxf({a}, {b})",
                 5: lambda a, b: f"({a} > {b} ? 1.f : {a} < {b} ? -1.f : 0.f)",
                 6: lambda a, b: f"({a} >= {b} ? 1.f : 0.f)",
                 7: lambda a, b: f"({a} < {b} ? 1.f : 0.f)"}[sub]
            self.write_d([f(s[i], t[i]) for i in range(n)], n, VD(raw))
        return True

    def vcmp(self):
        """vcmp: the lane results go into the condition bits of the CC control register."""
        raw = self.raw
        n, cond = vsize(raw), raw & 15
        binary = cond in (1, 2, 3, 5, 6, 7)
        unary = cond >= 8
        s = self.src(n, VS(raw), 0, "s") if binary or unary else None
        t = self.src(n, VT(raw), 1, "t") if binary else None
        for i in range(n):
            a, b = (s[i], t[i]) if binary else (s[i] if unary else None, None)
            expr = {0: "0", 1: f"{a} == {b}", 2: f"{a} < {b}", 3: f"{a} <= {b}", 4: "1", 5: f"{a} != {b}",
                    6: f"{a} >= {b}", 7: f"{a} > {b}", 8: f"{a} == 0.f", 9: f"!!isnan({a})", 10: f"!!isinf({a})",
                    11: f"(!!isnan({a}) || !!isinf({a}))", 12: f"{a} != 0.f", 13: f"!isnan({a})",
                    14: f"!isinf({a})", 15: f"!(isnan({a}) || isinf({a}))"}[cond]
            self.emit(f"unsigned r{i} = {expr};")
        lanes = range(n)
        self.emit(f"unsigned ca = {' | '.join(f'r{i}' for i in lanes)}, cl = {' & '.join(f'r{i}' for i in lanes)};")
        bits = " | ".join(f"(r{i} << {i})" for i in lanes)
        affected = 0x30 | ((1 << n) - 1)
        self.emit(f"c->vfpu_ctrl[3] = (c->vfpu_ctrl[3] & ~{affected:#x}u) | {bits} | (ca << 4) | (cl << 5);")
        return True

    def vcmov(self):
        """vcmovt / vcmovf: lanes of the destination are replaced when the chosen CC bit is set (t) or clear (f)."""
        raw = self.raw
        n = vsize(raw)
        tf, imm3 = (raw >> 19) & 1, (raw >> 16) & 7
        s = self.src(n, VS(raw), 0, "s")
        t = self.src(n, VD(raw), 1, "t")   # the destination's old value, through the target prefix
        self.emit("uint32_t cc = c->vfpu_ctrl[3];")
        d = []
        for i in range(n):
            bit = imm3 if imm3 < 6 else (i if imm3 == 6 else None)
            if bit is None:
                d.append(t[i])
            elif tf:
                d.append(f"((cc >> {bit}) & 1u ? {t[i]} : {s[i]})")
            else:
                d.append(f"((cc >> {bit}) & 1u ? {s[i]} : {t[i]})")
        self.write_d(d, n, VD(raw))
        return True

    def vfpu7(self):
        """The packing / unpacking conversions (vuc2ifs ... vi2s): integer lanes in, integer lanes out."""
        raw = self.raw
        n, fn = vsize(raw), (raw >> 16) & 31
        if fn < 24 or self.pfx(0) != NONE_ST:
            return False
        if fn == 24:   # vuc2ifs
            d = [f"(((s0 >> {8 * i}) & 0xFFu) * 0x01010101u) >> 1" for i in range(4)]
        elif fn == 25:   # vc2i
            d = ["(s0 & 0xFFu) << 24", "(s0 & 0xFF00u) << 16", "(s0 & 0xFF0000u) << 8", "s0 & 0xFF000000u"]
        elif fn in (26, 27):   # vus2i / vs2i
            d = []
            for i in range(2 if n >= 2 else 1):
                if fn == 26:
                    d += [f"(s{i} & 0xFFFFu) << 15", f"(s{i} & 0xFFFF0000u) >> 1"]
                else:
                    d += [f"(s{i} & 0xFFFFu) << 16", f"s{i} & 0xFFFF0000u"]
        elif fn in (28, 29):   # vi2uc / vi2c
            def byte(i):
                if fn == 28:
                    return f"(uint32_t)((((int32_t)s{i} < 0 ? 0 : (int32_t)s{i}) >> 23) & 0xFF) << {8 * i}"
                return f"(s{i} >> 24) << {8 * i}"
            d = [" | ".join(byte(i) for i in range(4))]
        else:   # vi2us / vi2s
            d = []
            for i in range((n + 1) // 2):
                lo, hi = 2 * i, 2 * i + 1
                if fn == 30:
                    d.append(f"(uint32_t)(((int32_t)s{lo} < 0 ? 0 : (int32_t)s{lo}) >> 15) | "
                             f"((uint32_t)(((int32_t)s{hi} < 0 ? 0 : (int32_t)s{hi}) >> 15) << 16)")
                else:
                    d.append(f"(s{lo} >> 16) | ((s{hi} >> 16) << 16)")
        regs = vec_regs(n, VS(raw))
        for i in range(4):
            if any(f"s{i}" in e for e in d):
                self.emit(f"uint32_t s{i} = " + (f"c->v[{regs[i]}].u;" if i < n else "0u;"))
        self.write_i(d, len(d), VD(raw))
        return True

    def vfpu4(self):
        raw = self.raw
        n, fn = vsize(raw), (raw >> 16) & 31
        if fn == 3:
            lane = VD(raw) & (3 if n >= 3 else 1)
            self.write_d(["1.f" if i == lane else "0.f" for i in range(n)], n, VD(raw))
            return True
        if fn in (6, 7):
            self.write_d([("1.f" if fn == 7 else "0.f")] * n, n, VD(raw))
            return True
        one = {0: "{x}", 1: "fabsf({x})", 2: "(-{x})", 4: "vfpu_clampf({x}, 0.f, 1.f)",
               5: "vfpu_clampf({x}, -1.f, 1.f)", 16: "(1.f / {x})", 17: "(1.f / sqrtf({x}))",
               18: "vfpu_qsin({x})", 19: "vfpu_qcos({x})", 20: "exp2f({x})", 21: "log2f({x})",
               22: "sqrtf(fabsf({x}))", 23: "(asinf({x}) / (float)M_PI_2)", 24: "(-1.f / {x})",
               26: "(-vfpu_qsin({x}))", 28: "(1.f / exp2f({x}))"}
        if fn not in one:
            return False
        s = self.src(n, VS(raw), 0, "s")
        self.write_d([one[fn].format(x=s[i]) for i in range(n)], n, VD(raw))
        return True

    def vfpu9(self):
        raw = self.raw
        n, fn = vsize(raw), (raw >> 16) & 31
        if fn not in (2, 3, 4, 6, 7, 10):
            return False
        if fn in (2, 3) and n != 4:
            # the interpreter reads four lanes regardless; leave other sizes to it
            return False
        s = self.src(n, VS(raw), 0, "s")
        if fn == 2:
            d = [f"{s[0]} + {s[1]}", f"{s[0]} - {s[1]}", f"{s[2]} + {s[3]}", f"{s[2]} - {s[3]}"]
            self.write_d(d, 4, VD(raw))
        elif fn == 3:
            d = [f"{s[0]} + {s[2]}", f"{s[1]} + {s[3]}", f"{s[0]} - {s[2]}", f"{s[1]} - {s[3]}"]
            self.write_d(d, 4, VD(raw))
        elif fn == 4:
            self.write_d([f"1.f - {s[i]}" for i in range(n)], n, VD(raw))
        elif fn in (6, 7):
            self.emit("float sum = 0.f;")
            for i in range(n):
                self.emit(f"sum += {s[i]};")
            self.write_d(["sum" if fn == 6 else f"sum / (float){n}"], 1, VD(raw))
        else:
            self.write_d([f"({s[i]} > 0.f ? 1.f : {s[i]} < 0.f ? -1.f : 0.f)" for i in range(n)], n, VD(raw))
        return True

    def vfpu4jump(self):
        raw = self.raw
        sub, n = (raw >> 21) & 31, vsize(raw)
        if sub == 0:
            return self.vfpu4()
        if sub == 1:
            return self.vfpu7()
        if sub == 2:
            return self.vfpu9()
        if sub == 21:
            return self.vcmov()
        if sub == 3:   # vcst
            self.write_d([CST[(raw >> 16) & 31]] * n, n, VD(raw))
            return True
        if sub in (16, 17, 18, 19):   # vf2in / vf2iz / vf2iu / vf2id
            s = self.src(n, VS(raw), 0, "s")
            sh = (raw >> 16) & 31
            fn = ("vfpu_f2in", "vfpu_f2iz", "vfpu_f2iu", "vfpu_f2id")[sub - 16]
            self.write_i([f"{fn}({s[i]}, {sh})" for i in range(n)], n, VD(raw))
            return True
        if sub == 20:   # vi2f: the prefix applies to the integer bits viewed as a float
            if self.pfx(0) != NONE_ST:
                return False
            scale = float_lit((127 - ((raw >> 16) & 31)) << 23)
            regs = vec_regs(n, VS(raw))
            self.write_d([f"(float)c->v[{regs[i]}].s * {scale}" for i in range(n)], n, VD(raw))
            return True
        return False

    def vfpu6(self):
        raw = self.raw
        sub, n = (raw >> 21) & 31, vsize(raw)
        if sub < 4:   # vmmul
            sm, tm, dm = mtx_regs(n, VS(raw)), mtx_regs(n, VT(raw)), mtx_regs(n, VD(raw))
            for j in range(n):
                for i in range(n):
                    self.emit(f"float s{j * 4 + i} = c->v[{sm[j * 4 + i]}].f;")
            for j in range(n):
                for i in range(n):
                    self.emit(f"float t{j * 4 + i} = c->v[{tm[j * 4 + i]}].f;")
            for a in range(n):
                for b in range(n):
                    terms = [f"s{b * 4 + k} * t{a * 4 + k}" for k in range(n)]
                    self.emit(f"c->v[{dm[a * 4 + b]}].f = vfpu_dot{n}({', '.join(terms)});")
            return True
        if sub < 16:   # vtfm / vhtfm
            m = ((raw >> 23) & 3) + 1
            sm = mtx_regs(m, VS(raw))
            v = ["0.f"] * 4
            regs = vec_regs(n, VT(raw))
            for i in range(n):
                self.emit(f"float v{i} = c->v[{regs[i]}].f;")
                v[i] = f"v{i}"
            if n < m:
                v[n] = "1.f"
            for j in range(m):
                for i in range(m):
                    self.emit(f"float s{i * 4 + j} = c->v[{sm[i * 4 + j]}].f;")
            d = []
            for i in range(m):
                terms = [f"s{i * 4 + k} * {v[k]}" for k in range(m)]
                d.append(f"vfpu_dot{m}({', '.join(terms)})")
            self.write_d(d, m, VD(raw))
            return True
        if sub < 20:   # vmscl
            k = vec_regs(1, VT(raw))[0]
            sm, dm = mtx_regs(n, VS(raw)), mtx_regs(n, VD(raw))
            self.emit(f"float k = c->v[{k}].f;")
            for j in range(n):
                for i in range(n):
                    self.emit(f"float s{j * 4 + i} = c->v[{sm[j * 4 + i]}].f;")
            for j in range(n):
                for i in range(n):
                    self.emit(f"c->v[{dm[j * 4 + i]}].f = s{j * 4 + i} * k;")
            return True
        if sub < 24:   # vcrsp.t / vqmul.q
            if n not in (3, 4):
                return False
            a = self.src(n, VS(raw), 0, "a")
            b = self.load(n, VT(raw), "b")
            if n == 3:
                d = [f"{a[1]} * {b[2]} - {a[2]} * {b[1]}", f"{a[2]} * {b[0]} - {a[0]} * {b[2]}",
                     f"{a[0]} * {b[1]} - {a[1]} * {b[0]}"]
            else:
                d = [f"{a[0]} * {b[3]} + {a[1]} * {b[2]} - {a[2]} * {b[1]} + {a[3]} * {b[0]}",
                     f"-{a[0]} * {b[2]} + {a[1]} * {b[3]} + {a[2]} * {b[0]} + {a[3]} * {b[1]}",
                     f"{a[0]} * {b[1]} - {a[1]} * {b[0]} + {a[2]} * {b[3]} + {a[3]} * {b[2]}",
                     f"-{a[0]} * {b[0]} - {a[1]} * {b[1]} - {a[2]} * {b[2]} + {a[3]} * {b[3]}"]
            self.write_d(d, n, VD(raw))
            return True
        if sub == 28:
            fn = (raw >> 16) & 15
            dm = mtx_regs(n, VD(raw))
            if fn == 0:   # vmmov
                sm = mtx_regs(n, VS(raw))
                for j in range(n):
                    for i in range(n):
                        self.emit(f"float s{j * 4 + i} = c->v[{sm[j * 4 + i]}].f;")
                for j in range(n):
                    for i in range(n):
                        self.emit(f"c->v[{dm[j * 4 + i]}].f = s{j * 4 + i};")
                return True
            if fn not in (3, 6, 7):
                return False
            for j in range(n):
                for i in range(n):
                    v = ("1.f" if i == j else "0.f") if fn == 3 else ("1.f" if fn == 7 else "0.f")
                    self.emit(f"c->v[{dm[j * 4 + i]}].f = {v};")
            return True
        if sub == 29:   # vrot
            imm = (raw >> 16) & 31
            sin_lane, cos_lane = (imm >> 2) & 3, imm & 3
            sr = vec_regs(1, VS(raw))[0]
            self.emit(f"float ang = c->v[{sr}].f;")
            self.emit("float sn = vfpu_qsin(ang), cs = vfpu_qcos(ang);")
            if imm & 0x10:
                self.emit("sn = -sn;")
            r = ["0.f"] * 4
            if sin_lane == cos_lane:
                r = ["sn"] * 4
            else:
                r[sin_lane] = "sn"
            r[cos_lane] = "cs"
            self.write_d(r[:n], n, VD(raw))
            return True
        return False

    def dispatch(self):
        raw = self.raw
        major = raw >> 26
        if major in (0x18, 0x19, 0x1B):
            return self.alu3()
        if major == 0x34:
            return self.vfpu4jump()
        if major == 0x3C:
            return self.vfpu6()
        if major == 0x37:   # viim / vfim (the prefix instructions are handled by translate())
            if (raw >> 23) & 7 == 6:
                ival = struct.unpack("<h", struct.pack("<H", raw & 0xFFFF))[0]
                v = float_lit(struct.unpack("<I", struct.pack("<f", float(ival)))[0])
            else:
                v = float_lit(half_to_float_bits(raw & 0xFFFF))
            self.write_d([v], 1, VT(raw))
            return True
        return False


def translate(raw, addr, st):
    """Translate one VFPU instruction.

    Returns (lines, new_state), or None when the instruction is left to
    vfpu_exec().  `st` is the known prefix state (see the module doc).
    """
    major = raw >> 26
    st = list(st)
    # -- prefix instructions: remember the value, store it ------------------
    if major == 0x37 and ((raw >> 23) & 7) < 6:
        which = ((raw >> 23) & 7) >> 1
        val = raw & (0xFFF if which == 2 else 0xFFFFF)
        st[which] = val
        return [f"c->vfpu_ctrl[{which}] = {val:#x}u;"], tuple(st)
    if major == 0x3F:
        return [";"], tuple(st)
    # -- loads and stores (no prefixes involved) ---------------------------------
    if major in (0x32, 0x3A):
        base = (raw >> 21) & 31
        off = ((raw & 0xFFFC) ^ 0x8000) - 0x8000
        reg = ((raw >> 16) & 0x1F) | ((raw & 3) << 5)
        a = f"c->r[{base}] + {off}" if off else f"c->r[{base}]"
        if major == 0x32:
            return [f"c->v[{reg}].u = mem_r32({a});"], tuple(st)
        return [f"mem_w32({a}, c->v[{reg}].u);"], tuple(st)
    if major in (0x36, 0x3E):
        base = (raw >> 21) & 31
        off = ((raw & 0xFFFC) ^ 0x8000) - 0x8000
        vt = ((raw >> 16) & 0x1F) | ((raw & 1) << 5)
        r = vec_regs(4, vt)
        a = f"c->r[{base}] + {off}" if off else f"c->r[{base}]"
        fn = "vfpu_lvq" if major == 0x36 else "vfpu_svq"
        return [f"{fn}(c, {a}, {r[0]}, {r[1]}, {r[2]}, {r[3]});"], tuple(st)
    if major == 0x12:   # mfv / mfvc / mtv / mtvc
        imm, rt, kind = raw & 0xFF, (raw >> 16) & 31, (raw >> 21) & 31
        if kind == 3:
            if not rt:
                return [";"], tuple(st)
            if imm < 128:
                return [f"c->r[{rt}] = c->v[{imm}].u;"], tuple(st)
            return [f"c->r[{rt}] = c->vfpu_ctrl[{(imm - 128) & 15}];"], tuple(st)
        if kind == 7:
            if imm < 128:
                return [f"c->v[{imm}].u = c->r[{rt}];"], tuple(st)
            ci = (imm - 128) & 15
            if ci < 3:
                st[ci] = None
            return [f"c->vfpu_ctrl[{ci}] = c->r[{rt}];"], tuple(st)
        return None
    if major not in (0x18, 0x19, 0x1B, 0x34, 0x3C, 0x37):
        return None

    g = Gen(raw, addr, st)
    body_start = len(g.out)
    if not g.dispatch():
        return None
    # Prefixes the operation reads that are not known: guard on their default.
    unknown = [w for w in sorted(g.used) if st[w] is None]
    lines = []
    if unknown:
        cond = " && ".join(f"c->vfpu_ctrl[{w}] == {DEFAULTS[w]:#x}u" for w in unknown)
        # the unknown prefixes are taken to be the defaults inside the guard
        lines.append(f"if (__builtin_expect({cond}, 1)) {{")
    else:
        lines.append("{")
    lines += ["    " + l for l in g.out]
    # eat_prefixes(): reset what is not already the default.  After a guard
    # the unchecked-but-unknown registers also need the store.
    for w in range(3):
        known = st[w]
        guarded = w in unknown
        if known is None and not guarded:
            lines.append(f"    c->vfpu_ctrl[{w}] = {DEFAULTS[w]:#x}u;")
        elif known is not None and known != DEFAULTS[w]:
            lines.append(f"    c->vfpu_ctrl[{w}] = {DEFAULTS[w]:#x}u;")
    if unknown:
        lines.append(f"}} else {{ vfpu_exec(c, {addr:#010x}u, {raw:#010x}u); }}")
    else:
        lines.append("}")
    return lines, DEFAULTS
