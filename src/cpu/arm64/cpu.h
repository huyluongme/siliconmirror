/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * AArch64 CPU model.
 *
 * Written from the Arm Architecture Reference Manual for A-profile
 * architecture (ARM DDI 0487), issue M.d. Section numbers in the source
 * refer to that manual.
 *
 * The model runs in AArch64 state only: AArch32 is not supported. It
 * takes synchronous exceptions and IRQ and FIQ interrupts to AArch64
 * state; SErrors are not implemented yet. It has the Generic Timer, which
 * counts from the system counter that the board connects, and the
 * interrupt inputs of a Cortex-A53, which the board connects to an
 * interrupt controller. Its MMU does stage 1 translation, with a TLB;
 * stage 2 is not implemented yet.
 *
 * The System registers whose reset value is UNKNOWN reset to 0, unless
 * the CPU model gives a value, so that every run is the same.
 */

#ifndef CPU_ARM64_CPU_H
#define CPU_ARM64_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "core/counter.h"
#include "core/cpu.h"
#include "core/event.h"
#include "core/irq.h"

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
#define ARM64_SCR_IRQ (1u << 1)              /* IRQs to EL3 */
#define ARM64_SCR_FIQ (1u << 2)              /* FIQs to EL3 */
#define ARM64_SCR_SMD (1u << 7)              /* SMC disabled */
#define ARM64_SCR_HCE (1u << 8)              /* HVC enabled */
#define ARM64_SCR_SIF (1u << 9)              /* no Secure fetch from NS */
#define ARM64_SCR_RW (1u << 10)              /* EL2, or EL1, uses AArch64 */
#define ARM64_SCR_ST (1u << 11)              /* Secure EL1 has CNTPS_* */
#define ARM64_SCR_TWI (1u << 12)             /* trap WFI to EL3 */
#define ARM64_SCR_TWE (1u << 13)             /* trap WFE to EL3 */
#define ARM64_HCR_VM (1u << 0)               /* HCR_EL2: stage 2 */
#define ARM64_HCR_FMO (1u << 3)              /* FIQs to EL2 */
#define ARM64_HCR_IMO (1u << 4)              /* IRQs to EL2 */
#define ARM64_HCR_VF (1u << 6)               /* virtual FIQ pending */
#define ARM64_HCR_VI (1u << 7)               /* virtual IRQ pending */
#define ARM64_HCR_DC (1u << 12)              /* default cacheable */
#define ARM64_HCR_TWI (1u << 13)             /* trap WFI to EL2 */
#define ARM64_HCR_TWE (1u << 14)             /* trap WFE to EL2 */
#define ARM64_HCR_TID1 (1u << 16)            /* trap ID group 1 */
#define ARM64_HCR_TID2 (1u << 17)            /* ID group 2 */
#define ARM64_HCR_TID3 (1u << 18)            /* ID group 3 */
#define ARM64_HCR_TSC (1u << 19)             /* trap SMC to EL2 */
#define ARM64_HCR_TIDCP (1u << 20)           /* IMPLEMENTATION DEFINED regs */
#define ARM64_HCR_TACR (1u << 21)            /* ACTLR_EL1 */
#define ARM64_HCR_TSW (1u << 22)             /* DC by set/way */
#define ARM64_HCR_TPC (1u << 23)             /* DC to Point of Coherency */
#define ARM64_HCR_TPU (1u << 24)             /* cache ops to PoU */
#define ARM64_HCR_TTLB (1u << 25)            /* TLBI */
#define ARM64_HCR_TVM (1u << 26)             /* writes of the VM registers */
#define ARM64_HCR_TGE (1u << 27)             /* EL0 traps go to EL2 */
#define ARM64_HCR_TDZ (1u << 28)             /* DC ZVA */
#define ARM64_HCR_HCD (1u << 29)             /* HVC disabled, without EL3 */
#define ARM64_HCR_TRVM (1u << 30)            /* reads of the VM registers */
#define ARM64_HCR_RW (UINT32_C(1) << 31)     /* EL1 uses AArch64 */
#define ARM64_SCTLR_M (1u << 0)              /* MMU enable */
#define ARM64_SCTLR_A (1u << 1)              /* alignment check */
#define ARM64_SCTLR_SA (1u << 3)             /* SP alignment check */
#define ARM64_SCTLR_SA0 (1u << 4)            /* SP alignment check at EL0 */
#define ARM64_SCTLR_UMA (1u << 9)            /* EL0 access to DAIF */
#define ARM64_SCTLR_DZE (1u << 14)           /* EL0 DC ZVA */
#define ARM64_SCTLR_UCT (1u << 15)           /* EL0 access to CTR_EL0 */
#define ARM64_SCTLR_NTWI (1u << 16)          /* EL0 WFI not trapped */
#define ARM64_SCTLR_NTWE (1u << 18)          /* EL0 WFE not trapped */
#define ARM64_SCTLR_WXN (1u << 19)           /* writable means XN */
#define ARM64_SCTLR_E0E (1u << 24)           /* EL0 big-endian */
#define ARM64_SCTLR_EE (1u << 25)            /* big-endian */
#define ARM64_SCTLR_UCI (1u << 26)           /* EL0 cache maintenance */
#define ARM64_CPTR_TCPAC (UINT32_C(1) << 31) /* CPTR_EL2, CPTR_EL3: CPACR */
#define ARM64_MDCR_TDE (1u << 8)             /* MDCR_EL2: debug to EL2 */
#define ARM64_MDCR_TDA (1u << 9)             /* MDCR_EL3: debug registers */

/*
 * The interrupt inputs of the PE, the nIRQ, nFIQ, nVIRQ and nVFIQ pins of
 * a Cortex-A53 (Cortex-A53 TRM, A.5). A line is high while the request
 * is asserted, which is LOW on the pins. A virtual interrupt is pending
 * while its pin or its bit in HCR_EL2, VI or VF, requests it.
 */
typedef enum Arm64Input {
	ARM64_INPUT_IRQ,
	ARM64_INPUT_FIQ,
	ARM64_INPUT_VIRQ,
	ARM64_INPUT_VFIQ,
	ARM64_NUM_INPUTS,
} Arm64Input;

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

	/*
	 * One copy for all the cores of a cluster, such as a register of a
	 * shared L2 memory system: see Arm64Cluster.
	 */
	bool cluster;
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

/*
 * What the cores of one cluster share: the IMPLEMENTATION DEFINED
 * registers that their model marks as cluster registers. The cluster
 * resets on its own, as the cluster-wide reset of a real cluster does:
 * resetting a core leaves them alone.
 */
typedef struct Arm64Cluster {
	const Arm64CpuModel *model;
	uint64_t impdef[ARM64_MAX_IMPDEF]; /* indexed as model->impdef */
} Arm64Cluster;

/*
 * Create a cluster of cores of the given model, in its reset state.
 * Returns NULL when model is NULL or invalid, or memory runs out.
 */
Arm64Cluster *arm64_cluster_create(const Arm64CpuModel *model);
void arm64_cluster_reset(Arm64Cluster *cluster);
void arm64_cluster_destroy(Arm64Cluster *cluster);

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

	/*
	 * The cluster of the core, of the same model, which must stay valid
	 * while the core runs. NULL gives the core its own copy of the
	 * cluster registers, which then reset with the core: a cluster of
	 * one core.
	 */
	Arm64Cluster *cluster;

	/*
	 * The system counter that the Generic Timer counts from, which must
	 * stay valid while the core exists. NULL gives a counter stuck at 0.
	 */
	SystemCounter *counter;
} Arm64Config;

/*
 * The timers of the Generic Timer of an Armv8.0 PE with EL3 and EL2,
 * D12.2.4. Each one has an interrupt output; the names in the comments
 * are those of the Cortex-A53's signals.
 */
typedef enum Arm64TimerId {
	ARM64_TIMER_PHYS,   /* EL1 physical timer, CNTP_*: nCNTPNSIRQ */
	ARM64_TIMER_VIRT,   /* EL1 virtual timer, CNTV_*: nCNTVIRQ */
	ARM64_TIMER_HYP,    /* EL2 physical timer, CNTHP_*: nCNTHPIRQ */
	ARM64_TIMER_SECURE, /* EL3 physical timer, CNTPS_*: nCNTPSIRQ */
	ARM64_NUM_TIMERS,
} Arm64TimerId;

/* One timer: its registers, and the event of its condition being met. */
typedef struct Arm64Timer {
	struct Arm64Cpu *cpu;
	uint64_t ctl;  /* ENABLE and IMASK; ISTATUS is computed when read */
	uint64_t cval; /* the CompareValue */
	Event event;   /* when the condition will be met */
	IrqLine *irq;  /* the interrupt output, NULL when not connected */
} Arm64Timer;

/* The Generic Timer registers of a PE, and its state. */
typedef struct Arm64GenericTimer {
	uint64_t cntfrq_el0, cntkctl_el1, cnthctl_el2, cntvoff_el2;
	Arm64Timer timers[ARM64_NUM_TIMERS];
	/*
	 * The next event of each event stream, D12.2.3: [0] from the
	 * virtual count, under CNTKCTL_EL1; [1] from the physical count,
	 * under CNTHCTL_EL2.
	 */
	Event streams[2];
} Arm64GenericTimer;

/*
 * A translation that the TLB holds, for one 4KB page of virtual
 * addresses, whatever the size of the block or page that maps it.
 */
typedef struct Arm64TlbEntry {
	/*
	 * Bits 63:12 of the virtual address, with its tag removed when TBI
	 * applies, and the translation regime in the low bits: 1 for EL1&0,
	 * 2 for EL2, 3 for EL3. 0 for an unused entry.
	 */
	uint64_t tag;
	uint64_t pa;   /* the physical address of the page */
	uint8_t perms; /* ARM64_PERM_*, see mmu.c */
	uint8_t attr;  /* the memory attribute from MAIR_ELx */
	uint8_t sh;    /* the shareability field of the descriptor */
	uint8_t level; /* the lookup level of the descriptor */
	bool ns;       /* the output address is Non-secure */
} Arm64TlbEntry;

#define ARM64_TLB_ENTRIES 256

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

/* A 4KB page of RAM that an access at el translated. */
#define ARM64_PAGE_ENTRIES 64 /* for each kind of access; a power of 2 */
enum { ARM64_PAGE_FETCH, ARM64_PAGE_READ, ARM64_PAGE_WRITE, ARM64_PAGE_KINDS };

typedef struct Arm64Page {
	uint64_t va;   /* of the page */
	uint8_t *host; /* its bytes in host memory, or NULL: no page */
	unsigned el;
	bool device; /* Device memory: unaligned data accesses fault */
} Arm64Page;

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
	bool il;       /* PSTATE.IL: Illegal Execution state */

	/* The Event Register of WFE and SEV, D1.7.1.1. */
	bool event_register;
	Arm64Wait wait; /* meaningful while cpu.state is CPU_HALTED */

	/* Address of the next instruction, set while one executes. */
	uint64_t next_pc;

	/*
	 * The access that failed, when an instruction ends in a data abort:
	 * a data access, or a read of a translation table (walk). A data
	 * access went to the physical address that the MMU made from
	 * virtual_address. arm64_bus_failure() fills it in.
	 */
	struct {
		uint64_t address;
		unsigned size; /* in bytes */
		bool write;
		const char *reason;
		bool walk;
		uint64_t virtual_address;
	} data_abort;

	/*
	 * The synchronous exception that an instruction raised: the target
	 * Exception level, the syndrome for ESR_ELx, the preferred return
	 * address for ELR_ELx, and the faulting address for FAR_ELx when the
	 * exception has one.
	 */
	struct {
		unsigned target_el;
		uint32_t esr;
		uint64_t return_address;
		bool far_valid;
		uint64_t far;
	} exception;

	/* What is not implemented, when an instruction says so; or "". */
	char detail[64];

	Arm64SysRegs sys;
	Arm64GenericTimer timer;
	IrqLine inputs[ARM64_NUM_INPUTS];
	uint32_t asserted; /* bit n set while inputs[n] is high */
	/*
	 * The MMU: what ID_AA64MMFR0_EL1 says the core has, the size of
	 * physical addresses in bits and whether each translation granule,
	 * 4KB, 16KB or 64KB, is implemented, and the TLB.
	 */
	unsigned pa_bits;
	bool granule_4k, granule_16k, granule_64k;
	Arm64TlbEntry tlb[ARM64_TLB_ENTRIES];

	/*
	 * 4KB pages of RAM that fetches, reads and writes translated, so
	 * that the next accesses there skip the MMU and the bus. What
	 * translates them changes only with the TLB, so that emptying the
	 * TLB empties these too (see internal.h).
	 */
	Arm64Page pages[ARM64_PAGE_KINDS][ARM64_PAGE_ENTRIES];
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

/*
 * Connect the interrupt output of a timer to line, or disconnect it with
 * NULL. The line takes the output's current level.
 */
void arm64_cpu_connect_timer(Arm64Cpu *c, Arm64TimerId id, IrqLine *line);

/* An interrupt input of the PE, for the board to drive. */
IrqLine *arm64_cpu_input(Arm64Cpu *c, Arm64Input input);

#endif /* CPU_ARM64_CPU_H */
