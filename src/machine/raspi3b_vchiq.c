/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The VideoCore's end of VCHIQ; see raspi3b_vchiq.h. */

#include "machine/raspi3b_vchiq.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/bus.h"
#include "util/byteorder.h"
#include "util/macros.h"

/* The VideoCore's bus address of the RAM: the alias in bits 31:30 goes. */
#define BUS_ALIAS_MASK 0x3fffffffu

/* vchiq_cfg.h and vchiq_core.h of Raspberry Pi's Linux. */
#define MAGIC 0x56434849u /* 'VCHI' */
#define VERSION 8         /* the VideoCore's, smirror's choice */
#define VERSION_MIN 3
#define SLOT_SIZE 4096u
#define MAX_SLOTS 128
#define MAX_SLOTS_PER_SIDE 64
#define DEBUG_MAX 11 /* the entries of a side's debug array */

/* struct vchiq_slot_zero: its fields' offsets, and its size. */
enum {
	Z_MAGIC = 0,
	Z_VERSION = 4, /* 16 bits, then version_min */
	Z_VERSION_MIN = 6,
	Z_SLOT_ZERO_SIZE = 8,
	Z_SLOT_SIZE = 12,
	Z_MAX_SLOTS = 16,
	Z_MAX_SLOTS_PER_SIDE = 20,
	Z_MASTER = 32, /* the VideoCore's half, then the Arm's */
	SLOT_ZERO_SIZE = 1288,
};

/* struct vchiq_shared_state, a half of slot zero. */
enum {
	S_INITIALISED = 0,
	S_SLOT_FIRST = 4,
	S_SLOT_LAST = 8,
	S_SLOT_SYNC = 12,
	S_TRIGGER = 16, /* struct remote_event: armed, fired, unused */
	S_TX_POS = 28,
	S_RECYCLE = 32,
	S_SLOT_QUEUE_RECYCLE = 44,
	S_SYNC_TRIGGER = 48,
	S_SYNC_RELEASE = 60,
	S_SLOT_QUEUE = 72,
	S_DEBUG = 328,
};

#define EVENT_ARMED 0
#define EVENT_FIRED 4
#define MSGID_PADDING 0 /* a message header's msgid: an empty slot */

struct Raspi3bVchiq {
	Machine *machine;
	uint32_t ram_size;
	bool started;
};

static void stop(Raspi3bVchiq *v, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void stop(Raspi3bVchiq *v, const char *fmt, ...)
{
	char why[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	machine_stop(v->machine, MACHINE_STOP_ERROR, "firmware: VCHIQ: %s", why);
}

Raspi3bVchiq *raspi3b_vchiq_create(Machine *m, uint32_t ram_size)
{
	Raspi3bVchiq *v = calloc(1, sizeof(*v));

	if (v == NULL)
		return NULL;
	v->machine = m;
	v->ram_size = ram_size;
	return v;
}

void raspi3b_vchiq_destroy(Raspi3bVchiq *v)
{
	free(v);
}

void raspi3b_vchiq_reset(Raspi3bVchiq *v)
{
	v->started = false;
}

static void put(Raspi3bVchiq *v, uint64_t address, uint32_t value)
{
	bus_write(v->machine->bus, address, 4, value);
}

/* The bytes of slot zero that the VideoCore checks: up to its slots. */
#define HEAD_SIZE (Z_MASTER + S_TRIGGER)

/*
 * Check what the Arm wrote in slot zero, at zero, of which head holds
 * the first bytes. Returns false when it stopped the machine.
 */
static bool check_slot_zero(Raspi3bVchiq *v, uint32_t zero, const uint8_t *head)
{
	uint32_t first = load_le32(head + Z_MASTER + S_SLOT_FIRST);
	uint32_t last = load_le32(head + Z_MASTER + S_SLOT_LAST);
	uint32_t sync = load_le32(head + Z_MASTER + S_SLOT_SYNC);
	uint16_t version = load_le16(head + Z_VERSION);
	uint16_t version_min = load_le16(head + Z_VERSION_MIN);

	if (load_le32(head + Z_MAGIC) != MAGIC) {
		stop(v, "slot zero at 0x%08" PRIx32 " has no magic 'VCHI'", zero);
		return false;
	}
	if (version < VERSION_MIN || version_min > VERSION) {
		stop(v,
		     "the Arm's version %u (from %u) is not the VideoCore's, 8 (from "
		     "3)",
		     version, version_min);
		return false;
	}
	if (load_le32(head + Z_SLOT_ZERO_SIZE) != SLOT_ZERO_SIZE ||
	    load_le32(head + Z_SLOT_SIZE) != SLOT_SIZE ||
	    load_le32(head + Z_MAX_SLOTS) != MAX_SLOTS ||
	    load_le32(head + Z_MAX_SLOTS_PER_SIDE) != MAX_SLOTS_PER_SIDE) {
		stop(v, "slot zero's sizes are not the VideoCore's");
		return false;
	}
	if (sync == 0 || first != sync + 1 || last < first ||
	    last - first >= MAX_SLOTS_PER_SIDE ||
	    (uint64_t)zero + (uint64_t)(last + 1) * SLOT_SIZE > v->ram_size) {
		stop(v,
		     "the VideoCore's slots %" PRIu32 " (synchronous), %" PRIu32
		     " to %" PRIu32 " are not emulated",
		     sync, first, last);
		return false;
	}
	if (load_le32(head + Z_MASTER + S_INITIALISED) != 0) {
		stop(v, "the VideoCore's half of slot zero is initialised already");
		return false;
	}
	return true;
}

bool raspi3b_vchiq_init(Raspi3bVchiq *v, uint32_t slots)
{
	uint8_t head[HEAD_SIZE];
	uint32_t zero = slots & BUS_ALIAS_MASK;
	uint64_t master = (uint64_t)zero + Z_MASTER;
	uint32_t first, last, sync;

	if (v->started) {
		stop(v, "a second VCHIQ_INIT is not emulated");
		return false;
	}
	if (zero % SLOT_SIZE != 0 ||
	    bus_read_block(v->machine->bus, zero, head, sizeof(head)) != BUS_OK) {
		stop(v, "slots at 0x%08" PRIx32 " are not emulated", slots);
		return false;
	}
	if (!check_slot_zero(v, zero, head))
		return false;
	first = load_le32(head + Z_MASTER + S_SLOT_FIRST);
	last = load_le32(head + Z_MASTER + S_SLOT_LAST);
	sync = load_le32(head + Z_MASTER + S_SLOT_SYNC);

	/* Its slots, to send in; it starts with the first. */
	for (uint32_t i = 0; i <= last - first; i++)
		put(v, master + S_SLOT_QUEUE + 4 * i, first + i);
	put(v, master + S_TX_POS, 0);
	put(v, master + S_SLOT_QUEUE_RECYCLE, last - first + 1);
	/* It waits for the Arm's messages, recycled slots and sync messages. */
	put(v, master + S_TRIGGER + EVENT_ARMED, 1);
	put(v, master + S_RECYCLE + EVENT_ARMED, 1);
	put(v, master + S_SYNC_TRIGGER + EVENT_ARMED, 1);
	/* Its synchronous slot is empty, and free for the Arm to use. */
	put(v, (uint64_t)zero + (uint64_t)sync * SLOT_SIZE, MSGID_PADDING);
	put(v, master + S_SYNC_RELEASE + EVENT_ARMED, 0);
	put(v, master + S_SYNC_RELEASE + EVENT_FIRED, 1);
	put(v, master + S_DEBUG, DEBUG_MAX);
	put(v, master + S_INITIALISED, 1);
	v->started = true;
	return true;
}

void raspi3b_vchiq_doorbell(void *ctx)
{
	stop(ctx, "the Arm rang the VideoCore's doorbell: the VideoCore's "
	          "messages are not emulated yet");
}
