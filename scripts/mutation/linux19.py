# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Floating point (step E3), in the working tree: every line of fp.c,
# fp_scalar.c and simd_fp.c, and the lines of the Advanced SIMD files
# changed since eff7bf1.
SNAPSHOT = None
FILES = [
    {"path": "src/cpu/arm64/fp.c", "lines": "all"},
    {"path": "src/cpu/arm64/fp_scalar.c", "lines": "all"},
    {"path": "src/cpu/arm64/simd_fp.c", "lines": "all"},
    {"path": "src/cpu/arm64/simd.c", "lines": "since:eff7bf1"},
    {"path": "src/cpu/arm64/simd_int.c", "lines": "since:eff7bf1"},
    {"path": "src/cpu/arm64/simd_same.c", "lines": "since:eff7bf1"},
    {"path": "src/cpu/arm64/simd_shift.c", "lines": "since:eff7bf1"},
    {"path": "src/cpu/arm64/simd_long.c", "lines": "since:eff7bf1"},
]
WORKERS = 4
JOBS = 3
