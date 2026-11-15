/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Advanced SIMD classes of C4.1 that move elements between and within
 * registers, which simd.c gives to this file: table lookup (TBL, TBX),
 * permute (UZP1, UZP2, TRN1, TRN2, ZIP1, ZIP2) and extract (EXT). Each
 * instruction follows its description in C7.2.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/*
 * Advanced SIMD table lookup, 0 Q 001110 op2 0 Rm 0 len op 00 Rn Rd: each
 * byte of Vm indexes the table of len + 1 registers from Vn, V31 followed
 * by V0. An index past the table gives 0 for TBL and leaves the byte of
 * Vd for TBX.
 */
Arm64Exec arm64_simd_table_lookup(Arm64Cpu *c, uint32_t insn)
{
	bool q = bit32(insn, 30), tbx = bit32(insn, 12);
	unsigned m = field32(insn, 20, 16), regs = field32(insn, 14, 13) + 1;
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	Arm64Exec result;

	if (field32(insn, 23, 22) != 0) /* UNALLOCATED, or LUTI4 (FEAT_LUT) */
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	Arm64Vreg table[4], indices = c->v[m], r = c->v[d];

	for (unsigned i = 0; i < regs; i++)
		table[i] = c->v[(n + i) % 32];
	for (unsigned e = 0; e < (q ? 16u : 8u); e++) {
		unsigned index = (unsigned)vreg_elem(&indices, e, 1);

		if (index < 16 * regs)
			vreg_set_elem(&r, e, 1,
			              vreg_elem(&table[index / 16], index % 16, 1));
		else if (!tbx)
			vreg_set_elem(&r, e, 1, 0);
	}
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD permute, 0 Q 001110 size 0 Rm 0 opcode 10 Rn Rd: opcode
 * 001 UZP1, 010 TRN1, 011 ZIP1, and 101, 110 and 111 the second forms.
 */
Arm64Exec arm64_simd_permute(Arm64Cpu *c, uint32_t insn)
{
	bool q = bit32(insn, 30);
	unsigned size = field32(insn, 23, 22), m = field32(insn, 20, 16);
	unsigned opcode = field32(insn, 14, 12), part = opcode >> 2;
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned esize = 1u << size, elements = (q ? 16 : 8) / esize;
	unsigned pairs = elements / 2;
	Arm64Exec result;

	if ((opcode & 3) == 0 || (size == 3 && !q))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[n], vm = c->v[m];
	Arm64Vreg r = {{0, 0}};

	for (unsigned e = 0; e < elements; e++) {
		unsigned i;
		const Arm64Vreg *src;

		switch (opcode & 3) {
		case 1: /* UZP: element 2e + part of Vm:Vn */
			i = 2 * e + part;
			src = i < elements ? &vn : &vm;
			i %= elements;
			break;
		case 2: /* TRN: Vn's of the pair in the even place, Vm's in the odd */
			i = (e & ~1u) + part;
			src = (e & 1) ? &vm : &vn;
			break;
		default: /* ZIP: from the lower half, or the upper for ZIP2 */
			i = part * pairs + e / 2;
			src = (e & 1) ? &vm : &vn;
			break;
		}
		vreg_set_elem(&r, e, esize, vreg_elem(src, i, esize));
	}
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD extract, 0 Q 101110 op2 0 Rm 0 imm4 0 Rn Rd: EXT, the
 * bytes of Vm:Vn from byte imm4 on.
 */
Arm64Exec arm64_simd_extract(Arm64Cpu *c, uint32_t insn)
{
	bool q = bit32(insn, 30);
	unsigned m = field32(insn, 20, 16), imm4 = field32(insn, 14, 11);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned datasize = q ? 16 : 8;
	Arm64Exec result;

	if (field32(insn, 23, 22) != 0 || (!q && (imm4 & 8)))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg vn = c->v[n], vm = c->v[m];
	Arm64Vreg r = {{0, 0}};

	for (unsigned e = 0; e < datasize; e++) {
		unsigned i = imm4 + e;

		vreg_set_elem(&r, e, 1,
		              i < datasize ? vreg_elem(&vn, i, 1)
		                           : vreg_elem(&vm, i - datasize, 1));
	}
	set_v(c, d, r.d[0], q ? r.d[1] : 0);
	return ARM64_EXEC_OK;
}
