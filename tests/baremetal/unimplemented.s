// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Start with an instruction that smirror does not implement yet, to
// check that it stops with an error: floating-point arithmetic.

	.globl	_start
_start:
	fadd	d0, d1, d2
