/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The ARM interrupt controller of the BCM2835. Section numbers refer to
 * chapter 7 of Broadcom's "BCM2835 ARM Peripherals" (2012).
 *
 * Every interrupt is level sensitive and has an enable bit (7.1). The
 * pending registers show the interrupts that are both raised and enabled
 * (7.5): pending 1 and 2 the GPU's, and the basic pending register the
 * ARM's, eleven selected GPU interrupts again, and two bits that tell
 * whether pending 1 or 2 has others. The IRQ output is set while any
 * interrupt is pending. The FIQ output follows the one interrupt that
 * the FIQ register selects, enabled or not (7.3).
 *
 * What the document leaves open, this model chooses:
 *  - The disable registers read as the enable bits, like the enable
 *    registers.
 *  - The table of section 7.5 marks the FIQ enable bit as read-only, but
 *    software sets it to enable the FIQ: it is read-write.
 *  - Selecting an FIQ source of 72 to 127, which the document says not to
 *    use, with the FIQ enabled, stops the machine.
 *  - Writes to the pending registers are ignored.
 */

#include "hw/intc/bcm2835_ic.h"

#include <inttypes.h>
#include <stdlib.h>

/* Registers, as offsets from 0x7E00B200. */
enum {
	REG_BASIC_PENDING = 0x00,
	REG_PENDING1 = 0x04,
	REG_PENDING2 = 0x08,
	REG_FIQ = 0x0c,
	REG_ENABLE1 = 0x10,
	REG_ENABLE2 = 0x14,
	REG_ENABLE_BASIC = 0x18,
	REG_DISABLE1 = 0x1c,
	REG_DISABLE2 = 0x20,
	REG_DISABLE_BASIC = 0x24,
	REG_END = 0x28,
};

#define FIQ_ENABLE (1u << 7)
#define FIQ_SOURCE 0x7fu
#define FIQ_FIRST_ARM 64 /* FIQ sources 64 to 71 are the ARM interrupts */
#define FIQ_SOURCES 72

/*
 * The GPU interrupts that the basic pending register shows again, in its
 * bits 10 to 20 (section 7.5).
 */
static const uint8_t selected[] = {7, 9, 10, 18, 19, 53, 54, 55, 56, 57, 62};

#define BASIC_SELECTED0 10 /* the bit of selected[0] */
#define BASIC_PENDING1 (1u << 8)
#define BASIC_PENDING2 (1u << 9)

struct Bcm2835Ic {
	Machine *machine;
	IrqLine gpu[BCM2835_IC_NUM_GPU];
	IrqLine arm[BCM2835_IC_NUM_ARM];
	IrqLine *irq_out, *fiq_out;

	uint64_t gpu_raised; /* bit n: GPU interrupt n is raised */
	uint32_t arm_raised; /* bit n: ARM interrupt n is raised */
	uint64_t gpu_enabled;
	uint32_t arm_enabled;
	uint32_t fiq;
};

/* All the selected GPU interrupts, as a mask. */
static uint64_t selected_mask(void)
{
	uint64_t mask = 0;

	for (unsigned i = 0; i < sizeof(selected); i++)
		mask |= UINT64_C(1) << selected[i];
	return mask;
}

/* The basic pending register, section 7.5. */
static uint32_t basic_pending(const Bcm2835Ic *ic)
{
	uint64_t gpu = ic->gpu_raised & ic->gpu_enabled;
	uint64_t others = gpu & ~selected_mask();
	uint32_t value = ic->arm_raised & ic->arm_enabled;

	if ((uint32_t)others != 0)
		value |= BASIC_PENDING1;
	if ((uint32_t)(others >> 32) != 0)
		value |= BASIC_PENDING2;
	for (unsigned i = 0; i < sizeof(selected); i++)
		if (gpu >> selected[i] & 1)
			value |= 1u << (BASIC_SELECTED0 + i);
	return value;
}

static bool fiq_level(const Bcm2835Ic *ic)
{
	unsigned source = ic->fiq & FIQ_SOURCE;

	/* A write never enables the FIQ with a source past 71. */
	if (!(ic->fiq & FIQ_ENABLE))
		return false;
	if (source < FIQ_FIRST_ARM)
		return (ic->gpu_raised >> source & 1) != 0;
	return (ic->arm_raised >> (source - FIQ_FIRST_ARM) & 1) != 0;
}

/* Drive the outputs after a change. */
static void update(Bcm2835Ic *ic)
{
	bool irq = (ic->gpu_raised & ic->gpu_enabled) != 0 ||
	           (ic->arm_raised & ic->arm_enabled) != 0;

	irq_set(ic->irq_out, irq);
	irq_set(ic->fiq_out, fiq_level(ic));
}

/* Inputs 0 to 63 are the GPU interrupts, 64 to 71 the ARM ones. */
static void input_changed(void *ctx, unsigned n, bool level)
{
	Bcm2835Ic *ic = ctx;

	if (n < BCM2835_IC_NUM_GPU) {
		uint64_t bit = UINT64_C(1) << n;

		ic->gpu_raised = level ? ic->gpu_raised | bit : ic->gpu_raised & ~bit;
	} else {
		uint32_t bit = 1u << (n - BCM2835_IC_NUM_GPU);

		ic->arm_raised = level ? ic->arm_raised | bit : ic->arm_raised & ~bit;
	}
	update(ic);
}

static void not_implemented(Bcm2835Ic *ic, const char *access, uint64_t offset,
                            unsigned size)
{
	machine_stop(ic->machine, MACHINE_STOP_ERROR,
	             "bcm2835-ic: %u-byte %s offset 0x%03" PRIx64
	             " is not implemented yet",
	             size, access, offset);
}

static uint64_t ic_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Ic *ic = ctx;
	uint64_t gpu = ic->gpu_raised & ic->gpu_enabled;

	if (size != 4 || offset >= REG_END) {
		not_implemented(ic, "read from", offset, size);
		return 0;
	}
	switch (offset) {
	case REG_BASIC_PENDING:
		return basic_pending(ic);
	case REG_PENDING1:
		return (uint32_t)gpu;
	case REG_PENDING2:
		return (uint32_t)(gpu >> 32);
	case REG_FIQ:
		return ic->fiq;
	case REG_ENABLE1:
	case REG_DISABLE1:
		return (uint32_t)ic->gpu_enabled;
	case REG_ENABLE2:
	case REG_DISABLE2:
		return (uint32_t)(ic->gpu_enabled >> 32);
	default: /* REG_ENABLE_BASIC, REG_DISABLE_BASIC */
		return ic->arm_enabled;
	}
}

static void ic_write(void *ctx, uint64_t offset, uint64_t value, unsigned size)
{
	Bcm2835Ic *ic = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset >= REG_END) {
		not_implemented(ic, "write to", offset, size);
		return;
	}
	switch (offset) {
	case REG_FIQ:
		if ((v & FIQ_ENABLE) && (v & FIQ_SOURCE) >= FIQ_SOURCES) {
			machine_stop(ic->machine, MACHINE_STOP_ERROR,
			             "bcm2835-ic: FIQ source %" PRIu32
			             " is one that the chip's document says not to use",
			             v & FIQ_SOURCE);
			return;
		}
		ic->fiq = v & (FIQ_ENABLE | FIQ_SOURCE);
		break;
	case REG_ENABLE1:
		ic->gpu_enabled |= v;
		break;
	case REG_ENABLE2:
		ic->gpu_enabled |= (uint64_t)v << 32;
		break;
	case REG_ENABLE_BASIC:
		ic->arm_enabled |= v & 0xff;
		break;
	case REG_DISABLE1:
		ic->gpu_enabled &= ~(uint64_t)v;
		break;
	case REG_DISABLE2:
		ic->gpu_enabled &= ~((uint64_t)v << 32);
		break;
	case REG_DISABLE_BASIC:
		ic->arm_enabled &= ~v;
		break;
	default:
		break; /* the pending registers are read-only */
	}
	update(ic);
}

static const BusMmioOps ic_mmio_ops = {
	.read = ic_read,
	.write = ic_write,
};

Bcm2835Ic *bcm2835_ic_create(Machine *m)
{
	Bcm2835Ic *ic = calloc(1, sizeof(*ic));

	if (ic == NULL)
		return NULL;
	ic->machine = m;
	for (unsigned n = 0; n < BCM2835_IC_NUM_GPU; n++)
		irq_init(&ic->gpu[n], input_changed, ic, n);
	for (unsigned n = 0; n < BCM2835_IC_NUM_ARM; n++)
		irq_init(&ic->arm[n], input_changed, ic, BCM2835_IC_NUM_GPU + n);
	bcm2835_ic_reset(ic);
	return ic;
}

/* Section 7.5: every register resets to 0. */
void bcm2835_ic_reset(Bcm2835Ic *ic)
{
	ic->gpu_enabled = 0;
	ic->arm_enabled = 0;
	ic->fiq = 0;
	update(ic);
}

void bcm2835_ic_destroy(Bcm2835Ic *ic)
{
	free(ic);
}

BusStatus bcm2835_ic_map(Bcm2835Ic *ic, Bus *bus, const char *name,
                         uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_IC_MMIO_SIZE, &ic_mmio_ops,
	                    ic);
}

IrqLine *bcm2835_ic_gpu_input(Bcm2835Ic *ic, unsigned n)
{
	return &ic->gpu[n];
}

IrqLine *bcm2835_ic_arm_input(Bcm2835Ic *ic, unsigned n)
{
	return &ic->arm[n];
}

void bcm2835_ic_connect(Bcm2835Ic *ic, IrqLine *irq, IrqLine *fiq)
{
	ic->irq_out = irq;
	ic->fiq_out = fiq;
	update(ic);
}
