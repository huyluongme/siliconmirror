/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Loads and Stores: the instructions of C4.1 with bit 27 set and bit 25
 * clear. Each one follows its description in C6.2.
 *
 * Implemented: the register forms with an immediate offset (unsigned
 * offset, unscaled, unprivileged, pre-index and post-index) or a register
 * offset, the literal forms and the register pairs, for general-purpose
 * and SIMD&FP registers (C7.2); the prefetches, which are hints with no
 * effect here; the exclusive and ordered class, which is in exclusive.c;
 * and the Advanced SIMD structure classes, in simd_ldst.c. Classes that
 * only hold instructions newer than Armv8.0 are UNDEFINED.
 *
 * The unprivileged forms, LDTR and STTR and their byte, halfword and
 * signed variants (C6.2.245 to C6.2.252, C6.2.435 to C6.2.437), check
 * the permissions of EL0 when they run at EL1, as AArch64.CheckPermission()
 * does for an unprivileged access; elsewhere they are LDUR and STUR.
 *
 * Where the manual leaves a choice of CONSTRAINED UNPREDICTABLE
 * behaviors, this model makes these:
 *  - A load with writeback whose base register is also loaded: the
 *    writeback is suppressed (Constraint_WBSUPPRESS).
 *  - A store with writeback whose base register is also stored: the
 *    value from before the writeback is stored (Constraint_NONE).
 *  - A load pair with Rt == Rt2: the register gets the second value, one
 *    of the UNKNOWN values allowed (Constraint_UNKNOWN).
 *
 * A load or store whose base register is SP checks its alignment first
 * (CheckSPAlignment()); the prefetches do not.
 */

#include "cpu/arm64/internal.h"
#include "util/bits.h"

/* A load or store of one general-purpose register. */
typedef struct RegisterAccess {
	unsigned size; /* bytes accessed: 1, 2, 4 or 8 */
	bool store;
	bool signed_load; /* sign-extend the data instead of zero-extending */
	unsigned regsize; /* bits written to Rt by a load: 32 or 64 */
	bool prefetch;    /* PRFM or PRFUM */
} RegisterAccess;

/*
 * Decode size (bits 31:30) and opc (bits 23:22) of the load/store
 * register classes, from their tables in C4.1, for VR = 0. Returns false
 * for UNALLOCATED combinations.
 */
static bool decode_register_access(unsigned size, unsigned opc,
                                   RegisterAccess *access)
{
	*access = (RegisterAccess){.size = 1u << size, .regsize = 32};

	switch (opc) {
	case 0: /* STRB, STRH, STR */
		access->store = true;
		return true;
	case 1: /* LDRB, LDRH, LDR */
		if (size == 3)
			access->regsize = 64;
		return true;
	case 2: /* LDRSB and LDRSH to 64 bits, LDRSW, PRFM */
		if (size == 3)
			access->prefetch = true;
		access->signed_load = true;
		access->regsize = 64;
		return true;
	default: /* LDRSB and LDRSH to 32 bits */
		access->signed_load = true;
		return size < 2;
	}
}

/* The value a load writes to its register, from the data it read. */
static uint64_t loaded_value(const RegisterAccess *access, uint64_t data)
{
	if (access->signed_load)
		data = sign_extend64(data, access->size * 8);
	return access->regsize == 64 ? data : data & UINT32_MAX;
}

/*
 * The SIMD&FP forms of the load/store register classes: LDR and STR
 * (immediate, SIMD&FP) and (register, SIMD&FP), LDUR and STUR (SIMD&FP),
 * C7.2.230, C7.2.387, C7.2.232, C7.2.388, C7.2.235 and C7.2.391. The B,
 * H, S, D and Q registers; the caller scaled the offset by the size.
 */
static Arm64Exec load_store_simd(Arm64Cpu *c, uint32_t insn, uint64_t offset,
                                 bool wback, bool postindex)
{
	unsigned size = field32(insn, 31, 30), opc = field32(insn, 23, 22);
	unsigned n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);
	unsigned bytes = (opc & 2) ? 16 : 1u << size;
	Arm64Exec result;

	if ((opc & 2) && size != 0)
		return ARM64_EXEC_UNDEFINED;
	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t address = get_xsp(c, n);

	if (!postindex)
		address += offset;
	if (opc & 1) {
		uint64_t data[2];

		result = arm64_mem_read_vec(c, address, bytes, false, data);
		if (result != ARM64_EXEC_OK)
			return result;
		set_v(c, t, data[0], bytes == 16 ? data[1] : 0);
	} else {
		result = arm64_mem_write_vec(c, address, bytes, false, c->v[t].d);
		if (result != ARM64_EXEC_OK)
			return result;
	}
	if (wback)
		set_xsp(c, n, postindex ? address + offset : address);
	return ARM64_EXEC_OK;
}

/*
 * The load/store register classes with an immediate offset: LDR, LDRB,
 * LDRH, LDRSB, LDRSH, LDRSW, STR, STRB and STRH (immediate), LDUR and
 * STUR and their byte, halfword and signed forms, PRFM (immediate) and
 * PRFUM. The caller decodes the addressing mode.
 */
static Arm64Exec load_store_register(Arm64Cpu *c, uint32_t insn,
                                     uint64_t offset, bool wback,
                                     bool postindex, bool unprivileged)
{
	RegisterAccess access;
	unsigned n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);
	Arm64Exec result;

	if (bit32(insn, 26)) /* VR = 1: SIMD&FP registers */
		return load_store_simd(c, insn, offset, wback, postindex);
	if (!decode_register_access(field32(insn, 31, 30), field32(insn, 23, 22),
	                            &access))
		return ARM64_EXEC_UNDEFINED;
	if (access.prefetch) {
		/* Only the unsigned offset and unscaled classes have them. */
		if (wback || unprivileged)
			return ARM64_EXEC_UNDEFINED;
		return ARM64_EXEC_OK; /* a hint: no access, no fault */
	}
	if (wback && n == t && n != 31 && !access.store)
		wback = false; /* Constraint_WBSUPPRESS */
	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t address = get_xsp(c, n);
	unsigned el = unprivileged && c->el == 1 ? 0 : c->el;

	if (!postindex)
		address += offset;

	if (access.store) {
		result = arm64_mem_write_as(c, address, access.size, el, get_x(c, t));
		if (result != ARM64_EXEC_OK)
			return result;
	} else {
		uint64_t data;

		result = arm64_mem_read_as(c, address, access.size, el, &data);
		if (result != ARM64_EXEC_OK)
			return result;
		set_x(c, t, loaded_value(&access, data));
	}

	if (wback)
		set_xsp(c, n, postindex ? address + offset : address);
	return ARM64_EXEC_OK;
}

/*
 * LDR (literal), LDRSW (literal) and PRFM (literal), C6.2.217, C6.2.229
 * and C6.2.316: a PC-relative address within 1MB.
 */
static Arm64Exec load_literal(Arm64Cpu *c, uint32_t insn)
{
	unsigned opc = field32(insn, 31, 30);
	unsigned t = field32(insn, 4, 0);
	uint64_t address = c->pc + sign_extend64(field32(insn, 23, 5) << 2, 21);
	RegisterAccess access = {.size = 4, .regsize = 32};
	uint64_t data;

	if (bit32(insn, 26)) { /* LDR (literal, SIMD&FP), C7.2.231 */
		unsigned bytes = 4u << opc;
		uint64_t vdata[2];
		Arm64Exec result;

		if (opc == 3)
			return ARM64_EXEC_UNDEFINED;
		if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
			return result;
		result = arm64_mem_read_vec(c, address, bytes, false, vdata);
		if (result != ARM64_EXEC_OK)
			return result;
		set_v(c, t, vdata[0], bytes == 16 ? vdata[1] : 0);
		return ARM64_EXEC_OK;
	}

	switch (opc) {
	case 0: /* LDR, 32-bit */
		break;
	case 1: /* LDR, 64-bit */
		access.size = 8;
		access.regsize = 64;
		break;
	case 2: /* LDRSW */
		access.signed_load = true;
		access.regsize = 64;
		break;
	default: /* PRFM: a hint */
		return ARM64_EXEC_OK;
	}

	Arm64Exec result = arm64_mem_read(c, address, access.size, &data);

	if (result != ARM64_EXEC_OK)
		return result;
	set_x(c, t, loaded_value(&access, data));
	return ARM64_EXEC_OK;
}

/*
 * The SIMD&FP forms of the pair classes: LDP, STP, LDNP and STNP
 * (SIMD&FP), C7.2.229, C7.2.386, C7.2.228 and C7.2.385. The two
 * registers are one access, of twice the size of a register (Mem{} with
 * ispair). A load with Rt == Rt2 writes the second value, one of the
 * UNKNOWN values allowed (Constraint_UNKNOWN).
 */
static Arm64Exec load_store_pair_simd(Arm64Cpu *c, uint32_t insn, bool wback,
                                      bool postindex)
{
	unsigned opc = field32(insn, 31, 30);
	bool load = bit32(insn, 22);
	unsigned t2 = field32(insn, 14, 10);
	unsigned n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);
	Arm64Exec result;

	if (opc == 3) /* FEAT_LSUI, or UNALLOCATED */
		return ARM64_EXEC_UNDEFINED;

	unsigned scale = 2 + opc;
	unsigned size = 1u << scale; /* bytes per register */
	uint64_t offset = sign_extend64(field32(insn, 21, 15), 7) << scale;
	uint64_t data[4] = {0};

	if ((result = arm64_check_fp_enabled(c)) != ARM64_EXEC_OK)
		return result;
	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t address = get_xsp(c, n);

	if (!postindex)
		address += offset;
	if (load) {
		result = arm64_mem_read_vec(c, address, 2 * size, true, data);
		if (result != ARM64_EXEC_OK)
			return result;
		if (size == 4) {
			set_scalar(c, t, 4, data[0]);
			set_scalar(c, t2, 4, data[0] >> 32);
		} else if (size == 8) {
			set_scalar(c, t, 8, data[0]);
			set_scalar(c, t2, 8, data[1]);
		} else {
			set_v(c, t, data[0], data[1]);
			set_v(c, t2, data[2], data[3]);
		}
	} else {
		if (size == 4) {
			data[0] = (get_d(c, t) & UINT32_MAX) | get_d(c, t2) << 32;
		} else if (size == 8) {
			data[0] = get_d(c, t);
			data[1] = get_d(c, t2);
		} else {
			data[0] = c->v[t].d[0];
			data[1] = c->v[t].d[1];
			data[2] = c->v[t2].d[0];
			data[3] = c->v[t2].d[1];
		}
		result = arm64_mem_write_vec(c, address, 2 * size, true, data);
		if (result != ARM64_EXEC_OK)
			return result;
	}
	if (wback)
		set_xsp(c, n, postindex ? address + offset : address);
	return ARM64_EXEC_OK;
}

/*
 * The load/store register pair classes: LDP, STP, LDPSW, LDNP and STNP,
 * C6.2.214, C6.2.414, C6.2.215, C6.2.213 and C6.2.413. no_allocate
 * selects the no-allocate pair (offset) class, LDNP and STNP, whose
 * non-temporal hint has no effect here.
 */
static Arm64Exec load_store_pair(Arm64Cpu *c, uint32_t insn, bool wback,
                                 bool postindex, bool no_allocate)
{
	unsigned opc = field32(insn, 31, 30);
	bool load = bit32(insn, 22);
	unsigned t2 = field32(insn, 14, 10);
	unsigned n = field32(insn, 9, 5);
	unsigned t = field32(insn, 4, 0);

	if (bit32(insn, 26))
		return load_store_pair_simd(c, insn, wback, postindex);
	if (opc == 3) /* LDTP, STTP, LDTNP, STTNP: FEAT_LSUI */
		return ARM64_EXEC_UNDEFINED;
	/* opc = 01 is LDPSW or STGP (FEAT_MTE), or UNALLOCATED for LDNP. */
	if (opc == 1 && (!load || no_allocate))
		return ARM64_EXEC_UNDEFINED;

	unsigned scale = 2 + (opc >> 1);
	unsigned size = 1u << scale; /* bytes per register */
	uint64_t offset = sign_extend64(field32(insn, 21, 15), 7) << scale;

	if (wback && (t == n || t2 == n) && n != 31 && load)
		wback = false; /* Constraint_WBSUPPRESS */

	Arm64Exec result = ARM64_EXEC_OK;

	if (n == 31 && (result = arm64_check_sp_alignment(c)) != ARM64_EXEC_OK)
		return result;

	uint64_t address = get_xsp(c, n);

	if (!postindex)
		address += offset;

	/* Rt goes to the lower address. */
	if (load) {
		RegisterAccess access = {
			.size = size,
			.signed_load = opc == 1, /* LDPSW */
			.regsize = opc == 0 ? 32 : 64,
		};
		uint64_t data1, data2;

		result = arm64_mem_read(c, address, size, &data1);
		if (result == ARM64_EXEC_OK)
			result = arm64_mem_read(c, address + size, size, &data2);
		if (result != ARM64_EXEC_OK)
			return result;
		set_x(c, t, loaded_value(&access, data1));
		set_x(c, t2, loaded_value(&access, data2)); /* wins if t == t2 */
	} else {
		uint64_t data1 = get_x(c, t), data2 = get_x(c, t2);

		result = arm64_mem_write(c, address, size, data1);
		if (result == ARM64_EXEC_OK)
			result = arm64_mem_write(c, address + size, size, data2);
		if (result != ARM64_EXEC_OK)
			return result;
	}

	if (wback)
		set_xsp(c, n, postindex ? address + offset : address);
	return ARM64_EXEC_OK;
}

Arm64Exec arm64_exec_load_store(Arm64Cpu *c, uint32_t insn)
{
	uint64_t imm9 = sign_extend64(field32(insn, 20, 12), 9);

	/* op0 is bits 31:28 of the table in C4.1; only bits 29:28 matter. */
	switch (field32(insn, 29, 28)) {
	case 0:
		/*
		 * Exclusive, ordered and compare and swap classes, and the
		 * Advanced SIMD structure loads and stores. With V clear, bit 24
		 * clear holds the exclusive, ordered and compare and swap
		 * classes; bit 24 set, their unprivileged forms (FEAT_LSUI),
		 * newer than Armv8.0, and UNALLOCATED encodings. With V set,
		 * op0 = 1x00 is UNALLOCATED and 0x00 holds the structure classes.
		 */
		if (bit32(insn, 26))
			return bit32(insn, 31) ? ARM64_EXEC_UNDEFINED
			                       : arm64_exec_simd_ldst(c, insn);
		if (bit32(insn, 24))
			return ARM64_EXEC_UNDEFINED;
		return arm64_exec_exclusive(c, insn);
	case 1:
		/*
		 * With bit 24 set, every class here (ordered, RCW, GCS, memory
		 * tags, memory copy and set) is newer than Armv8.0, or
		 * UNALLOCATED.
		 */
		if (bit32(insn, 24))
			return ARM64_EXEC_UNDEFINED;
		return load_literal(c, insn);
	case 2:
		switch (field32(insn, 24, 23)) {
		case 0: /* no-allocate pair (offset) */
			return load_store_pair(c, insn, false, false, true);
		case 1: /* post-indexed */
			return load_store_pair(c, insn, true, true, false);
		case 2: /* offset */
			return load_store_pair(c, insn, false, false, false);
		default: /* pre-indexed */
			return load_store_pair(c, insn, true, false, false);
		}
	default:
		break;
	}

	/*
	 * xx11: the load/store register classes. An offset scales by the
	 * size of the access: 16 bytes for the Q registers, size 00 with V
	 * and opc[1] set.
	 */
	unsigned scale = field32(insn, 31, 30);

	if (bit32(insn, 26) && bit32(insn, 23))
		scale = 4;
	if (bit32(insn, 24)) {
		/* Unsigned immediate. */
		uint64_t imm12 = field32(insn, 21, 10);

		return load_store_register(c, insn, imm12 << scale, false, false,
		                           false);
	}
	if (bit32(insn, 21)) {
		switch (field32(insn, 11, 10)) {
		case 0: /* Atomic memory operations: FEAT_LSE */
			return ARM64_EXEC_UNDEFINED;
		case 2: { /* register offset, LDR (register) and others */
			unsigned option = field32(insn, 15, 13);
			unsigned shift = bit32(insn, 12) ? scale : 0;

			/* option<1> = 0 would index a sub-word register. */
			if (!(option & 2))
				return ARM64_EXEC_UNDEFINED;
			return load_store_register(
				c, insn,
				extend_reg(c, field32(insn, 20, 16), option, shift, 64), false,
				false, false);
		}
		default: /* LDRAA and LDRAB: FEAT_PAuth */
			return ARM64_EXEC_UNDEFINED;
		}
	}
	switch (field32(insn, 11, 10)) {
	case 0: /* unscaled immediate */
		return load_store_register(c, insn, imm9, false, false, false);
	case 1: /* immediate post-indexed */
		return load_store_register(c, insn, imm9, true, true, false);
	case 2: /* unprivileged: no SIMD&FP forms */
		if (bit32(insn, 26))
			return ARM64_EXEC_UNDEFINED;
		return load_store_register(c, insn, imm9, false, false, true);
	default: /* immediate pre-indexed */
		return load_store_register(c, insn, imm9, true, false, false);
	}
}
