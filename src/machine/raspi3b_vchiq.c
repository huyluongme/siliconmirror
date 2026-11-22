/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The VideoCore's end of VCHIQ; see raspi3b_vchiq.h. */

#include "machine/raspi3b_vchiq.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bus.h"
#include "core/event.h"
#include "util/byteorder.h"
#include "util/macros.h"

/* The VideoCore's bus address of the RAM: the alias in bits 31:30 goes. */
#define BUS_ALIAS_MASK 0x3fffffffu

/* vchiq_cfg.h and vchiq_core.h of Raspberry Pi's Linux. */
#define MAGIC 0x56434849u /* 'VCHI' */
#define VERSION 8         /* the VideoCore's, smirror's choice */
#define VERSION_MIN 3
#define SLOT_SIZE 4096u
#define SLOT_MASK (SLOT_SIZE - 1)
#define MAX_SLOTS 128
#define MAX_SLOTS_PER_SIDE 64
#define SLOT_QUEUE_MASK (MAX_SLOTS_PER_SIDE - 1)
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
	Z_MASTER = 32, /* the VideoCore's half */
	Z_SLAVE = 404, /* the Arm's */
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

/* A message: its header, msgid then size, then its data. */
#define HEADER_SIZE 8u
#define MSG_TYPE(msgid) ((msgid) >> 24)
#define MSG_SRCPORT(msgid) (((msgid) >> 12) & 0xfffu)
#define MSG_DSTPORT(msgid) ((msgid) & 0xfffu)
#define MAKE_MSG(type, src, dst) ((uint32_t)(type) << 24 | (src) << 12 | (dst))

enum {
	MSG_PADDING,
	MSG_CONNECT,
	MSG_OPEN,
	MSG_OPENACK,
	MSG_CLOSE,
	MSG_DATA,
	MSG_BULK_RX,
	MSG_BULK_TX,
	MSG_BULK_RX_DONE,
	MSG_BULK_TX_DONE,
	MSG_PAUSE,
	MSG_RESUME,
	MSG_REMOTE_USE,
	MSG_REMOTE_RELEASE,
	MSG_REMOTE_USE_ACTIVE,
	MSG_TYPES,
};

static const char *const msg_names[MSG_TYPES] = {
	"PADDING", "CONNECT", "OPEN",       "OPENACK",        "CLOSE",
	"DATA",    "BULK_RX", "BULK_TX",    "BULK_RX_DONE",   "BULK_TX_DONE",
	"PAUSE",   "RESUME",  "REMOTE_USE", "REMOTE_RELEASE", "REMOTE_USE_ACTIVE",
};

/* struct vchiq_open_payload: fourcc, client_id, version, version_min. */
#define OPEN_PAYLOAD_SIZE 12u

#define WARNED_MAX 16 /* the services the warnings named */

#define SERVICES_MAX 8
#define PORTS_MAX 32 /* the VideoCore's ports, 1 to 32: smirror's choice */
#define WAITING_MAX 65536u /* bytes of messages waiting for slots */

/* A port of the VideoCore's: the service open on it, and the Arm's. */
typedef struct Port {
	const Raspi3bVchiqService *service; /* NULL: free */
	uint32_t remote;
} Port;

/* A message of the VideoCore's waiting for a slot. */
typedef struct Waiting {
	struct Waiting *next;
	uint32_t msgid, size;
	uint8_t data[];
} Waiting;

struct Raspi3bVchiq {
	Machine *machine;
	uint32_t ram_size;
	Bcm2835Doorbell *doorbell; /* the Arm's */
	uint64_t latency_ns;
	void (*warn)(void *ctx, const char *message);
	void *warn_ctx;
	Event work; /* the VideoCore answers the doorbell */

	bool started;
	bool connected;
	uint32_t zero;                /* slot zero, in the RAM */
	uint32_t first, last;         /* the VideoCore's slots */
	uint32_t arm_first, arm_last; /* the Arm's */
	uint32_t rx_pos;              /* the Arm's next message */
	uint32_t tx_pos;              /* where the VideoCore's next one goes */
	uint32_t warned[WARNED_MAX];
	unsigned warned_count;

	Raspi3bVchiqService services[SERVICES_MAX];
	unsigned service_count;
	Port ports[PORTS_MAX + 1];       /* port 0 is not one */
	Waiting *waiting, **waiting_end; /* in the order sent */
	uint32_t waiting_bytes;
	bool paused; /* the Arm's messages wait for the VideoCore's to go */
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

static void work(void *ctx);

Raspi3bVchiq *raspi3b_vchiq_create(Machine *m, uint32_t ram_size)
{
	Raspi3bVchiq *v = calloc(1, sizeof(*v));

	if (v == NULL)
		return NULL;
	v->machine = m;
	v->ram_size = ram_size;
	v->waiting_end = &v->waiting;
	event_init(&v->work, work, v);
	return v;
}

/* The messages waiting for slots are dropped. */
static void drop_waiting(Raspi3bVchiq *v)
{
	while (v->waiting != NULL) {
		Waiting *w = v->waiting;

		v->waiting = w->next;
		free(w);
	}
	v->waiting_end = &v->waiting;
	v->waiting_bytes = 0;
	v->paused = false;
}

/* Every service open is closed. */
static void close_all(Raspi3bVchiq *v)
{
	for (unsigned port = 1; port <= PORTS_MAX; port++) {
		const Raspi3bVchiqService *s = v->ports[port].service;

		v->ports[port].service = NULL;
		if (s != NULL && s->close != NULL)
			s->close(s->ctx, port);
	}
}

bool raspi3b_vchiq_add_service(Raspi3bVchiq *v, const Raspi3bVchiqService *s)
{
	if (v->service_count == SERVICES_MAX)
		return false;
	v->services[v->service_count++] = *s;
	return true;
}

void raspi3b_vchiq_destroy(Raspi3bVchiq *v)
{
	if (v == NULL)
		return;
	event_cancel(&v->machine->events, &v->work);
	drop_waiting(v);
	free(v);
}

void raspi3b_vchiq_attach(Raspi3bVchiq *v, Bcm2835Doorbell *doorbell,
                          uint64_t latency_ns,
                          void (*warn)(void *ctx, const char *message),
                          void *warn_ctx)
{
	v->doorbell = doorbell;
	v->latency_ns = latency_ns;
	v->warn = warn;
	v->warn_ctx = warn_ctx;
}

void raspi3b_vchiq_reset(Raspi3bVchiq *v)
{
	event_cancel(&v->machine->events, &v->work);
	v->started = false; /* VCHIQ_INIT sets the rest again */
	v->warned_count = 0;
	drop_waiting(v);
	close_all(v);
	for (unsigned i = 0; i < v->service_count; i++)
		if (v->services[i].reset != NULL)
			v->services[i].reset(v->services[i].ctx);
}

static uint32_t get(Raspi3bVchiq *v, uint64_t address)
{
	uint64_t value = 0;

	bus_read(v->machine->bus, address, 4, &value);
	return (uint32_t)value;
}

static void put(Raspi3bVchiq *v, uint64_t address, uint32_t value)
{
	bus_write(v->machine->bus, address, 4, value);
}

/* Where a slot of the shared memory is. */
static uint64_t slot_address(const Raspi3bVchiq *v, uint32_t index)
{
	return (uint64_t)v->zero + (uint64_t)index * SLOT_SIZE;
}

/* The bytes of slot zero that the VideoCore checks: up to its slots. */
#define HEAD_SIZE (Z_SLAVE + S_TRIGGER)

/* Whether a side's slots, synchronous then first to last, fit. */
static bool slots_fit(const Raspi3bVchiq *v, uint32_t zero, uint32_t sync,
                      uint32_t first, uint32_t last)
{
	return sync != 0 && first == sync + 1 && last >= first &&
	       last - first < MAX_SLOTS_PER_SIDE &&
	       (uint64_t)zero + (uint64_t)(last + 1) * SLOT_SIZE <= v->ram_size;
}

/*
 * Check what the Arm wrote in slot zero, at zero, of which head holds
 * the first bytes. Returns false when it stopped the machine.
 */
static bool check_slot_zero(Raspi3bVchiq *v, uint32_t zero, const uint8_t *head)
{
	uint32_t first = load_le32(head + Z_MASTER + S_SLOT_FIRST);
	uint32_t last = load_le32(head + Z_MASTER + S_SLOT_LAST);
	uint32_t sync = load_le32(head + Z_MASTER + S_SLOT_SYNC);
	uint32_t arm_first = load_le32(head + Z_SLAVE + S_SLOT_FIRST);
	uint32_t arm_last = load_le32(head + Z_SLAVE + S_SLOT_LAST);
	uint32_t arm_sync = load_le32(head + Z_SLAVE + S_SLOT_SYNC);
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
	if (!slots_fit(v, zero, sync, first, last)) {
		stop(v,
		     "the VideoCore's slots %" PRIu32 " (synchronous), %" PRIu32
		     " to %" PRIu32 " are not emulated",
		     sync, first, last);
		return false;
	}
	if (!slots_fit(v, zero, arm_sync, arm_first, arm_last)) {
		stop(v,
		     "the Arm's slots %" PRIu32 " (synchronous), %" PRIu32
		     " to %" PRIu32 " are not emulated",
		     arm_sync, arm_first, arm_last);
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
	uint32_t sync;

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
	v->zero = zero;
	v->first = load_le32(head + Z_MASTER + S_SLOT_FIRST);
	v->last = load_le32(head + Z_MASTER + S_SLOT_LAST);
	v->arm_first = load_le32(head + Z_SLAVE + S_SLOT_FIRST);
	v->arm_last = load_le32(head + Z_SLAVE + S_SLOT_LAST);
	sync = load_le32(head + Z_MASTER + S_SLOT_SYNC);
	v->rx_pos = 0;
	v->tx_pos = 0;
	v->connected = false; /* nothing waits or is open: see reset */

	/* Its slots, to send in; it starts with the first. */
	for (uint32_t i = 0; i <= v->last - v->first; i++)
		put(v, master + S_SLOT_QUEUE + 4 * i, v->first + i);
	put(v, master + S_TX_POS, 0);
	put(v, master + S_SLOT_QUEUE_RECYCLE, v->last - v->first + 1);
	/* It waits for the Arm's messages, recycled slots and sync messages. */
	put(v, master + S_TRIGGER + EVENT_ARMED, 1);
	put(v, master + S_RECYCLE + EVENT_ARMED, 1);
	put(v, master + S_SYNC_TRIGGER + EVENT_ARMED, 1);
	/* Its synchronous slot is empty, and free for the Arm to use. */
	put(v, slot_address(v, sync), MAKE_MSG(MSG_PADDING, 0, 0));
	put(v, master + S_SYNC_RELEASE + EVENT_ARMED, 0);
	put(v, master + S_SYNC_RELEASE + EVENT_FIRED, 1);
	put(v, master + S_DEBUG, DEBUG_MAX);
	put(v, master + S_INITIALISED, 1);
	v->started = true;
	return true;
}

/* remote_event_signal: fire an event of the Arm's, ringing if it waits. */
static void signal_arm(Raspi3bVchiq *v, uint64_t event)
{
	put(v, event + EVENT_FIRED, 1);
	if (get(v, event + EVENT_ARMED) != 0 && v->doorbell != NULL)
		bcm2835_doorbell_ring(v->doorbell);
}

/* calc_stride: a message's header and data, rounded up to 8 bytes. */
static uint32_t stride(uint32_t size)
{
	return (size + 2 * HEADER_SIZE - 1) & ~(HEADER_SIZE - 1);
}

/*
 * The slot of the side at half that the position pos is in, from the
 * side's slot queue; UINT32_MAX, with the machine stopped, when the
 * queue names a slot outside first to last.
 */
static uint32_t queued_slot(Raspi3bVchiq *v, uint64_t half, uint32_t pos,
                            uint32_t first, uint32_t last, const char *side)
{
	uint32_t index =
		get(v, half + S_SLOT_QUEUE + 4 * ((pos / SLOT_SIZE) & SLOT_QUEUE_MASK));

	if (index < first || index > last) {
		stop(v,
		     "the %s slot queue names slot %" PRIu32 ", not one of its own, "
		     "which is not emulated",
		     side, index);
		return UINT32_MAX;
	}
	return index;
}

/*
 * reserve_space and queue_message: put the first of the messages
 * waiting in the VideoCore's slots, padding the rest of a slot it does
 * not fit. Returns whether it went: not when it needs a slot the Arm
 * has not recycled, nor when it stopped the machine.
 */
static bool put_message(Raspi3bVchiq *v, const Waiting *w)
{
	uint64_t master = (uint64_t)v->zero + Z_MASTER;
	uint32_t pos = v->tx_pos;
	uint32_t space = SLOT_SIZE - (pos & SLOT_MASK);
	uint32_t index;
	uint64_t at;

	if (stride(w->size) > space) {
		index = queued_slot(v, master, pos, v->first, v->last, "VideoCore's");
		if (index == UINT32_MAX)
			return false;
		at = slot_address(v, index) + (pos & SLOT_MASK);
		put(v, at, MAKE_MSG(MSG_PADDING, 0, 0));
		put(v, at + 4, space - HEADER_SIZE);
		pos += space;
	}
	/* A new slot must have come back from the Arm. */
	if ((pos & SLOT_MASK) == 0 &&
	    (int32_t)(pos / SLOT_SIZE - get(v, master + S_SLOT_QUEUE_RECYCLE)) >= 0)
		return false;
	index = queued_slot(v, master, pos, v->first, v->last, "VideoCore's");
	if (index == UINT32_MAX)
		return false;
	at = slot_address(v, index) + (pos & SLOT_MASK);
	put(v, at, w->msgid);
	put(v, at + 4, w->size);
	if (w->size != 0)
		bus_write_block(v->machine->bus, at + HEADER_SIZE, w->data, w->size);
	v->tx_pos = pos + stride(w->size);
	put(v, master + S_TX_POS, v->tx_pos);
	signal_arm(v, (uint64_t)v->zero + Z_SLAVE + S_TRIGGER);
	return true;
}

/*
 * The messages waiting go, in order, until one does not: it needs a
 * slot the Arm has not recycled, or it stopped the machine (and waits).
 */
static void flush(Raspi3bVchiq *v)
{
	while (v->waiting != NULL) {
		Waiting *w = v->waiting;

		if (!put_message(v, w))
			return;
		v->waiting = w->next;
		if (v->waiting == NULL)
			v->waiting_end = &v->waiting;
		v->waiting_bytes -= w->size;
		free(w);
	}
}

/*
 * Send one of the VideoCore's messages, with size bytes of data, after
 * those waiting. Returns false when it could not wait, and stopped the
 * machine; one that waits has been sent, as far as its sender knows.
 */
static bool send_message(Raspi3bVchiq *v, uint32_t msgid, const void *data,
                         uint32_t size)
{
	Waiting *w;

	if (v->waiting_bytes + size > WAITING_MAX) {
		stop(v, "more than 64 KB of the VideoCore's messages waiting for "
		        "the Arm to recycle slots is not emulated");
		return false;
	}
	w = malloc(sizeof(*w) + size);
	if (w == NULL) {
		stop(v, "out of memory for the VideoCore's message");
		return false;
	}
	w->next = NULL;
	w->msgid = msgid;
	w->size = size;
	if (size != 0)
		memcpy(w->data, data, size);
	*v->waiting_end = w;
	v->waiting_end = &w->next;
	v->waiting_bytes += size;
	flush(v);
	return true;
}

/* A message of the VideoCore's, a header alone. */
static bool send(Raspi3bVchiq *v, uint32_t msgid)
{
	return send_message(v, msgid, NULL, 0);
}

bool raspi3b_vchiq_send(Raspi3bVchiq *v, unsigned port, const void *data,
                        uint32_t size)
{
	if (port == 0 || port > PORTS_MAX || v->ports[port].service == NULL) {
		stop(v, "a service sends from port %u, which is not open", port);
		return false;
	}
	if (size > RASPI3B_VCHIQ_MSG_MAX) {
		stop(v, "a message of %" PRIu32 " bytes does not fit a slot", size);
		return false;
	}
	return send_message(v, MAKE_MSG(MSG_DATA, port, v->ports[port].remote),
	                    data, size);
}

/* release_slot: hand a slot the VideoCore has read back to the Arm. */
static void release(Raspi3bVchiq *v, uint32_t index)
{
	uint64_t slave = (uint64_t)v->zero + Z_SLAVE;
	uint32_t recycle = get(v, slave + S_SLOT_QUEUE_RECYCLE);

	put(v, slave + S_SLOT_QUEUE + 4 * (recycle & SLOT_QUEUE_MASK), index);
	put(v, slave + S_SLOT_QUEUE_RECYCLE, recycle + 1);
	signal_arm(v, slave + S_RECYCLE);
}

/* The four characters of a service's fourcc, '?' for what is not text. */
static void fourcc_text(uint32_t fourcc, char text[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(fourcc >> (24 - 8 * i));

		text[i] = c >= 0x20 && c < 0x7f ? c : '?';
	}
	text[4] = '\0';
}

/* Warn, once for each service, that the VideoCore refused it. */
static void warn_service(Raspi3bVchiq *v, uint32_t fourcc)
{
	char text[5], message[160];

	for (unsigned i = 0; i < v->warned_count; i++)
		if (v->warned[i] == fourcc)
			return;
	if (v->warned_count < WARNED_MAX)
		v->warned[v->warned_count++] = fourcc;
	if (v->warn == NULL)
		return;
	fourcc_text(fourcc, text);
	snprintf(message, sizeof(message),
	         "VCHIQ: the VideoCore's service '%s' is not emulated: it "
	         "refuses the Arm's OPEN",
	         text);
	v->warn(v->warn_ctx, message);
}

/* The service of the fourcc, NULL when the VideoCore has none. */
static const Raspi3bVchiqService *find_service(const Raspi3bVchiq *v,
                                               uint32_t fourcc)
{
	for (unsigned i = 0; i < v->service_count; i++)
		if (v->services[i].fourcc == fourcc)
			return &v->services[i];
	return NULL;
}

/*
 * parse_open: the Arm's port opens a service, of which the OPEN's
 * payload, size bytes at data, names the fourcc and versions. Returns
 * false when it stopped the machine.
 */
static bool open_service(Raspi3bVchiq *v, uint32_t arm_port, uint32_t size,
                         uint64_t data)
{
	uint32_t refuse = MAKE_MSG(MSG_CLOSE, 0, arm_port);
	const Raspi3bVchiqService *s;
	uint32_t fourcc, versions;
	int16_t version, version_min;
	uint8_t ack[2];
	char text[5], message[160];

	/* An OPEN too short to name a service fails the same way. */
	if (size < OPEN_PAYLOAD_SIZE)
		return send(v, refuse);
	fourcc = get(v, data);
	versions = get(v, data + 8);
	version = (int16_t)(versions & 0xffffu);
	version_min = (int16_t)(versions >> 16);
	s = find_service(v, fourcc);
	if (s == NULL) {
		warn_service(v, fourcc);
		return send(v, refuse);
	}
	if (s->version < version_min || version < s->version_min) {
		if (v->warn != NULL) {
			fourcc_text(fourcc, text);
			snprintf(message, sizeof(message),
			         "VCHIQ: the Arm's '%s' version %d (from %d) does not "
			         "meet the VideoCore's, %u (from %u): it refuses the OPEN",
			         text, version, version_min, s->version, s->version_min);
			v->warn(v->warn_ctx, message);
		}
		return send(v, refuse);
	}
	for (unsigned port = 1; port <= PORTS_MAX; port++) {
		if (v->ports[port].service != NULL)
			continue;
		v->ports[port].service = s;
		v->ports[port].remote = arm_port;
		store_le16(ack, s->version);
		if (!send_message(v, MAKE_MSG(MSG_OPENACK, port, arm_port), ack,
		                  sizeof(ack)))
			return false;
		if (s->open != NULL)
			s->open(s->ctx, port);
		return true;
	}
	stop(v, "more than 32 of the VideoCore's services open is not emulated");
	return false;
}

/*
 * The VideoCore's port that a message of the Arm's port is to: the
 * port it names, if the Arm's port opened it, or, for a CLOSE to port
 * 0, the port the Arm's port opened (get_connected_service). 0: none.
 */
static unsigned port_of(const Raspi3bVchiq *v, uint32_t msgid)
{
	uint32_t port = MSG_DSTPORT(msgid);
	uint32_t arm_port = MSG_SRCPORT(msgid);

	if (port >= 1 && port <= PORTS_MAX && v->ports[port].service != NULL &&
	    v->ports[port].remote == arm_port)
		return port;
	if (port != 0 || MSG_TYPE(msgid) != MSG_CLOSE)
		return 0;
	for (unsigned p = 1; p <= PORTS_MAX; p++)
		if (v->ports[p].service != NULL && v->ports[p].remote == arm_port)
			return p;
	return 0;
}

/* The Arm's DATA, size bytes at data, to its service. */
static bool data_message(Raspi3bVchiq *v, uint32_t msgid, uint32_t size,
                         uint64_t data)
{
	unsigned port = port_of(v, msgid);
	const Raspi3bVchiqService *s;
	uint8_t bytes[SLOT_SIZE];

	if (port == 0)
		return true; /* no service there: dropped */
	s = v->ports[port].service;
	if (s->data == NULL)
		return true;
	bus_read_block(v->machine->bus, data, bytes, size);
	return s->data(s->ctx, port, bytes, size);
}

/* The Arm's CLOSE: the service closes, and says so. */
static bool close_message(Raspi3bVchiq *v, uint32_t msgid)
{
	unsigned port = port_of(v, msgid);
	const Raspi3bVchiqService *s;

	if (port == 0)
		return true; /* no service of the VideoCore's to close */
	s = v->ports[port].service;
	v->ports[port].service = NULL;
	if (s->close != NULL)
		s->close(s->ctx, port);
	return send(v, MAKE_MSG(MSG_CLOSE, port, MSG_SRCPORT(msgid)));
}

/*
 * parse_message: one of the Arm's messages, with size bytes of data at
 * data. Returns false when it stopped the machine.
 */
static bool receive(Raspi3bVchiq *v, uint32_t msgid, uint32_t size,
                    uint64_t data)
{
	uint32_t type = MSG_TYPE(msgid);

	switch (type) {
	case MSG_PADDING:
		return true;
	case MSG_CONNECT:
		if (v->connected) {
			stop(v, "a second CONNECT is not emulated");
			return false;
		}
		v->connected = true;
		return send(v, MAKE_MSG(MSG_CONNECT, 0, 0));
	case MSG_OPEN:
		if (!v->connected) {
			stop(v, "an OPEN before CONNECT is not emulated");
			return false;
		}
		return open_service(v, MSG_SRCPORT(msgid), size, data);
	case MSG_DATA:
		return data_message(v, msgid, size, data);
	case MSG_CLOSE:
		return close_message(v, msgid);
	default:
		if (type < MSG_TYPES)
			stop(v, "the Arm's %s message is not emulated", msg_names[type]);
		else
			stop(v, "the Arm's message 0x%08" PRIx32 " has no type", msgid);
		return false;
	}
}

/*
 * parse_rx_slots: the Arm's messages, up to its tx_pos; it pauses when
 * the VideoCore's own wait for slots.
 */
static void receive_all(Raspi3bVchiq *v)
{
	uint64_t slave = (uint64_t)v->zero + Z_SLAVE;
	uint32_t tx_pos = get(v, slave + S_TX_POS);

	v->paused = false;
	while (v->rx_pos != tx_pos) {
		if (v->waiting != NULL) {
			v->paused = true;
			return;
		}
		uint32_t offset = v->rx_pos & SLOT_MASK;
		uint32_t index = queued_slot(v, slave, v->rx_pos, v->arm_first,
		                             v->arm_last, "Arm's");
		uint64_t header;
		uint32_t msgid, size;

		if (index == UINT32_MAX)
			return;
		header = slot_address(v, index) + offset;
		msgid = get(v, header);
		size = get(v, header + 4);
		if (size > SLOT_SIZE - HEADER_SIZE - offset) {
			stop(v,
			     "the Arm's message 0x%08" PRIx32 " of %" PRIu32
			     " bytes goes past the end of its slot",
			     msgid, size);
			return;
		}
		if (!receive(v, msgid, size, header + HEADER_SIZE))
			return;
		v->rx_pos += stride(size);
		if ((v->rx_pos & SLOT_MASK) == 0)
			release(v, index);
	}
}

/*
 * The VideoCore's events, after the Arm rang: a recycled slot lets the
 * messages waiting go (put_message reads slot_queue_recycle); then, if
 * none waits, new messages are read, and those that paused.
 */
static void work(void *ctx)
{
	Raspi3bVchiq *v = ctx;
	uint64_t master = (uint64_t)v->zero + Z_MASTER;

	if (get(v, master + S_SYNC_TRIGGER + EVENT_FIRED) != 0) {
		stop(v, "the Arm's synchronous messages are not emulated");
		return;
	}
	put(v, master + S_RECYCLE + EVENT_FIRED, 0);
	flush(v);
	if (v->waiting != NULL)
		return;
	if (get(v, master + S_TRIGGER + EVENT_FIRED) != 0) {
		put(v, master + S_TRIGGER + EVENT_FIRED, 0);
		receive_all(v);
	} else if (v->paused) {
		receive_all(v);
	}
}

void raspi3b_vchiq_doorbell(void *ctx)
{
	Raspi3bVchiq *v = ctx;

	if (!v->started) {
		stop(v, "the Arm rang the VideoCore's doorbell before VCHIQ_INIT, "
		        "which is not emulated");
		return;
	}
	if (!event_pending(&v->work))
		event_schedule(&v->machine->events, &v->work,
		               machine_now(v->machine) + v->latency_ns);
}
