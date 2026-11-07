/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Top-level A64 decoding, following the encoding tables of C4.1.
 *
 * Each group of the table gets its own function; the groups fill in as
 * instructions are implemented.
 */

#include "cpu/arm64/internal.h"

Arm64Group arm64_group(uint32_t insn)
{
	unsigned op0 = insn >> 31;
	unsigned op1 = (insn >> 25) & 0xf; /* bits 28:25 */

	if (op1 == 0x0)
		return op0 ? ARM64_GROUP_SME : ARM64_GROUP_RESERVED;
	if (op1 == 0x2)
		return ARM64_GROUP_SVE;
	if ((op1 & 0xd) == 0x1) /* 00x1 */
		return ARM64_GROUP_UNALLOCATED;
	if ((op1 & 0xe) == 0x8) /* 100x */
		return ARM64_GROUP_DP_IMM;
	if ((op1 & 0xe) == 0xa) /* 101x */
		return ARM64_GROUP_BRANCH_SYS;
	if ((op1 & 0x7) == 0x5) /* x101 */
		return ARM64_GROUP_DP_REG;
	if ((op1 & 0x7) == 0x7) /* x111 */
		return ARM64_GROUP_SIMD_FP;
	return ARM64_GROUP_LOAD_STORE; /* x1x0, the only values left */
}

const char *arm64_group_name(Arm64Group group)
{
	switch (group) {
	case ARM64_GROUP_RESERVED:
		return "Reserved";
	case ARM64_GROUP_SME:
		return "SME encodings";
	case ARM64_GROUP_SVE:
		return "SVE encodings";
	case ARM64_GROUP_UNALLOCATED:
		return "UNALLOCATED";
	case ARM64_GROUP_DP_IMM:
		return "Data Processing -- Immediate";
	case ARM64_GROUP_BRANCH_SYS:
		return "Branches, Exception Generating and System instructions";
	case ARM64_GROUP_DP_REG:
		return "Data Processing -- Register";
	case ARM64_GROUP_SIMD_FP:
		return "Data Processing -- Scalar Floating-Point and Advanced "
			   "SIMD";
	case ARM64_GROUP_LOAD_STORE:
		return "Loads and Stores";
	}
	return "unknown group";
}

Arm64Exec arm64_execute(Arm64Cpu *c, uint32_t insn)
{
	switch (arm64_group(insn)) {
	case ARM64_GROUP_RESERVED:
		/* UDF and the UNALLOCATED encodings around it. */
		return ARM64_EXEC_UNDEFINED;
	case ARM64_GROUP_SME:
	case ARM64_GROUP_SVE:
		/* Not part of the Armv8.0 feature set this model provides. */
		return ARM64_EXEC_UNDEFINED;
	case ARM64_GROUP_UNALLOCATED:
		return ARM64_EXEC_UNDEFINED;
	case ARM64_GROUP_DP_IMM:
		return arm64_exec_dp_imm(c, insn);
	case ARM64_GROUP_BRANCH_SYS:
	case ARM64_GROUP_DP_REG:
	case ARM64_GROUP_SIMD_FP:
	case ARM64_GROUP_LOAD_STORE:
		break;
	}
	return ARM64_EXEC_UNIMPLEMENTED;
}
