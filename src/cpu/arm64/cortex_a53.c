/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Arm Cortex-A53, revision r0p4. Values come from its Technical
 * Reference Manual (ARM DDI 0500J), section 4.2, and from its Advanced
 * SIMD and Floating-point Extension TRM (ARM DDI 0502G) for MVFR0-2.
 *
 * Some values depend on how the SoC configures the core. The BCM2837 of
 * the Raspberry Pi 3 has four cores, the Advanced SIMD and Floating-point
 * Extension, no Cryptographic Extension, no GIC CPU interface, 32KB L1
 * caches and a 512KB L2 cache (Linux's bcm2837.dtsi).
 */

#include "cpu/arm64/cpu.h"
#include "cpu/arm64/sysreg.h"
#include "util/macros.h"

static const Arm64RegValue bcm2837_values[] = {
	/* Identification, Table 4-1 and DDI 0502G Table 2-1. */
	{ARM64_MIDR_EL1, 0x410fd034},
	{ARM64_ID_PFR0_EL1, 0x00000131},
	{ARM64_ID_PFR1_EL1, 0x00011011}, /* no GIC CPU interface */
	{ARM64_ID_DFR0_EL1, 0x03010066},
	{ARM64_ID_MMFR0_EL1, 0x10201105},
	{ARM64_ID_MMFR1_EL1, 0x40000000},
	{ARM64_ID_MMFR2_EL1, 0x01260000},
	{ARM64_ID_MMFR3_EL1, 0x02102211},
	{ARM64_ID_ISAR0_EL1, 0x02101110},
	{ARM64_ID_ISAR1_EL1, 0x13112111},
	{ARM64_ID_ISAR2_EL1, 0x21232042},
	{ARM64_ID_ISAR3_EL1, 0x01112131},
	{ARM64_ID_ISAR4_EL1, 0x00011142},
	{ARM64_ID_ISAR5_EL1, 0x00010001}, /* no Cryptographic Extension */
	{ARM64_MVFR0_EL1, 0x10110222},
	{ARM64_MVFR1_EL1, 0x12111111},
	{ARM64_MVFR2_EL1, 0x00000043},
	{ARM64_ID_AA64PFR0_EL1, 0x00002222}, /* FP and SIMD, no GIC interface */
	{ARM64_ID_AA64DFR0_EL1, 0x10305106},
	{ARM64_ID_AA64ISAR0_EL1, 0x00010000}, /* CRC32, no cryptography */
	{ARM64_ID_AA64MMFR0_EL1, 0x00001122},
	{ARM64_CTR_EL0, 0x84448004},
	{ARM64_DCZID_EL0, 0x00000004},
	{ARM64_CLIDR_EL1, 0x0a200023}, /* with an L2 cache */
	{ARM64_VPIDR_EL2, 0x410fd034},

	/* Reset values, Tables 4-3, 4-12 and 4-13. */
	{ARM64_SCTLR_EL1, 0x00c50838},
	{ARM64_SCTLR_EL2, 0x30c50838},
	{ARM64_SCTLR_EL3, 0x00c50838},
	{ARM64_HCR_EL2, 0x00000002},
	{ARM64_MDCR_EL2, 0x00000006},
	{ARM64_CPTR_EL2, 0x000033ff},
	{ARM64_RMR_EL3, 0x00000001},
};

/* By CSSELR_EL1: L1 data 32KB, L1 instruction 32KB, L2 512KB. Table 4-191. */
static const uint32_t bcm2837_ccsidr[] = {0x700fe01a, 0x201fe00a, 0x703fe07a};

/*
 * Table 4-17. ACTLR_EL3 and ACTLR_EL2 bits 0, 1, 4, 5 and 6 control the
 * write access to five of them (Table 4-74). The TRM does not say what a
 * write without access does; this model traps it to the Exception level
 * whose ACTLR denies it.
 *
 * The number of cores in L2CTLR_EL1 is 4; its other fields are fixed by
 * the SoC and not documented, so they read as 0, and no write changes
 * them. No memory errors happen in the emulator, so the error syndrome
 * registers record none; a write of any value sets them to the value of
 * sections 4.3.80 and 4.3.81. CBAR_EL1 holds the PERIPHBASE input, not
 * documented for the BCM2837, which has no GIC: it reads as 0.
 *
 * The four cores share a single L2 memory system (section 1.1), so the
 * cluster has one copy of its registers, which nL2RESET resets on its
 * own, apart from the cores (section 2.3.3). The TRM says so outright
 * for L2CTLR_EL1 and L2ECTLR_EL1 (sections 4.3.64 and 4.3.65); for
 * L2ACTLR_EL1 and L2MERRSR_EL1, which control and report on that same
 * L2 memory system, this model assumes it.
 */
#define RW(encoding, name, reset, actlr_bit, cluster)                          \
	{encoding, name, reset, false, false, 0, actlr_bit, cluster}
#define FIXED(encoding, name, reset, written, actlr_bit, cluster)              \
	{encoding, name, reset, false, true, written, actlr_bit, cluster}
#define READ_ONLY(encoding, name, reset)                                       \
	{encoding, name, reset, true, false, 0, -1, false}
#define S3_1(crn, crm, op2) ARM64_SYSREG(3, 1, crn, crm, op2)
#define CORE false
#define CLUSTER true
#define L2CTLR 0x03000000 /* four cores */

static const Arm64ImpDefReg bcm2837_impdef[] = {
	FIXED(S3_1(11, 0, 2), "L2CTLR_EL1", L2CTLR, L2CTLR, 4, CLUSTER),
	RW(S3_1(11, 0, 3), "L2ECTLR_EL1", 0, 5, CLUSTER),
	RW(S3_1(15, 0, 0), "L2ACTLR_EL1", 0x80000000, 6, CLUSTER),
	RW(S3_1(15, 2, 0), "CPUACTLR_EL1", 0x090ca000, 0, CORE),
	RW(S3_1(15, 2, 1), "CPUECTLR_EL1", 0, 1, CORE),
	FIXED(S3_1(15, 2, 2), "CPUMERRSR_EL1", 0, 0, -1, CORE),
	FIXED(S3_1(15, 2, 3), "L2MERRSR_EL1", 0, 0x10000000, -1, CLUSTER),
	READ_ONLY(S3_1(15, 3, 0), "CBAR_EL1", 0),
};

/*
 * Registers of the core that the emulator does not implement yet: the
 * debug (section 11.3) and performance monitor (section 12.4) registers,
 * FPCR and FPSR, and the registers that hold AArch32 state. Their
 * encodings come from the GNU assembler. The list is sorted by encoding.
 */
static const Arm64NamedReg bcm2837_missing[] = {
	{ARM64_SYSREG(2, 0, 0, 0, 2), "OSDTRRX_EL1"},
	{ARM64_SYSREG(2, 0, 0, 0, 4), "DBGBVR0_EL1"},
	{ARM64_SYSREG(2, 0, 0, 0, 5), "DBGBCR0_EL1"},
	{ARM64_SYSREG(2, 0, 0, 0, 6), "DBGWVR0_EL1"},
	{ARM64_SYSREG(2, 0, 0, 0, 7), "DBGWCR0_EL1"},
	{ARM64_SYSREG(2, 0, 0, 1, 4), "DBGBVR1_EL1"},
	{ARM64_SYSREG(2, 0, 0, 1, 5), "DBGBCR1_EL1"},
	{ARM64_SYSREG(2, 0, 0, 1, 6), "DBGWVR1_EL1"},
	{ARM64_SYSREG(2, 0, 0, 1, 7), "DBGWCR1_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 0), "MDCCINT_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 2), "MDSCR_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 4), "DBGBVR2_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 5), "DBGBCR2_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 6), "DBGWVR2_EL1"},
	{ARM64_SYSREG(2, 0, 0, 2, 7), "DBGWCR2_EL1"},
	{ARM64_SYSREG(2, 0, 0, 3, 2), "OSDTRTX_EL1"},
	{ARM64_SYSREG(2, 0, 0, 3, 4), "DBGBVR3_EL1"},
	{ARM64_SYSREG(2, 0, 0, 3, 5), "DBGBCR3_EL1"},
	{ARM64_SYSREG(2, 0, 0, 3, 6), "DBGWVR3_EL1"},
	{ARM64_SYSREG(2, 0, 0, 3, 7), "DBGWCR3_EL1"},
	{ARM64_SYSREG(2, 0, 0, 4, 4), "DBGBVR4_EL1"},
	{ARM64_SYSREG(2, 0, 0, 4, 5), "DBGBCR4_EL1"},
	{ARM64_SYSREG(2, 0, 0, 5, 4), "DBGBVR5_EL1"},
	{ARM64_SYSREG(2, 0, 0, 5, 5), "DBGBCR5_EL1"},
	{ARM64_SYSREG(2, 0, 0, 6, 2), "OSECCR_EL1"},
	{ARM64_SYSREG(2, 0, 1, 0, 0), "MDRAR_EL1"},
	{ARM64_SYSREG(2, 0, 1, 0, 4), "OSLAR_EL1"},
	{ARM64_SYSREG(2, 0, 1, 1, 4), "OSLSR_EL1"},
	{ARM64_SYSREG(2, 0, 1, 3, 4), "OSDLR_EL1"},
	{ARM64_SYSREG(2, 0, 1, 4, 4), "DBGPRCR_EL1"},
	{ARM64_SYSREG(2, 0, 7, 8, 6), "DBGCLAIMSET_EL1"},
	{ARM64_SYSREG(2, 0, 7, 9, 6), "DBGCLAIMCLR_EL1"},
	{ARM64_SYSREG(2, 0, 7, 14, 6), "DBGAUTHSTATUS_EL1"},
	{ARM64_SYSREG(2, 3, 0, 1, 0), "MDCCSR_EL0"},
	{ARM64_SYSREG(2, 3, 0, 4, 0), "DBGDTR_EL0"},
	{ARM64_SYSREG(2, 3, 0, 5, 0), "DBGDTRRX_EL0/DBGDTRTX_EL0"},
	{ARM64_SYSREG(2, 4, 0, 7, 0), "DBGVCR32_EL2"},
	{ARM64_SYSREG(3, 0, 9, 14, 1), "PMINTENSET_EL1"},
	{ARM64_SYSREG(3, 0, 9, 14, 2), "PMINTENCLR_EL1"},
	{ARM64_SYSREG(3, 3, 4, 4, 0), "FPCR"},
	{ARM64_SYSREG(3, 3, 4, 4, 1), "FPSR"},
	{ARM64_SYSREG(3, 3, 9, 12, 0), "PMCR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 1), "PMCNTENSET_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 2), "PMCNTENCLR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 3), "PMOVSCLR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 4), "PMSWINC_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 5), "PMSELR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 6), "PMCEID0_EL0"},
	{ARM64_SYSREG(3, 3, 9, 12, 7), "PMCEID1_EL0"},
	{ARM64_SYSREG(3, 3, 9, 13, 0), "PMCCNTR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 13, 1), "PMXEVTYPER_EL0"},
	{ARM64_SYSREG(3, 3, 9, 13, 2), "PMXEVCNTR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 14, 0), "PMUSERENR_EL0"},
	{ARM64_SYSREG(3, 3, 9, 14, 3), "PMOVSSET_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 0), "PMEVCNTR0_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 1), "PMEVCNTR1_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 2), "PMEVCNTR2_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 3), "PMEVCNTR3_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 4), "PMEVCNTR4_EL0"},
	{ARM64_SYSREG(3, 3, 14, 8, 5), "PMEVCNTR5_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 0), "PMEVTYPER0_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 1), "PMEVTYPER1_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 2), "PMEVTYPER2_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 3), "PMEVTYPER3_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 4), "PMEVTYPER4_EL0"},
	{ARM64_SYSREG(3, 3, 14, 12, 5), "PMEVTYPER5_EL0"},
	{ARM64_SYSREG(3, 3, 14, 15, 7), "PMCCFILTR_EL0"},
	{ARM64_SYSREG(3, 4, 3, 0, 0), "DACR32_EL2"},
	{ARM64_SYSREG(3, 4, 4, 3, 0), "SPSR_irq"},
	{ARM64_SYSREG(3, 4, 4, 3, 1), "SPSR_abt"},
	{ARM64_SYSREG(3, 4, 4, 3, 2), "SPSR_und"},
	{ARM64_SYSREG(3, 4, 4, 3, 3), "SPSR_fiq"},
	{ARM64_SYSREG(3, 4, 5, 0, 1), "IFSR32_EL2"},
	{ARM64_SYSREG(3, 4, 5, 3, 0), "FPEXC32_EL2"},
};

const Arm64CpuModel arm64_cortex_a53_bcm2837 = {
	.name = "Cortex-A53 r0p4 (BCM2837)",
	.values = bcm2837_values,
	.num_values = ARRAY_SIZE(bcm2837_values),
	.ccsidr = bcm2837_ccsidr,
	.num_ccsidr = ARRAY_SIZE(bcm2837_ccsidr),
	.impdef = bcm2837_impdef,
	.num_impdef = ARRAY_SIZE(bcm2837_impdef),
	.missing = bcm2837_missing,
	.num_missing = ARRAY_SIZE(bcm2837_missing),
};
