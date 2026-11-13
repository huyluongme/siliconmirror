/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835 mailboxes; see bcm2835_mbox.h. */

#include "hw/misc/bcm2835_mbox.h"

#include <inttypes.h>
#include <stdlib.h>

#include "core/event.h"

/* Registers; mailbox 1's are at the same offsets plus 0x20. */
enum {
	REG_DATA = 0x00, /* read pops mailbox 0; write pushes mailbox 1 */
	REG_PEEK = 0x10,
	REG_SENDER = 0x14,
	REG_STATUS = 0x18,
	REG_CONFIG = 0x1c,
	MAILBOX_1 = 0x20,
};

#define STATUS_FULL (1u << 31)
#define STATUS_EMPTY (1u << 30)
#define CONFIG_DATA_IRQ (1u << 0) /* interrupt while mailbox 0 has data */

typedef struct Fifo {
	uint32_t words[BCM2835_MBOX_DEPTH];
	unsigned head, count;
} Fifo;

struct Bcm2835Mbox {
	Machine *machine;
	Bcm2835MboxVc vc;
	uint64_t latency_ns;
	IrqLine *irq;
	Fifo to_arm;     /* mailbox 0 */
	Fifo to_vc;      /* mailbox 1 */
	uint32_t config; /* of mailbox 0 */
	Event vc_done;   /* the VideoCore is through with a message */
};

static uint32_t fifo_head(const Fifo *f)
{
	return f->words[f->head];
}

static uint32_t fifo_pop(Fifo *f)
{
	uint32_t word = f->words[f->head];

	f->head = (f->head + 1) % BCM2835_MBOX_DEPTH;
	f->count--;
	return word;
}

static void fifo_push(Fifo *f, uint32_t word)
{
	f->words[(f->head + f->count) % BCM2835_MBOX_DEPTH] = word;
	f->count++;
}

static void update_irq(Bcm2835Mbox *mbox)
{
	irq_set(mbox->irq,
	        (mbox->config & CONFIG_DATA_IRQ) && mbox->to_arm.count != 0);
}

/*
 * Start the VideoCore on the next message, unless it is busy, has none,
 * or has no room for an answer: it waits for the Arm to take one.
 */
static void vc_start(Bcm2835Mbox *mbox)
{
	if (event_pending(&mbox->vc_done) || mbox->to_vc.count == 0 ||
	    mbox->to_arm.count == BCM2835_MBOX_DEPTH)
		return;
	event_schedule(&mbox->machine->events, &mbox->vc_done,
	               machine_now(mbox->machine) + mbox->latency_ns);
}

static void vc_finished(void *ctx)
{
	Bcm2835Mbox *mbox = ctx;
	uint32_t reply;

	if (mbox->vc.receive(mbox->vc.ctx, fifo_pop(&mbox->to_vc), &reply)) {
		fifo_push(&mbox->to_arm, reply);
		update_irq(mbox);
	}
	vc_start(mbox);
}

static uint32_t status(const Fifo *f)
{
	return (f->count == BCM2835_MBOX_DEPTH ? STATUS_FULL : 0) |
	       (f->count == 0 ? STATUS_EMPTY : 0);
}

static void stop(Bcm2835Mbox *mbox, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(mbox->machine, MACHINE_STOP_ERROR,
	             "bcm2835-mbox: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

static uint64_t mbox_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Mbox *mbox = ctx;
	uint32_t word;

	if (size != 4 || offset % 4 != 0) {
		stop(mbox, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case REG_DATA:
	case REG_PEEK:
		if (mbox->to_arm.count == 0) {
			stop(mbox, "read of", offset, size,
			     "is not emulated: mailbox 0 is empty");
			return 0;
		}
		if (offset == REG_PEEK)
			return fifo_head(&mbox->to_arm);
		word = fifo_pop(&mbox->to_arm);
		update_irq(mbox);
		vc_start(mbox);
		return word;
	case REG_STATUS:
		return status(&mbox->to_arm);
	case REG_CONFIG:
		return mbox->config;
	case MAILBOX_1 + REG_STATUS:
		return status(&mbox->to_vc);
	case REG_SENDER:
	case MAILBOX_1 + REG_SENDER:
		stop(mbox, "read of", offset, size,
		     "is not emulated (the sender register)");
		return 0;
	case MAILBOX_1 + REG_DATA:
	case MAILBOX_1 + REG_PEEK:
		stop(mbox, "read of", offset, size,
		     "is not emulated: the Arm never reads mailbox 1");
		return 0;
	default:
		stop(mbox, "read of", offset, size, "is not a documented register");
		return 0;
	}
}

static void mbox_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Bcm2835Mbox *mbox = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset % 4 != 0) {
		stop(mbox, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	switch (offset) {
	case MAILBOX_1 + REG_DATA:
		if (mbox->to_vc.count == BCM2835_MBOX_DEPTH) {
			stop(mbox, "write to", offset, size,
			     "is not emulated: mailbox 1 is full");
			return;
		}
		fifo_push(&mbox->to_vc, v);
		vc_start(mbox);
		return;
	case REG_CONFIG:
		if (v & ~CONFIG_DATA_IRQ) {
			stop(mbox, "write to", offset, size,
			     "sets bits that are not emulated");
			return;
		}
		mbox->config = v;
		update_irq(mbox);
		return;
	case REG_DATA:
	case REG_PEEK:
	case REG_SENDER:
	case REG_STATUS:
		stop(mbox, "write to", offset, size,
		     "is not emulated: the Arm never writes mailbox 0");
		return;
	case MAILBOX_1 + REG_PEEK:
	case MAILBOX_1 + REG_SENDER:
	case MAILBOX_1 + REG_STATUS:
	case MAILBOX_1 + REG_CONFIG:
		stop(mbox, "write to", offset, size,
		     "is not emulated (a register of mailbox 1)");
		return;
	default:
		stop(mbox, "write to", offset, size, "is not a documented register");
		return;
	}
}

static const BusMmioOps mbox_mmio_ops = {
	.read = mbox_read,
	.write = mbox_write,
};

void bcm2835_mbox_reset(Bcm2835Mbox *mbox)
{
	event_cancel(&mbox->machine->events, &mbox->vc_done);
	mbox->to_arm = (Fifo){0};
	mbox->to_vc = (Fifo){0};
	mbox->config = 0;
	update_irq(mbox);
}

Bcm2835Mbox *bcm2835_mbox_create(Machine *m, const Bcm2835MboxVc *vc,
                                 uint64_t latency_ns, IrqLine *irq)
{
	Bcm2835Mbox *mbox = calloc(1, sizeof(*mbox));

	if (mbox == NULL)
		return NULL;
	mbox->machine = m;
	mbox->vc = *vc;
	mbox->latency_ns = latency_ns;
	mbox->irq = irq;
	event_init(&mbox->vc_done, vc_finished, mbox);
	bcm2835_mbox_reset(mbox);
	return mbox;
}

void bcm2835_mbox_destroy(Bcm2835Mbox *mbox)
{
	event_cancel(&mbox->machine->events, &mbox->vc_done);
	free(mbox);
}

BusStatus bcm2835_mbox_map(Bcm2835Mbox *mbox, Bus *bus, const char *name,
                           uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_MBOX_MMIO_SIZE, &mbox_mmio_ops,
	                    mbox);
}
