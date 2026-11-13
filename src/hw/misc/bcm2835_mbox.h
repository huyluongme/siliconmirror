/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The mailboxes of the BCM2835, through which the Arm cores and the
 * VideoCore talk. Mailbox 0 carries messages from the VideoCore to the
 * Arm, mailbox 1 from the Arm to the VideoCore; each is a FIFO of eight
 * 32-bit words. A message is a channel number in bits 3:0 and data in
 * bits 31:4.
 *
 * The BCM2835 datasheet does not describe the registers (section 7.5
 * only names the interrupt). Written from the Raspberry Pi firmware wiki
 * ("Mailboxes" and "Accessing mailboxes": the register offsets, the
 * depth, the full and empty flags) and from Linux's
 * drivers/mailbox/bcm2835-mailbox.c (bit 0 of the configuration register
 * enables the interrupt of mailbox 0).
 *
 * What neither says stops the machine: reading an empty mailbox, writing
 * a full one, the sender registers, the Arm reading mailbox 1 or writing
 * mailbox 0, other configuration bits, and accesses that are not 32-bit
 * accesses to a register. The low bits of a status register read as 0.
 */

#ifndef HW_MISC_BCM2835_MBOX_H
#define HW_MISC_BCM2835_MBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bus.h"
#include "core/irq.h"
#include "core/machine.h"

/* The registers of both mailboxes, from 0x7E00B880. */
#define BCM2835_MBOX_MMIO_SIZE 0x40

#define BCM2835_MBOX_DEPTH 8 /* words in each mailbox */

typedef struct Bcm2835Mbox Bcm2835Mbox;

/*
 * The VideoCore's end: the firmware, which takes the messages of mailbox
 * 1 one at a time, each a fixed time after it could start on it, and may
 * answer in mailbox 0. receive() gets a message; it returns true with the
 * answer in *reply, or false for none.
 */
typedef struct Bcm2835MboxVc {
	bool (*receive)(void *ctx, uint32_t message, uint32_t *reply);
	void *ctx;
} Bcm2835MboxVc;

/*
 * Create the mailboxes, empty. They take the time from m and stop m on
 * what they do not emulate. vc answers the Arm; it is copied, and its
 * ctx must outlive the mailboxes. The VideoCore takes latency_ns over
 * each message. irq is the interrupt of mailbox 0, or NULL. Returns NULL
 * when memory runs out.
 */
Bcm2835Mbox *bcm2835_mbox_create(Machine *m, const Bcm2835MboxVc *vc,
                                 uint64_t latency_ns, IrqLine *irq);
void bcm2835_mbox_destroy(Bcm2835Mbox *mbox);

/* Empty both mailboxes and disable the interrupt. */
void bcm2835_mbox_reset(Bcm2835Mbox *mbox);

/* Map the registers on bus at base. */
BusStatus bcm2835_mbox_map(Bcm2835Mbox *mbox, Bus *bus, const char *name,
                           uint64_t base);

#endif /* HW_MISC_BCM2835_MBOX_H */
