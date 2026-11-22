/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The VideoCore's shared memory service; see raspi3b_smem.h. */

#include "machine/raspi3b_smem.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"
#include "util/macros.h"

#define FOURCC 0x534d454du /* 'SMEM' */
#define VERSION 1          /* VC_SM_VER */
#define VERSION_MIN 0      /* VC_SM_MIN_VER */

/* enum vc_sm_msg_type of vc_sm_defs.h. */
enum {
	TYPE_FREE = 4,
	TYPE_IMPORT = 8,
	TYPE_CLIENT_VERSION = 9,
	TYPE_RELEASED = 11,
};

#define VPU_COMMAND 0x80000000u /* a message the VideoCore starts */
#define RELEASED_FROM 2         /* the client version that wants RELEASED */

/* The header, type and trans_id, then the bodies' sizes. */
#define HEADER 8u
#define VERSION_SIZE 4u /* struct vc_sm_version */
#define IMPORT_SIZE 52u /* struct vc_sm_import: 5 words and a name of 32 */
#define FREE_SIZE 8u    /* struct vc_sm_free_t */

/* The VideoCore's bus address of the RAM: the alias in bits 31:30 goes. */
#define BUS_ALIAS_MASK 0x3fffffffu

#define HANDLES 256

typedef struct Import {
	bool used;
	uint32_t addr, size, kernel_id;
} Import;

struct Raspi3bSmem {
	Machine *machine;
	Raspi3bVchiq *vchiq;
	uint32_t ram_size;
	uint32_t client_version;
	Import imports[HANDLES]; /* handle i + 1 */
};

static void stop(Raspi3bSmem *s, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void stop(Raspi3bSmem *s, const char *fmt, ...)
{
	char why[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	machine_stop(s->machine, MACHINE_STOP_ERROR, "firmware: SMEM: %s", why);
}

/* Whether the message's body, size bytes, is at least need. */
static bool long_enough(Raspi3bSmem *s, uint32_t type, uint32_t size,
                        uint32_t need)
{
	if (size >= HEADER + need)
		return true;
	stop(s, "a message of type %" PRIu32 " of %" PRIu32 " bytes is too short",
	     type, size);
	return false;
}

static bool client_version(Raspi3bSmem *s, unsigned port, const uint8_t *body)
{
	uint8_t ack[8];

	s->client_version = load_le32(body);
	store_le32(ack, VPU_COMMAND | TYPE_CLIENT_VERSION);
	store_le32(ack + 4, 0);
	return raspi3b_vchiq_send(s->vchiq, port, ack, sizeof(ack));
}

static bool import(Raspi3bSmem *s, unsigned port, uint32_t trans_id,
                   const uint8_t *body)
{
	uint32_t addr = load_le32(body + 4);
	uint32_t size = load_le32(body + 8);
	uint8_t reply[8];

	if ((uint64_t)(addr & BUS_ALIAS_MASK) + size > s->ram_size) {
		stop(s,
		     "an IMPORT of %" PRIu32 " bytes at 0x%08" PRIx32
		     ", outside the RAM, is not emulated",
		     size, addr);
		return false;
	}
	for (unsigned i = 0; i < HANDLES; i++) {
		Import *im = &s->imports[i];

		if (im->used)
			continue;
		im->used = true;
		im->addr = addr;
		im->size = size;
		im->kernel_id = load_le32(body + 12);
		store_le32(reply, trans_id);
		store_le32(reply + 4, i + 1);
		return raspi3b_vchiq_send(s->vchiq, port, reply, sizeof(reply));
	}
	stop(s, "more than 256 handles is not emulated");
	return false;
}

static bool free_handle(Raspi3bSmem *s, unsigned port, const uint8_t *body)
{
	uint32_t handle = load_le32(body);
	uint8_t released[20];
	Import *im;

	if (handle == 0 || handle > HANDLES || !s->imports[handle - 1].used) {
		stop(s,
		     "a FREE of handle %" PRIu32 ", which is not one, is not "
		     "emulated",
		     handle);
		return false;
	}
	im = &s->imports[handle - 1];
	im->used = false;
	if (s->client_version < RELEASED_FROM)
		return true;
	/* struct vc_sm_released */
	store_le32(released, VPU_COMMAND | TYPE_RELEASED);
	store_le32(released + 4, im->addr);
	store_le32(released + 8, im->size);
	store_le32(released + 12, im->kernel_id);
	store_le32(released + 16, handle);
	return raspi3b_vchiq_send(s->vchiq, port, released, sizeof(released));
}

static bool data(void *ctx, unsigned port, const uint8_t *msg, uint32_t size)
{
	Raspi3bSmem *s = ctx;
	uint32_t type, trans_id;

	if (size < HEADER) {
		stop(s, "a message of %" PRIu32 " bytes has no header", size);
		return false;
	}
	type = load_le32(msg);
	trans_id = load_le32(msg + 4);
	switch (type) {
	case TYPE_CLIENT_VERSION:
		return long_enough(s, type, size, VERSION_SIZE) &&
		       client_version(s, port, msg + HEADER);
	case TYPE_IMPORT:
		return long_enough(s, type, size, IMPORT_SIZE) &&
		       import(s, port, trans_id, msg + HEADER);
	case TYPE_FREE:
		return long_enough(s, type, size, FREE_SIZE) &&
		       free_handle(s, port, msg + HEADER);
	default:
		stop(s, "the Arm's message of type %" PRIu32 " is not emulated", type);
		return false;
	}
}

/* The firmware starts again. */
static void reset(void *ctx)
{
	Raspi3bSmem *s = ctx;

	s->client_version = 0;
	memset(s->imports, 0, sizeof(s->imports));
}

Raspi3bSmem *raspi3b_smem_create(Machine *m, Raspi3bVchiq *v, uint32_t ram_size)
{
	Raspi3bSmem *s = calloc(1, sizeof(*s));
	Raspi3bVchiqService service = {
		.fourcc = FOURCC,
		.version = VERSION,
		.version_min = VERSION_MIN,
		.data = data,
		.reset = reset,
	};

	if (s == NULL)
		return NULL;
	s->machine = m;
	s->vchiq = v;
	s->ram_size = ram_size;
	service.ctx = s;
	if (!raspi3b_vchiq_add_service(v, &service)) {
		free(s);
		return NULL;
	}
	return s;
}

void raspi3b_smem_destroy(Raspi3bSmem *s)
{
	free(s);
}
