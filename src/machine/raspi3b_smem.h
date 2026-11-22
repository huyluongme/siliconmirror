/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's shared memory service, 'SMEM', over VCHIQ, as the
 * firmware plays it for Linux's vc-sm-cma (drivers/staging/
 * vc04_services/vc-sm-cma of Raspberry Pi's Linux, at cff533a; its
 * vc_sm_defs.h gives the messages). The service is version 1, from 0,
 * those of vc-sm-cma.
 *
 * Each of the Arm's messages starts with its type and a transaction
 * id; a reply starts with the transaction id, and a message the
 * VideoCore starts itself with its type, bit 31 set, in that place.
 *   CLIENT_VERSION (9): the Arm's protocol version. The VideoCore
 *     acknowledges it with a message of its own, CLIENT_VERSION with
 *     bit 31 (vc_sm_vpu_event takes that as a firmware that knows the
 *     command, and then waits for RELEASED).
 *   IMPORT (8): the Arm's memory, a bus address and size, which the
 *     VideoCore wraps in a handle; it answers with the handle.
 *   FREE (4): the Arm is done with a handle. Nothing on the VideoCore
 *     holds the memory, so it releases it at once: to a client of
 *     version 2 and up, it says so with RELEASED (11), the memory's
 *     address, size, the Arm's kernel_id and the handle.
 *
 * smirror's choices, where nothing documents the firmware: the
 * acknowledgement of CLIENT_VERSION is 8 bytes, its type and a 0
 * (vc_sm_result_t's shape; Linux reads only the first word); handles
 * count from 1, the lowest free, 256 of them. Not modelled, and the
 * machine stops: the other messages (ALLOC, LOCK, UNLOCK, RESIZE,
 * WALK_ALLOC, ACTION_CLEAN, VC_MEM_REQUEST_REPLY, which Linux's
 * vc-sm-cma does not send), a message too short for its type, memory
 * outside the RAM, a FREE of a handle not given, and more than 256
 * handles.
 */

#ifndef MACHINE_RASPI3B_SMEM_H
#define MACHINE_RASPI3B_SMEM_H

#include <stdint.h>

#include "core/machine.h"
#include "machine/raspi3b_vchiq.h"

typedef struct Raspi3bSmem Raspi3bSmem;

/*
 * The service, added to v's; the Arm's memory must be in the first
 * ram_size bytes. NULL when memory or v's room runs out. When the
 * firmware starts again (raspi3b_vchiq_reset), it has no handle and no
 * client version.
 */
Raspi3bSmem *raspi3b_smem_create(Machine *m, Raspi3bVchiq *v,
                                 uint32_t ram_size);
void raspi3b_smem_destroy(Raspi3bSmem *s);

#endif /* MACHINE_RASPI3B_SMEM_H */
