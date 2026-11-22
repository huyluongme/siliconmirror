/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "machine/raspi3b_mmal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"
#include "util/macros.h"

#define SERVICE 0x6d6d616cu /* 'mmal', VCHIQ's fourcc */
#define VERSION 15          /* VC_MMAL_VER */
#define VERSION_MIN 10      /* VC_MMAL_MIN_VER */

/* MMAL_FOURCC: the first character in bits 7:0. */
#define FOURCC(a, b, c, d)                                                     \
	((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 |                \
	 (uint32_t)(d) << 24)
#define MAGIC FOURCC('m', 'm', 'a', 'l')

/* enum mmal_msg_type of mmal-msg.h. */
enum {
	TYPE_COMPONENT_CREATE = 4,
	TYPE_COMPONENT_DESTROY = 5,
	TYPE_PORT_INFO_GET = 8,
	TYPE_PORT_PARAMETER_GET = 15,
	TYPE_LAST = 27, /* MMAL_MSG_TYPE_MSG_LAST */
};

static const char *const type_names[TYPE_LAST] = {
	[1] = "QUIT",
	[2] = "SERVICE_CLOSED",
	[3] = "GET_VERSION",
	[6] = "COMPONENT_ENABLE",
	[7] = "COMPONENT_DISABLE",
	[9] = "PORT_INFO_SET",
	[10] = "PORT_ACTION",
	[11] = "BUFFER_FROM_HOST",
	[12] = "BUFFER_TO_HOST",
	[13] = "GET_STATS",
	[14] = "PORT_PARAMETER_SET",
	[16] = "EVENT_TO_HOST",
	[17] = "GET_CORE_STATS_FOR_PORT",
	[18] = "OPAQUE_ALLOCATOR",
	[19] = "CONSUME_MEM",
	[20] = "LMK",
	[21] = "OPAQUE_ALLOCATOR_DESC",
	[22] = "DRM_GET_LHS32",
	[23] = "DRM_GET_TIME",
	[24] = "BUFFER_FROM_HOST_ZEROLEN",
	[25] = "PORT_FLUSH",
	[26] = "HOST_LOG",
};

/* struct mmal_msg_header: magic, type, control_service, context, status. */
#define HEADER 24u
#define HEADER_TYPE 4u
#define HEADER_STATUS 16u

/* The Arm's bodies, and the replies'. */
#define CREATE_SIZE 136u /* client_component, name[128], pid */
#define CREATE_NAME 4u
#define NAME_SIZE 128u
#define DESTROY_SIZE 4u  /* component_handle */
#define INFO_SIZE 12u    /* component_handle, port_type, index */
#define PARAM_SIZE 16u   /* component_handle, port_handle, id, size */
#define CREATE_REPLY 20u /* status, handle, input, output, clock_num */
#define DESTROY_REPLY 4u /* status */
#define INFO_REPLY 292u  /* struct mmal_msg_port_info_get_reply */
#define PARAM_REPLY 12u  /* status, id, size; then the value */
#define PARAM_SPACE 384u /* MMAL_WORKER_PORT_PARAMETER_SPACE words */
#define PARAM_HEADER 8u  /* a parameter's id and size, in its size */

/* port_info_get_reply: the mmal_port, the mmal_es_format. */
#define INFO_PORT 24u
#define INFO_FORMAT 88u

enum { STATUS_SUCCESS = 0, STATUS_ENOSPC = 2 };     /* enum mmal_msg_status */
enum { PORT_CONTROL = 1, PORT_INPUT, PORT_OUTPUT }; /* enum mmal_port_type */
#define ES_VIDEO 3u                                 /* MMAL_ES_TYPE_VIDEO */

/* mmal-parameters.h */
#define PARAMETER_SUPPORTED_ENCODINGS 0x1u
#define PARAMETER_CAMERA_INFO 0x1001cu
#define CAMERA_INFO_SIZE 144u /* struct mmal_parameter_camera_info */

/* mmal-encodings.h */
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
#define RGB16 FOURCC('R', 'G', 'B', '2')
#define RGB24 FOURCC('R', 'G', 'B', '3')
#define BGR24 FOURCC('B', 'G', 'R', '3')
#define RGBA FOURCC('R', 'G', 'B', 'A')
#define BGRA FOURCC('B', 'G', 'R', 'A')
#define SAND FOURCC('S', 'A', 'N', 'D')  /* YUVUV128 */
#define STATS FOURCC('S', 'T', 'A', 'T') /* BRCM_STATS */

typedef struct Encodings {
	const uint32_t *list;
	unsigned count;
} Encodings;

#define ENCODINGS(a) {a, ARRAY_SIZE(a)}

static const uint32_t decode_in[] = {MP4V, H264, H263, MJPEG, VP8};
static const uint32_t decode_out[] = {I420, YV12, NV12, NV21, RGB16};
static const uint32_t encode_in[] = {I420,  YV12, NV12, NV21, RGB16, RGB24,
                                     BGR24, BGRA, YUYV, YVYU, UYVY,  VYUY};
static const uint32_t encode_out[] = {H264, MJPEG};
static const uint32_t isp_in[] = {
	/* Bayer: 8 bits, 10, 12 and 14 packed, 16; BGGR, RGGB, GBRG, GRBG */
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
	/* grey: 8 bits, 10, 12 and 14 packed, 16 */
	FOURCC('G', 'R', 'E', 'Y'),
	FOURCC('Y', '1', '0', 'P'),
	FOURCC('Y', '1', '2', 'P'),
	FOURCC('Y', '1', '4', 'P'),
	FOURCC('Y', '1', '6', ' '),
	/* YUV and RGB */
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
	RGB16,
	NV12,
	NV21,
	/* Bayer of 10, 12 and 14 bits unpacked, then grey */
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
static const uint32_t isp_out0[] = {YUYV,  YVYU, VYUY, UYVY,  I420, YV12, RGB24,
                                    BGR24, RGBA, BGRA, RGB16, NV12, NV21};
static const uint32_t isp_out1[] = {YUYV, YVYU, VYUY, UYVY,
                                    I420, YV12, NV12, NV21};
static const uint32_t isp_stats[] = {STATS};
static const uint32_t fx[] = {I420, SAND};
static const uint32_t image_encode_out[] = {JPEG};

#define PORTS_MAX 4 /* ril.isp's input and three outputs */

/* A component the firmware has: its inputs, then its outputs. */
typedef struct Kind {
	const char *name;
	unsigned inputs, outputs;
	Encodings ports[PORTS_MAX];
	bool camera_info;
} Kind;

static const Kind kinds[] = {
	{.name = "ril.video_decode",
     .inputs = 1,
     .outputs = 1,
     .ports = {ENCODINGS(decode_in), ENCODINGS(decode_out)}},
	{.name = "ril.video_encode",
     .inputs = 1,
     .outputs = 1,
     .ports = {ENCODINGS(encode_in), ENCODINGS(encode_out)}},
	{.name = "ril.isp",
     .inputs = 1,
     .outputs = 3,
     .ports = {ENCODINGS(isp_in), ENCODINGS(isp_out0), ENCODINGS(isp_out1),
               ENCODINGS(isp_stats)}},
	{.name = "ril.image_fx",
     .inputs = 1,
     .outputs = 1,
     .ports = {ENCODINGS(fx), ENCODINGS(fx)}},
	{.name = "ril.image_encode",
     .inputs = 1,
     .outputs = 1,
     .ports = {ENCODINGS(encode_in), ENCODINGS(image_encode_out)}},
	{.name = "camera_info", .camera_info = true},
};

#define COMPONENTS 16

typedef struct Component {
	const Kind *kind; /* NULL: free */
	unsigned client;  /* the VideoCore's port of the client's service */
} Component;

struct Raspi3bMmal {
	Machine *machine;
	Raspi3bVchiq *vchiq;
	Component components[COMPONENTS]; /* handle i + 1 */
};

static void stop(Raspi3bMmal *s, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void stop(Raspi3bMmal *s, const char *fmt, ...)
{
	char why[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	machine_stop(s->machine, MACHINE_STOP_ERROR, "firmware: MMAL: %s", why);
}

/* Whether the message, size bytes, has a body of at least need. */
static bool long_enough(Raspi3bMmal *s, const char *type, uint32_t size,
                        uint32_t need)
{
	if (size >= HEADER + need)
		return true;
	stop(s, "a %s of %" PRIu32 " bytes is too short", type, size);
	return false;
}

/*
 * The reply to the Arm's message msg: its header, the status 0, then
 * body_size bytes of body.
 */
static bool answer(Raspi3bMmal *s, unsigned port, const uint8_t *msg,
                   const uint8_t *body, uint32_t body_size)
{
	uint8_t reply[HEADER + PARAM_REPLY + PARAM_SPACE]; /* the largest */

	memcpy(reply, msg, HEADER);
	store_le32(reply + HEADER_STATUS, 0);
	memcpy(reply + HEADER, body, body_size);
	return raspi3b_vchiq_send(s->vchiq, port, reply, HEADER + body_size);
}

/* The client's component of handle, or NULL when it stopped. */
static Component *component_of(Raspi3bMmal *s, unsigned port, const char *type,
                               uint32_t handle)
{
	if (handle >= 1 && handle <= COMPONENTS) {
		Component *c = &s->components[handle - 1];

		if (c->kind != NULL && c->client == port)
			return c;
	}
	stop(s,
	     "a %s of component %" PRIu32 ", which is not the client's, is not "
	     "emulated",
	     type, handle);
	return NULL;
}

static bool create(Raspi3bMmal *s, unsigned port, const uint8_t *msg)
{
	const uint8_t *name = msg + HEADER + CREATE_NAME;
	char shown[NAME_SIZE] = {0}; /* the last stays NUL */
	uint8_t body[CREATE_REPLY] = {0};
	const Kind *kind = NULL;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(kinds); i++)
		if (memcmp(name, kinds[i].name, strlen(kinds[i].name) + 1) == 0)
			kind = &kinds[i];
	if (kind == NULL) {
		for (i = 0; i < NAME_SIZE - 1 && name[i] != 0; i++)
			shown[i] = name[i] >= 0x20 && name[i] < 0x7f ? (char)name[i] : '?';
		stop(s, "the component '%s' is not emulated", shown);
		return false;
	}
	for (i = 0; i < COMPONENTS; i++) {
		Component *c = &s->components[i];

		if (c->kind != NULL)
			continue;
		c->kind = kind;
		c->client = port;
		store_le32(body + 4, i + 1);
		store_le32(body + 8, kind->inputs);
		store_le32(body + 12, kind->outputs);
		return answer(s, port, msg, body, sizeof(body)); /* no clocks */
	}
	stop(s, "more than 16 components is not emulated");
	return false;
}

static bool destroy(Raspi3bMmal *s, unsigned port, const uint8_t *msg)
{
	Component *c =
		component_of(s, port, "COMPONENT_DESTROY", load_le32(msg + HEADER));
	uint8_t body[DESTROY_REPLY] = {0};

	if (c == NULL)
		return false;
	c->kind = NULL;
	return answer(s, port, msg, body, sizeof(body));
}

/*
 * The encodings of the component's port of handle, its index among all
 * the ports; NULL for the control port.
 */
static const Encodings *encodings_of(const Kind *kind, uint32_t handle)
{
	return handle == 0 ? NULL : &kind->ports[handle - 1];
}

static bool info_get(Raspi3bMmal *s, unsigned port, const uint8_t *msg)
{
	uint32_t handle = load_le32(msg + HEADER);
	uint32_t type = load_le32(msg + HEADER + 4);
	uint32_t index = load_le32(msg + HEADER + 8);
	Component *c = component_of(s, port, "PORT_INFO_GET", handle);
	uint8_t body[INFO_REPLY] = {0};
	uint8_t *p = body + INFO_PORT, *f = body + INFO_FORMAT;
	const Encodings *e;
	uint32_t all;

	if (c == NULL)
		return false;
	if (type == PORT_CONTROL && index == 0)
		all = 0;
	else if (type == PORT_INPUT && index < c->kind->inputs)
		all = 1 + index;
	else if (type == PORT_OUTPUT && index < c->kind->outputs)
		all = 1 + c->kind->inputs + index;
	else {
		stop(s,
		     "a PORT_INFO_GET of port %" PRIu32 " of type %" PRIu32
		     ", which %s has not, is not emulated",
		     index, type, c->kind->name);
		return false;
	}
	e = encodings_of(c->kind, all);
	store_le32(body + 4, handle);
	store_le32(body + 8, type);
	store_le32(body + 12, index);
	store_le32(body + 16, 1); /* found */
	store_le32(body + 20, all);
	/* struct mmal_port */
	store_le32(p + 8, type);
	store_le16(p + 12, (uint16_t)index);
	store_le16(p + 14, (uint16_t)all);
	store_le32(p + 24, 1); /* buffer_num_min */
	store_le32(p + 36, 1); /* buffer_num_recommended */
	store_le32(p + 44, 1); /* buffer_num */
	/* struct mmal_es_format */
	if (e != NULL) {
		store_le32(f, ES_VIDEO);
		store_le32(f + 4, e->list[0]);
	}
	return answer(s, port, msg, body, sizeof(body));
}

static bool parameter_get(Raspi3bMmal *s, unsigned port, const uint8_t *msg)
{
	uint32_t port_handle = load_le32(msg + HEADER + 4);
	uint32_t id = load_le32(msg + HEADER + 8);
	uint32_t size = load_le32(msg + HEADER + 12);
	Component *c =
		component_of(s, port, "PORT_PARAMETER_GET", load_le32(msg + HEADER));
	uint8_t body[PARAM_REPLY + PARAM_SPACE] = {0};
	const Encodings *e;
	uint32_t need, room;

	if (c == NULL)
		return false;
	if (port_handle > c->kind->inputs + c->kind->outputs) {
		stop(s,
		     "a PORT_PARAMETER_GET of port %" PRIu32
		     ", which %s has not, is not emulated",
		     port_handle, c->kind->name);
		return false;
	}
	if (size < PARAM_HEADER || size > PARAM_HEADER + PARAM_SPACE) {
		stop(s, "a PORT_PARAMETER_GET of %" PRIu32 " bytes is not emulated",
		     size);
		return false;
	}
	e = encodings_of(c->kind, port_handle);
	if (id == PARAMETER_SUPPORTED_ENCODINGS && e != NULL) {
		need = 4 * e->count;
		for (unsigned i = 0; i < e->count; i++)
			store_le32(body + PARAM_REPLY + 4 * i, e->list[i]);
	} else if (id == PARAMETER_CAMERA_INFO && c->kind->camera_info) {
		need = CAMERA_INFO_SIZE; /* no camera, no flash: all 0 */
	} else {
		stop(s,
		     "the parameter 0x%" PRIx32 " of port %" PRIu32
		     " of %s is not emulated",
		     id, port_handle, c->kind->name);
		return false;
	}
	room = size - PARAM_HEADER;
	store_le32(body, need <= room ? STATUS_SUCCESS : STATUS_ENOSPC);
	store_le32(body + 4, id);
	store_le32(body + 8, PARAM_HEADER + need);
	return answer(s, port, msg, body,
	              PARAM_REPLY + (need <= room ? need : room));
}

static bool data(void *ctx, unsigned port, const uint8_t *msg, uint32_t size)
{
	Raspi3bMmal *s = ctx;
	uint32_t type;

	if (size < HEADER) {
		stop(s, "a message of %" PRIu32 " bytes has no header", size);
		return false;
	}
	if (load_le32(msg) != MAGIC) {
		stop(s,
		     "a message without MMAL's magic (0x%08" PRIx32 ") is not "
		     "emulated",
		     load_le32(msg));
		return false;
	}
	type = load_le32(msg + HEADER_TYPE);
	switch (type) {
	case TYPE_COMPONENT_CREATE:
		return long_enough(s, "COMPONENT_CREATE", size, CREATE_SIZE) &&
		       create(s, port, msg);
	case TYPE_COMPONENT_DESTROY:
		return long_enough(s, "COMPONENT_DESTROY", size, DESTROY_SIZE) &&
		       destroy(s, port, msg);
	case TYPE_PORT_INFO_GET:
		return long_enough(s, "PORT_INFO_GET", size, INFO_SIZE) &&
		       info_get(s, port, msg);
	case TYPE_PORT_PARAMETER_GET:
		return long_enough(s, "PORT_PARAMETER_GET", size, PARAM_SIZE) &&
		       parameter_get(s, port, msg);
	default:
		if (type < TYPE_LAST && type_names[type] != NULL)
			stop(s, "the Arm's %s (%" PRIu32 ") is not emulated",
			     type_names[type], type);
		else
			stop(s, "the Arm's message of type %" PRIu32 " is not emulated",
			     type);
		return false;
	}
}

/*
 * The Arm closed the client's service, or the firmware started again
 * (which closes them all): its components go.
 */
static void close_client(void *ctx, unsigned port)
{
	Raspi3bMmal *s = ctx;

	for (unsigned i = 0; i < COMPONENTS; i++)
		if (s->components[i].client == port)
			s->components[i].kind = NULL;
}

Raspi3bMmal *raspi3b_mmal_create(Machine *m, Raspi3bVchiq *v)
{
	Raspi3bMmal *s = calloc(1, sizeof(*s));
	Raspi3bVchiqService service = {
		.fourcc = SERVICE,
		.version = VERSION,
		.version_min = VERSION_MIN,
		.data = data,
		.close = close_client,
	};

	if (s == NULL)
		return NULL;
	s->machine = m;
	s->vchiq = v;
	service.ctx = s;
	if (!raspi3b_vchiq_add_service(v, &service)) {
		free(s);
		return NULL;
	}
	return s;
}

void raspi3b_mmal_destroy(Raspi3bMmal *s)
{
	free(s);
}
