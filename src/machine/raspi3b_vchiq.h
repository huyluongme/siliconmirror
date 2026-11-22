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
 *   OPEN: for one of its services (raspi3b_vchiq_add_service) whose
 *     versions meet the Arm's, the VideoCore answers OPENACK, with its
 *     service's version, from a port of its own to the Arm's, as
 *     parse_open does; the service is then open. Any other OPEN it
 *     refuses with a CLOSE to the Arm's port from port 0, as parse_open
 *     does for a service it does not have; the Arm's open then fails
 *     (the service waits in CLOSEWAIT). A warning names each service it
 *     does not have, once, and each version that does not meet.
 *   DATA: to the service open on its port, from the Arm's port that
 *     opened it; to any other port it is dropped, as parse_message drops
 *     a message to a port without a service.
 *   CLOSE: the service open on its port, or, to port 0, the one the
 *     Arm's port opened (a CLOSE before the Arm had the OPENACK), is
 *     closed and answers with a CLOSE of its own, as
 *     vchiq_close_service_internal does for an open service that
 *     receives one; it listens again. Any other is dropped.
 *   PADDING: skipped.
 * A service sends DATA from its port to the Arm's
 * (raspi3b_vchiq_send). A message that a slot's rest does not fit goes
 * in the next slot, the rest a PADDING message. With no slot to send
 * in, the VideoCore's messages wait for the Arm to recycle one, and it
 * reads no more of the Arm's until they are sent, as queue_message
 * waits and parse_rx_slots stops when a reply finds no space.
 *
 * smirror's choices: the VideoCore speaks VCHIQ version 8 and accepts 3
 * and up, those of Linux of the same release; slot zero must start the
 * memory, at a 4 KB boundary, as Linux's always does; the VideoCore
 * answers the Arm's CONNECT rather than connecting first; each OPEN of
 * a service gets the VideoCore's lowest free port from 1, of 32, and a
 * service may be open many times, the firmware's instances being
 * unknown. Not modelled, and the machine stops: a slot zero that the
 * VideoCore does not know, a second VCHIQ_INIT, the doorbell before
 * VCHIQ_INIT, the other messages (OPENACK, the bulk transfers, PAUSE,
 * RESUME, the REMOTE_USE family, a second CONNECT, an OPEN before
 * CONNECT), synchronous messages, a message past the end of its slot, a
 * slot queue that names a slot outside its side, more than 32 services
 * open, and more than 64 KB of the VideoCore's messages waiting for
 * slots.
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

/* The largest message: a slot less its header (VCHIQ_MAX_MSG_SIZE). */
#define RASPI3B_VCHIQ_MSG_MAX 4088u

/*
 * One of the VideoCore's services, which the Arm opens by its fourcc
 * (the four characters, the first in bits 31:24) and versions. Each
 * call has its own ctx and the VideoCore's port that the Arm opened;
 * NULL: none.
 *   open: the Arm opened it (the OPENACK is on its way).
 *   data: the Arm's DATA, size bytes. Returns false when it stopped the
 *     machine.
 *   close: the Arm closed it, or the firmware started again.
 *   reset: the firmware started again (after close).
 */
typedef struct Raspi3bVchiqService {
	uint32_t fourcc;
	uint16_t version, version_min;
	void (*open)(void *ctx, unsigned port);
	bool (*data)(void *ctx, unsigned port, const uint8_t *data, uint32_t size);
	void (*close)(void *ctx, unsigned port);
	void *ctx;
	void (*reset)(void *ctx);
} Raspi3bVchiqService;

/*
 * Add a service, which the VideoCore keeps a copy of; at most 8.
 * Returns false when there is no room.
 */
bool raspi3b_vchiq_add_service(Raspi3bVchiq *v, const Raspi3bVchiqService *s);

/*
 * The service open on port sends the Arm DATA of size bytes, at most
 * RASPI3B_VCHIQ_MSG_MAX. Returns false when it stopped the machine
 * before the message could go or wait; it may go and stop it after.
 */
bool raspi3b_vchiq_send(Raspi3bVchiq *v, unsigned port, const void *data,
                        uint32_t size);

#endif /* MACHINE_RASPI3B_VCHIQ_H */
