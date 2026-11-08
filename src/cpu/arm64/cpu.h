/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * AArch64 CPU model.
 *
 * Written from the Arm Architecture Reference Manual for A-profile
 * architecture (ARM DDI 0487), issue M.d. Section numbers in the source
 * refer to that manual.
 *
 * The model runs in AArch64 state only: AArch32 is not supported.
 * Exceptions are not implemented yet; anything that would take one stops
 * the machine with an error message instead.
 *
 * The System registers whose reset value is UNKNOWN reset to 0, unless
 * the CPU model gives a value, so that every run is the same.
 */

#ifndef CPU_ARM64_CPU_H
#define CPU_ARM64_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cpu.h"

/* PSTATE.{N,Z,C,V}, in their positions in the NZCV register. */
#define ARM64_NZCV_V (1u << 28)
#define ARM64_NZCV_C (1u << 29)
#define ARM64_NZCV_Z (1u << 30)
#define ARM64_NZCV_N (1u << 31)

/* PSTATE.{D,A,I,F}, in their positions in the DAIF register. */
#define ARM64_DAIF_F (1u << 6)
#define ARM64_DAIF_I (1u << 7)
#define ARM64_DAIF_A (1u << 8)
#define ARM64_DAIF_D (1u << 9)

/* Fields of System registers that the model acts on. */
#define ARM64_SCR_NS (1u << 0)               /* SCR_EL3: lower ELs Non-secure */
#define ARM64_HCR_TID1 (1u << 16)            /* HCR_EL2: trap ID group 1 */
#define ARM64_HCR_TID2 (1u << 17)            /* ID group 2 */
#define ARM64_HCR_TID3 (1u << 18)            /* ID group 3 */
#define ARM64_HCR_TACR (1u << 21)            /* ACTLR_EL1 */
#define ARM64_HCR_TVM (1u << 26)             /* writes of the VM registers */
#define ARM64_HCR_TGE (1u << 27)             /* EL0 traps go to EL2 */
#define ARM64_HCR_TRVM (1u << 30)            /* reads of the VM registers */
#define ARM64_SCTLR_M (1u << 0)              /* MMU enable */
#define ARM64_SCTLR_UMA (1u << 9)            /* EL0 access to DAIF */
#define ARM64_SCTLR_UCT (1u << 15)           /* EL0 access to CTR_EL0 */
#define ARM64_SCTLR_E0E (1u << 24)           /* EL0 big-endian */
#define ARM64_SCTLR_EE (1u << 25)            /* big-endian */
#define ARM64_CPTR_TCPAC (UINT32_C(1) << 31) /* CPTR_EL2, CPTR_EL3: CPACR */
#define ARM64_MDCR_TDA (1u << 9)             /* MDCR_EL3: debug registers */

/* What a CPU_HALTED CPU waits for, D1.7. */
typedef enum Arm64Wait {
	ARM64_WAIT_INTERRUPT, /* after WFI */
	ARM64_WAIT_EVENT,     /* after WFE */
} Arm64Wait;

/*
 * A System register value from the manual of a core: the value of an ID
 * register, or the reset value of another register.
 */
typedef struct Arm64RegValue {
	uint16_t encoding; /* ARM64_SYSREG(), see sysreg.h */
	uint64_t value;
} Arm64RegValue;

/*
 * An IMPLEMENTATION DEFINED System register of a core. It can be read
 * from EL1 up. Unless it is read-only, EL3 can write it, and so can EL2
 * when bit actlr_bit of ACTLR_EL3 is set, and EL1 when that bit is also
 * set in ACTLR_EL2, or EL1 is Secure. actlr_bit is -1 for a register
 * that ACTLR does not control.
 */
typedef struct Arm64ImpDefReg {
	uint16_t encoding;
	const char *name;
	uint64_t reset;
	bool read_only;

	/*
	 * A write of any value stores write_value: for a register whose
	 * fields software cannot change, or one that any write clears.
	 */
	bool fixed_write;
	uint64_t write_value;

	int actlr_bit;
} Arm64ImpDefReg;

#define ARM64_MAX_IMPDEF 8

/* A System register by encoding and name. */
typedef struct Arm64NamedReg {
	uint16_t encoding;
	const char *name;
} Arm64NamedReg;

/* What the manual of a core gives about its System registers. */
typedef struct Arm64CpuModel {
	const char *name;

	/* ID registers and reset values; registers not listed hold 0. */
	const Arm64RegValue *values;
	unsigned num_values;

	/* CCSIDR_EL1 for each value of CSSELR_EL1; 0 past the end. */
	const uint32_t *ccsidr;
	unsigned num_ccsidr;

	const Arm64ImpDefReg *impdef;
	unsigned num_impdef; /* at most ARM64_MAX_IMPDEF */

	/*
	 * Registers that the core has and that this emulator does not
	 * implement yet: using one stops the machine and names it. Other
	 * unknown encodings are UNDEFINED.
	 */
	const Arm64NamedReg *missing;
	unsigned num_missing;
} Arm64CpuModel;

/* The Cortex-A53 r0p4 as the BCM2837 of the Raspberry Pi 3 has it. */
extern const Arm64CpuModel arm64_cortex_a53_bcm2837;

typedef struct Arm64Config {
	/*
	 * Address of the first instruction after reset: the IMPLEMENTATION
	 * DEFINED reset vector, RVBAR_ELx.
	 */
	uint64_t reset_pc;

	/* Highest implemented Exception level, 1 to 3. Reset enters it. */
	unsigned highest_el;

	/*
	 * The core. NULL gives a core whose ID registers all read as 0 and
	 * that has no IMPLEMENTATION DEFINED registers.
	 */
	const Arm64CpuModel *model;

	/* MPIDR_EL1: where this core sits in the system. */
	uint64_t mpidr;
} Arm64Config;

/*
 * The System registers that are stored as they are written. The arrays
 * hold one register per Exception level, indexed by the level; levels
 * without such a register are unused. Registers that come from PSTATE or
 * from the CPU model, such as NZCV or MIDR_EL1, are not here.
 */
typedef struct Arm64SysRegs {
	uint64_t sctlr[4], actlr[4], vbar[4], esr[4], far[4], elr[4], spsr[4];
	uint64_t afsr0[4], afsr1[4], tpidr[4];
	uint64_t ttbr0[4], tcr[4], mair[4], amair[4];
	uint64_t scr_el3, hcr_el2, cpacr_el1, cptr_el2, cptr_el3;
	uint64_t mdcr_el2, mdcr_el3, hstr_el2, hacr_el2, sder32_el3;
	uint64_t ttbr1_el1, contextidr_el1, vttbr_el2, vtcr_el2, par_el1;
	uint64_t hpfar_el2, csselr_el1, vpidr_el2, vmpidr_el2, tpidrro_el0;
	uint64_t rmr_el3;
	uint64_t impdef[ARM64_MAX_IMPDEF]; /* as listed in config.model */
} Arm64SysRegs;

typedef struct Arm64Cpu {
	Cpu cpu;
	Arm64Config config;

	uint64_t x[31];    /* R0-R30; number 31 is the zero register or SP */
	uint64_t sp_el[4]; /* SP_EL0 to SP_EL3 */
	uint64_t pc;

	/* PSTATE fields, see the PSTATE pseudocode in J1. */
	uint32_t nzcv; /* N, Z, C and V in bits 31:28, as in NZCV */
	uint32_t daif; /* D, A, I and F in bits 9:6, as in DAIF */
	unsigned el;   /* PSTATE.EL, the current Exception level */
	bool sp_sel;   /* PSTATE.SP: false selects SP_EL0, true SP_ELx */

	/* The Event Register of WFE and SEV, D1.7.1.1. */
	bool event_register;
	Arm64Wait wait; /* meaningful while cpu.state is CPU_HALTED */

	/* Address of the next instruction, set while one executes. */
	uint64_t next_pc;

	/* The data access that failed, when an instruction ends in a data abort. */
	struct {
		uint64_t address;
		unsigned size; /* in bytes */
		bool write;
		const char *reason;
	} data_abort;

	/*
	 * The exception that an instruction raised: the target Exception
	 * level and the syndrome for ESR_ELx.
	 */
	struct {
		unsigned target_el;
		uint32_t esr;
	} exception;

	/* What is not implemented, when an instruction says so; or "". */
	char detail[64];

	Arm64SysRegs sys;
} Arm64Cpu;

/*
 * Create a CPU in its reset state. Add it to a machine with
 * machine_add_cpu() before running it. Returns NULL when config is
 * invalid or memory runs out.
 */
Cpu *arm64_cpu_create(const Arm64Config *config);

/* The Arm64Cpu that contains cpu, which must come from arm64_cpu_create(). */
Arm64Cpu *arm64_cpu_of(Cpu *cpu);

/*
 * Read or write a System register directly, as a board that plays the
 * firmware's part or a debugger does: without the access checks of MRS
 * and MSR. encoding is an ARM64_SYSREG() value, see sysreg.h. Return
 * false for a register that the model does not implement, and for a
 * write to a read-only one.
 */
bool arm64_cpu_read_sysreg(Arm64Cpu *c, uint16_t encoding, uint64_t *value);
bool arm64_cpu_write_sysreg(Arm64Cpu *c, uint16_t encoding, uint64_t value);

#endif /* CPU_ARM64_CPU_H */
