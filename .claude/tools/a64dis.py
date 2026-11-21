#!/usr/bin/env python3
# Disassemble A64 words with binutils: dis.py 0x4e100420 ... (".inst" = invalid)
import subprocess, sys, tempfile, os, re
words = [int(w, 16) for w in sys.argv[1:]]
with tempfile.TemporaryDirectory() as d:
    b = os.path.join(d, 'a.bin')
    open(b, 'wb').write(b''.join(w.to_bytes(4, 'little') for w in words))
    out = subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64', b], capture_output=True, text=True).stdout
for l in out.splitlines():
    m = re.match(r'^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)$', l)
    if m:
        print('0x' + m.group(1), m.group(2).replace('\t', ' '))
