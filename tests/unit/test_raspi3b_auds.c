/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the VideoCore's audio service
 * (src/machine/raspi3b_auds.c), with the messages of Linux's
 * snd-bcm2835 (vc_vchi_audioserv_defs.h, bcm2835-vchiq.c, Raspberry
 * Pi's Linux at cff533a), over the Arm's end of vchiq_link.h, and an
 * audio output that keeps what it is given.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine/raspi3b_auds.h"
#include "machine/raspi3b_property.h"
#include "machine/raspi3b_vchiq.h"
#include "test.h"
#include "vchiq_link.h"

#define ARM_PORT 7
#define ARM_PORT2 9

enum {
	RESULT,
	COMPLETE,
	CONFIG,
	CONTROL,
	AOPEN,
	ACLOSE,
	START,
	STOP,
	WRITE,
};

#define COOKIE1 0x42434d41u /* 'BCMA' */
#define COOKIE2 0x44415441u /* 'DATA' */
#define FINISHED (1u << 30)
#define BLOCK 10000000u /* ns */
#define FIFO 0x80000u
#define MAX_PACKET 4000u

/* An output that keeps what it is given, and fails when told to. */
typedef struct Fake {
	AudioOut out;
	unsigned starts, stops;
	bool playing;
	AudioFormat format;
	uint8_t *data;
	size_t len;
	bool fail_start, fail_play, fail_stop;
} Fake;

#define FAKE_SIZE 0x100000u

static bool fake_start(AudioOut *a, const AudioFormat *f)
{
	Fake *k = (Fake *)a;

	CHECK(!k->playing);
	k->starts++;
	k->format = *f;
	k->playing = true;
	strcpy(a->error, "no start");
	return !k->fail_start;
}

/*
 * A call out of the contract ends the test at once: a service that
 * loops on it would print without end.
 */
static bool fake_play(AudioOut *a, const uint8_t *buf, size_t len)
{
	Fake *k = (Fake *)a;

	if (!k->playing || len == 0 || len % audio_frame_size(&k->format) != 0 ||
	    len > 4096) {
		fprintf(stderr, "fake_play: %zu bytes, playing %d\n", len, k->playing);
		abort();
	}
	if (k->len + len <= FAKE_SIZE)
		memcpy(k->data + k->len, buf, len);
	k->len += len;
	strcpy(a->error, "no play");
	return !k->fail_play;
}

static bool fake_stop(AudioOut *a)
{
	Fake *k = (Fake *)a;

	CHECK(k->playing);
	k->stops++;
	k->playing = false;
	strcpy(a->error, "no stop");
	return !k->fail_stop;
}

static void fake_destroy(AudioOut *a)
{
	(void)a;
}

static const AudioOutOps fake_ops = {fake_start, fake_play, fake_stop,
                                     fake_destroy};

typedef struct Auds {
	Link l;
	Raspi3bAuds *auds;
	Fake fake;
} Auds;

/* The service opened by the Arm on port, versions 2 from 1. */
static void auds_open(Auds *t, unsigned port, unsigned vc_port)
{
	uint32_t payload[3] = {AUDS, 0, 1u << 16 | 2};
	uint8_t ack[SLOT];
	uint32_t size;

	arm_send(&t->l, MSG(OPEN, port, 0), 12, payload);
	CHECK_EQ(vc_message(&t->l, &size, ack), MSG(OPENACK, vc_port, port));
	CHECK_EQ(size, 2);
	CHECK_EQ(ack[0] | ack[1] << 8, 2);
}

static void auds_up(Auds *t, bool output)
{
	link_up(&t->l, true);
	t->auds = raspi3b_auds_create(t->l.s.m, t->l.s.v);
	CHECK(t->auds != NULL);
	memset(&t->fake, 0, sizeof(t->fake));
	t->fake.out.ops = &fake_ops;
	t->fake.data = malloc(FAKE_SIZE);
	CHECK(t->fake.data != NULL);
	raspi3b_auds_set_output(t->auds, output ? &t->fake.out : NULL);
	arm_send(&t->l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t->l), MSG(CONNECT, 0, 0));
	auds_open(t, ARM_PORT, 1);
}

static void auds_down(Auds *t)
{
	raspi3b_auds_destroy(t->auds);
	free(t->fake.data);
	link_down(&t->l);
}

/* A vc_audio_msg of the Arm's, from port to the VideoCore's vc_port. */
static void send_from(Auds *t, unsigned port, unsigned vc_port, uint32_t type,
                      uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4)
{
	uint32_t m[5] = {type, w1, w2, w3, w4};

	arm_send(&t->l, MSG(DATA, port, vc_port), sizeof(m), m);
}

static void send_msg(Auds *t, uint32_t type, uint32_t w1, uint32_t w2,
                     uint32_t w3)
{
	send_from(t, ARM_PORT, 1, type, w1, w2, w3, 0);
}

/* The VideoCore's next message, of type and three words, to port. */
static void check_msg_to(Auds *t, unsigned vc_port, unsigned port,
                         uint32_t type, uint32_t w1, uint32_t w2, uint32_t w3)
{
	uint32_t got[SLOT / 4], size, msgid;

	do
		msgid = vc_message(&t->l, &size, (uint8_t *)got);
	while (msgid == MSG(PADDING, 0, 0));
	CHECK_EQ(msgid, MSG(DATA, vc_port, port));
	CHECK_EQ(size, 20);
	CHECK_EQ(got[0], type);
	CHECK_EQ(got[1], w1);
	CHECK_EQ(got[2], w2);
	CHECK_EQ(got[3], w3);
	CHECK_EQ(got[4], 0);
}

static void check_result(Auds *t)
{
	check_msg_to(t, 1, ARM_PORT, RESULT, 0, 0, 0);
}

static void check_complete(Auds *t, uint32_t count)
{
	check_msg_to(t, 1, ARM_PORT, COMPLETE, count, COOKIE1, COOKIE2);
}

static void check_quiet(Auds *t)
{
	CHECK_EQ(rd(&t->l.s, SLOTS + MASTER + TX_POS), t->l.vc_pos);
}

static void check_running(Auds *t)
{
	CHECK_EQ(t->l.s.m->stop, MACHINE_STOP_NONE);
}

static void check_stopped(Auds *t, const char *message)
{
	check_stop_by(&t->l.s, "firmware: AUDS: ", message);
}

static void control(Auds *t, int32_t volume, uint32_t dest)
{
	send_msg(t, CONTROL, (uint32_t)volume, dest, 0);
	check_result(t);
}

static void config(Auds *t, uint32_t channels, uint32_t rate, uint32_t bits)
{
	send_msg(t, CONFIG, channels, rate, bits);
	check_result(t);
}

/* OPEN, then CONTROL and CONFIG, as Linux's open and prepare. */
static void prepare(Auds *t, uint32_t channels, uint32_t rate, uint32_t bits)
{
	send_msg(t, AOPEN, 0, 0, 0);
	control(t, 0, 1);
	config(t, channels, rate, bits);
}

/* The byte at i of what the tests write. */
static uint8_t pattern(uint32_t i)
{
	return (uint8_t)(i * 7 + (i >> 8) * 3 + (i >> 16));
}

/* A WRITE of n bytes of the pattern from at, from port, in DATA. */
static void write_from(Auds *t, unsigned port, unsigned vc_port, uint32_t at,
                       uint32_t n)
{
	uint8_t data[MAX_PACKET + 4];

	send_from(t, port, vc_port, WRITE, n, COOKIE1, COOKIE2, MAX_PACKET << 16);
	while (n > 0) {
		uint32_t k = n < MAX_PACKET ? n : MAX_PACKET;

		for (uint32_t i = 0; i < k; i++)
			data[i] = pattern(at + i);
		arm_send(&t->l, MSG(DATA, port, vc_port), k, (uint32_t *)data);
		at += k;
		n -= k;
	}
}

static void write_pattern(Auds *t, uint32_t at, uint32_t n)
{
	write_from(t, ARM_PORT, 1, at, n);
}

/* The output kept n bytes, the pattern from at. */
static void check_output(Auds *t, uint32_t at, uint32_t n)
{
	CHECK_EQ(t->fake.len, n);
	for (uint32_t i = 0; i < n && i < t->fake.len; i++)
		if (t->fake.data[i] != pattern(at + i)) {
			CHECK_EQ(i, n); /* the first byte that differs */
			break;
		}
}

/* Run to the next block of the stream started at started. */
static void run_block(Auds *t)
{
	run(&t->l, BLOCK);
}

/* Linux's stream: written, started, then played 10 ms at a time. */
static void test_stream(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 2, 48000, 16);
	/* 25 ms: 1200 frames of 4 bytes. */
	write_pattern(&t, 0, 4800);
	check_quiet(&t);
	CHECK_EQ(t.fake.starts, 0);
	send_msg(&t, START, 0, 0, 0);
	check_quiet(&t);
	run(&t.l, BLOCK - 1);
	check_quiet(&t);
	CHECK_EQ(t.fake.starts, 0);
	run(&t.l, 1);
	check_complete(&t, 1920);
	CHECK_EQ(t.fake.starts, 1);
	CHECK_EQ(t.fake.format.channels, 2);
	CHECK_EQ(t.fake.format.rate, 48000);
	CHECK_EQ(t.fake.format.bits, 16);
	check_output(&t, 0, 1920);
	run_block(&t);
	check_complete(&t, 1920);
	/* The rest, then the end, a block later. */
	run_block(&t);
	check_complete(&t, 960);
	check_output(&t, 0, 4800);
	CHECK_EQ(t.fake.stops, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	CHECK_EQ(t.fake.stops, 1);
	run(&t.l, 10 * BLOCK);
	check_quiet(&t);
	check_running(&t);

	/* Linux's XRUN: STOP, then prepare and START again. */
	send_msg(&t, STOP, 0, 0, 0);
	control(&t, 0, 1);
	config(&t, 2, 48000, 16);
	write_pattern(&t, 4800, 8);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 8);
	CHECK_EQ(t.fake.starts, 2);
	check_output(&t, 0, 4808);
	run_block(&t);
	check_complete(&t, FINISHED);
	CHECK_EQ(t.fake.stops, 2);

	/* CLOSE: a RESULT. */
	send_msg(&t, ACLOSE, 0, 0, 0);
	check_result(&t);
	check_quiet(&t);
	check_running(&t);
	auds_down(&t);
}

/* The frames whose time has come, at rates that do not divide. */
static void test_rates(void)
{
	static const struct {
		uint32_t channels, rate, bits;
		uint32_t first, second, third;
	} cases[] = {
		{1, 22050, 16, 440, 442, 440}, /* 220.5 frames a block */
		{1, 44100, 8, 441, 441, 441},
		{3, 8000, 8, 240, 240, 240},
		{8, 192000, 16, 30720, 30720, 30720},
		{1, 11025, 8, 110, 110, 110}, /* 110.25 */
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t total = cases[i].first + cases[i].second + cases[i].third;
		Auds t;

		auds_up(&t, true);
		prepare(&t, cases[i].channels, cases[i].rate, cases[i].bits);
		write_pattern(&t, 0, total + 1000 * cases[i].channels);
		send_msg(&t, START, 0, 0, 0);
		run_block(&t);
		check_complete(&t, cases[i].first);
		run_block(&t);
		check_complete(&t, cases[i].second);
		run_block(&t);
		check_complete(&t, cases[i].third);
		CHECK_EQ(t.fake.format.channels, cases[i].channels);
		CHECK_EQ(t.fake.format.rate, cases[i].rate);
		CHECK_EQ(t.fake.format.bits, cases[i].bits);
		check_output(&t, 0, total);
		check_running(&t);
		auds_down(&t);
	}
}

/* Fewer frames than the block's, and a part of a frame left. */
static void test_short(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 2, 48000, 16);
	write_pattern(&t, 0, 7);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 4);
	check_output(&t, 0, 4);
	run_block(&t);
	check_complete(&t, FINISHED);
	/* The 3 bytes left were dropped with the end: 1 more is no frame. */
	write_pattern(&t, 100, 1);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	check_output(&t, 0, 4);

	/* START with nothing written: the end, at the first block. */
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	CHECK_EQ(t.fake.starts, 1);
	CHECK_EQ(t.fake.stops, 1);
	check_running(&t);
	auds_down(&t);

	/* A last frame of one byte. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 81);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	run_block(&t);
	check_complete(&t, 1);
	check_output(&t, 0, 81);
	run_block(&t);
	check_complete(&t, FINISHED);
	auds_down(&t);
}

/* STOP drops what was not played; with draining, it plays on. */
static void test_stop(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 200);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	send_msg(&t, STOP, 0, 0, 0);
	CHECK_EQ(t.fake.stops, 1);
	run(&t.l, 5 * BLOCK);
	check_quiet(&t);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	CHECK_EQ(t.fake.starts, 1);
	check_output(&t, 0, 80);

	/* Draining: to the end of what was written. */
	write_pattern(&t, 80, 100);
	send_msg(&t, START, 0, 0, 0);
	send_msg(&t, STOP, 1, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	run_block(&t);
	check_complete(&t, 20);
	run_block(&t);
	check_complete(&t, FINISHED);
	check_output(&t, 0, 180);

	/* STOP before START drops what was written; draining keeps it. */
	write_pattern(&t, 180, 10);
	send_msg(&t, STOP, 1, 0, 0);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 10);
	run_block(&t);
	check_complete(&t, FINISHED);
	write_pattern(&t, 190, 10);
	send_msg(&t, STOP, 0, 0, 0);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	check_output(&t, 0, 190);
	check_running(&t);
	auds_down(&t);
}

/* A new CONFIG drops what was written; the next stream is new. */
static void test_config_drops(void)
{
	Auds t;

	/* No CONTROL: 0 dB and auto, as the service starts. */
	auds_up(&t, true);
	send_msg(&t, AOPEN, 0, 0, 0);
	config(&t, 2, 48000, 16);
	write_pattern(&t, 0, 400);
	config(&t, 1, 48000, 16);
	write_pattern(&t, 0, 2);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 2);
	CHECK_EQ(t.fake.format.channels, 1);
	check_output(&t, 0, 2);
	check_running(&t);
	auds_down(&t);
}

/* The FIFO's end wraps to its start, in the middle of a frame. */
static void test_wrap(void)
{
	uint32_t played = 0;
	Auds t;

	auds_up(&t, true);
	prepare(&t, 3, 192000, 8);
	write_pattern(&t, 0, 300000);
	send_msg(&t, START, 0, 0, 0);
	for (int i = 0; i < 30; i++) {
		run_block(&t);
		check_complete(&t, 5760);
		played += 5760;
	}
	write_pattern(&t, 300000, 300000);
	check_running(&t);
	while (played < 600000 - 5760) {
		run_block(&t);
		check_complete(&t, 5760);
		played += 5760;
	}
	run_block(&t);
	check_complete(&t, 600000 - played);
	run_block(&t);
	check_complete(&t, FINISHED);
	check_output(&t, 0, 600000);
	check_running(&t);
	auds_down(&t);
}

/* As much as the FIFO holds, and one byte more. */
static void test_fifo_full(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 2, 48000, 16);
	write_pattern(&t, 0, FIFO - 4);
	write_pattern(&t, 0, 4);
	check_running(&t);
	send_from(&t, ARM_PORT, 1, WRITE, 1, COOKIE1, COOKIE2, MAX_PACKET << 16);
	check_stopped(&t, "more than 512 KB of samples waiting to play is not "
	                  "emulated");
	auds_down(&t);
}

/* The volume: 10^(-dB/20), rounded to the nearest, within the range. */
static void test_volume(void)
{
	static const struct {
		int32_t volume;
		int16_t in[4], out[4];
		uint8_t in8[3], out8[3];
	} cases[] = {
		/*
	     * 10 dB: 0.316; the halves on each side of the rounding:
	     * 2.53, -94.55, -90.44, 0.63.
	     */
		{2560,
	     {8, -299, -286, 2},
	     {3, -95, -90, 1},
	     {0x88, 0x00, 0x82},
	     {0x83, 0x58, 0x81}},
		/* 20 dB: a tenth. */
		{5120,
	     {32764, -1236, 32767, -32768},
	     {3276, -124, 3277, -3277},
	     {0xff, 0x00, 0x80},
	     {0x8d, 0x73, 0x80}},
		/* +4 dB: 1.585, clamped. */
		{-1024,
	     {1000, -1000, 32767, -32768},
	     {1585, -1585, 32767, -32768},
	     {0xff, 0x00, 0x90},
	     {0xff, 0x00, 0x99}},
		/* CHIP_MIN_VOLUME: 102.4 dB down, silence. */
		{26214,
	     {32767, -32768, 1, -1},
	     {0, 0, 0, 0},
	     {0xff, 0x00, 0x81},
	     {0x80, 0x80, 0x80}},
		/* 0 dB: as it is. */
		{0,
	     {32767, -32768, 1, -1},
	     {32767, -32768, 1, -1},
	     {0xff, 0x00, 0x81},
	     {0xff, 0x00, 0x81}},
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t words[2];
		Auds t;

		memcpy(words, cases[i].in, 8);
		auds_up(&t, true);
		send_msg(&t, AOPEN, 0, 0, 0);
		control(&t, cases[i].volume, 1);
		config(&t, 2, 48000, 16);
		send_from(&t, ARM_PORT, 1, WRITE, 8, COOKIE1, COOKIE2,
		          MAX_PACKET << 16);
		arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 8, words);
		send_msg(&t, START, 0, 0, 0);
		run_block(&t);
		check_complete(&t, 8);
		CHECK_EQ(t.fake.len, 8);
		for (unsigned j = 0; j < 4; j++)
			CHECK_EQ(
				(int16_t)(t.fake.data[2 * j] | t.fake.data[2 * j + 1] << 8),
				cases[i].out[j]);
		run_block(&t);
		check_complete(&t, FINISHED);

		/* 8 bits: about 128. */
		send_msg(&t, STOP, 0, 0, 0);
		config(&t, 1, 8000, 8);
		memset(words, 0, sizeof(words));
		memcpy(words, cases[i].in8, 3);
		send_from(&t, ARM_PORT, 1, WRITE, 3, COOKIE1, COOKIE2,
		          MAX_PACKET << 16);
		arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 3, words);
		send_msg(&t, START, 0, 0, 0);
		run_block(&t);
		check_complete(&t, 3);
		CHECK_EQ(t.fake.len, 11);
		for (unsigned j = 0; j < 3; j++)
			CHECK_EQ(t.fake.data[8 + j], cases[i].out8[j]);
		check_running(&t);
		auds_down(&t);
	}
}

/*
 * The destination: auto and the headphones go to the output; HDMI,
 * with no display, nowhere, at the same pace. A change while playing
 * moves the stream.
 */
static void test_destinations(void)
{
	Auds t;

	for (uint32_t dest = 0; dest <= 3; dest++) {
		auds_up(&t, true);
		send_msg(&t, AOPEN, 0, 0, 0);
		control(&t, 0, dest);
		config(&t, 1, 8000, 8);
		write_pattern(&t, 0, 100);
		send_msg(&t, START, 0, 0, 0);
		run_block(&t);
		check_complete(&t, 80);
		CHECK_EQ(t.fake.starts, dest <= 1);
		check_output(&t, 0, dest <= 1 ? 80 : 0);
		run_block(&t);
		check_complete(&t, 20);
		run_block(&t);
		check_complete(&t, FINISHED);
		CHECK_EQ(t.fake.stops, dest <= 1);
		check_running(&t);
		auds_down(&t);
	}

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	control(&t, 0, 2);
	CHECK_EQ(t.fake.stops, 1);
	run_block(&t);
	check_complete(&t, 80);
	control(&t, 0, 3);
	run_block(&t);
	check_complete(&t, 80);
	control(&t, 0, 0);
	CHECK_EQ(t.fake.starts, 1);
	run_block(&t);
	check_complete(&t, 80);
	CHECK_EQ(t.fake.starts, 2);
	CHECK_EQ(t.fake.len, 160);
	for (uint32_t i = 0; i < 80; i++) {
		CHECK_EQ(t.fake.data[i], pattern(i));
		CHECK_EQ(t.fake.data[80 + i], pattern(240 + i));
	}
	/* The volume changes, the stream goes on. */
	control(&t, 0, 1);
	CHECK_EQ(t.fake.stops, 1);
	check_running(&t);
	auds_down(&t);
}

/* Nothing plugged in: the stream plays as with an output. */
static void test_no_output(void)
{
	Auds t;

	auds_up(&t, false);
	prepare(&t, 2, 48000, 16);
	write_pattern(&t, 0, 2000);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 1920);
	run_block(&t);
	check_complete(&t, 80);
	run_block(&t);
	check_complete(&t, FINISHED);
	send_msg(&t, ACLOSE, 0, 0, 0);
	check_result(&t);
	CHECK_EQ(t.fake.starts, 0);
	check_running(&t);
	auds_down(&t);
}

/* Two streams at once: one to HDMI, or both to the headphones. */
static void test_two_streams(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	auds_open(&t, ARM_PORT2, 2);
	send_from(&t, ARM_PORT2, 2, AOPEN, 0, 0, 0, 0);
	send_from(&t, ARM_PORT2, 2, CONTROL, 0, 2, 0, 0);
	check_msg_to(&t, 2, ARM_PORT2, RESULT, 0, 0, 0);
	send_from(&t, ARM_PORT2, 2, CONFIG, 1, 16000, 8, 0);
	check_msg_to(&t, 2, ARM_PORT2, RESULT, 0, 0, 0);
	write_from(&t, ARM_PORT2, 2, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	send_from(&t, ARM_PORT2, 2, START, 0, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	check_msg_to(&t, 2, ARM_PORT2, COMPLETE, 160, COOKIE1, COOKIE2);
	check_output(&t, 0, 80);
	check_running(&t);

	/* The second to the headphones too: mixing. */
	send_from(&t, ARM_PORT2, 2, CONTROL, 0, 1, 0, 0);
	check_msg_to(&t, 2, ARM_PORT2, RESULT, 0, 0, 0);
	run_block(&t);
	check_stopped(&t, "two streams playing to the headphones at once "
	                  "(mixing) is not emulated");
	auds_down(&t);
}

/* CLOSE ends the stream; then only OPEN. */
static void test_close(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	send_msg(&t, ACLOSE, 0, 0, 0);
	check_result(&t);
	CHECK_EQ(t.fake.stops, 1);
	run(&t.l, 5 * BLOCK);
	check_quiet(&t);
	/* Open again: as new, with nothing written and no format. */
	send_msg(&t, AOPEN, 0, 0, 0);
	send_from(&t, ARM_PORT, 1, WRITE, 1, COOKIE1, COOKIE2, MAX_PACKET << 16);
	check_stopped(&t, "a WRITE before CONFIG is not emulated");
	auds_down(&t);

	auds_up(&t, true);
	send_msg(&t, AOPEN, 0, 0, 0);
	send_msg(&t, ACLOSE, 0, 0, 0);
	check_result(&t);
	send_msg(&t, CONTROL, 0, 1, 0);
	check_stopped(&t, "a message of type 3 before OPEN is not emulated");
	auds_down(&t);

	/* What CLOSE forgot: the volume, the destination, the cookies. */
	auds_up(&t, true);
	send_msg(&t, AOPEN, 0, 0, 0);
	control(&t, 5120, 2);
	config(&t, 1, 8000, 16);
	send_from(&t, ARM_PORT, 1, WRITE, 0, 1, 2, MAX_PACKET << 16);
	send_msg(&t, ACLOSE, 0, 0, 0);
	check_result(&t);
	send_msg(&t, AOPEN, 0, 0, 0);
	config(&t, 1, 8000, 16);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_msg_to(&t, 1, ARM_PORT, COMPLETE, FINISHED, 0, 0);
	send_msg(&t, STOP, 0, 0, 0);
	send_from(&t, ARM_PORT, 1, WRITE, 2, COOKIE1, COOKIE2, MAX_PACKET << 16);
	arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 2, (uint32_t[]){0x1234});
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 2);
	CHECK_EQ(t.fake.len, 2);
	CHECK_EQ(t.fake.data[0] | t.fake.data[1] << 8, 0x1234);
	check_running(&t);
	auds_down(&t);

	/* A stream without a WRITE ends with cookies of 0. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_msg_to(&t, 1, ARM_PORT, COMPLETE, FINISHED, 0, 0);
	auds_down(&t);
}

/*
 * The Arm closes the service, or the firmware starts again: the stream
 * ends, and the port is new.
 */
static void test_service_close(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	arm_send(&t.l, MSG(CLOSE, ARM_PORT, 1), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CLOSE, 1, ARM_PORT));
	CHECK_EQ(t.fake.stops, 1);
	run(&t.l, 5 * BLOCK);
	check_quiet(&t);
	auds_open(&t, ARM_PORT, 1);
	send_msg(&t, START, 0, 0, 0);
	check_stopped(&t, "a message of type 6 before OPEN is not emulated");
	auds_down(&t);

	/* Closed in the middle of a WRITE: the rest is not awaited. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_from(&t, ARM_PORT, 1, WRITE, 100, COOKIE1, COOKIE2, MAX_PACKET << 16);
	arm_send(&t.l, MSG(CLOSE, ARM_PORT, 1), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CLOSE, 1, ARM_PORT));
	auds_open(&t, ARM_PORT, 1);
	send_msg(&t, AOPEN, 0, 0, 0);
	config(&t, 1, 8000, 8);
	check_running(&t);
	auds_down(&t);

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	raspi3b_vchiq_reset(t.l.s.v);
	CHECK_EQ(t.fake.stops, 1);
	run(&t.l, 5 * BLOCK);
	check_running(&t);
	link_start(&t.l);
	arm_send(&t.l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CONNECT, 0, 0));
	auds_open(&t, ARM_PORT, 1);
	send_msg(&t, AOPEN, 0, 0, 0);
	send_msg(&t, START, 0, 0, 0);
	check_stopped(&t, "a START before CONFIG is not emulated");
	auds_down(&t);
}

/* What the output's host refuses stops the machine. */
static void test_output_failures(void)
{
	static const char *const why[] = {"no start", "no play", "no stop"};
	char message[64];

	for (unsigned i = 0; i < 3; i++) {
		Auds t;

		auds_up(&t, true);
		t.fake.fail_start = i == 0;
		t.fake.fail_play = i == 1;
		t.fake.fail_stop = i == 2;
		prepare(&t, 1, 8000, 8);
		write_pattern(&t, 0, 10);
		send_msg(&t, START, 0, 0, 0);
		run_block(&t);
		if (i == 2) {
			check_complete(&t, 10);
			check_running(&t);
			run_block(&t);
		}
		snprintf(message, sizeof(message), "the host's audio output: %s",
		         why[i]);
		check_stopped(&t, message);
		check_quiet(&t);
		auds_down(&t);
	}
}

/* Each kind of message the VideoCore does not take: a fresh machine. */
static void expect_stop(Auds *t, const char *message)
{
	check_stopped(t, message);
	auds_down(t);
}

static void test_not_emulated(void)
{
	Auds t;

	/* Before OPEN, a second OPEN, short, and types not the Arm's. */
	auds_up(&t, true);
	send_msg(&t, CONFIG, 2, 48000, 16);
	expect_stop(&t, "a message of type 2 before OPEN is not emulated");
	auds_up(&t, true);
	send_msg(&t, AOPEN, 0, 0, 0);
	check_running(&t);
	send_msg(&t, AOPEN, 0, 0, 0);
	expect_stop(&t, "a second OPEN is not emulated");
	auds_up(&t, true);
	arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 19, (uint32_t[5]){AOPEN});
	expect_stop(&t, "a message of 19 bytes, not a vc_audio_msg, is not "
	                "emulated");
	for (uint32_t type = 0; type <= 9; type += type == 1 ? 8 : 1) {
		char message[64];

		auds_up(&t, true);
		send_msg(&t, AOPEN, 0, 0, 0);
		send_msg(&t, type, 0, 0, 0);
		snprintf(message, sizeof(message),
		         "the Arm's message of type %u is not emulated", type);
		expect_stop(&t, message);
	}

	/* CONFIG: the edges of what is taken, and past them. */
	static const uint32_t configs[][4] = {
		{1, 8000, 8, 1},   {8, 192000, 16, 1}, {0, 48000, 16, 0},
		{9, 48000, 16, 0}, {2, 7999, 16, 0},   {2, 192001, 16, 0},
		{2, 48000, 24, 0}, {2, 48000, 0, 0},   {2, 48000, 7, 0},
		{2, 48000, 32, 0},
	};
	for (unsigned i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		char message[96];

		auds_up(&t, true);
		send_msg(&t, AOPEN, 0, 0, 0);
		send_msg(&t, CONFIG, configs[i][0], configs[i][1], configs[i][2]);
		if (configs[i][3]) {
			check_result(&t);
			check_running(&t);
			auds_down(&t);
			continue;
		}
		snprintf(message, sizeof(message),
		         "a CONFIG of %u channels, %u Hz and %u bits is not emulated",
		         configs[i][0], configs[i][1], configs[i][2]);
		expect_stop(&t, message);
	}

	/* CONTROL: Linux's volumes and destinations, and past them. */
	static const int32_t controls[][3] = {
		{-1024, 0, 1}, {26214, 3, 1}, {-1025, 1, 0},
		{26215, 1, 0}, {0, 4, 0},     {INT32_MIN, 1, 0},
	};
	for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
		char message[96];

		auds_up(&t, true);
		send_msg(&t, AOPEN, 0, 0, 0);
		send_msg(&t, CONTROL, (uint32_t)controls[i][0],
		         (uint32_t)controls[i][1], 0);
		if (controls[i][2]) {
			check_result(&t);
			check_running(&t);
			auds_down(&t);
			continue;
		}
		snprintf(message, sizeof(message),
		         "a CONTROL of volume %d and destination %d is not emulated",
		         controls[i][0], controls[i][1]);
		expect_stop(&t, message);
	}

	/* Out of order. */
	auds_up(&t, true);
	send_msg(&t, AOPEN, 0, 0, 0);
	send_msg(&t, START, 0, 0, 0);
	expect_stop(&t, "a START before CONFIG is not emulated");
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_msg(&t, START, 0, 0, 0);
	send_msg(&t, START, 0, 0, 0);
	expect_stop(&t, "a START while the stream plays is not emulated");
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_msg(&t, START, 0, 0, 0);
	send_msg(&t, CONFIG, 1, 8000, 8);
	expect_stop(&t, "a CONFIG while the stream plays is not emulated");

	/* A WRITE whose max_packet is 0: the samples in a bulk transfer. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_msg(&t, WRITE, 8, COOKIE1, COOKIE2);
	expect_stop(&t, "a WRITE by bulk transfer is not emulated");

	/* DATA past its WRITE: max_packet, then the rest of the WRITE. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_from(&t, ARM_PORT, 1, WRITE, 200, COOKIE1, COOKIE2, 100u << 16);
	arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 100, NULL);
	check_running(&t);
	arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 101, NULL);
	expect_stop(&t, "DATA of 101 bytes, more than max_packet (100) or the 100 "
	                "left of its WRITE, is not emulated");
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_from(&t, ARM_PORT, 1, WRITE, 50, COOKIE1, COOKIE2, 100u << 16);
	arm_send(&t.l, MSG(DATA, ARM_PORT, 1), 51, NULL);
	expect_stop(&t, "DATA of 51 bytes, more than max_packet (100) or the 50 "
	                "left of its WRITE, is not emulated");

	/* A WRITE of nothing expects nothing; the next is a message. */
	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	send_from(&t, ARM_PORT, 1, WRITE, 0, COOKIE1, COOKIE2, 100u << 16);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, FINISHED);
	check_running(&t);
	auds_down(&t);
}

/* The service, as the Arm finds it: 'AUDS', version 2 from 1. */
static void test_versions(void)
{
	static const struct {
		uint16_t version, min;
		bool open;
	} cases[] = {
		{2, 1, true}, {1, 1, true}, {9, 2, true}, {0, 0, false}, {9, 3, false},
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t payload[3] = {AUDS, 0,
		                       (uint32_t)cases[i].min << 16 | cases[i].version};
		Auds t;

		link_up(&t.l, true);
		t.auds = raspi3b_auds_create(t.l.s.m, t.l.s.v);
		t.fake.data = NULL;
		arm_send(&t.l, MSG(CONNECT, 0, 0), 0, NULL);
		CHECK_EQ(vc_header(&t.l), MSG(CONNECT, 0, 0));
		arm_send(&t.l, MSG(OPEN, ARM_PORT, 0), 12, payload);
		if (cases[i].open) {
			uint8_t ack[SLOT];
			uint32_t size;

			CHECK_EQ(vc_message(&t.l, &size, ack), MSG(OPENACK, 1, ARM_PORT));
			CHECK_EQ(ack[0] | ack[1] << 8, 2);
		} else {
			CHECK_EQ(vc_header(&t.l), MSG(CLOSE, 0, ARM_PORT));
		}
		auds_down(&t);
	}
}

/*
 * The board's property service makes the audio service, and hands it
 * the output: the link on its VCHIQ.
 */
static void test_property_output(void)
{
	Raspi3bProperty *p;
	Fake fake = {.out.ops = &fake_ops};
	Auds t;

	memset(&t, 0, sizeof(t));
	t.l.s.m = machine_create();
	CHECK(t.l.s.m != NULL);
	CHECK_EQ(bus_map_ram(t.l.s.m->bus, "ram", 0, RAM_SIZE), BUS_OK);
	p = raspi3b_property_create(t.l.s.m, RAM_SIZE);
	CHECK(p != NULL);
	t.l.s.v = raspi3b_property_vchiq(p);
	irq_init(&t.l.irq, NULL, NULL, 0);
	t.l.db = bcm2835_doorbell_create(
		t.l.s.m, &t.l.irq,
		&(Bcm2835DoorbellVc){raspi3b_vchiq_doorbell, t.l.s.v});
	CHECK_EQ(bcm2835_doorbell_map(t.l.db, t.l.s.m->bus, "doorbell", BELL),
	         BUS_OK);
	raspi3b_vchiq_attach(t.l.s.v, t.l.db, LATENCY, NULL, NULL);
	link_start(&t.l);
	fake.data = malloc(FAKE_SIZE);
	t.fake = fake;
	raspi3b_property_set_audio(p, &t.fake.out);
	arm_send(&t.l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CONNECT, 0, 0));
	auds_open(&t, ARM_PORT, 1);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 80);
	send_msg(&t, START, 0, 0, 0);
	run_block(&t);
	check_complete(&t, 80);
	CHECK_EQ(t.fake.starts, 1);
	check_output(&t, 0, 80);
	check_running(&t);
	bcm2835_doorbell_destroy(t.l.db);
	raspi3b_property_destroy(p);
	machine_destroy(t.l.s.m);
	free(t.fake.data);
}

/* Destroyed while it plays: nothing is left to fire. */
static void test_destroy(void)
{
	Auds t;

	auds_up(&t, true);
	prepare(&t, 1, 8000, 8);
	write_pattern(&t, 0, 400);
	send_msg(&t, START, 0, 0, 0);
	raspi3b_auds_destroy(t.auds);
	run(&t.l, 5 * BLOCK);
	check_quiet(&t);
	free(t.fake.data);
	link_down(&t.l);
}

int main(void)
{
	RUN_TEST(test_stream);
	RUN_TEST(test_rates);
	RUN_TEST(test_short);
	RUN_TEST(test_stop);
	RUN_TEST(test_config_drops);
	RUN_TEST(test_wrap);
	RUN_TEST(test_fifo_full);
	RUN_TEST(test_volume);
	RUN_TEST(test_destinations);
	RUN_TEST(test_no_output);
	RUN_TEST(test_two_streams);
	RUN_TEST(test_close);
	RUN_TEST(test_service_close);
	RUN_TEST(test_output_failures);
	RUN_TEST(test_not_emulated);
	RUN_TEST(test_versions);
	RUN_TEST(test_property_output);
	RUN_TEST(test_destroy);
	return test_summary();
}
