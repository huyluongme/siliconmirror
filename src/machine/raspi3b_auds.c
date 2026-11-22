/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's audio service; see raspi3b_auds.h. The messages are
 * those of vc_vchi_audioserv_defs.h, Raspberry Pi's Linux at cff533a,
 * whose bcm2835-vchiq.c and bcm2835-pcm.c show what Linux does with the
 * answers.
 */

#include "machine/raspi3b_auds.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/event.h"
#include "util/byteorder.h"
#include "util/macros.h"
#include "util/muldiv.h"

#define FOURCC 0x41554453u /* 'AUDS' */
#define VERSION 2          /* VC_AUDIOSERV_VER */
#define VERSION_MIN 1      /* VC_AUDIOSERV_MIN_VER */

/* enum vc_audio_msg_type */
enum {
	TYPE_RESULT,
	TYPE_COMPLETE,
	TYPE_CONFIG,
	TYPE_CONTROL,
	TYPE_OPEN,
	TYPE_CLOSE,
	TYPE_START,
	TYPE_STOP,
	TYPE_WRITE,
};

#define MSG_SIZE 20u /* struct vc_audio_msg: the type, vc_audio_write's 16 */
#define BODY 4u      /* where the union starts */
#define FINISHED (1u << 30) /* COMPLETE: all that was given is played */

#define BLOCK_NS 10000000u /* the VideoCore plays 10 ms at a time */
#define FIFO_SIZE 0x80000u /* 512 KB a stream: Linux's buffer_bytes_max */
#define STREAMS 32         /* one a port of the VideoCore's, 1 to 32 */
#define CHANNELS_MAX 8     /* Linux's channels_max */
#define RATE_MIN 8000u     /* Linux's rate_min and rate_max */
#define RATE_MAX 192000u
#define VOLUME_MIN (-1024) /* +4 dB: Linux's CTRL_VOL_MAX, 400, alsa2chip */
#define VOLUME_MAX 26214   /* CHIP_MIN_VOLUME, which mutes */
#define DEST_HEADPHONES 1u /* enum snd_bcm2835_route; 0 is auto */
#define DEST_MAX 3u        /* AUDIO_DEST_HDMI1 */
#define SCRATCH 4096u      /* bytes passed to the output at a time */

typedef struct Stream {
	Raspi3bAuds *auds;
	unsigned port;
	bool opened;     /* the Arm's OPEN came, and no CLOSE since */
	bool configured; /* a CONFIG came */
	AudioFormat format;
	double gain; /* what the volume multiplies a sample by */
	uint32_t dest;
	bool playing;
	uint64_t started; /* when START came */
	uint64_t frames;  /* the frames whose time has come since */
	Event block;      /* the next 10 ms */
	uint8_t *fifo;    /* what was written and not played */
	uint32_t head, len;
	uint32_t expect; /* bytes of the last WRITE still to come */
	uint32_t max_packet;
	uint32_t cookie1, cookie2;
} Stream;

struct Raspi3bAuds {
	Machine *machine;
	Raspi3bVchiq *vchiq;
	AudioOut *out;
	Stream *owner; /* the stream that plays to out */
	Stream streams[STREAMS];
	uint8_t scratch[SCRATCH];
};

static void stop(Raspi3bAuds *a, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void stop(Raspi3bAuds *a, const char *fmt, ...)
{
	char why[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	machine_stop(a->machine, MACHINE_STOP_ERROR, "firmware: AUDS: %s", why);
}

/* A message of the VideoCore's: a type, three words, zeros to 20 bytes. */
static bool send(Stream *s, uint32_t type, uint32_t w1, uint32_t w2,
                 uint32_t w3)
{
	uint8_t m[MSG_SIZE] = {0};

	store_le32(m, type);
	store_le32(m + 4, w1);
	store_le32(m + 8, w2);
	store_le32(m + 12, w3);
	return raspi3b_vchiq_send(s->auds->vchiq, s->port, m, sizeof(m));
}

static bool result(Stream *s)
{
	return send(s, TYPE_RESULT, 0, 0, 0);
}

static bool output_failed(Raspi3bAuds *a)
{
	stop(a, "the host's audio output: %s", a->out->error);
	return false;
}

/* Whether the stream's sound goes to the output: the headphones. */
static bool heard(const Stream *s)
{
	return s->auds->out != NULL && s->dest <= DEST_HEADPHONES;
}

/* The stream's sound stops going to the output. */
static bool release_output(Stream *s)
{
	Raspi3bAuds *a = s->auds;

	if (a->owner != s)
		return true;
	a->owner = NULL;
	return a->out->ops->stop(a->out) || output_failed(a);
}

static bool end_play(Stream *s)
{
	event_cancel(&s->auds->machine->events, &s->block);
	s->playing = false;
	return release_output(s);
}

static void drop(Stream *s)
{
	s->len = 0;
}

/* x times gain, rounded to the nearest, within lo and hi. */
static int32_t scale(int32_t x, double gain, int32_t lo, int32_t hi)
{
	double v = x * gain;
	int32_t r = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);

	return r < lo ? lo : r > hi ? hi : r;
}

/* At 0 dB, a gain of 1, each sample stays as it is. */
static void apply_volume(const Stream *s, uint8_t *p, uint32_t n)
{
	if (s->format.bits == 8) {
		for (uint32_t i = 0; i < n; i++)
			p[i] = (uint8_t)(scale(p[i] - 128, s->gain, -128, 127) + 128);
		return;
	}
	for (uint32_t i = 0; i < n; i += 2)
		store_le16(p + i, (uint16_t)scale((int16_t)load_le16(p + i), s->gain,
		                                  INT16_MIN, INT16_MAX));
}

/* Play bytes, whole frames, from the stream's FIFO. */
static bool play(Stream *s, uint32_t bytes)
{
	Raspi3bAuds *a = s->auds;
	uint32_t frame = (uint32_t)audio_frame_size(&s->format);
	bool to_out = heard(s);

	if (to_out && a->owner != s) {
		if (a->owner != NULL) {
			stop(a, "two streams playing to the headphones at once "
			        "(mixing) is not emulated");
			return false;
		}
		if (!a->out->ops->start(a->out, &s->format))
			return output_failed(a);
		a->owner = s;
	}
	while (bytes > 0) {
		uint32_t n = SCRATCH - SCRATCH % frame;
		uint32_t first;

		if (n > bytes)
			n = bytes;
		first = FIFO_SIZE - s->head < n ? FIFO_SIZE - s->head : n;
		memcpy(a->scratch, s->fifo + s->head, first);
		memcpy(a->scratch + first, s->fifo, n - first);
		s->head = (s->head + n) % FIFO_SIZE;
		s->len -= n;
		bytes -= n;
		if (!to_out)
			continue;
		apply_volume(s, a->scratch, n);
		if (!a->out->ops->play(a->out, a->scratch, n))
			return output_failed(a);
	}
	return true;
}

/*
 * The next 10 ms: the frames whose time has come, as many as the FIFO
 * holds, then their COMPLETE. With nothing left, the stream has played
 * all it was given.
 */
static void play_block(void *ctx)
{
	Stream *s = ctx;
	Machine *m = s->auds->machine;
	uint64_t now = machine_now(m);
	uint32_t frame = (uint32_t)audio_frame_size(&s->format);
	uint64_t due = mul_div_floor_ns(now - s->started, s->format.rate);
	uint64_t frames = due - s->frames;
	uint32_t bytes;

	s->frames = due;
	if (s->len < frame) {
		drop(s);
		if (end_play(s))
			send(s, TYPE_COMPLETE, FINISHED, s->cookie1, s->cookie2);
		return;
	}
	if (frames > s->len / frame)
		frames = s->len / frame;
	bytes = (uint32_t)frames * frame;
	if (play(s, bytes) && send(s, TYPE_COMPLETE, bytes, s->cookie1, s->cookie2))
		event_schedule(&m->events, &s->block, now + BLOCK_NS);
}

static bool msg_open(Stream *s)
{
	if (s->opened) {
		stop(s->auds, "a second OPEN is not emulated");
		return false;
	}
	if (s->fifo == NULL && (s->fifo = malloc(FIFO_SIZE)) == NULL) {
		stop(s->auds, "out of memory for a stream");
		return false;
	}
	s->opened = true;
	return true;
}

/* struct vc_audio_config */
static bool msg_config(Stream *s, const uint8_t *b)
{
	uint32_t channels = load_le32(b);
	uint32_t rate = load_le32(b + 4);
	uint32_t bits = load_le32(b + 8);

	if (s->playing) {
		stop(s->auds, "a CONFIG while the stream plays is not emulated");
		return false;
	}
	if (channels < 1 || channels > CHANNELS_MAX || rate < RATE_MIN ||
	    rate > RATE_MAX || (bits != 8 && bits != 16)) {
		stop(s->auds,
		     "a CONFIG of %" PRIu32 " channels, %" PRIu32 " Hz and %" PRIu32
		     " bits is not emulated",
		     channels, rate, bits);
		return false;
	}
	drop(s);
	s->format.channels = channels;
	s->format.rate = rate;
	s->format.bits = bits;
	s->configured = true;
	return result(s);
}

/* struct vc_audio_control */
static bool msg_control(Stream *s, const uint8_t *b)
{
	int32_t volume = (int32_t)load_le32(b);
	uint32_t dest = load_le32(b + 4);

	if (volume < VOLUME_MIN || volume > VOLUME_MAX || dest > DEST_MAX) {
		stop(s->auds,
		     "a CONTROL of volume %" PRId32 " and destination %" PRIu32
		     " is not emulated",
		     volume, dest);
		return false;
	}
	s->gain = pow(10.0, -volume / 5120.0); /* 256ths of dB, 20 dB a decade */
	s->dest = dest;
	if (!heard(s) && !release_output(s))
		return false;
	return result(s);
}

static bool msg_start(Stream *s)
{
	Machine *m = s->auds->machine;

	if (!s->configured || s->playing) {
		stop(s->auds, "a START %s is not emulated",
		     s->playing ? "while the stream plays" : "before CONFIG");
		return false;
	}
	s->playing = true;
	s->started = machine_now(m);
	s->frames = 0;
	event_schedule(&m->events, &s->block, s->started + BLOCK_NS);
	return true;
}

/* struct vc_audio_stop: draining plays on to the end of what was given. */
static bool msg_stop(Stream *s, const uint8_t *b)
{
	if (load_le32(b) != 0)
		return true;
	drop(s);
	return end_play(s);
}

/* struct vc_audio_write; its silence is not used. */
static bool msg_write(Stream *s, const uint8_t *b)
{
	uint32_t count = load_le32(b);
	uint16_t max_packet = load_le16(b + 14);

	if (!s->configured) {
		stop(s->auds, "a WRITE before CONFIG is not emulated");
		return false;
	}
	if (max_packet == 0) {
		stop(s->auds, "a WRITE by bulk transfer is not emulated");
		return false;
	}
	if (count > FIFO_SIZE - s->len) {
		stop(s->auds, "more than 512 KB of samples waiting to play is not "
		              "emulated");
		return false;
	}
	s->expect = count;
	s->max_packet = max_packet;
	s->cookie1 = load_le32(b + 4);
	s->cookie2 = load_le32(b + 8);
	return true;
}

/* DATA after a WRITE: samples, at the FIFO's end. */
static bool samples(Stream *s, const uint8_t *data, uint32_t size)
{
	uint32_t tail = (s->head + s->len) % FIFO_SIZE;
	uint32_t first = FIFO_SIZE - tail < size ? FIFO_SIZE - tail : size;

	if (size > s->max_packet || size > s->expect) {
		stop(s->auds,
		     "DATA of %" PRIu32 " bytes, more than max_packet (%" PRIu32
		     ") or the %" PRIu32 " left of its WRITE, is not emulated",
		     size, s->max_packet, s->expect);
		return false;
	}
	memcpy(s->fifo + tail, data, first);
	memcpy(s->fifo, data + first, size - first);
	s->len += size;
	s->expect -= size;
	return true;
}

/*
 * As the stream was before the Arm's OPEN; what it holds is dropped by
 * the CONFIG it needs before START or WRITE.
 */
static void forget(Stream *s)
{
	end_play(s); /* an output that fails has stopped the machine */
	s->opened = false;
	s->configured = false;
	s->gain = 1.0;
	s->dest = 0;
	s->expect = 0;
	s->cookie1 = 0;
	s->cookie2 = 0;
}

static bool msg_close(Stream *s)
{
	forget(s);
	return result(s);
}

static bool data(void *ctx, unsigned port, const uint8_t *msg, uint32_t size)
{
	Raspi3bAuds *a = ctx;
	Stream *s = &a->streams[port - 1];
	uint32_t type;

	if (s->expect > 0)
		return samples(s, msg, size);
	if (size < MSG_SIZE) {
		stop(a,
		     "a message of %" PRIu32 " bytes, not a vc_audio_msg, is not "
		     "emulated",
		     size);
		return false;
	}
	type = load_le32(msg);
	if (!s->opened && type != TYPE_OPEN) {
		stop(a, "a message of type %" PRIu32 " before OPEN is not emulated",
		     type);
		return false;
	}
	switch (type) {
	case TYPE_OPEN:
		return msg_open(s);
	case TYPE_CLOSE:
		return msg_close(s);
	case TYPE_CONFIG:
		return msg_config(s, msg + BODY);
	case TYPE_CONTROL:
		return msg_control(s, msg + BODY);
	case TYPE_START:
		return msg_start(s);
	case TYPE_STOP:
		return msg_stop(s, msg + BODY);
	case TYPE_WRITE:
		return msg_write(s, msg + BODY);
	default:
		stop(a, "the Arm's message of type %" PRIu32 " is not emulated", type);
		return false;
	}
}

/* The Arm closed the service, or the firmware started again. */
static void service_close(void *ctx, unsigned port)
{
	Raspi3bAuds *a = ctx;
	Stream *s = &a->streams[port - 1];

	forget(s);
	free(s->fifo);
	s->fifo = NULL;
}

Raspi3bAuds *raspi3b_auds_create(Machine *m, Raspi3bVchiq *v)
{
	Raspi3bAuds *a = calloc(1, sizeof(*a));
	Raspi3bVchiqService service = {
		.fourcc = FOURCC,
		.version = VERSION,
		.version_min = VERSION_MIN,
		.data = data,
		.close = service_close,
	};

	if (a == NULL)
		return NULL;
	a->machine = m;
	a->vchiq = v;
	for (unsigned i = 0; i < STREAMS; i++) {
		Stream *s = &a->streams[i];

		s->auds = a;
		s->port = i + 1;
		s->gain = 1.0;
		event_init(&s->block, play_block, s);
	}
	service.ctx = a;
	if (!raspi3b_vchiq_add_service(v, &service)) {
		free(a);
		return NULL;
	}
	return a;
}

void raspi3b_auds_destroy(Raspi3bAuds *a)
{
	for (unsigned i = 0; i < STREAMS; i++) {
		event_cancel(&a->machine->events, &a->streams[i].block);
		free(a->streams[i].fifo);
	}
	free(a);
}

void raspi3b_auds_set_output(Raspi3bAuds *a, AudioOut *out)
{
	a->out = out;
}
