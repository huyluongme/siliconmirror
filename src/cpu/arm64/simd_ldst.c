/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Advanced SIMD load/store structure classes of C4.1, with and
 * without post-indexing: LD1, LD2, LD3, LD4, ST1, ST2, ST3 and ST4 of
 * multiple structures and of a single structure, and LD1R, LD2R, LD3R
 * and LD4R, each after its description in C7.2.
 *
 * They access memory one element at a time, in the order of the
 * pseudocode, and write the registers as they go: an access that faults
 * leaves the elements before it loaded or stored, as on the PE whose
 * pseudocode this is. Each element is a SIMD&FP access of its own size,
 * aligned on that size where SCTLR_ELx.A or Device memory asks for it.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* The register after t, wrapping from V31 to V0. */
static unsigned next_register(unsigned t)
{
	return (t + 1) % 32;
}

/* Load or store one element of size bytes of Vt at index. */
static Arm64Exec element_access(Arm64Cpu *c, bool load, unsigned t,
                                unsigned index, unsigned size, uint64_t address)
{
	uint64_t data;

	if (!load) {
		data = get_elem(c, t, index, size);
		return arm64_mem_write_vec(c, address, size, false, &data);
	}

	Arm64Exec result = arm64_mem_read_vec(c, address, size, false, &data);

	if (result == ARM64_EXEC_OK)
		set_elem(c, t, index, size, data);
	return result;
}

/*
 * The writeback of the post-indexed forms: Rm, or, when Rm is 31, the
 * number of bytes accessed.
 */
static void writeback(Arm64Cpu *c, unsigned n, unsigned m, uint64_t address,
                      uint64_t bytes)
{
	set_xsp(c, n, address + (m != 31 ? get_x(c, m) : bytes));
}

/*
 * Multiple structures: 0 Q[30] 001100 post[23] L[22] 0 Rm[20:16]
 * opcode[15:12] size[11:10] Rn Rt. rpt registers of selem elements per
 * structure each, C7.2.204 and the others.
 */
static Arm64Exec multiple(Arm64Cpu *c, uint32_t insn, bool wback)
{
	bool load = bit32(insn, 22);
	unsigned m = field32(insn, 20, 16), n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);
	unsigned size = 1u << field32(insn, 11, 10); /* bytes per element */
	unsigned datasize = bit32(insn, 30) ? 16 : 8;
	unsigned elements = datasize / size;
	unsigned rpt, selem;
	Arm64Exec result;

	switch (field32(insn, 15, 12)) {
	case 0x0: /* LD4, ST4 */
		rpt = 1, selem = 4;
		break;
	case 0x2: /* LD1, ST1: four registers */
		rpt = 4, selem = 1;
		break;
	case 0x4: /* LD3, ST3 */
		rpt = 1, selem = 3;
		break;
	case 0x6: /* LD1, ST1: three registers */
		rpt = 3, selem = 1;
		break;
	case 0x7: /* LD1, ST1: one register */
		rpt = 1, selem = 1;
		break;
	case 0x8: /* LD2, ST2 */
		rpt = 1, selem = 2;
		break;
	case 0xa: /* LD1, ST1: two registers */
		rpt = 2, selem = 1;
		break;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
	/* The .1D arrangement only exists for LD1 and ST1. */
	if (size == 8 && datasize == 8 && selem != 1)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t address = get_xsp(c, n);
	uint64_t offs = 0;

	for (unsigned r = 0; r < rpt; r++) {
		for (unsigned e = 0; e < elements; e++) {
			unsigned tt = (t + r) % 32;

			for (unsigned s = 0; s < selem; s++) {
				result = element_access(c, load, tt, e, size, address + offs);
				if (result != ARM64_EXEC_OK)
					return result;
				/*
				 * A load of a 64-bit arrangement clears the upper
				 * half, V{datasize}(tt) = rval.
				 */
				if (load && datasize == 8)
					c->v[tt].d[1] = 0;
				offs += size;
				tt = next_register(tt);
			}
		}
	}
	if (wback)
		writeback(c, n, m, address, offs);
	return ARM64_EXEC_OK;
}

/*
 * Single structure: 0 Q[30] 001101 post[23] L[22] R[21] Rm[20:16]
 * opcode[15:13] S[12] size[11:10] Rn Rt. One element of each of selem
 * registers, or, for LD1R to LD4R, one element replicated to all of
 * them. C7.2.205 and the others.
 */
static Arm64Exec single(Arm64Cpu *c, uint32_t insn, bool wback)
{
	bool load = bit32(insn, 22), s_bit = bit32(insn, 12);
	unsigned m = field32(insn, 20, 16), n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);
	unsigned q = bit32(insn, 30), size = field32(insn, 11, 10);
	unsigned opcode = field32(insn, 15, 13);
	unsigned scale = opcode >> 1;
	unsigned selem = ((opcode & 1) << 1 | bit32(insn, 21)) + 1;
	bool replicate = false;
	unsigned index = 0;
	Arm64Exec result;

	switch (scale) {
	case 3: /* load and replicate */
		if (!load || s_bit)
			return ARM64_EXEC_UNDEFINED;
		scale = size;
		replicate = true;
		break;
	case 0: /* B[0-15] */
		index = q << 3 | s_bit << 2 | size;
		break;
	case 1: /* H[0-7] */
		if (size & 1)
			return ARM64_EXEC_UNDEFINED;
		index = q << 2 | s_bit << 1 | size >> 1;
		break;
	default: /* S[0-3] or D[0-1] */
		if (size & 2)
			return ARM64_EXEC_UNDEFINED;
		if (size == 0) {
			index = q << 1 | s_bit;
		} else {
			if (s_bit)
				return ARM64_EXEC_UNDEFINED;
			index = q;
			scale = 3;
		}
		break;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	unsigned bytes = 1u << scale;
	uint64_t address = get_xsp(c, n);
	uint64_t offs = 0;

	for (unsigned s = 0; s < selem; s++) {
		if (replicate) {
			uint64_t element, lo;

			result =
				arm64_mem_read_vec(c, address + offs, bytes, false, &element);
			if (result != ARM64_EXEC_OK)
				return result;
			lo = element;
			for (unsigned width = 8 * bytes; width < 64; width *= 2)
				lo |= lo << width;
			set_v(c, t, lo, q ? lo : 0);
		} else {
			result = element_access(c, load, t, index, bytes, address + offs);
			if (result != ARM64_EXEC_OK)
				return result;
		}
		offs += bytes;
		t = next_register(t);
	}
	if (wback)
		writeback(c, n, m, address, offs);
	return ARM64_EXEC_OK;
}

/*
 * The four classes, by bits 24:23: multiple structures, multiple
 * structures (post-indexed), single structure, single structure
 * (post-indexed). The forms without post-indexing must have bits 21:16,
 * or 20:16 for a single structure, clear; the post-indexed multiple
 * structures must have bit 21 clear. Other encodings are UNALLOCATED.
 */
Arm64Exec arm64_exec_simd_ldst(Arm64Cpu *c, uint32_t insn)
{
	switch (field32(insn, 24, 23)) {
	case 0:
		if (field32(insn, 21, 16) != 0)
			return ARM64_EXEC_UNDEFINED;
		return multiple(c, insn, false);
	case 1:
		if (bit32(insn, 21))
			return ARM64_EXEC_UNDEFINED;
		return multiple(c, insn, true);
	case 2:
		if (field32(insn, 20, 16) != 0)
			return ARM64_EXEC_UNDEFINED;
		return single(c, insn, false);
	default:
		return single(c, insn, true);
	}
}
