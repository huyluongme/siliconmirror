#!/usr/bin/env python3
"""Sweep the floating-point parts of the Advanced SIMD classes and say,
from binutils, what an Armv8.0 core without FEAT_FP16 does with each
encoding: 0 UNDEFINED (binutils knows no instruction there, or one of a
later feature), 1 an Armv8.0 floating-point instruction (not implemented
yet in E2). Prints C rows {insn, expected}, six to a line."""
import os, re, subprocess, tempfile

LATER = re.compile(r'^(frint32|frint64|bf|f1cvtl|f2cvtl|famax|famin|fscale|fmlal|fmlsl|fcmla|fcadd|fdot|luti|fcvtn2? v\d+\.(8b|16b)|fmlall)')

def disassemble(words):
    with tempfile.TemporaryDirectory() as d:
        b = os.path.join(d, 'a.bin')
        open(b, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in words))
        out = subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64', b],
                             capture_output=True, text=True).stdout
    res = {}
    for l in out.splitlines():
        m = re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)$', l)
        if m:
            res[int(m.group(1), 16)] = m.group(2).strip()
    return [res[w] for w in words]

def expected(text):
    if text.startswith('.inst') or text.startswith('udf'):
        return 0
    parts = text.split(None, 1)
    mn, ops = parts[0], (parts[1] if len(parts) > 1 else '')
    ops = ops.split('//')[0]
    if LATER.match(mn + ' ' + ops):
        return 0
    # half precision arithmetic: FEAT_FP16. FCVTN and FCVTL convert to and
    # from half precision in Armv8.0.
    if (re.search(r'\.(4h|8h|2h|h)\b|\bh\d+\b', ops) and mn[0] in 'fsu'
            and not mn.startswith(('sqdm', 'sqrdm', 'fcvtn', 'fcvtl'))):
        return 0
    return 1

words = []
for u in (0, 1):
    for size in range(4):
        for op in list(range(0x0c, 0x10)) + list(range(0x16, 0x20)):
            # two-register misc, vector Q=1 and scalar
            words.append(0x4e200800 | u << 29 | size << 22 | op << 12 | 1 << 5)
            words.append(0x5e200800 | u << 29 | size << 22 | op << 12 | 1 << 5)
        for op in range(0x18, 0x20):
            # three same, vector Q=1 and scalar
            words.append(0x4e200400 | u << 29 | size << 22 | 2 << 16 | op << 11 | 1 << 5)
            words.append(0x5e200400 | u << 29 | size << 22 | 2 << 16 | op << 11 | 1 << 5)
        for op in (0x1, 0x5, 0x9):
            # by element, vector Q=1 and scalar, with L = H = M = 0
            words.append(0x4f000000 | u << 29 | size << 22 | 2 << 16 | op << 12 | 1 << 5)
            words.append(0x5f000000 | u << 29 | size << 22 | 2 << 16 | op << 12 | 1 << 5)
        for op in (0x0c, 0x0f):
            for q in (0, 1):
                # across lanes
                words.append(0x0e300800 | q << 30 | u << 29 | size << 22 | op << 12 | 1 << 5)
        for op in (0x0c, 0x0d, 0x0f):
            # scalar pairwise
            words.append(0x5e300800 | u << 29 | size << 22 | op << 12 | 1 << 5)
    for immh in range(1, 16):
        for op in (0x1c, 0x1f):
            for q in (0, 1):
                words.append(0x0f000400 | q << 30 | u << 29 | immh << 19 | op << 11 | 1 << 5)
            words.append(0x5f000400 | u << 29 | immh << 19 | op << 11 | 1 << 5)
texts = disassemble(words)
rows = [f'{{0x{w:08x}, {expected(t)}}}' for w, t in zip(words, texts)]
for i in range(0, len(rows), 6):
    print('\t' + ', '.join(rows[i:i + 6]) + ',')
import sys
print(len(rows), 'rows', sum(expected(t) for t in texts), 'fp', file=sys.stderr)
