#!/usr/bin/env python3
# Compile files of the repo with gcc 13's cc1 (CI's compiler, Ubuntu 24.04)
# using the flags of a build's compile_commands.json.
# Usage (from the repo): python3 .claude/tools/gcc13_check.py [BUILD] [SUBSTRING...]
# BUILD defaults to build/ci-release-gcc; SUBSTRINGs filter the files (default: all).
import json, os, shlex, subprocess, sys
cc1 = os.path.expanduser('~/.cache/siliconmirror-tools/gcc13/root/usr/libexec/gcc/x86_64-linux-gnu/13/cc1')
if not os.path.exists(cc1):
	sys.exit('no gcc 13 cc1: run sh .claude/tools/setup-gcc13.sh first')
build = sys.argv[1] if len(sys.argv) > 1 else 'build/ci-release-gcc'
subs = sys.argv[2:]
inc = subprocess.run(['gcc', '-print-file-name=include'], capture_output=True, text=True).stdout.strip()
bad = 0
for e in json.load(open(build + '/compile_commands.json')):
    f = e['file']
    if subs and not any(s in f for s in subs):
        continue
    keep, skip = [], False
    for a in shlex.split(e['command'])[1:]:
        if skip:
            skip = False; continue
        if a in ('-o', '-c', '-MF', '-MT', '-MD', '-MQ'):
            skip = a in ('-o', '-MF', '-MT', '-MQ'); continue
        if a.endswith('.c'):
            continue
        keep.append(a)
    r = subprocess.run([cc1, '-quiet', '-imultiarch', 'x86_64-linux-gnu', '-isystem', inc] + keep + [f, '-o', '/dev/null'],
                       capture_output=True, text=True, cwd=e['directory'])
    if r.returncode or r.stderr.strip():
        bad += 1
        print(f, r.stderr.strip().splitlines()[:3])
print('gcc13 problems:', bad)
