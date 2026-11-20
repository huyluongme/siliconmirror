/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The integer instructions of some Advanced SIMD classes of C4.1, which
 * simd.c gives to this file: two-register miscellaneous, vector and
 * scalar, across lanes and scalar pairwise. Each instruction follows its
 * description in C7.2. The floating-point instructions of these classes
 * stop the machine as not implemented yet, and those that the core lacks
 * (FEAT_FP16, FEAT_FRINTTS, FEAT_BF16 and newer) are UNDEFINED.
 *
 * A saturating instruction sets FPSR.QC when a result saturates, as
 * SatQ() in J1.3 has it.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

__extension__ typedef __int128 int128;

#define FPSR_QC (UINT32_C(1) << 27)

/* The key of an instruction of a class: U and opcode. */
#define KEY(u, opcode) ((u) << 5 | (opcode))

/* SInt() of a value of size bytes. */
static int64_t sint(uint64_t value, unsigned size)
{
	return (int64_t)sign_extend64(value & bytes_mask(size), 8 * size);
}

/* Int(): SInt() or UInt(). */
static int128 to_int(uint64_t value, unsigned size, bool is_unsigned)
{
	return is_unsigned ? (int128)(value & bytes_mask(size))
	                   : (int128)sint(value, size);
}

/*
 * SatQ(), SignedSatQ() and UnsignedSatQ() of J1.3: value as an integer
 * of size bytes, saturated, setting FPSR.QC when it saturates.
 */
static uint64_t saturate(Arm64Cpu *c, int128 value, unsigned size,
                         bool is_unsigned)
{
	int128 high = is_unsigned ? ((int128)1 << (8 * size)) - 1
	                          : ((int128)1 << (8 * size - 1)) - 1;
	int128 low = is_unsigned ? 0 : -high - 1;

	if (value > high) {
		value = high;
		c->fpsr |= FPSR_QC;
	} else if (value < low) {
		value = low;
		c->fpsr |= FPSR_QC;
	}
	return (uint64_t)value & bytes_mask(size);
}

/* CountLeadingZeroBits() of the low bits bits of value. */
static unsigned leading_zeros(uint64_t value, unsigned bits)
{
	unsigned count = 0;

	while (count < bits && !((value >> (bits - 1 - count)) & 1))
		count++;
	return count;
}

static unsigned population(uint64_t value)
{
	unsigned count = 0;

	for (; value != 0; value &= value - 1)
		count++;
	return count;
}

static uint64_t reverse_byte(uint64_t byte)
{
	uint64_t result = 0;

	for (unsigned i = 0; i < 8; i++)
		result |= ((byte >> i) & 1) << (7 - i);
	return result;
}

/* V{datasize}(d) = result, for datasize 8 or 16 bytes. */
static void write_vector(Arm64Cpu *c, unsigned d, const Arm64Vreg *result,
                         unsigned datasize)
{
	set_v(c, d, result->d[0], datasize == 16 ? result->d[1] : 0);
}

/*
 * Whether the floating-point opcode of the vector two-register
 * miscellaneous class is an instruction of Armv8.0, from the table of
 * C4.1: its size field holds sz in bit 0 and another opcode bit in bit 1.
 */
static bool fp_misc(bool u, unsigned size, unsigned opcode)
{
	bool high = size & 2;

	switch (opcode) {
	case 0x0c: /* FCMGT, FCMGE (zero) */
	case 0x0d: /* FCMEQ, FCMLE (zero) */
	case 0x0f: /* FABS, FNEG */
		return high;
	case 0x0e: /* FCMLT (zero) */
		return high && !u;
	case 0x16: /* FCVTN, FCVTXN */
		return u ? size == 1 : !high;
	case 0x17: /* FCVTL */
		return !u && !high;
	case 0x18: /* FRINTN, FRINTP, FRINTA */
		return !(u && high);
	case 0x1c: /* FCVTAS, FCVTAU; URECPE, URSQRTE of words only */
		return size != 3;
	case 0x19: /* FRINTM, FRINTZ, FRINTX, FRINTI */
	case 0x1a: /* FCVTNS, FCVTPS, FCVTNU, FCVTPU */
	case 0x1b: /* FCVTMS, FCVTZS, FCVTMU, FCVTZU */
	case 0x1d: /* SCVTF, FRECPE, UCVTF, FRSQRTE */
		return true;
	case 0x1f: /* FSQRT */
		return u && high;
	default: /* 0x1e: FRINT32Z and others, FEAT_FRINTTS */
		return false;
	}
}

/* The same for the scalar class. */
static bool fp_scalar_misc(bool u, unsigned size, unsigned opcode)
{
	bool high = size & 2;

	switch (opcode) {
	case 0x0c: /* FCMGT, FCMGE (zero) */
	case 0x0d: /* FCMEQ, FCMLE (zero) */
		return high;
	case 0x0e: /* FCMLT (zero) */
		return high && !u;
	case 0x16: /* FCVTXN */
		return u && size == 1;
	case 0x1a: /* FCVTNS, FCVTPS, FCVTNU, FCVTPU */
	case 0x1b: /* FCVTMS, FCVTZS, FCVTMU, FCVTZU */
	case 0x1d: /* SCVTF, FRECPE, UCVTF, FRSQRTE */
		return true;
	case 0x1c: /* FCVTAS, FCVTAU */
		return !high;
	case 0x1f: /* FRECPX */
		return high && !u;
	default:
		return false;
	}
}

/*
 * The comparisons with zero, CMGT, CMGE, CMEQ, CMLE and CMLT (zero), of
 * an element of size bytes: all ones when it holds, else 0.
 */
static uint64_t compare_zero(unsigned key, uint64_t element, unsigned size)
{
	int64_t value = sint(element, size);
	bool holds;

	switch (key) {
	case KEY(0, 0x08):
		holds = value > 0;
		break;
	case KEY(1, 0x08):
		holds = value >= 0;
		break;
	case KEY(0, 0x09):
		holds = value == 0;
		break;
	case KEY(1, 0x09):
		holds = value <= 0;
		break;
	default: /* KEY(0, 0x0a) */
		holds = value < 0;
		break;
	}
	return holds ? bytes_mask(size) : 0;
}

/*
 * The operations of one element of size bytes that both forms have:
 * SUQADD, USQADD, SQABS, SQNEG, the comparisons with zero, ABS and NEG.
 * acc is the element of Vd, which SUQADD and USQADD add to.
 */
static uint64_t element_op(Arm64Cpu *c, unsigned key, uint64_t element,
                           uint64_t acc, unsigned size)
{
	int64_t value = sint(element, size);

	switch (key) {
	case KEY(0, 0x03): /* SUQADD: signed Vd plus unsigned Vn */
		return saturate(c,
		                to_int(acc, size, false) + to_int(element, size, true),
		                size, false);
	case KEY(1, 0x03): /* USQADD: unsigned Vd plus signed Vn */
		return saturate(c, to_int(acc, size, true) + value, size, true);
	case KEY(0, 0x07): /* SQABS */
		return saturate(c, value < 0 ? -(int128)value : value, size, false);
	case KEY(1, 0x07): /* SQNEG */
		return saturate(c, -(int128)value, size, false);
	case KEY(0, 0x0b): /* ABS */
		return (value < 0 ? 0 - element : element) & bytes_mask(size);
	case KEY(1, 0x0b): /* NEG */
		return (0 - element) & bytes_mask(size);
	default:
		return compare_zero(key, element, size);
	}
}

/*
 * The narrowing instructions XTN, SQXTN, UQXTN and SQXTUN: an element of
 * 2 * size bytes to one of size bytes.
 */
static uint64_t narrow(Arm64Cpu *c, unsigned key, uint64_t element,
                       unsigned size)
{
	switch (key) {
	case KEY(0, 0x12): /* XTN */
		return element & bytes_mask(size);
	case KEY(0, 0x14): /* SQXTN */
		return saturate(c, sint(element, 2 * size), size, false);
	case KEY(1, 0x14): /* UQXTN */
		return saturate(c, to_int(element, 2 * size, true), size, true);
	default: /* KEY(1, 0x12): SQXTUN */
		return saturate(c, sint(element, 2 * size), size, true);
	}
}

/*
 * Advanced SIMD scalar two-register miscellaneous: 01 U 11110 size 10000
 * opcode 10 Rn Rd, on the element of size bytes at the bottom of Vn.
 */
static Arm64Exec scalar_misc(Arm64Cpu *c, bool u, unsigned size,
                             unsigned opcode, unsigned n, unsigned d)
{
	unsigned key = KEY(u, opcode), esize = 1u << size;
	Arm64Exec result;
	uint64_t value;

	switch (key) {
	case KEY(0, 0x03): /* SUQADD */
	case KEY(1, 0x03): /* USQADD */
	case KEY(0, 0x07): /* SQABS */
	case KEY(1, 0x07): /* SQNEG */
		break;
	case KEY(0, 0x08): /* CMGT (zero) */
	case KEY(1, 0x08): /* CMGE (zero) */
	case KEY(0, 0x09): /* CMEQ (zero) */
	case KEY(1, 0x09): /* CMLE (zero) */
	case KEY(0, 0x0a): /* CMLT (zero) */
	case KEY(0, 0x0b): /* ABS */
	case KEY(1, 0x0b): /* NEG */
		if (size != 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(1, 0x12): /* SQXTUN */
	case KEY(0, 0x14): /* SQXTN */
	case KEY(1, 0x14): /* UQXTN */
		if (size == 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (opcode == 0x12 || opcode == 0x14) /* SQXTUN, SQXTN, UQXTN */
		value = narrow(c, key, get_elem(c, n, 0, 2 * esize), esize);
	else
		value = element_op(c, key, get_elem(c, n, 0, esize),
		                   get_elem(c, d, 0, esize), esize);
	set_scalar(c, d, esize, value);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD two-register miscellaneous, 0 Q U 01110 size 10000
 * opcode 10 Rn Rd, and its scalar form.
 */
Arm64Exec arm64_simd_two_reg_misc(Arm64Cpu *c, uint32_t insn, bool scalar)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), opcode = field32(insn, 16, 12);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned key = KEY(u, opcode), esize = 1u << size;
	unsigned datasize = q ? 16 : 8, elements = datasize / esize;
	bool reserved = size == 3 && !q; /* size:Q = 110, the .1D form */
	Arm64Exec result;

	/* The tables take in no integer opcode. */
	if (scalar ? fp_scalar_misc(u, size, opcode) : fp_misc(u, size, opcode))
		return arm64_simd_fp_misc(c, insn, scalar);
	if (scalar)
		return scalar_misc(c, u, size, opcode, n, d);

	switch (key) {
	case KEY(0, 0x00): /* REV64 */
	case KEY(1, 0x00): /* REV32 */
	case KEY(0, 0x01): /* REV16: csize = 64 >> UInt(o0:U) bits */
		if ((8u >> ((opcode & 1) << 1 | u)) <= esize)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(0, 0x02): /* SADDLP */
	case KEY(1, 0x02): /* UADDLP */
	case KEY(0, 0x04): /* CLS (vector) */
	case KEY(1, 0x04): /* CLZ (vector) */
	case KEY(0, 0x06): /* SADALP */
	case KEY(1, 0x06): /* UADALP */
	case KEY(0, 0x12): /* XTN */
	case KEY(1, 0x12): /* SQXTUN */
	case KEY(1, 0x13): /* SHLL */
	case KEY(0, 0x14): /* SQXTN */
	case KEY(1, 0x14): /* UQXTN */
		if (size == 3)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(0, 0x05): /* CNT */
		if (size != 0)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(1, 0x05): /* NOT with size 00, RBIT (vector) with 01 */
		if (size > 1)
			return ARM64_EXEC_UNDEFINED;
		break;
	case KEY(0, 0x03): /* SUQADD */
	case KEY(1, 0x03): /* USQADD */
	case KEY(0, 0x07): /* SQABS */
	case KEY(1, 0x07): /* SQNEG */
	case KEY(0, 0x08): /* CMGT (zero) */
	case KEY(1, 0x08): /* CMGE (zero) */
	case KEY(0, 0x09): /* CMEQ (zero) */
	case KEY(1, 0x09): /* CMLE (zero) */
	case KEY(0, 0x0a): /* CMLT (zero) */
	case KEY(0, 0x0b): /* ABS */
	case KEY(1, 0x0b): /* NEG (vector) */
		if (reserved)
			return ARM64_EXEC_UNDEFINED;
		break;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	const Arm64Vreg op = c->v[n], acc = c->v[d];
	Arm64Vreg r = {{0, 0}};

	switch (key) {
	case KEY(0, 0x00):
	case KEY(1, 0x00):
	case KEY(0, 0x01): {
		/* Reverse the elements within each container. */
		unsigned per = (8u >> ((opcode & 1) << 1 | u)) / esize;

		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(
				&r, e, esize,
				vreg_elem(&op, e - e % per + (per - 1 - e % per), esize));
		break;
	}
	case KEY(0, 0x02):
	case KEY(1, 0x02):
	case KEY(0, 0x06):
	case KEY(1, 0x06):
		/* Add pairs into elements twice as wide, and accumulate. */
		for (unsigned e = 0; e < elements / 2; e++) {
			int128 sum = to_int(vreg_elem(&op, 2 * e, esize), esize, u) +
			             to_int(vreg_elem(&op, 2 * e + 1, esize), esize, u);

			if (opcode == 0x06)
				sum += vreg_elem(&acc, e, 2 * esize);
			vreg_set_elem(&r, e, 2 * esize, (uint64_t)sum);
		}
		break;
	case KEY(0, 0x04): /* CLS: CountLeadingSignBits() */
		for (unsigned e = 0; e < elements; e++) {
			uint64_t x = vreg_elem(&op, e, esize);

			vreg_set_elem(&r, e, esize,
			              leading_zeros((x >> 1) ^ x, 8 * esize - 1));
		}
		break;
	case KEY(1, 0x04): /* CLZ */
		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(&r, e, esize,
			              leading_zeros(vreg_elem(&op, e, esize), 8 * esize));
		break;
	case KEY(0, 0x05): /* CNT */
		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(&r, e, 1, population(vreg_elem(&op, e, 1)));
		break;
	case KEY(1, 0x05): /* NOT, RBIT: of bytes, whatever size says */
		for (unsigned e = 0; e < datasize; e++) {
			uint64_t byte = vreg_elem(&op, e, 1);

			vreg_set_elem(&r, e, 1, size == 0 ? ~byte : reverse_byte(byte));
		}
		break;
	case KEY(0, 0x12):
	case KEY(1, 0x12):
	case KEY(0, 0x14):
	case KEY(1, 0x14):
		/* Narrow into the lower or, for XTN2 and the others, the upper half. */
		for (unsigned e = 0; e < 8 / esize; e++)
			vreg_set_elem(&r, e, esize,
			              narrow(c, key, vreg_elem(&op, e, 2 * esize), esize));
		if (q)
			c->v[d].d[1] = r.d[0];
		else
			set_v(c, d, r.d[0], 0);
		return ARM64_EXEC_OK;
	case KEY(1, 0x13): /* SHLL, SHLL2: the lower or upper half, widened */
		for (unsigned e = 0; e < 8 / esize; e++)
			vreg_set_elem(&r, e, 2 * esize,
			              vreg_elem(&op, (q ? 8 / esize : 0) + e, esize)
			                  << (8 * esize));
		set_v(c, d, r.d[0], r.d[1]);
		return ARM64_EXEC_OK;
	default:
		for (unsigned e = 0; e < elements; e++)
			vreg_set_elem(&r, e, esize,
			              element_op(c, key, vreg_elem(&op, e, esize),
			                         vreg_elem(&acc, e, esize), esize));
		break;
	}
	write_vector(c, d, &r, datasize);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD across lanes, 0 Q U 01110 size 11000 opcode 10 Rn Rd:
 * SADDLV, UADDLV, SMAXV, UMAXV, SMINV, UMINV and ADDV, into a scalar.
 */
Arm64Exec arm64_simd_across_lanes(Arm64Cpu *c, uint32_t insn)
{
	bool q = bit32(insn, 30), u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), opcode = field32(insn, 16, 12);
	unsigned n = field32(insn, 9, 5), d = field32(insn, 4, 0);
	unsigned esize = 1u << size, elements = (q ? 16 : 8) / esize;
	Arm64Exec result;

	switch (opcode) {
	case 0x03: /* SADDLV, UADDLV */
	case 0x0a: /* SMAXV, UMAXV */
	case 0x1a: /* SMINV, UMINV */
		break;
	case 0x1b: /* ADDV */
		if (u)
			return ARM64_EXEC_UNDEFINED;
		break;
	case 0x0c: /* FMAXNMV, FMINNMV */
	case 0x0f: /* FMAXV, FMINV */
		/* Single precision only; with U = 0, FEAT_FP16. */
		if (u && !(size & 1) && q)
			return arm64_simd_fp_across(c, insn);
		return ARM64_EXEC_UNDEFINED;
	default:
		return ARM64_EXEC_UNDEFINED;
	}
	/* No 64-bit elements, and four 32-bit ones only. */
	if (size == 3 || (size == 2 && !q))
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;

	int128 acc = to_int(get_elem(c, n, 0, esize), esize, u);

	for (unsigned e = 1; e < elements; e++) {
		int128 value = to_int(get_elem(c, n, e, esize), esize, u);

		if (opcode == 0x0a ? value > acc : opcode == 0x1a ? value < acc : 0)
			acc = value;
		else if (opcode == 0x03 || opcode == 0x1b)
			acc += value;
	}
	set_scalar(c, d, opcode == 0x03 ? 2 * esize : esize, (uint64_t)acc);
	return ARM64_EXEC_OK;
}

/*
 * Advanced SIMD scalar pairwise, 01 U 11110 size 11000 opcode 10 Rn Rd:
 * ADDP (scalar), the sum of the two doublewords of Vn. The others are
 * floating-point.
 */
Arm64Exec arm64_simd_scalar_pairwise(Arm64Cpu *c, uint32_t insn)
{
	bool u = bit32(insn, 29);
	unsigned size = field32(insn, 23, 22), opcode = field32(insn, 16, 12);
	Arm64Exec result;

	if (!u) {
		if (size != 3 || opcode != 0x1b) /* else UNALLOCATED or FEAT_FP16 */
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		unsigned n = field32(insn, 9, 5);

		set_scalar(c, field32(insn, 4, 0), 8, c->v[n].d[0] + c->v[n].d[1]);
		return ARM64_EXEC_OK;
	}
	/* FMAXNMP, FMINNMP, FMAXP, FMINP; FADDP of single or double only. */
	if (opcode == 0x0c || opcode == 0x0f || (opcode == 0x0d && size < 2))
		return arm64_simd_fp_pairwise(c, insn);
	return ARM64_EXEC_UNDEFINED;
}
