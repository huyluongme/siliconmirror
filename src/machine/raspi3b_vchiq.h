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
 * first, marks its synchronous slot empty, and is then initialised. It
 * waits for the Arm's messages, its events armed, so that the Arm rings
 * its doorbell when it sends one.
 *
 * smirror's choices: the VideoCore speaks VCHIQ version 8 and accepts 3
 * and up, those of Linux of the same release; slot zero must start the
 * memory, at a 4 KB boundary, as Linux's always does. A slot zero that
 * the VideoCore does not know, a second VCHIQ_INIT, and the Arm ringing
 * the VideoCore's doorbell, as the VideoCore does not handle messages
 * yet, stop the machine.
 */

#ifndef MACHINE_RASPI3B_VCHIQ_H
#define MACHINE_RASPI3B_VCHIQ_H

#include <stdbool.h>
#include <stdint.h>

#include "core/machine.h"

typedef struct Raspi3bVchiq Raspi3bVchiq;

/*
 * The VideoCore's end, not started; the shared memory must be in the
 * first ram_size bytes of m's bus. NULL when memory runs out.
 */
Raspi3bVchiq *raspi3b_vchiq_create(Machine *m, uint32_t ram_size);
void raspi3b_vchiq_destroy(Raspi3bVchiq *v);

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
