/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the IRQ and FIQ interrupts of the AArch64 model
 * (src/cpu/arm64/interrupt.c). Section numbers refer to the Arm
 * Architecture Reference Manual, ARM DDI 0487 M.d.
 *
 * A test drives the inputs of a Cortex-A53, puts a NOP, WFI or WFE at
 * the PC and runs one step: the PE either takes an interrupt, and is at
 * a vector, or executes the instruction.
 */

#include <stdint.h>

#include "arm64_test.h"
#include "core/irq.h"
#include "cpu/arm64/internal.h"
#include "cpu/arm64/sysreg.h"

#define NOP 0xd503201fu
#define WFE 0xd503205fu
#define WFI 0xd503207fu
#define VBAR(el) (UINT64_C(0x1000) * (el))
#define ESR_BEFORE 0x12345678 /* what ESR_ELx holds before an interrupt */

/* Vector offsets, D1.4.1.6. */
#define CURRENT_SP0 0x000
#define CURRENT_SPX 0x200
#define LOWER_64 0x400
#define IRQ 0x80
#define FIQ 0x100

static Machine *machine(Arm64Cpu **c)
{
	Arm64Config config = {
		.reset_pc = RESET_PC,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
	};
	Machine *m = machine_create();
	Cpu *base = arm64_cpu_create(&config);

	CHECK(m != NULL && base != NULL);
	CHECK_EQ(bus_map_ram(m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK(machine_add_cpu(m, base));
	*c = arm64_cpu_of(base);
	for (unsigned el = 1; el <= 3; el++) {
		(*c)->sys.vbar[el] = VBAR(el);
		(*c)->sys.esr[el] = ESR_BEFORE;
	}
	return m;
}

/*
 * Put the PE at el with SP_ELx and the masks I and F clear, every level
 * using AArch64, Non-secure unless secure.
 */
static void enter(Arm64Cpu *c, unsigned el, bool secure)
{
	c->sys.scr_el3 = ARM64_SCR_RW | (secure ? 0 : ARM64_SCR_NS);
	c->sys.hcr_el2 = ARM64_HCR_RW;
	c->el = el;
	c->sp_sel = el > 0;
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A;
	c->pc = RESET_PC;
}

static void set_input(Arm64Cpu *c, Arm64Input input, bool level)
{
	irq_set(arm64_cpu_input(c, input), level);
}

/* Run one step with insn at the PC. */
static void step(Machine *m, Arm64Cpu *c, uint32_t insn)
{
	CHECK_EQ(bus_write(m->bus, c->pc, 4, insn), BUS_OK);
	machine_run(m, 1);
}

/*
 * Check that one step takes an interrupt to target at the vector offset,
 * as AArch64_TakeException() does: SPSR and ELR get PSTATE and the
 * address of the instruction not run, ESR is left alone, and every mask
 * is set.
 */
static void check_taken(Machine *m, Arm64Cpu *c, unsigned target,
                        uint64_t offset, int line)
{
	uint64_t spsr = (uint64_t)c->nzcv | c->daif | (uint64_t)c->el << 2 |
	                (c->sp_sel ? 1 : 0);
	uint64_t pc = c->pc;

	step(m, c, NOP);
	if (c->el != target || c->pc != VBAR(target) + offset ||
	    c->sys.elr[target] != pc || c->sys.spsr[target] != spsr ||
	    c->sys.esr[target] != ESR_BEFORE || !c->sp_sel ||
	    c->daif !=
	        (ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F)) {
		fprintf(stderr,
		        "%s:%d: at EL%u PC 0x%" PRIx64 ", expected EL%u PC 0x%" PRIx64
		        "\n",
		        __FILE__, line, c->el, c->pc, target, VBAR(target) + offset);
		test_failures++;
	}
}

/* Check that one step runs the NOP at the PC, taking no interrupt. */
static void check_not_taken(Machine *m, Arm64Cpu *c, int line)
{
	uint64_t pc = c->pc;
	unsigned el = c->el;

	step(m, c, NOP);
	if (c->el != el || c->pc != pc + 4) {
		fprintf(stderr, "%s:%d: an interrupt was taken to EL%u\n", __FILE__,
		        line, c->el);
		test_failures++;
	}
}

#define CHECK_TAKEN(m, c, target, offset)                                      \
	check_taken(m, c, target, offset, __LINE__)
#define CHECK_NOT_TAKEN(m, c) check_not_taken(m, c, __LINE__)

/*
 * Table D1-22: where a physical IRQ goes from each level, and whether
 * PSTATE.I masks it (B), it is taken regardless (A), or not at all (C).
 */
static void test_physical_irq(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	set_input(c, ARM64_INPUT_IRQ, true);

	/* At EL3: C without SCR_EL3.IRQ, B with it. */
	enter(c, 3, false);
	CHECK_NOT_TAKEN(m, c);
	c->sys.scr_el3 |= ARM64_SCR_IRQ;
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 3, CURRENT_SPX + IRQ);

	/* At EL2: B, to EL2; A to EL3 with SCR_EL3.IRQ. */
	enter(c, 2, false);
	c->sp_sel = false;
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 2, CURRENT_SP0 + IRQ);
	enter(c, 2, false);
	c->sys.scr_el3 |= ARM64_SCR_IRQ;
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	c->daif |= ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 3, LOWER_64 + IRQ);

	/* At EL1: B to EL1; A to EL2 with HCR_EL2.IMO, and to EL3. */
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	c->daif |= ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 2, LOWER_64 + IRQ);
	enter(c, 1, false);
	c->sys.scr_el3 |= ARM64_SCR_IRQ;
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	c->daif |= ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 3, LOWER_64 + IRQ);

	/* At EL0: B to EL1, which PSTATE.I masks; A to EL2 with TGE. */
	enter(c, 0, false);
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 1, LOWER_64 + IRQ);
	enter(c, 0, false);
	c->sys.hcr_el2 |= ARM64_HCR_TGE;
	c->daif |= ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 2, LOWER_64 + IRQ);

	/* Secure EL1 has no EL2: HCR_EL2 does not route, B to EL1. */
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_TGE;
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);

	/* PSTATE.F does not mask an IRQ. */
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_F;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);

	/* With the line low, nothing happens. */
	set_input(c, ARM64_INPUT_IRQ, false);
	enter(c, 1, false);
	CHECK_NOT_TAKEN(m, c);
	machine_destroy(m);
}

/* The same for an FIQ, with SCR_EL3.FIQ, HCR_EL2.FMO and PSTATE.F. */
static void test_physical_fiq(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	set_input(c, ARM64_INPUT_FIQ, true);
	enter(c, 3, false);
	c->sys.scr_el3 |= ARM64_SCR_IRQ;
	CHECK_NOT_TAKEN(m, c);
	c->sys.scr_el3 |= ARM64_SCR_FIQ;
	c->daif |= ARM64_DAIF_F;
	CHECK_NOT_TAKEN(m, c);
	c->daif = ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 3, CURRENT_SPX + FIQ);

	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	c->daif |= ARM64_DAIF_F;
	CHECK_NOT_TAKEN(m, c);
	c->sys.hcr_el2 |= ARM64_HCR_FMO;
	CHECK_TAKEN(m, c, 2, LOWER_64 + FIQ);

	enter(c, 0, false);
	CHECK_TAKEN(m, c, 1, LOWER_64 + FIQ);
	enter(c, 0, false);
	c->sys.scr_el3 |= ARM64_SCR_FIQ;
	c->daif |= ARM64_DAIF_F;
	CHECK_TAKEN(m, c, 3, LOWER_64 + FIQ);
	machine_destroy(m);
}

/* This model's order: physical FIQ, IRQ, then virtual FIQ, IRQ. */
static void test_priority(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	set_input(c, ARM64_INPUT_IRQ, true);
	set_input(c, ARM64_INPUT_FIQ, true);
	enter(c, 1, false);
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + FIQ);
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_F;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);

	/* A physical IRQ to EL2 goes before a virtual FIQ to EL1. */
	set_input(c, ARM64_INPUT_FIQ, false);
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_FMO | ARM64_HCR_VF;
	CHECK_TAKEN(m, c, 2, LOWER_64 + IRQ);
	set_input(c, ARM64_INPUT_IRQ, false);
	enter(c, 1, false);
	c->sys.hcr_el2 |=
		ARM64_HCR_IMO | ARM64_HCR_FMO | ARM64_HCR_VF | ARM64_HCR_VI;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + FIQ);
	machine_destroy(m);
}

/*
 * Virtual interrupts (D1.4.6.4.2): from HCR_EL2.VI and VF or the nVIRQ
 * and nVFIQ pins, at EL0 and EL1 when HCR_EL2 routes the physical ones to
 * EL2 and TGE is 0; to EL1, masked by PSTATE.
 */
static void test_virtual(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_VI;
	CHECK_NOT_TAKEN(m, c); /* IMO is 0 */
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	c->daif |= ARM64_DAIF_I;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_I;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);

	enter(c, 0, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_VI;
	CHECK_TAKEN(m, c, 1, LOWER_64 + IRQ);

	/* Not with TGE, at EL2, nor in Secure state. */
	enter(c, 0, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_VI | ARM64_HCR_TGE;
	CHECK_NOT_TAKEN(m, c);
	enter(c, 2, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_VI;
	CHECK_NOT_TAKEN(m, c);
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_VI;
	CHECK_NOT_TAKEN(m, c);

	/* The pins request them too. */
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_FMO;
	CHECK_NOT_TAKEN(m, c);
	set_input(c, ARM64_INPUT_VIRQ, true);
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);
	set_input(c, ARM64_INPUT_VIRQ, false);
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_FMO;
	set_input(c, ARM64_INPUT_VFIQ, true);
	c->daif |= ARM64_DAIF_F;
	CHECK_NOT_TAKEN(m, c);
	c->daif &= ~ARM64_DAIF_F;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + FIQ);
	set_input(c, ARM64_INPUT_VFIQ, false);
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_FMO | ARM64_HCR_VF;
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + FIQ);
	machine_destroy(m);
}

/*
 * WFI (Hint_WFI()): it does not wait while an interrupt is pending,
 * masked or not, and a pending interrupt ends the wait.
 */
static void test_wfi(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	/* Masked and pending: WFI completes at once. */
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_I;
	set_input(c, ARM64_INPUT_IRQ, true);
	step(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK_EQ(c->pc, RESET_PC + 4);

	/* Unmasked: the interrupt comes first, WFI returns to the WFI. */
	c->daif &= ~ARM64_DAIF_I;
	c->pc = RESET_PC;
	CHECK_EQ(bus_write(m->bus, RESET_PC, 4, WFI), BUS_OK);
	machine_run(m, 1);
	CHECK_EQ(c->pc, VBAR(1) + CURRENT_SPX + IRQ);
	CHECK_EQ(c->sys.elr[1], RESET_PC);
	set_input(c, ARM64_INPUT_IRQ, false);

	/* Nothing pending: WFI waits, and a masked IRQ ends the wait. */
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_I;
	step(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	CHECK_EQ(c->pc, RESET_PC + 4);
	set_input(c, ARM64_INPUT_FIQ, false);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_input(c, ARM64_INPUT_IRQ, true);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK_NOT_TAKEN(m, c);
	set_input(c, ARM64_INPUT_IRQ, false);

	/* So does one that cannot be taken here, at EL3. */
	enter(c, 3, false);
	step(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_input(c, ARM64_INPUT_FIQ, true);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	set_input(c, ARM64_INPUT_FIQ, false);

	/* A virtual one, only when it can be pending. */
	enter(c, 1, false);
	step(m, c, WFI);
	set_input(c, ARM64_INPUT_VIRQ, true);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_input(c, ARM64_INPUT_VIRQ, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	set_input(c, ARM64_INPUT_VIRQ, true);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + IRQ);
	CHECK_EQ(c->sys.elr[1], RESET_PC + 4);
	set_input(c, ARM64_INPUT_VIRQ, false);

	/* HCR_EL2.VI or VF pending makes WFI complete at once. */
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_VI;
	c->daif |= ARM64_DAIF_I;
	step(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK_EQ(c->pc, RESET_PC + 4);
	enter(c, 1, false);
	c->sys.hcr_el2 |= ARM64_HCR_FMO | ARM64_HCR_VF;
	c->daif |= ARM64_DAIF_F;
	step(m, c, WFI);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK_EQ(c->pc, RESET_PC + 4);
	machine_destroy(m);
}

/* WFE (D1.7.1.4): only an interrupt that the PE takes ends the wait. */
static void test_wfe(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	enter(c, 1, false);
	c->event_register = false;
	c->daif |= ARM64_DAIF_I;
	step(m, c, WFE);
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_input(c, ARM64_INPUT_IRQ, true); /* masked */
	CHECK_EQ(c->cpu.state, CPU_HALTED);
	set_input(c, ARM64_INPUT_FIQ, true);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	CHECK(!c->event_register);
	CHECK_TAKEN(m, c, 1, CURRENT_SPX + FIQ);
	CHECK_EQ(c->sys.elr[1], RESET_PC + 4);

	/* A masked interrupt pending does not stop WFE from waiting. */
	enter(c, 1, false);
	c->daif |= ARM64_DAIF_I | ARM64_DAIF_F;
	step(m, c, WFE);
	CHECK_EQ(c->cpu.state, CPU_HALTED);

	/* One to EL2 cannot be masked at EL1. */
	set_input(c, ARM64_INPUT_IRQ, false);
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	set_input(c, ARM64_INPUT_IRQ, true);
	CHECK_EQ(c->cpu.state, CPU_RUNNING);
	machine_destroy(m);
}

/* ISR_EL1 (D24.2): physical, or at EL1 with IMO or FMO, virtual. */
static void test_isr(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);
	uint64_t isr;

	enter(c, 1, false);
	CHECK(arm64_cpu_read_sysreg(c, ARM64_ISR_EL1, &isr));
	CHECK_EQ(isr, 0);
	set_input(c, ARM64_INPUT_IRQ, true);
	CHECK_EQ(arm64_isr(c), 0x80);
	set_input(c, ARM64_INPUT_FIQ, true);
	CHECK_EQ(arm64_isr(c), 0xc0);
	set_input(c, ARM64_INPUT_IRQ, false);
	CHECK_EQ(arm64_isr(c), 0x40);

	/* At EL1 with FMO, F is the virtual FIQ. */
	c->sys.hcr_el2 |= ARM64_HCR_FMO;
	CHECK_EQ(arm64_isr(c), 0);
	c->sys.hcr_el2 |= ARM64_HCR_VF | ARM64_HCR_VI;
	CHECK_EQ(arm64_isr(c), 0x40);
	set_input(c, ARM64_INPUT_IRQ, true);
	CHECK_EQ(arm64_isr(c), 0xc0); /* I is the physical IRQ */
	c->sys.hcr_el2 &= ~ARM64_HCR_VI;
	c->sys.hcr_el2 |= ARM64_HCR_IMO;
	CHECK_EQ(arm64_isr(c), 0x40);
	set_input(c, ARM64_INPUT_VIRQ, true);
	CHECK_EQ(arm64_isr(c), 0xc0);

	/* At EL2, and in Secure EL1, the physical ones. */
	set_input(c, ARM64_INPUT_FIQ, false);
	c->el = 2;
	CHECK_EQ(arm64_isr(c), 0x80);
	enter(c, 1, true);
	c->sys.hcr_el2 |= ARM64_HCR_IMO | ARM64_HCR_FMO | ARM64_HCR_VF;
	CHECK_EQ(arm64_isr(c), 0x80);
	machine_destroy(m);
}

/* The inputs are wires: a reset of the PE leaves their levels alone. */
static void test_reset_keeps_inputs(void)
{
	Arm64Cpu *c;
	Machine *m = machine(&c);

	set_input(c, ARM64_INPUT_IRQ, true);
	machine_reset(m);
	CHECK(irq_level(arm64_cpu_input(c, ARM64_INPUT_IRQ)));
	CHECK_EQ(arm64_isr(c), 0x80);
	CHECK_EQ(c->el, 3);
	CHECK_NOT_TAKEN(m, c);
	machine_destroy(m);
}

int main(void)
{
	RUN_TEST(test_physical_irq);
	RUN_TEST(test_physical_fiq);
	RUN_TEST(test_priority);
	RUN_TEST(test_virtual);
	RUN_TEST(test_wfi);
	RUN_TEST(test_wfe);
	RUN_TEST(test_isr);
	RUN_TEST(test_reset_keeps_inputs);
	return test_summary();
}
