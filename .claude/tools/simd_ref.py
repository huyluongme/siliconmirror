#!/usr/bin/env python3
"""Reference model of integer Advanced SIMD instructions, for test tables.

Written from the instruction descriptions of the Arm ARM (C7.2), apart
from the emulator's C code. Each case is an assembly line and the input
registers V0 (destination, also an accumulator) and V1, V2 (sources);
the script assembles it with GNU as, computes V0 and FPSR.QC and prints a
C table row: {insn, v1 lo, v1 hi, v2 lo, v2 hi, v0 lo, v0 hi, want lo,
want hi, qc}, with the assembly line as a comment.

Usage: simd_ref.py CASES_FILE   (a Python file defining CASES =
[(asm, v0, v1[, v2]), ...] where each v is (lo, hi)).
"""
import os
import re
import subprocess
import sys
import tempfile

M64 = (1 << 64) - 1


def assemble(lines):
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, 'a.s'), os.path.join(d, 'a.o')
        open(src, 'w').write('.arch armv8-a\n' + ''.join(l + '\n' for l in lines))
        subprocess.run(['aarch64-linux-gnu-as', src, '-o', obj], check=True)
        out = subprocess.run(['aarch64-linux-gnu-objdump', '-d', obj],
                             capture_output=True, text=True).stdout
    return [int(m.group(1), 16) for m in
            (re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s', l) for l in out.splitlines()) if m]


SIZES = {'b': 8, 'h': 16, 's': 32, 'd': 64}


def operand(tok):
    """('v', reg, lane bits, lanes) for v1.16b, ('s', reg, bits, 1) for s1."""
    tok = tok.strip()
    m = re.match(r'v(\d+)\.(\d+)([bhsd])$', tok)
    if m:
        return ('v', int(m.group(1)), SIZES[m.group(3)], int(m.group(2)))
    m = re.match(r'v(\d+)\.([bhsd])\[(\d+)\]$', tok)
    if m:
        return ('e', int(m.group(1)), SIZES[m.group(2)], int(m.group(3)))
    m = re.match(r'([bhsd])(\d+)$', tok)
    if m:
        return ('s', int(m.group(2)), SIZES[m.group(1)], 1)
    return ('imm', tok)


def lanes(value, bits, count):
    return [(value >> (i * bits)) & ((1 << bits) - 1) for i in range(count)]


def join(values, bits):
    r = 0
    for i, v in enumerate(values):
        r |= (v & ((1 << bits) - 1)) << (i * bits)
    return r


def s(x, bits):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


class Sat:
    def __init__(self):
        self.qc = 0

    def signed(self, x, bits):
        hi, lo = (1 << (bits - 1)) - 1, -(1 << (bits - 1))
        if x > hi or x < lo:
            self.qc = 1
            x = hi if x > hi else lo
        return x & ((1 << bits) - 1)

    def unsigned(self, x, bits):
        hi = (1 << bits) - 1
        if x > hi or x < 0:
            self.qc = 1
            x = hi if x > hi else 0
        return x


def clz(x, bits):
    n = 0
    while n < bits and not (x >> (bits - 1 - n)) & 1:
        n += 1
    return n


SHIFTS = {'sshr', 'ushr', 'ssra', 'usra', 'srshr', 'urshr', 'srsra', 'ursra', 'sri', 'sli',
          'shl', 'sqshl', 'uqshl', 'sqshlu', 'shrn', 'rshrn', 'sqshrn', 'sqrshrn', 'sqshrun',
          'sqrshrun', 'uqshrn', 'uqrshrn', 'sshll', 'ushll'}


def shift_op(base, upper, dst, src, a, acc, shift, V, sat):
    """The shift by immediate instructions (C7.2), vector and scalar."""
    def mask(bits):
        return (1 << bits) - 1
    scalar = dst[0] == 's'
    if base in ('shrn', 'rshrn', 'sqshrn', 'sqrshrn', 'sqshrun', 'sqrshrun', 'uqshrn', 'uqrshrn'):
        nb = dst[2]
        count = 1 if scalar else 64 // nb
        out = []
        for e in range(count):
            x = a[e]
            signed_in = base.startswith('sq')
            v = s(x, 2 * nb) if signed_in else x
            if 'rshr' in base:
                v += 1 << (shift - 1)
            v >>= shift
            if base in ('shrn', 'rshrn'):
                out.append(v & mask(nb))
            elif base.endswith('un'):
                out.append(sat.unsigned(v, nb))
            elif signed_in:
                out.append(sat.signed(v, nb))
            else:
                out.append(sat.unsigned(v, nb))
        if scalar:
            r = out[0]
        elif upper:
            r = (V[0] & M64) | join(out, nb) << 64
        else:
            r = join(out, nb)
        return r & M64, r >> 64, sat.qc
    if base in ('sshll', 'ushll'):
        eb = src[2]
        count = 64 // eb
        start = count if upper else 0
        out = [((s(a[start + e], eb) if base == 'sshll' else a[start + e]) << shift) & mask(2 * eb)
               for e in range(count)]
        r = join(out, 2 * eb)
        return r & M64, r >> 64, sat.qc
    eb = dst[2]
    count = 1 if scalar else dst[3]
    out = []
    for e in range(count):
        x, d = a[e], acc[e]
        if base in ('sshr', 'ssra', 'srshr', 'srsra', 'ushr', 'usra', 'urshr', 'ursra'):
            v = s(x, eb) if base[0] == 's' else x
            if 'rs' in base[1:3]:
                v += 1 << (shift - 1)
            v >>= shift
            if base.endswith('ra'):
                v += d
            out.append(v & mask(eb))
        elif base == 'sri':
            keep = mask(eb) >> shift
            out.append((d & ~keep & mask(eb)) | ((x >> shift) & keep))
        elif base == 'sli':
            keep = (mask(eb) << shift) & mask(eb)
            out.append((d & ~keep & mask(eb)) | ((x << shift) & keep))
        elif base == 'shl':
            out.append((x << shift) & mask(eb))
        elif base == 'sqshl':
            out.append(sat.signed(s(x, eb) << shift, eb))
        elif base == 'uqshl':
            out.append(sat.unsigned(x << shift, eb))
        elif base == 'sqshlu':
            out.append(sat.unsigned(s(x, eb) << shift, eb))
    if scalar:
        r = out[0]
    else:
        r = join(out, eb)
        if dst[2] * dst[3] < 128:
            r &= M64
    return r & M64, r >> 64, sat.qc


SAME = {'shadd', 'uhadd', 'sqadd', 'uqadd', 'srhadd', 'urhadd', 'shsub', 'uhsub', 'sqsub',
        'uqsub', 'cmgt', 'cmhi', 'cmge', 'cmhs', 'sshl', 'ushl', 'sqshl', 'uqshl', 'srshl',
        'urshl', 'sqrshl', 'uqrshl', 'smax', 'umax', 'smin', 'umin', 'sabd', 'uabd', 'saba',
        'uaba', 'add', 'sub', 'cmtst', 'cmeq', 'mla', 'mls', 'mul', 'pmul', 'smaxp', 'umaxp',
        'sminp', 'uminp', 'sqdmulh', 'sqrdmulh', 'addp', 'and', 'bic', 'orr', 'orn', 'eor',
        'bsl', 'bit', 'bif'}


def same_op(base, dst, on, om, V, sat):
    """Advanced SIMD three same, vector and scalar (C7.2)."""
    eb = dst[2]
    count = dst[3]
    scalar = dst[0] == 's'
    bits = 128 if (not scalar and dst[2] * dst[3] == 128) else (64 if not scalar else eb)
    mk = (1 << eb) - 1
    if base in ('and', 'bic', 'orr', 'orn', 'eor', 'bsl', 'bit', 'bif'):
        n, m, d = V[on[1]], V[om[1]], V[dst[1]]
        all1 = (1 << 128) - 1
        r = {'and': n & m, 'bic': n & ~m, 'orr': n | m, 'orn': n | (~m & all1), 'eor': n ^ m,
             'bsl': (d & n) | (~d & m), 'bit': (m & n) | (~m & d), 'bif': (~m & n) | (m & d)}[base]
        r &= (1 << bits) - 1
        return r & M64, r >> 64, sat.qc
    a = lanes(V[on[1]], eb, 128 // eb)
    b = lanes(V[om[1]], eb, 128 // eb)
    acc = lanes(V[dst[1]], eb, 128 // eb)
    u = base[0] == 'u' or base in ('cmhi', 'cmhs')
    f = (lambda x: x) if u else (lambda x: s(x, eb))
    out = []
    if base in ('smaxp', 'umaxp', 'sminp', 'uminp', 'addp'):
        cat = a[:count] + b[:count]
        for e in range(count):
            x, y = f(cat[2 * e]), f(cat[2 * e + 1])
            v = x + y if base == 'addp' else (max(x, y) if 'max' in base else min(x, y))
            out.append(v & mk)
    else:
        for e in range(count):
            x, y, d = f(a[e]), f(b[e]), acc[e]
            if base in ('shadd', 'uhadd'):
                v = (x + y) >> 1
            elif base in ('srhadd', 'urhadd'):
                v = (x + y + 1) >> 1
            elif base in ('shsub', 'uhsub'):
                v = (x - y) >> 1
            elif base in ('sqadd', 'uqadd'):
                v = sat.unsigned(x + y, eb) if u else sat.signed(x + y, eb)
            elif base in ('sqsub', 'uqsub'):
                v = sat.unsigned(x - y, eb) if u else sat.signed(x - y, eb)
            elif base in ('cmgt', 'cmhi'):
                v = mk if x > y else 0
            elif base in ('cmge', 'cmhs'):
                v = mk if x >= y else 0
            elif base in ('sshl', 'ushl', 'sqshl', 'uqshl', 'srshl', 'urshl', 'sqrshl', 'uqrshl'):
                sh = s(b[e] & 0xff, 8)
                rnd = 'rshl' in base
                val = x
                if sh >= 0:
                    val = val << sh
                else:
                    if rnd:
                        val += 1 << (-sh - 1)
                    val >>= -sh
                if base.startswith(('sq', 'uq')):
                    v = sat.unsigned(val, eb) if u else sat.signed(val, eb)
                else:
                    v = val
            elif base in ('smax', 'umax'):
                v = max(x, y)
            elif base in ('smin', 'umin'):
                v = min(x, y)
            elif base in ('sabd', 'uabd'):
                v = abs(x - y)
            elif base in ('saba', 'uaba'):
                v = d + abs(x - y)
            elif base == 'add':
                v = a[e] + b[e]
            elif base == 'sub':
                v = a[e] - b[e]
            elif base == 'cmtst':
                v = mk if a[e] & b[e] else 0
            elif base == 'cmeq':
                v = mk if a[e] == b[e] else 0
            elif base == 'mla':
                v = d + a[e] * b[e]
            elif base == 'mls':
                v = d - a[e] * b[e]
            elif base == 'mul':
                v = a[e] * b[e]
            elif base == 'pmul':
                v = 0
                for i in range(8):
                    if (b[e] >> i) & 1:
                        v ^= a[e] << i
            elif base in ('sqdmulh', 'sqrdmulh'):
                p = 2 * s(a[e], eb) * s(b[e], eb)
                if base == 'sqrdmulh':
                    p += 1 << (eb - 1)
                v = sat.signed(p >> eb, eb)
            out.append(v & mk)
    r = out[0] if scalar else join(out, eb)
    r &= (1 << bits) - 1
    return r & M64, r >> 64, sat.qc


def perm_op(mn, rest, V):
    """TBL, TBX, the permutes and EXT (C7.2)."""
    if mn in ('tbl', 'tbx'):
        m = re.match(r'\s*v(\d+)\.(\d+)b,\s*\{(.*)\},\s*v(\d+)\.\d+b', rest)
        d, nb, regs, mreg = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))
        table = [int(x.strip().split('.')[0][1:]) for x in regs.split(',')]
        tbytes = []
        for r in table:
            tbytes += lanes(V[r], 8, 16)
        idx = lanes(V[mreg], 8, 16)
        cur = lanes(V[d], 8, 16)
        out = [tbytes[i] if i < len(tbytes) else (cur[e] if mn == 'tbx' else 0)
               for e, i in enumerate(idx[:nb])]
        r = join(out, 8)
        return r & M64, r >> 64, 0
    ops = [operand(t) for t in rest.split(',')]
    d, n, m = ops[0], ops[1], ops[2]
    eb, count = d[2], d[3]
    a = lanes(V[n[1]], eb, 128 // eb)[:count]
    b = lanes(V[m[1]], eb, 128 // eb)[:count]
    if mn == 'ext':
        pos = int(ops[3][1].lstrip('#'), 0)
        cat = a + b
        out = cat[pos:pos + count]
        eb = 8
    elif mn.startswith('uzp'):
        part = int(mn[-1]) - 1
        cat = a + b
        out = [cat[2 * e + part] for e in range(count)]
    elif mn.startswith('trn'):
        part = int(mn[-1]) - 1
        out = []
        for p in range(count // 2):
            out += [a[2 * p + part], b[2 * p + part]]
    else:
        part = int(mn[-1]) - 1
        base = part * (count // 2)
        out = []
        for p in range(count // 2):
            out += [a[base + p], b[base + p]]
    r = join(out, eb)
    return r & M64, r >> 64, 0


LONG = {'saddl', 'uaddl', 'saddw', 'uaddw', 'ssubl', 'usubl', 'ssubw', 'usubw', 'addhn',
        'raddhn', 'subhn', 'rsubhn', 'sabal', 'uabal', 'sabdl', 'uabdl', 'smlal', 'umlal',
        'sqdmlal', 'smlsl', 'umlsl', 'sqdmlsl', 'smull', 'umull', 'sqdmull', 'pmull'}


def long_op(base, upper, ops, V, sat):
    """Three different and by element (C7.2), vector and scalar."""
    d, n, m = ops[0], ops[1], ops[2]
    scalar = d[0] == 's'
    if base in ('addhn', 'raddhn', 'subhn', 'rsubhn'):
        nb = d[2]
        a = lanes(V[n[1]], 2 * nb, 128 // (2 * nb))
        b = lanes(V[m[1]], 2 * nb, 128 // (2 * nb))
        out = []
        for e in range(64 // nb):
            v = a[e] + b[e] if 'add' in base else a[e] - b[e]
            if base[0] == 'r':
                v += 1 << (nb - 1)
            out.append((v >> nb) & ((1 << nb) - 1))
        r = join(out, nb)
        r = (V[0] & M64) | r << 64 if upper else r
        return r & M64, r >> 64, sat.qc
    if base in ('mla', 'mls', 'mul', 'sqdmulh', 'sqrdmulh'):
        eb = d[2]
        count = 1 if scalar else d[3]
        a = lanes(V[n[1]], eb, 128 // eb)
        el = lanes(V[m[1]], eb, 128 // eb)[m[3]]
        acc = lanes(V[d[1]], eb, 128 // eb)
        out = []
        for e in range(count):
            if base == 'mla':
                v = acc[e] + a[e] * el
            elif base == 'mls':
                v = acc[e] - a[e] * el
            elif base == 'mul':
                v = a[e] * el
            else:
                p = 2 * s(a[e], eb) * s(el, eb)
                if base == 'sqrdmulh':
                    p += 1 << (eb - 1)
                v = sat.signed(p >> eb, eb)
            out.append(v & ((1 << eb) - 1))
        r = out[0] if scalar else join(out, eb)
        if not scalar and eb * count < 128:
            r &= M64
        return r & M64, r >> 64, sat.qc
    wide = base in ('saddw', 'uaddw', 'ssubw', 'usubw')
    eb = m[2]
    count = 1 if scalar else 64 // eb
    u = base[0] == 'u'
    f = (lambda x, bits: x) if u else (lambda x, bits: s(x, bits))
    start = count if upper else 0
    if wide:
        a = lanes(V[n[1]], 2 * eb, 128 // (2 * eb))[:count]
    else:
        a = lanes(V[n[1]], eb, 128 // eb)[start:start + count]
    if m[0] == 'e':
        b = [lanes(V[m[1]], eb, 128 // eb)[m[3]]] * count
    else:
        b = lanes(V[m[1]], eb, 128 // eb)[start:start + count]
    acc = lanes(V[d[1]], 2 * eb, 128 // (2 * eb))
    mk = (1 << (2 * eb)) - 1
    out = []
    for e in range(count):
        x = f(a[e], 2 * eb if wide else eb)
        y = f(b[e], eb)
        if base in ('saddl', 'uaddl', 'saddw', 'uaddw'):
            v = x + y
        elif base in ('ssubl', 'usubl', 'ssubw', 'usubw'):
            v = x - y
        elif base in ('sabal', 'uabal'):
            v = acc[e] + abs(x - y)
        elif base in ('sabdl', 'uabdl'):
            v = abs(x - y)
        elif base in ('smlal', 'umlal'):
            v = acc[e] + x * y
        elif base in ('smlsl', 'umlsl'):
            v = acc[e] - x * y
        elif base in ('smull', 'umull'):
            v = x * y
        elif base == 'sqdmull':
            v = sat.signed(2 * x * y, 2 * eb)
        elif base in ('sqdmlal', 'sqdmlsl'):
            p = s(sat.signed(2 * x * y, 2 * eb), 2 * eb)
            v = sat.signed(s(acc[e], 2 * eb) + (p if base == 'sqdmlal' else -p), 2 * eb)
        elif base == 'pmull':
            v = 0
            for i in range(8):
                if (b[e] >> i) & 1:
                    v ^= a[e] << i
        out.append(v & mk)
    r = out[0] if scalar else join(out, 2 * eb)
    return r & M64, r >> 64, sat.qc


def run(asm, v0, v1, v2):
    mn, _, rest = asm.partition(' ')
    ops = [operand(t) for t in rest.split(',')]
    V = {0: v0[0] | v0[1] << 64, 1: v1[0] | v1[1] << 64, 2: v2[0] | v2[1] << 64}
    V.update({r: 0 for r in range(3, 32)})  # the test machine resets them to 0
    if mn in ('tbl', 'tbx', 'uzp1', 'uzp2', 'trn1', 'trn2', 'zip1', 'zip2', 'ext'):
        return perm_op(mn, rest, V)
    sat = Sat()
    dst = ops[0]
    src = ops[1]
    a = lanes(V[src[1]], src[2], 128 // src[2])
    acc = lanes(V[dst[1]], dst[2], 128 // dst[2])
    eb = src[2]
    n_src = src[3]
    full = dst[0] == 'v' and dst[2] * dst[3] == 128

    def mask(bits):
        return (1 << bits) - 1

    result = None  # list of lanes of dst size, count dst[3]
    keep_low = False
    base = mn.rstrip('2') if mn.endswith('2') and mn not in ('rev32',) else mn
    upper = mn.endswith('2') and mn != 'rev32'
    if base in LONG or (base in ('mla', 'mls', 'mul', 'sqdmulh', 'sqrdmulh') and ops[2][0] == 'e'):
        return long_op(base, upper, ops, V, sat)
    if len(ops) == 3 and ops[2][0] in ('v', 's') and base in SAME:
        return same_op(base, dst, ops[1], ops[2], V, sat)
    if base in ('rev64', 'rev32', 'rev16'):
        c = int(base[3:])
        per = c // eb
        result = [a[e - e % per + per - 1 - e % per] for e in range(n_src)]
    elif base in ('saddlp', 'uaddlp', 'sadalp', 'uadalp'):
        sg = base[0] == 's'
        f = (lambda x: s(x, eb)) if sg else (lambda x: x)
        result = []
        for e in range(dst[3]):
            v = f(a[2 * e]) + f(a[2 * e + 1])
            if base.endswith('alp'):
                v += acc[e]
            result.append(v & mask(2 * eb))
    elif base == 'suqadd':
        result = [sat.signed(s(acc[e], eb) + a[e], eb) for e in range(n_src)]
    elif base == 'usqadd':
        result = [sat.unsigned(acc[e] + s(a[e], eb), eb) for e in range(n_src)]
    elif base == 'cls':
        result = [clz(((x >> 1) ^ x) & mask(eb - 1), eb - 1) for x in a[:n_src]]
    elif base == 'clz':
        result = [clz(x, eb) for x in a[:n_src]]
    elif base == 'cnt':
        result = [bin(x).count('1') for x in a[:n_src]]
    elif base in ('not', 'mvn'):
        result = [~x & 0xff for x in a[:n_src]]
    elif base == 'rbit':
        result = [int(f'{x:08b}'[::-1], 2) for x in a[:n_src]]
    elif base == 'sqabs':
        result = [sat.signed(abs(s(x, eb)), eb) for x in a[:n_src]]
    elif base == 'sqneg':
        result = [sat.signed(-s(x, eb), eb) for x in a[:n_src]]
    elif base in ('cmgt', 'cmge', 'cmeq', 'cmle', 'cmlt'):
        cond = {'cmgt': lambda v: v > 0, 'cmge': lambda v: v >= 0, 'cmeq': lambda v: v == 0,
                'cmle': lambda v: v <= 0, 'cmlt': lambda v: v < 0}[base]
        result = [mask(eb) if cond(s(x, eb)) else 0 for x in a[:n_src]]
    elif base == 'abs':
        result = [abs(s(x, eb)) & mask(eb) for x in a[:n_src]]
    elif base == 'neg':
        result = [-s(x, eb) & mask(eb) for x in a[:n_src]]
    elif base in ('xtn', 'sqxtn', 'uqxtn', 'sqxtun'):
        nb = dst[2]
        count = 64 // nb
        f = {'xtn': lambda x: x & mask(nb),
             'sqxtn': lambda x: sat.signed(s(x, 2 * nb), nb),
             'uqxtn': lambda x: sat.unsigned(x, nb),
             'sqxtun': lambda x: sat.unsigned(s(x, 2 * nb), nb)}[base]
        if dst[0] == 's':
            count = 1
        narrowed = [f(x) for x in a[:count]]
        if dst[0] == 's':
            out = narrowed[0]
        elif upper:
            out = (V[0] & M64) | join(narrowed, nb) << 64
        else:
            out = join(narrowed, nb)
        return out & M64, out >> 64, sat.qc
    elif base == 'shll':
        count = 64 // eb
        start = count if upper else 0
        result = [a[start + e] << eb for e in range(count)]
    elif ops[-1][0] == 'imm' and base in SHIFTS:
        return shift_op(base, upper, dst, src, a, acc, int(ops[-1][1].lstrip('#'), 0), V, sat)
    elif base in ('saddlv', 'uaddlv', 'smaxv', 'umaxv', 'sminv', 'uminv', 'addv'):
        f = (lambda x: s(x, eb)) if base[0] == 's' else (lambda x: x)
        vals = [f(x) for x in a[:n_src]]
        v = {'saddlv': sum, 'uaddlv': sum, 'addv': sum,
             'smaxv': max, 'umaxv': max, 'sminv': min, 'uminv': min}[base](vals)
        out = v & mask(dst[2])
        return out & M64, out >> 64, sat.qc
    elif base == 'addp' and dst[0] == 's':
        out = (a[0] + a[1]) & M64
        return out, 0, sat.qc
    else:
        raise SystemExit('no model for ' + asm)
    if dst[0] == 's':
        out = result[0] & mask(dst[2])
    else:
        out = join(result, dst[2])
        if not full:
            out &= M64
    return out & M64, out >> 64, sat.qc


def main():
    ns = {}
    exec(open(sys.argv[1]).read(), ns)
    cases = ns['CASES']
    words = assemble([c[0] for c in cases])
    for (case, w) in zip(cases, words):
        asm, v0, v1 = case[0], case[1], case[2]
        v2 = case[3] if len(case) > 3 else (0, 0)
        lo, hi, qc = run(asm, v0, v1, v2)
        print(f'\t{{0x{w:08x}, 0x{v1[0]:016x}, 0x{v1[1]:016x}, 0x{v2[0]:016x}, 0x{v2[1]:016x},'
              f' 0x{v0[0]:016x}, 0x{v0[1]:016x}, 0x{lo:016x}, 0x{hi:016x}, {qc}}},'
              f' /* {asm} */')


main()
