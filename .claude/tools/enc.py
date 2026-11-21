#!/usr/bin/env python3
# Print the A64 encodings of instructions with GNU as: enc.py 'ldr q0, [x1]' ...
# '-arch=armv8.2-a+fp16' first picks another architecture.
# Output: 0x3dc00020, /* ldr q0, [x1] */
import subprocess, sys, tempfile, os
arch = 'armv8-a'
args = sys.argv[1:]
if args and args[0].startswith('-arch='):
    arch = args.pop(0)[6:]
insns = args
with tempfile.TemporaryDirectory() as d:
    src = os.path.join(d, 'a.s'); obj = os.path.join(d, 'a.o')
    open(src, 'w').write('.arch ' + arch + '\n' + ''.join(i + '\n' for i in insns))
    subprocess.run(['aarch64-linux-gnu-as', src, '-o', obj], check=True)
    out = subprocess.run(['aarch64-linux-gnu-objdump', '-d', obj], capture_output=True, text=True).stdout
import re
words = [m.group(1) for m in (re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s', l) for l in out.splitlines()) if m]
for w, i in zip(words, insns):
    print(f'0x{w}, /* {i} */')
