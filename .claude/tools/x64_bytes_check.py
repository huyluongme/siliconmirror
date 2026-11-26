#!/usr/bin/env python3
# Check the expected bytes of tests/unit/test_jit_x64.c against GNU as.
# Each `CASE(code, bytes...);` followed by a comment `/* asm */` (Intel
# syntax, `;` between instructions) is assembled with `as --64` and the
# bytes compared. Cases without a comment are skipped.
# Usage (from the repo): python3 .claude/tools/x64_bytes_check.py [FILE]
import os, re, subprocess, sys, tempfile

path = sys.argv[1] if len(sys.argv) > 1 else 'tests/unit/test_jit_x64.c'
src = open(path).read()


def split_args(text, start):
	"""The top-level arguments of the call whose '(' is at start, and the
	index after its ')'."""
	args, depth, cur, i = [], 0, '', start + 1
	while True:
		c = text[i]
		if c in '({[':
			depth += 1
		elif c in ')}]':
			if depth == 0:
				args.append(cur.strip())
				return args, i + 1
			depth -= 1
		elif c == ',' and depth == 0:
			args.append(cur.strip())
			cur = ''
			i += 1
			continue
		cur += c
		i += 1


cases = []
for m in re.finditer(r'\bCASE\(', src):
	args, end = split_args(src, m.end() - 1)
	tail = re.match(r'\s*;\s*/\*(.*?)\*/', src[end:], re.S)
	if not tail or not re.match(r'\s*[a-z{]', tail.group(1)):
		continue
	line = src.count('\n', 0, m.start()) + 1
	want = bytes(int(a, 16) for a in args[1:])
	cases.append((line, ' '.join(tail.group(1).split()), want))

bad = 0
with tempfile.TemporaryDirectory() as tmp:
	for line, asm, want in cases:
		s, o, b = (os.path.join(tmp, n) for n in ('t.s', 't.o', 't.bin'))
		with open(s, 'w') as f:
			f.write('.intel_syntax noprefix\n' + asm.replace(';', '\n') + '\n')
		r = subprocess.run(['as', '--64', '-o', o, s], capture_output=True, text=True)
		if r.returncode:
			print(f'{path}:{line}: as failed on "{asm}": {r.stderr.strip()}')
			bad += 1
			continue
		subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', o, b], check=True)
		got = open(b, 'rb').read()
		if got != want:
			print(f'{path}:{line}: "{asm}": as {got.hex(" ")}, test {want.hex(" ")}')
			bad += 1
print(f'{len(cases)} cases checked, {bad} differ')
sys.exit(1 if bad else 0)
