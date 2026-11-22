/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's end of VCHIQ, the message queues between the Arm and
 * the VideoCore, as the Raspberry Pi firmware plays it.
 *
 * Linux's vchiq_arm (drivers/staging/vc04_services of Raspberry Pi's
 * Linux, at cff533a) allocates the memory that both share: "slot zero",
 * which describes the queues, and the slots that hold the messages. It
 * fills in slot zero and its own half, the slave's, and gives the
 * memory's bus address to the firmware with the property tag VCHIQ_INIT
 * (0x00048010), to which the firmware answers 0. Nothing documents what
 * the firmware does then; the VideoCore is the other end, the master,
 * and smirror's does for its half what vchiq_core.c's vchiq_init_state
 * does for the Arm's: it queues its slots for sending, starts at the
 * first, marks its synchronous slot empty, and is then initialised.
 *
 * The VideoCore then plays its side of vchiq_core.c, the same code on
 * both ends. Its events stay armed, so that the Arm rings its doorbell
 * when it signals one; a VideoCore's latency after the ring, it reads
 * the Arm's messages from its rx position to the Arm's tx_pos, and
 * hands each of the Arm's slots back (slot_queue_recycle, the Arm's
 * recycle event) once it has read to the slot's end, as parse_rx_slots
 * and release_slot do; a message is 8 bytes of header and its data,
 * rounded up to 8 bytes, and the rest of a slot that the next does not
 * fit is a PADDING message. It sends as queue_message does: its
 * messages, headers alone, in its slots in the order of its slot queue,
 * the next slot once the Arm has recycled enough; its tx_pos, then the
 * Arm's trigger event, with the Arm's doorbell when the Arm waits on it
 * (armed).
 *
 * The messages:
 *   CONNECT: the VideoCore answers with its own, once.
 *   OPEN: none of the VideoCore's services is emulated yet, so it
 *     refuses each with a CLOSE to the Arm's port from port 0, as
 *     parse_open does for a service it does not have; the Arm's open
 *     then fails (the service waits in CLOSEWAIT). A warning names each
 *     service, once.
 *   CLOSE: there is no service on the VideoCore's side to close; it is
 *     dropped, as parse_message drops a message to a port without one.
 *   PADDING: skipped.
 *
 * smirror's choices: the VideoCore speaks VCHIQ version 8 and accepts 3
 * and up, those of Linux of the same release; slot zero must start the
 * memory, at a 4 KB boundary, as Linux's always does; the VideoCore
 * answers the Arm's CONNECT rather than connecting first. Not modelled,
 * and the machine stops: a slot zero that the VideoCore does not know,
 * a second VCHIQ_INIT, the doorbell before VCHIQ_INIT, the other
 * messages (DATA, the bulk transfers, PAUSE, RESUME, the REMOTE_USE
 * family, a second CONNECT, an OPEN before CONNECT), synchronous
 * messages, a message past the end of its slot, a slot queue that names
 * a slot outside its side, and the VideoCore running out of slots to
 * send in.
 */

#ifndef MACHINE_RASPI3B_VCHIQ_H
#define MACHINE_RASPI3B_VCHIQ_H

#include <stdbool.h>
#include <stdint.h>

#include "core/machine.h"
#include "hw/misc/bcm2835_doorbell.h"

typedef struct Raspi3bVchiq Raspi3bVchiq;

/*
 * The VideoCore's end, not started; the shared memory must be in the
 * first ram_size bytes of m's bus. NULL when memory runs out.
 */
Raspi3bVchiq *raspi3b_vchiq_create(Machine *m, uint32_t ram_size);
void raspi3b_vchiq_destroy(Raspi3bVchiq *v);

/*
 * What the VideoCore's end acts through: the Arm's doorbell, which it
 * rings (NULL: none), its latency after the Arm rings it, and warn, for
 * what does not stop the board (NULL: none).
 */
void raspi3b_vchiq_attach(Raspi3bVchiq *v, Bcm2835Doorbell *doorbell,
                          uint64_t latency_ns,
                          void (*warn)(void *ctx, const char *message),
                          void *warn_ctx);

/* The firmware starts again: VCHIQ is not started. */
void raspi3b_vchiq_reset(Raspi3bVchiq *v);

/*
 * VCHIQ_INIT: start the VideoCore's end on the memory at the bus address
 * slots. Returns false when it stopped the machine.
 */
bool raspi3b_vchiq_init(Raspi3bVchiq *v, uint32_t slots);

/* The Arm rang the VideoCore's doorbell (Bcm2835DoorbellVc's ring). */
void raspi3b_vchiq_doorbell(void *ctx);

#endif /* MACHINE_RASPI3B_VCHIQ_H */
