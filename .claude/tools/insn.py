#!/usr/bin/env python3
# Print a C7/C6 instruction description of the Arm ARM text without page
# noise: insn.py 'C7.2.230' [max_lines]. Pseudocode lines keep indentation.
import os, re, sys
sec = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 400
lines = open(os.path.join(os.environ.get('SMIRROR_SPECS', os.path.expanduser('~/specs')), 'armarm.txt'), errors='replace').read().split('\n')
start = next(i for i, l in enumerate(lines) if re.match(r'\s*' + re.escape(sec) + r'\s', l) and i > 150000)
head = re.compile(r'^\s*C[67]\.2\.\d+\s')
noise = re.compile(r'Copyright ©|Non-Confidential|^ARM DDI|^M\.d\s*$|^\s*C[67]\.2 Alphabetical|^\s*A64 Base|^\s*A64 Advanced')
out = []
for l in lines[start + 1:]:
    if head.match(l):
        break
    if not l.strip() or noise.search(l):
        continue
    out.append(l.rstrip())
print(lines[start].strip())
print('\n'.join(out[:limit]))
