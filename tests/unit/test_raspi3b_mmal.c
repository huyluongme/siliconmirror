/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's 'mmal' service (machine/raspi3b_mmal.c) over the
 * VideoCore's end of VCHIQ, as Linux's MMAL users drive it at probe
 * (bcm2835-codec, bcm2835-isp, bcm2835-camera; mmal-vchiq.c), and what
 * it does not model.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "machine/raspi3b_mmal.h"
#include "machine/raspi3b_vchiq.h"
#include "test.h"
#include "vchiq_link.h"

#define FOURCC(a, b, c, d)                                                     \
	((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 |                \
	 (uint32_t)(d) << 24)
#define MAGIC FOURCC('m', 'm', 'a', 'l')
#define SERVICE 0xc0ffee01u     /* the header's control_service */
#define STATUS_WORD 0x55aa55aau /* the Arm's status, not 0 */
#define PADDING_WORD 0xabcd1234u

enum { CREATE = 4, DESTROY = 5, INFO_GET = 8, PARAM_GET = 15 };
enum { CONTROL = 1, INPUT = 2, OUTPUT = 3, CLOCK = 4 };
enum { SUCCESS = 0, ENOSPC = 2 };
#define VIDEO 3u
#define ENCODINGS 1u
#define CAMERA_INFO 0x1001cu
#define ROOM 280u /* the drivers' MAX_SUPPORTED_ENCODINGS, 70 */

/* The encodings, in the order that V4L2 lists them on boards. */
#define H264 FOURCC('H', '2', '6', '4')
#define H263 FOURCC('H', '2', '6', '3')
#define MP4V FOURCC('M', 'P', '4', 'V')
#define MJPEG FOURCC('M', 'J', 'P', 'G')
#define VP8 FOURCC('V', 'P', '8', ' ')
#define JPEG FOURCC('J', 'P', 'E', 'G')
#define I420 FOURCC('I', '4', '2', '0')
#define YV12 FOURCC('Y', 'V', '1', '2')
#define NV12 FOURCC('N', 'V', '1', '2')
#define NV21 FOURCC('N', 'V', '2', '1')
#define YUYV FOURCC('Y', 'U', 'Y', 'V')
#define YVYU FOURCC('Y', 'V', 'Y', 'U')
#define UYVY FOURCC('U', 'Y', 'V', 'Y')
#define VYUY FOURCC('V', 'Y', 'U', 'Y')
#define RGB565 FOURCC('R', 'G', 'B', '2')
#define RGB24 FOURCC('R', 'G', 'B', '3')
#define BGR24 FOURCC('B', 'G', 'R', '3')
#define RGBA FOURCC('R', 'G', 'B', 'A')
#define BGRA FOURCC('B', 'G', 'R', 'A')

static const uint32_t decode_in[] = {MP4V, H264, H263, MJPEG, VP8};
static const uint32_t decode_out[] = {I420, YV12, NV12, NV21, RGB565};
static const uint32_t encode_in[] = {I420,  YV12, NV12, NV21, RGB565, RGB24,
                                     BGR24, BGRA, YUYV, YVYU, UYVY,   VYUY};
static const uint32_t encode_out[] = {H264, MJPEG};
/* A Zero 2 W's /dev/video13: BA81, RGGB, ... Y14. */
static const uint32_t isp_in[] = {
	FOURCC('B', 'A', '8', '1'),
	FOURCC('R', 'G', 'G', 'B'),
	FOURCC('G', 'B', 'R', 'G'),
	FOURCC('G', 'R', 'B', 'G'),
	FOURCC('p', 'B', 'A', 'A'),
	FOURCC('p', 'R', 'A', 'A'),
	FOURCC('p', 'G', 'A', 'A'),
	FOURCC('p', 'g', 'A', 'A'),
	FOURCC('p', 'B', '1', '2'),
	FOURCC('p', 'R', '1', '2'),
	FOURCC('p', 'G', '1', '2'),
	FOURCC('p', 'g', '1', '2'),
	FOURCC('p', 'B', 'E', 'E'),
	FOURCC('p', 'R', 'E', 'E'),
	FOURCC('p', 'G', 'E', 'E'),
	FOURCC('p', 'g', 'E', 'E'),
	FOURCC('B', 'G', '1', '6'),
	FOURCC('R', 'G', '1', '6'),
	FOURCC('G', 'B', '1', '6'),
	FOURCC('G', 'R', '1', '6'),
	FOURCC('G', 'R', 'E', 'Y'),
	FOURCC('Y', '1', '0', 'P'),
	FOURCC('Y', '1', '2', 'P'),
	FOURCC('Y', '1', '4', 'P'),
	FOURCC('Y', '1', '6', ' '),
	YUYV,
	YVYU,
	VYUY,
	UYVY,
	I420,
	YV12,
	RGB24,
	BGR24,
	RGBA,
	BGRA,
	RGB565,
	NV12,
	NV21,
	FOURCC('B', 'G', '1', '0'),
	FOURCC('R', 'G', '1', '0'),
	FOURCC('G', 'B', '1', '0'),
	FOURCC('B', 'A', '1', '0'),
	FOURCC('B', 'G', '1', '2'),
	FOURCC('R', 'G', '1', '2'),
	FOURCC('G', 'B', '1', '2'),
	FOURCC('B', 'A', '1', '2'),
	FOURCC('B', 'G', '1', '4'),
	FOURCC('R', 'G', '1', '4'),
	FOURCC('G', 'B', '1', '4'),
	FOURCC('G', 'R', '1', '4'),
	FOURCC('Y', '1', '0', ' '),
	FOURCC('Y', '1', '2', ' '),
	FOURCC('Y', '1', '4', ' '),
};
static const uint32_t isp_out0[] = {YUYV,   YVYU,  VYUY,  UYVY, I420,
                                    YV12,   RGB24, BGR24, RGBA, BGRA,
                                    RGB565, NV12,  NV21};
static const uint32_t isp_out1[] = {YUYV, YVYU, VYUY, UYVY,
                                    I420, YV12, NV12, NV21};
static const uint32_t isp_stats[] = {FOURCC('S', 'T', 'A', 'T')};
static const uint32_t fx[] = {I420, FOURCC('S', 'A', 'N', 'D')};
static const uint32_t image_encode_out[] = {JPEG};

typedef struct List {
	const uint32_t *list;
	uint32_t count;
} List;

#define LIST(a) {a, ARRAY_SIZE(a)}

typedef struct Probe {
	const char *name;
	uint32_t in, out;
	List ports[4];
} Probe;

/* bcm2835-codec's five, in the order it creates them. */
static const Probe codec[] = {
	{"ril.video_decode", 1, 1, {LIST(decode_in), LIST(decode_out)}},
	{"ril.video_encode", 1, 1, {LIST(encode_in), LIST(encode_out)}},
	{"ril.isp",
     1,
     3,
     {LIST(isp_in), LIST(isp_out0), LIST(isp_out1), LIST(isp_stats)}},
	{"ril.image_fx", 1, 1, {LIST(fx), LIST(fx)}},
	{"ril.image_encode", 1, 1, {LIST(encode_in), LIST(image_encode_out)}},
};

static const Probe isp = {
	"ril.isp",
	1,
	3,
	{LIST(isp_in), LIST(isp_out0), LIST(isp_out1), LIST(isp_stats)}};

/* A client: the Arm's port that opened the service, and the VideoCore's. */
typedef struct Client {
	unsigned arm, vc;
} Client;

static const Client A = {7, 1}, B = {8, 2};

typedef struct Mmal {
	Link l;
	Raspi3bMmal *mmal;
	uint32_t context; /* the Arm's last */
} Mmal;

/* OPEN by c's Arm port, version 15 from 10, as vchiq_mmal_init. */
static void mmal_open(Mmal *t, Client c)
{
	uint32_t payload[3] = {MMAL, 0, 10u << 16 | 15};
	uint8_t ack[SLOT];
	uint32_t size;

	arm_send(&t->l, MSG(OPEN, c.arm, 0), 12, payload);
	CHECK_EQ(vc_message(&t->l, &size, ack), MSG(OPENACK, c.vc, c.arm));
	CHECK_EQ(size, 2);
	CHECK_EQ(ack[0] | ack[1] << 8, 15);
}

/* The link connected, and the service opened by A. */
static void mmal_up(Mmal *t)
{
	link_up(&t->l, true);
	t->mmal = raspi3b_mmal_create(t->l.s.m, t->l.s.v);
	CHECK(t->mmal != NULL);
	t->context = 0x7f000100u; /* its top byte too */
	arm_send(&t->l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t->l), MSG(CONNECT, 0, 0));
	mmal_open(t, A);
}

static void mmal_down(Mmal *t)
{
	CHECK_EQ(t->l.warnings, 0);
	raspi3b_mmal_destroy(t->mmal);
	link_down(&t->l);
}

/*
 * c's message of type, size bytes of body after the header; the status
 * and padding are not 0, which the reply's must not copy blindly.
 */
static void mmal_send(Mmal *t, Client c, uint32_t type, const void *body,
                      uint32_t size)
{
	uint32_t m[SLOT / 4] = {MAGIC,        type,        SERVICE,
	                        ++t->context, STATUS_WORD, PADDING_WORD};

	memcpy(&m[6], body, size);
	arm_send(&t->l, MSG(DATA, c.arm, c.vc), 24 + size, m);
}

/* The VideoCore's next message, to c, a reply to type of size bytes. */
static void mmal_reply(Mmal *t, Client c, uint32_t type, void *body,
                       uint32_t size)
{
	uint32_t got[SLOT / 4], n, msgid;

	do
		msgid = vc_message(&t->l, &n, (uint8_t *)got);
	while (msgid == MSG(PADDING, 0, 0));
	CHECK_EQ(msgid, MSG(DATA, c.vc, c.arm));
	CHECK_EQ(n, 24 + size);
	CHECK_EQ(got[0], MAGIC);
	CHECK_EQ(got[1], type);
	CHECK_EQ(got[2], SERVICE);
	CHECK_EQ(got[3], t->context);
	CHECK_EQ(got[4], 0);
	CHECK_EQ(got[5], PADDING_WORD);
	memcpy(body, &got[6], size);
}

/* Nothing more from the VideoCore. */
static void check_quiet(Mmal *t)
{
	CHECK_EQ(rd(&t->l.s, SLOTS + MASTER + TX_POS), t->l.vc_pos);
}

/*
 * The machine stopped with message, and the VideoCore said nothing;
 * the message was not read: the next signal stops again.
 */
static void mmal_stop(Mmal *t, const char *message)
{
	check_stop_by(&t->l.s, "firmware: MMAL: ", message);
	check_quiet(t);
	arm_signal(&t->l);
	run(&t->l, LATENCY);
	check_stop_by(&t->l.s, "firmware: MMAL: ", message);
}

/* A fresh board, for the next stop. */
static void again(Mmal *t)
{
	mmal_down(t);
	mmal_up(t);
}

/* COMPONENT_CREATE, as create_component: client_component 3, pid 0. */
static void create(Mmal *t, Client c, const char *name, size_t length)
{
	uint8_t body[136] = {3};

	memcpy(body + 4, name, length);
	mmal_send(t, c, CREATE, body, sizeof(body));
}

static void check_created(Mmal *t, Client c, uint32_t handle, uint32_t in,
                          uint32_t out)
{
	uint32_t r[5];

	mmal_reply(t, c, CREATE, r, sizeof(r));
	CHECK_EQ(r[0], SUCCESS);
	CHECK_EQ(r[1], handle);
	CHECK_EQ(r[2], in);
	CHECK_EQ(r[3], out);
	CHECK_EQ(r[4], 0); /* clocks */
}

static void create_ok(Mmal *t, Client c, const char *name, uint32_t handle,
                      uint32_t in, uint32_t out)
{
	create(t, c, name, strlen(name) + 1);
	check_created(t, c, handle, in, out);
}

static void destroy(Mmal *t, Client c, uint32_t handle)
{
	mmal_send(t, c, DESTROY, &handle, 4);
}

static void destroy_ok(Mmal *t, Client c, uint32_t handle)
{
	uint32_t r;

	destroy(t, c, handle);
	mmal_reply(t, c, DESTROY, &r, 4);
	CHECK_EQ(r, SUCCESS);
}

static void info_get(Mmal *t, Client c, uint32_t handle, uint32_t type,
                     uint32_t index)
{
	uint32_t body[3] = {handle, type, index};

	mmal_send(t, c, INFO_GET, body, sizeof(body));
}

/*
 * The reply to PORT_INFO_GET of the port of type and index, all among
 * all the component's ports, its format es_type and encoding.
 */
static void check_info(Mmal *t, Client c, uint32_t handle, uint32_t type,
                       uint32_t index, uint32_t all, uint32_t es_type,
                       uint32_t encoding)
{
	uint32_t want[73] = {0}, got[73];

	want[1] = handle;
	want[2] = type;
	want[3] = index;
	want[4] = 1;   /* found */
	want[5] = all; /* the port's handle */
	/* struct mmal_port, from word 6 */
	want[6 + 2] = type;
	want[6 + 3] = index | all << 16;
	want[6 + 6] = 1;  /* buffer_num_min */
	want[6 + 9] = 1;  /* buffer_num_recommended */
	want[6 + 11] = 1; /* buffer_num */
	/* struct mmal_es_format, from word 22 */
	want[22] = es_type;
	want[23] = encoding;
	mmal_reply(t, c, INFO_GET, got, sizeof(got));
	for (unsigned i = 0; i < 73; i++)
		CHECK_EQ(got[i], want[i]);
}

static void param_get(Mmal *t, Client c, uint32_t handle, uint32_t port,
                      uint32_t id, uint32_t size)
{
	uint32_t body[4] = {handle, port, id, size};

	mmal_send(t, c, PARAM_GET, body, sizeof(body));
}

/*
 * The reply to PORT_PARAMETER_GET: status, id, the parameter's size
 * (8 + need), and the first bytes of value.
 */
static void check_param(Mmal *t, Client c, uint32_t status, uint32_t id,
                        uint32_t need, const void *value, uint32_t bytes)
{
	uint8_t got[12 + 384];

	mmal_reply(t, c, PARAM_GET, got, 12 + bytes);
	CHECK_EQ(got[0] | got[1] << 8 | got[2] << 16 | (uint32_t)got[3] << 24,
	         status);
	CHECK_EQ(got[4] | got[5] << 8 | got[6] << 16 | (uint32_t)got[7] << 24, id);
	CHECK_EQ(got[8] | got[9] << 8 | got[10] << 16 | (uint32_t)got[11] << 24,
	         8 + need);
	CHECK(memcmp(got + 12, value, bytes) == 0);
}

/*
 * vchiq_mmal_component_init of p's component, handle h, then
 * SUPPORTED_ENCODINGS of the first count ports, whose handles are 1 on
 * (bcm2835-codec asks input[0] and output[0]; bcm2835-isp the four).
 */
static void probe(Mmal *t, Client c, const Probe *p, uint32_t h, unsigned count)
{
	create_ok(t, c, p->name, h, p->in, p->out);
	info_get(t, c, h, CONTROL, 0);
	check_info(t, c, h, CONTROL, 0, 0, 0, 0);
	for (uint32_t i = 0; i < p->in; i++) {
		info_get(t, c, h, INPUT, i);
		check_info(t, c, h, INPUT, i, 1 + i, VIDEO, p->ports[i].list[0]);
	}
	for (uint32_t i = 0; i < p->out; i++) {
		info_get(t, c, h, OUTPUT, i);
		check_info(t, c, h, OUTPUT, i, 1 + p->in + i, VIDEO,
		           p->ports[p->in + i].list[0]);
	}
	for (uint32_t i = 0; i < count; i++) {
		const List *l = &p->ports[i];

		param_get(t, c, h, 1 + i, ENCODINGS, 8 + ROOM);
		check_param(t, c, SUCCESS, ENCODINGS, 4 * l->count, l->list,
		            4 * l->count);
	}
}

/* bcm2835-codec's probe: each component made, asked and destroyed. */
static void test_codec(void)
{
	Mmal t;

	mmal_up(&t);
	for (size_t i = 0; i < ARRAY_SIZE(codec); i++) {
		probe(&t, A, &codec[i], 1, 2);
		destroy_ok(&t, A, 1);
	}
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	check_quiet(&t);
	mmal_down(&t);
}

/*
 * bcm2835-isp's two instances, each its own client, which keep their
 * components; and bcm2835-camera's camera_info: no camera.
 */
static void test_isp_camera(void)
{
	static const uint8_t zero[144];
	Mmal t;

	mmal_up(&t);
	mmal_open(&t, B);
	probe(&t, A, &isp, 1, 4);
	probe(&t, B, &isp, 2, 4);
	CHECK_EQ(ARRAY_SIZE(isp_in), 53);
	/* camera_info, by a third client: control port only. */
	mmal_open(&t, (Client){9, 3});
	create_ok(&t, (Client){9, 3}, "camera_info", 3, 0, 0);
	info_get(&t, (Client){9, 3}, 3, CONTROL, 0);
	check_info(&t, (Client){9, 3}, 3, CONTROL, 0, 0, 0, 0);
	param_get(&t, (Client){9, 3}, 3, 0, CAMERA_INFO, 8 + 144);
	check_param(&t, (Client){9, 3}, SUCCESS, CAMERA_INFO, 144, zero, 144);
	/* A room too small: ENOSPC, the size, what fits. */
	param_get(&t, (Client){9, 3}, 3, 0, CAMERA_INFO, 8 + 143);
	check_param(&t, (Client){9, 3}, ENOSPC, CAMERA_INFO, 144, zero, 143);
	destroy_ok(&t, (Client){9, 3}, 3);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	mmal_down(&t);
}

/* The room of PORT_PARAMETER_GET: from 0 to 384 bytes. */
static void test_room(void)
{
	Mmal t;

	mmal_up(&t);
	create_ok(&t, A, "ril.video_decode", 1, 1, 1);
	param_get(&t, A, 1, 1, ENCODINGS, 8 + 20);
	check_param(&t, A, SUCCESS, ENCODINGS, 20, decode_in, 20);
	param_get(&t, A, 1, 1, ENCODINGS, 8 + 19);
	check_param(&t, A, ENOSPC, ENCODINGS, 20, decode_in, 19);
	param_get(&t, A, 1, 1, ENCODINGS, 8);
	check_param(&t, A, ENOSPC, ENCODINGS, 20, decode_in, 0);
	param_get(&t, A, 1, 2, ENCODINGS, 8 + 384);
	check_param(&t, A, SUCCESS, ENCODINGS, 20, decode_out, 20);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	mmal_down(&t);
}

/*
 * Handles: the lowest free from 1, 16 at a time; a client's go when it
 * closes the service, and the firmware's start closes them all.
 */
static void test_handles(void)
{
	Mmal t;

	mmal_up(&t);
	mmal_open(&t, B);
	for (uint32_t h = 1; h <= 16; h++)
		create_ok(&t, h % 2 ? A : B, "ril.image_fx", h, 1, 1);
	destroy_ok(&t, B, 16);
	create_ok(&t, B, "ril.image_fx", 16, 1, 1);
	create(&t, A, "ril.image_fx", 13);
	mmal_stop(&t, "more than 16 components is not emulated");
	again(&t);
	mmal_open(&t, B);
	create_ok(&t, A, "ril.isp", 1, 1, 3);
	create_ok(&t, B, "ril.isp", 2, 1, 3);
	create_ok(&t, A, "ril.isp", 3, 1, 3);
	destroy_ok(&t, A, 1);
	create_ok(&t, B, "camera_info", 1, 0, 0);
	/* A closes: its 3 goes, B's 1 and 2 stay. */
	arm_send(&t.l, MSG(CLOSE, A.arm, A.vc), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CLOSE, A.vc, A.arm));
	create_ok(&t, B, "ril.isp", 3, 1, 3);
	destroy_ok(&t, B, 2);
	info_get(&t, B, 1, CONTROL, 0);
	check_info(&t, B, 1, CONTROL, 0, 0, 0, 0);
	/* The firmware starts again: none. */
	raspi3b_vchiq_reset(t.l.s.v);
	link_start(&t.l);
	arm_send(&t.l, MSG(CONNECT, 0, 0), 0, NULL);
	CHECK_EQ(vc_header(&t.l), MSG(CONNECT, 0, 0));
	mmal_open(&t, (Client){B.arm, 1});
	create_ok(&t, (Client){B.arm, 1}, "ril.isp", 1, 1, 3);
	CHECK_EQ(t.l.s.m->stop, MACHINE_STOP_NONE);
	mmal_down(&t);
}

/* What the service does not model stops the machine. */
static void test_stops(void)
{
	static const struct {
		uint32_t type;
		const char *message;
	} types[] = {
		{0, "the Arm's message of type 0 is not emulated"},
		{1, "the Arm's QUIT (1) is not emulated"},
		{2, "SERVICE_CLOSED (2)"},
		{3, "GET_VERSION (3)"},
		{6, "COMPONENT_ENABLE (6)"},
		{7, "COMPONENT_DISABLE (7)"},
		{9, "PORT_INFO_SET (9)"},
		{10, "PORT_ACTION (10)"},
		{11, "BUFFER_FROM_HOST (11)"},
		{12, "BUFFER_TO_HOST (12)"},
		{13, "GET_STATS (13)"},
		{14, "PORT_PARAMETER_SET (14)"},
		{16, "EVENT_TO_HOST (16)"},
		{17, "GET_CORE_STATS_FOR_PORT (17)"},
		{18, "OPAQUE_ALLOCATOR (18)"},
		{19, "CONSUME_MEM (19)"},
		{20, "LMK (20)"},
		{21, "OPAQUE_ALLOCATOR_DESC (21)"},
		{22, "DRM_GET_LHS32 (22)"},
		{23, "DRM_GET_TIME (23)"},
		{24, "BUFFER_FROM_HOST_ZEROLEN (24)"},
		{25, "PORT_FLUSH (25)"},
		{26, "HOST_LOG (26)"},
		{27, "the Arm's message of type 27 is not emulated"},
		{0xffffffffu, "message of type 4294967295 is not emulated"},
	};
	static const struct {
		uint32_t type, size; /* one byte short of the body */
		const char *message;
	} shorts[] = {
		{CREATE, 135, "a COMPONENT_CREATE of 159 bytes is too short"},
		{DESTROY, 3, "a COMPONENT_DESTROY of 27 bytes is too short"},
		{INFO_GET, 11, "a PORT_INFO_GET of 35 bytes is too short"},
		{PARAM_GET, 15, "a PORT_PARAMETER_GET of 39 bytes is too short"},
	};
	static const struct {
		const char *name;
		size_t length;
		const char *message;
	} names[] = {
		{"ril.camera", 11, "the component 'ril.camera' is not emulated"},
		{"ril.isp2", 9, "the component 'ril.isp2' is not"},
		{"ril.is", 7, "the component 'ril.is' is not"},
		{"", 1, "the component '' is not"},
		{" ~\x1f\x7f\xff.", 7, "the component ' ~???.' is not"},
	};
	uint32_t m[SLOT / 4] = {MAGIC};
	uint8_t body[136] = {3}; /* a CREATE of ril.isp, were it long enough */
	char long_name[129], shown[160];
	Mmal t;

	mmal_up(&t);
	arm_send(&t.l, MSG(DATA, A.arm, A.vc), 23, m);
	mmal_stop(&t, "a message of 23 bytes has no header");
	again(&t);
	m[0] = MAGIC + 1;
	arm_send(&t.l, MSG(DATA, A.arm, A.vc), 24, m);
	mmal_stop(&t, "a message without MMAL's magic (0x6c616d6e) is not "
	              "emulated");
	for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
		again(&t);
		mmal_send(&t, A, types[i].type, m, 0);
		mmal_stop(&t, types[i].message);
	}
	memcpy(body + 4, "ril.isp", 8);
	for (size_t i = 0; i < ARRAY_SIZE(shorts); i++) {
		again(&t);
		mmal_send(&t, A, shorts[i].type, body, shorts[i].size);
		mmal_stop(&t, shorts[i].message);
	}
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		again(&t);
		create(&t, A, names[i].name, names[i].length);
		mmal_stop(&t, names[i].message);
	}
	/* A name of 128 bytes, no NUL: the first 127 shown. */
	again(&t);
	memset(long_name, 'a', 128);
	long_name[128] = 0;
	create(&t, A, long_name, 128);
	snprintf(shown, sizeof(shown), "the component '%.127s' is not emulated",
	         long_name);
	mmal_stop(&t, shown);

	/* Handles that are not the client's. */
	static const uint32_t handles[] = {0, 2, 17, 0xffffffffu};
	for (size_t i = 0; i < ARRAY_SIZE(handles); i++) {
		char message[100];

		again(&t);
		create_ok(&t, A, "ril.video_decode", 1, 1, 1);
		destroy(&t, A, handles[i]);
		snprintf(message, sizeof(message),
		         "a COMPONENT_DESTROY of component %" PRIu32
		         ", which is not the client's, is not emulated",
		         handles[i]);
		mmal_stop(&t, message);
	}
	again(&t);
	mmal_open(&t, B);
	create_ok(&t, A, "ril.video_decode", 1, 1, 1);
	destroy(&t, B, 1);
	mmal_stop(&t, "a COMPONENT_DESTROY of component 1, which is not");
	again(&t);
	create_ok(&t, A, "ril.video_decode", 1, 1, 1);
	destroy_ok(&t, A, 1);
	destroy(&t, A, 1);
	mmal_stop(&t, "a COMPONENT_DESTROY of component 1, which is not");
	again(&t);
	info_get(&t, A, 1, CONTROL, 0);
	mmal_stop(&t, "a PORT_INFO_GET of component 1, which is not");
	again(&t);
	param_get(&t, A, 1, 0, ENCODINGS, 8);
	mmal_stop(&t, "a PORT_PARAMETER_GET of component 1, which is not");

	/* Ports the component has not. */
	static const struct {
		const char *name;
		uint32_t type, index;
		const char *message;
	} ports[] = {
		{"ril.video_decode", CONTROL, 1,
	     "a PORT_INFO_GET of port 1 of type 1, which ril.video_decode "
	     "has not, is not emulated"},
		{"ril.video_decode", INPUT, 1, "port 1 of type 2, which"},
		{"ril.video_decode", OUTPUT, 1, "port 1 of type 3, which"},
		{"ril.isp", OUTPUT, 3, "port 3 of type 3, which ril.isp"},
		{"ril.video_decode", CLOCK, 0, "port 0 of type 4, which"},
		{"ril.video_decode", 0, 0, "port 0 of type 0, which"},
		{"camera_info", INPUT, 0, "port 0 of type 2, which camera_info"},
		{"camera_info", OUTPUT, 0, "port 0 of type 3, which camera_info"},
	};
	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		again(&t);
		create(&t, A, ports[i].name, strlen(ports[i].name) + 1);
		mmal_reply(&t, A, CREATE, m, 20);
		info_get(&t, A, 1, ports[i].type, ports[i].index);
		mmal_stop(&t, ports[i].message);
	}
	again(&t);
	create_ok(&t, A, "ril.isp", 1, 1, 3);
	info_get(&t, A, 1, OUTPUT, 2);
	check_info(&t, A, 1, OUTPUT, 2, 4, VIDEO, isp_stats[0]);

	/* Parameters. */
	static const struct {
		const char *name;
		uint32_t port, id, size;
		const char *message;
	} params[] = {
		{"ril.video_decode", 3, ENCODINGS, 8 + ROOM,
	     "a PORT_PARAMETER_GET of port 3, which ril.video_decode has "
	     "not, is not emulated"},
		{"ril.isp", 5, ENCODINGS, 8 + ROOM, "of port 5, which ril.isp"},
		{"camera_info", 1, CAMERA_INFO, 8 + 144,
	     "of port 1, which camera_info"},
		{"ril.video_decode", 1, ENCODINGS, 7,
	     "a PORT_PARAMETER_GET of 7 bytes is not emulated"},
		{"ril.video_decode", 1, ENCODINGS, 8 + 385,
	     "a PORT_PARAMETER_GET of 393 bytes is not emulated"},
		{"ril.video_decode", 1, 2, 8 + ROOM,
	     "the parameter 0x2 of port 1 of ril.video_decode is not "
	     "emulated"},
		{"ril.video_decode", 0, ENCODINGS, 8 + ROOM,
	     "the parameter 0x1 of port 0 of ril.video_decode"},
		{"ril.video_decode", 0, CAMERA_INFO, 8 + 144,
	     "the parameter 0x1001c of port 0 of ril.video_decode"},
		{"ril.video_decode", 1, CAMERA_INFO, 8 + 144,
	     "the parameter 0x1001c of port 1 of ril.video_decode"},
		{"camera_info", 0, ENCODINGS, 8 + ROOM,
	     "the parameter 0x1 of port 0 of camera_info"},
	};
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		again(&t);
		create(&t, A, params[i].name, strlen(params[i].name) + 1);
		mmal_reply(&t, A, CREATE, m, 20);
		param_get(&t, A, 1, params[i].port, params[i].id, params[i].size);
		mmal_stop(&t, params[i].message);
	}
	again(&t);
	create_ok(&t, A, "ril.isp", 1, 1, 3);
	param_get(&t, A, 1, 4, ENCODINGS, 8 + ROOM);
	check_param(&t, A, SUCCESS, ENCODINGS, 4, isp_stats, 4);
	mmal_down(&t);
}

/*
 * The VideoCore is version 15 from 10: Linux's 10 to 15 open it, and
 * neither 9 nor an Arm that needs 16.
 */
static void test_versions(void)
{
	static const struct {
		uint32_t version, min;
		bool opens;
	} arms[] = {{10, 10, true}, {9, 9, false}, {16, 15, true}, {16, 16, false}};
	Mmal t;

	for (size_t i = 0; i < ARRAY_SIZE(arms); i++) {
		uint32_t payload[3] = {MMAL, 0, arms[i].min << 16 | arms[i].version};

		mmal_up(&t);
		arm_send(&t.l, MSG(OPEN, 9, 0), 12, payload);
		if (arms[i].opens) {
			CHECK_EQ(vc_message(&t.l, &(uint32_t){0}, NULL),
			         MSG(OPENACK, 2, 9));
			CHECK_EQ(t.l.warnings, 0);
		} else {
			CHECK_EQ(vc_header(&t.l), MSG(CLOSE, 0, 9));
			CHECK_EQ(t.l.warnings, 1);
			t.l.warnings = 0;
		}
		mmal_down(&t);
	}
}

/* With no room among the VideoCore's services, there is no MMAL. */
static void test_create(void)
{
	Link l;

	link_up(&l, false);
	for (uint32_t i = 0; i < 8; i++)
		CHECK(raspi3b_vchiq_add_service(
			l.s.v,
			&(Raspi3bVchiqService){i, 1, 1, NULL, NULL, NULL, NULL, NULL}));
	CHECK(raspi3b_mmal_create(l.s.m, l.s.v) == NULL);
	link_down(&l);
}

int main(void)
{
	RUN_TEST(test_codec);
	RUN_TEST(test_isp_camera);
	RUN_TEST(test_room);
	RUN_TEST(test_handles);
	RUN_TEST(test_stops);
	RUN_TEST(test_versions);
	RUN_TEST(test_create);
	return test_summary();
}
