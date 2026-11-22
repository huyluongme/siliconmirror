/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the VideoCore's shared memory service
 * (src/machine/raspi3b_smem.c), with the messages of Linux's vc-sm-cma
 * (vc_sm_defs.h, Raspberry Pi's Linux at cff533a), over the Arm's end of
 * vchiq_link.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "machine/raspi3b_smem.h"
#include "machine/raspi3b_vchiq.h"
#include "test.h"
#include "vchiq_link.h"

#define SMEM 0x534d454du /* 'SMEM' */
#define ARM_PORT 7
#define CMD 0x80000000u

enum { FREE = 4, IMPORT = 8, CLIENT_VERSION = 9, RELEASED = 11 };

typedef struct Smem {
	Link l;
	Raspi3bSmem *smem;
} Smem;

/*
 * The link connected, and SMEM opened by vc-sm-cma, version 1 from 0,
 * on the VideoCore's port 1.
 */
static void smem_open(Smem *t)
{
	uint32_t payload[3] = {SMEM, 0, 0u << 16 | 1};
	uint8_t ack[SLOT];
	uint32_t size;

	arm_send(&t->l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t->l), MSG(CONNECT, 0, 0));
	arm_send(&t->l, MSG(OPEN, ARM_PORT, 0), 12, payload);
	CHECK_EQ(vc_message(&t->l, &size, ack), MSG(OPENACK, 1, ARM_PORT));
	CHECK_EQ(size, 2);
	CHECK_EQ(ack[0] | ack[1] << 8, 1);
}

static void smem_up(Smem *t)
{
	link_up(&t->l, true);
	t->smem = raspi3b_smem_create(t->l.s.m, t->l.s.v, RAM_SIZE);
	CHECK(t->smem != NULL);
	smem_open(t);
}

static void smem_down(Smem *t)
{
	raspi3b_smem_destroy(t->smem);
	link_down(&t->l);
}

/* The Arm's message, size bytes of words, to the service. */
static void smem_send(Smem *t, const uint32_t *words, uint32_t size)
{
	arm_send(&t->l, MSG(DATA, ARM_PORT, 1), size, words);
}

/* Nothing more from the VideoCore. */
static void check_quiet(Smem *t)
{
	CHECK_EQ(rd(&t->l.s, SLOTS + MASTER + TX_POS), t->l.vc_pos);
}

/*
 * The machine stopped with message, and the VideoCore said nothing;
 * the message was not read: the next signal stops again.
 */
static void smem_stop(Smem *t, const char *message)
{
	check_stop_by(&t->l.s, "firmware: SMEM: ", message);
	check_quiet(t);
	arm_signal(&t->l);
	run(&t->l, LATENCY);
	check_stop_by(&t->l.s, "firmware: SMEM: ", message);
}

/* The VideoCore's next message, which must be DATA of n words. */
static void check_reply(Smem *t, const uint32_t *words, uint32_t n)
{
	uint32_t got[SLOT / 4], size;

	uint32_t msgid;

	/* The rest of a slot that it does not fit is padding. */
	do
		msgid = vc_message(&t->l, &size, (uint8_t *)got);
	while (msgid == MSG(PADDING, 0, 0));
	CHECK_EQ(msgid, MSG(DATA, 1, ARM_PORT));
	CHECK_EQ(size, 4 * n);
	for (uint32_t i = 0; i < n && i < size / 4; i++)
		CHECK_EQ(got[i], words[i]);
}

/* An IMPORT, with transaction id trans, of size bytes at addr. */
static void import(Smem *t, uint32_t trans, uint32_t addr, uint32_t size,
                   uint32_t kernel_id)
{
	uint32_t m[15] = {IMPORT, trans, 1, addr, size, kernel_id, 4321};

	memcpy(&m[7], "vcsm-cma\0\0\0\0", 12);
	smem_send(t, m, sizeof(m));
}

static void free_handle(Smem *t, uint32_t handle)
{
	uint32_t m[4] = {FREE, 30, handle, 0xc0001000u};

	smem_send(t, m, sizeof(m));
}

/* vc_sm_cma_vchi_client_version, then IMPORT and FREE. */
static void test_messages(void)
{
	uint32_t version[3] = {CLIENT_VERSION, 1, 2};
	Smem t;

	smem_up(&t);
	smem_send(&t, version, sizeof(version));
	check_reply(&t, (uint32_t[]){CMD | CLIENT_VERSION, 0}, 2);
	/* Handles from 1; the alias bits are not the address. */
	import(&t, 2, 0xc0010000u, 0x1000, 77);
	check_reply(&t, (uint32_t[]){2, 1}, 2);
	import(&t, 3, 0x40020000u, 0x2000, 78);
	check_reply(&t, (uint32_t[]){3, 2}, 2);
	import(&t, 4, 0xc0000000u | (RAM_SIZE - 0x100), 0x100, 79);
	check_reply(&t, (uint32_t[]){4, 3}, 2);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	/* Freed: released at once, with what was imported. */
	free_handle(&t, 2);
	check_reply(&t, (uint32_t[]){CMD | RELEASED, 0x40020000u, 0x2000, 78, 2},
	            5);
	free_handle(&t, 1);
	check_reply(&t, (uint32_t[]){CMD | RELEASED, 0xc0010000u, 0x1000, 77, 1},
	            5);
	/* The lowest free handle again. */
	import(&t, 5, 0x10000, 0x10, 80);
	check_reply(&t, (uint32_t[]){5, 1}, 2);
	check_quiet(&t);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);

	/* A client of version 1 is not told. */
	version[2] = 1;
	smem_send(&t, version, sizeof(version));
	check_reply(&t, (uint32_t[]){CMD | CLIENT_VERSION, 0}, 2);
	free_handle(&t, 3);
	check_quiet(&t);
	/*
	 * The firmware starts again: no handle (1 was given), and no version
	 * (2 was given), so no RELEASED.
	 */
	version[2] = 2;
	smem_send(&t, version, sizeof(version));
	check_reply(&t, (uint32_t[]){CMD | CLIENT_VERSION, 0}, 2);
	raspi3b_vchiq_reset(t.l.s.v);
	link_start(&t.l);
	smem_open(&t);
	import(&t, 6, 0x10000, 0x10, 81);
	check_reply(&t, (uint32_t[]){6, 1}, 2);
	free_handle(&t, 1);
	check_quiet(&t);
	/* Version 3 and up are told. */
	version[2] = 3;
	smem_send(&t, version, sizeof(version));
	check_reply(&t, (uint32_t[]){CMD | CLIENT_VERSION, 0}, 2);
	import(&t, 7, 0x10000, 0x10, 82);
	check_reply(&t, (uint32_t[]){7, 1}, 2);
	free_handle(&t, 1);
	check_reply(&t, (uint32_t[]){CMD | RELEASED, 0x10000, 0x10, 82, 1}, 5);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	smem_down(&t);
}

/* What the service does not model stops the machine. */
static void test_stops(void)
{
	static const uint32_t types[] = {0, 1,  2,  3,  5,  6,
	                                 7, 10, 11, 12, 13, 0xffffffffu};
	static const struct {
		uint32_t type, size; /* the shortest that is long enough */
	} sizes[] = {{CLIENT_VERSION, 12}, {IMPORT, 60}, {FREE, 16}};
	uint32_t m[SLOT / 4] = {0};
	Smem t;

	smem_up(&t);
	for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
		m[0] = types[i];
		smem_send(&t, m, 8);
		smem_stop(&t, "the Arm's message of type");
		smem_down(&t);
		smem_up(&t);
	}
	smem_send(&t, m, 7);
	smem_stop(&t, "a message of 7 bytes has no header");
	smem_down(&t);
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		smem_up(&t);
		memset(m, 0, sizeof(m));
		m[0] = sizes[i].type;
		smem_send(&t, m, sizes[i].size - 1);
		smem_stop(&t, "is too short");
		smem_down(&t);
		smem_up(&t);
		m[3] = 0x10000; /* an IMPORT's address */
		m[4] = 0x10;
		if (sizes[i].type == FREE) {
			import(&t, 1, 0x10000, 0x10, 1);
			check_reply(&t, (uint32_t[]){1, 1}, 2);
			m[2] = 1;
		}
		smem_send(&t, m, sizes[i].size);
		CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
		smem_down(&t);
	}
	/*
	 * Each on a new link: a message that stops is read again at the
	 * next signal.
	 */
	smem_up(&t);
	{
		char text[100];

		snprintf(text, sizeof(text),
		         "an IMPORT of 257 bytes at 0x%08x, "
		         "outside the RAM, is not emulated",
		         0xc0000000u | (RAM_SIZE - 0x100));
		import(&t, 1, 0xc0000000u | (RAM_SIZE - 0x100), 0x101, 1);
		smem_stop(&t, text);
		check_quiet(&t);
	}
	smem_down(&t);
	smem_up(&t);
	import(&t, 1, 0xffffff00u, 0x100, 1);
	smem_stop(&t, "outside the RAM");
	smem_down(&t);
	smem_up(&t);
	/* An odd address: all its bits count. */
	import(&t, 1, 0xc0000000u | (RAM_SIZE - 0xff), 0x100, 1);
	smem_stop(&t, "outside the RAM");
	smem_down(&t);
	smem_up(&t);
	import(&t, 1, 0xc0000000u | (RAM_SIZE - 0xff), 0xff, 1);
	check_reply(&t, (uint32_t[]){1, 1}, 2);
	smem_down(&t);
	smem_up(&t);
	import(&t, 1, 0xffffffffu, 0xffffffffu, 1);
	smem_stop(&t, "outside the RAM");
	smem_down(&t);
	smem_up(&t);
	free_handle(&t, 0);
	smem_stop(&t, "a FREE of handle 0, which is not one, is not "
	              "emulated");
	smem_down(&t);
	smem_up(&t);
	free_handle(&t, 1);
	smem_stop(&t, "a FREE of handle 1, which is not one");
	smem_down(&t);
	smem_up(&t);
	free_handle(&t, 257);
	smem_stop(&t, "a FREE of handle 257, which is not one");
	smem_down(&t);
	smem_up(&t);
	/* 256 handles, then none. */
	for (uint32_t i = 1; i <= 256; i++) {
		import(&t, i, 0x10000, 0x10, i);
		check_reply(&t, (uint32_t[]){i, i}, 2);
	}
	free_handle(&t, 256);
	check_quiet(&t); /* no CLIENT_VERSION */
	import(&t, 300, 0x10000, 0x10, 300);
	check_reply(&t, (uint32_t[]){300, 256}, 2);
	import(&t, 301, 0x10000, 0x10, 301);
	smem_stop(&t, "more than 256 handles is not emulated");
	smem_down(&t);
}

/* An Arm of version 0 opens it too: the VideoCore's oldest is 0. */
static void test_versions(void)
{
	uint32_t payload[3] = {SMEM, 0, 0};
	Smem t;

	link_up(&t.l, true);
	t.smem = raspi3b_smem_create(t.l.s.m, t.l.s.v, RAM_SIZE);
	arm_send(&t.l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CONNECT, 0, 0));
	arm_send(&t.l, MSG(OPEN, ARM_PORT, 0), 12, payload);
	CHECK_EQ(vc_message(&t.l, &(uint32_t){0}, NULL), MSG(OPENACK, 1, ARM_PORT));
	CHECK_EQ(t.l.warnings, 0);
	smem_down(&t);
}

/* With no room among the VideoCore's services, there is no SMEM. */
static void test_create(void)
{
	Link l;

	link_up(&l, false);
	for (uint32_t i = 0; i < 8; i++)
		CHECK(raspi3b_vchiq_add_service(
			l.s.v,
			&(Raspi3bVchiqService){i, 1, 1, NULL, NULL, NULL, NULL, NULL}));
	CHECK(raspi3b_smem_create(l.s.m, l.s.v, RAM_SIZE) == NULL);
	link_down(&l);
}

int main(void)
{
	RUN_TEST(test_messages);
	RUN_TEST(test_stops);
	RUN_TEST(test_versions);
	RUN_TEST(test_create);
	return test_summary();
}
