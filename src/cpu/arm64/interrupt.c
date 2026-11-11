/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * IRQ and FIQ interrupts of the AArch64 model: the Exception level that
 * takes them and whether they are masked (D1.4.6.4, Table D1-22), taking
 * them (AArch64_TakePhysicalIRQException() and the like, J1.2.2.16 to
 * J1.2.2.20), how they end WFI and WFE (D1.7), and ISR_EL1.
 *
 * The model is an Armv8.0 PE: without FEAT_NMI no interrupt has
 * Superpriority, without FEAT_VHE HCR_EL2.E2H is 0, and without FEAT_SEL2
 * EL2 exists in Non-secure state only.
 *
 * The CPU looks for an interrupt to take before each instruction, so it
 * takes one as soon as it is pending and not masked, which also covers
 * the Context synchronization events of D1.4.6.5. When several are
 * pending, the order is IMPLEMENTATION DEFINED (D1.4.6.5); this model
 * takes a physical FIQ, then a physical IRQ, then a virtual FIQ, then a
 * virtual IRQ.
 */

#include "cpu/arm64/internal.h"

static uint32_t input_bit(Arm64Input input)
{
	return UINT32_C(1) << input;
}

/*
 * The level that a physical FIQ, or IRQ, goes to from the current level,
 * as AArch64_TakePhysicalFIQException() routes it: to EL3 when SCR_EL3
 * says so, to EL2 from EL0 or EL1 when HCR_EL2 does, and otherwise to
 * the current level or EL1. Returns false when it cannot be taken at
 * all: at EL3, one that does not go to EL3 (C in Table D1-22).
 */
static bool physical_target(const Arm64Cpu *c, bool fiq, unsigned *target)
{
	uint64_t hcr_route = ARM64_HCR_TGE | (fiq ? ARM64_HCR_FMO : ARM64_HCR_IMO);

	if (c->config.highest_el == 3 &&
	    (c->sys.scr_el3 & (fiq ? ARM64_SCR_FIQ : ARM64_SCR_IRQ)))
		*target = 3;
	else if (c->el == 3)
		return false;
	else if (c->el == 2 ||
	         (arm64_el2_enabled(c) && (c->sys.hcr_el2 & hcr_route)))
		*target = 2;
	else
		*target = 1;
	return true;
}

/*
 * Whether PSTATE.F, or PSTATE.I, masks an interrupt to target (RMHWBP,
 * RGFXKY, RXBYXL): it does at the target level, and from EL0 to EL1. An
 * interrupt to EL2 or EL3 from a lower level cannot be masked.
 */
static bool masked(const Arm64Cpu *c, unsigned target, bool fiq)
{
	if (target > c->el && target >= 2)
		return false;
	return (c->daif & (fiq ? ARM64_DAIF_F : ARM64_DAIF_I)) != 0;
}

/* Whether a virtual FIQ, or IRQ, is requested: by its pin or HCR_EL2. */
static bool virtual_requested(const Arm64Cpu *c, bool fiq)
{
	if (c->asserted & input_bit(fiq ? ARM64_INPUT_VFIQ : ARM64_INPUT_VIRQ))
		return true;
	return (c->sys.hcr_el2 & (fiq ? ARM64_HCR_VF : ARM64_HCR_VI)) != 0;
}

/*
 * Whether a virtual FIQ, or IRQ, is pending, as AArch64_InterruptPending()
 * sees it: at EL0 or EL1 with EL2 enabled, HCR_EL2.TGE 0, and FMO, or
 * IMO, set. It then goes to EL1, masked by PSTATE.F, or PSTATE.I
 * (D1.4.6.4.2).
 */
static bool virtual_pending(const Arm64Cpu *c, bool fiq)
{
	uint64_t hcr = c->sys.hcr_el2;

	if (c->el > 1 || !arm64_el2_enabled(c) || (hcr & ARM64_HCR_TGE))
		return false;
	if (!(hcr & (fiq ? ARM64_HCR_FMO : ARM64_HCR_IMO)))
		return false;
	return virtual_requested(c, fiq);
}

/*
 * The interrupt that the PE would take now, if any: whether it is an FIQ
 * and the level it goes to.
 */
static bool next_interrupt(const Arm64Cpu *c, bool *fiq, unsigned *target)
{
	for (int i = 0; i < 2; i++) {
		bool f = i == 0;

		if ((c->asserted & input_bit(f ? ARM64_INPUT_FIQ : ARM64_INPUT_IRQ)) &&
		    physical_target(c, f, target) && !masked(c, *target, f)) {
			*fiq = f;
			return true;
		}
	}
	for (int i = 0; i < 2; i++) {
		bool f = i == 0;

		if (virtual_pending(c, f) &&
		    !(c->daif & (f ? ARM64_DAIF_F : ARM64_DAIF_I))) {
			*fiq = f;
			*target = 1;
			return true;
		}
	}
	return false;
}

bool arm64_interrupt_pending(const Arm64Cpu *c)
{
	if (c->asserted & (input_bit(ARM64_INPUT_IRQ) | input_bit(ARM64_INPUT_FIQ)))
		return true;
	return virtual_pending(c, false) || virtual_pending(c, true);
}

bool arm64_take_pending_interrupt(Arm64Cpu *c)
{
	bool fiq;
	unsigned target;

	if (!next_interrupt(c, &fiq, &target))
		return false;
	arm64_take_interrupt(c, target, fiq);
	return true;
}

/*
 * Whether a PE that waits ends its wait. WFE ends on an interrupt that
 * the PE takes (D1.7.1.4). WFI ends on any interrupt that
 * AArch64_InterruptPending() sees, the same that make Hint_WFI() not
 * wait: D1.7.2.1 makes fewer of them wakeup events, but a PE may leave
 * the low-power state for any reason (IMFLXH).
 */
static bool wakes(const Arm64Cpu *c)
{
	bool fiq;
	unsigned target;

	if (c->wait == ARM64_WAIT_INTERRUPT)
		return arm64_interrupt_pending(c);
	return next_interrupt(c, &fiq, &target);
}

static void input_changed(void *ctx, unsigned n, bool level)
{
	Arm64Cpu *c = ctx;

	if (level)
		c->asserted |= UINT32_C(1) << n;
	else
		c->asserted &= ~(UINT32_C(1) << n);
	if (level && c->cpu.state == CPU_HALTED && wakes(c))
		c->cpu.state = CPU_RUNNING;
}

void arm64_interrupt_init(Arm64Cpu *c)
{
	for (unsigned i = 0; i < ARM64_NUM_INPUTS; i++)
		irq_init(&c->inputs[i], input_changed, c, i);
	c->asserted = 0;
}

/*
 * ISR_EL1, D24.2: whether an IRQ or FIQ is pending, physical or, at EL1
 * when HCR_EL2 routes physical ones to EL2, virtual. SErrors are not
 * implemented, so A is 0.
 */
uint64_t arm64_isr(const Arm64Cpu *c)
{
	uint64_t value = 0;

	for (int i = 0; i < 2; i++) {
		bool fiq = i == 0;
		bool is_virtual =
			c->el == 1 && arm64_el2_enabled(c) &&
			(c->sys.hcr_el2 & (fiq ? ARM64_HCR_FMO : ARM64_HCR_IMO));
		bool pending;

		if (is_virtual)
			pending = virtual_requested(c, fiq);
		else
			pending = c->asserted &
			          input_bit(fiq ? ARM64_INPUT_FIQ : ARM64_INPUT_IRQ);
		if (pending)
			value |= fiq ? ARM64_DAIF_F : ARM64_DAIF_I;
	}
	return value;
}

IrqLine *arm64_cpu_input(Arm64Cpu *c, Arm64Input input)
{
	return &c->inputs[input];
}
