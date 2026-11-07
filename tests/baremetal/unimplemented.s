// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Start with an instruction that smirror does not implement yet, to
// check that it stops with an error.

	.globl	_start
_start:
	mrs	x0, CurrentEL
