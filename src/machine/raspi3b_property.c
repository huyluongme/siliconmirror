/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The firmware's property channel on the Raspberry Pi 3; see the header. */

#include "machine/raspi3b_property.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bus.h"
#include "machine/raspi3b.h"
#include "util/byteorder.h"
#include "util/macros.h"

#define MB 1000000u
#define MIB (1024u * 1024u)

/*
 * The firmware revision is the time it was built, in seconds since 1970
 * (Linux prints it as a date: drivers/firmware/raspberrypi.c). Ours is the
 * time of raspberrypi/firmware commit 896fc67, of 12 November 2026.
 */
#define FIRMWARE_REVISION 0x6af5d384u

/* The bus address of a buffer: the alias in bits 31:30 is dropped. */
#define BUS_ALIAS_MASK 0x3fffffffu

#define BUFFER_MAX (1u << 20) /* larger buffers stop the machine */
#define ANSWER_MAX 4096u      /* the longest answer, the command line's */

/* Codes of the buffer and of its tags. */
#define REQUEST 0x00000000u
#define RESPONSE_OK 0x80000000u
#define RESPONSE_PARSE_ERROR 0x80000001u
#define TAG_RESPONSE (1u << 31)
#define END_TAG 0

/* Power states and clock states. */
#define STATE_ON (1u << 0)
#define STATE_ABSENT (1u << 1) /* the device or clock does not exist */
#define STATE_WAIT (1u << 1)   /* in a request: wait for the power */

#define NUM_DEVICES 9 /* power domains 0 (SD card) to 8 (CCP2TX) */

enum {
	CLOCK_EMMC = 1,
	CLOCK_UART,
	CLOCK_ARM,
	CLOCK_CORE,
	CLOCK_V3D,
	CLOCK_H264,
	CLOCK_ISP,
	CLOCK_SDRAM,
	CLOCK_PIXEL,
	CLOCK_PWM,
	CLOCK_HEVC,
	CLOCK_EMMC2,
	CLOCK_M2MC,
	CLOCK_PIXEL_BVB,
	NUM_CLOCKS
};

/* What is known of a clock of the Raspberry Pi 3 Model B. */
typedef enum ClockKind {
	CLOCK_ABSENT,    /* does not exist on the BCM2837 */
	CLOCK_UNKNOWN,   /* exists; its rates are not documented */
	CLOCK_FIXED,     /* its rate is documented; it cannot change here */
	CLOCK_ADJUSTABLE /* between its documented minimum and maximum */
} ClockKind;

typedef struct Clock {
	ClockKind kind;
	uint32_t rate, min, max; /* Hz */
} Clock;

/*
 * A tag being answered: the value the Arm sent, and the answer. Each
 * handler checks the request is long enough, writes its answer, and
 * returns false when it stopped the machine.
 */
typedef struct Value {
	uint32_t tag;
	const uint8_t *request;
	uint32_t request_len;
	uint8_t answer[ANSWER_MAX];
	uint32_t answer_len;
} Value;

struct Raspi3bProperty {
	Machine *machine;
	uint32_t ram_size;
	Raspi3bVcSettings settings;
	char *cmdline;
	uint32_t turbo;
	bool power_known[NUM_DEVICES]; /* set by the Arm since boot */
	bool power_on[NUM_DEVICES];
	Value value;              /* the tag being answered */
	Clock clocks[NUM_CLOCKS]; /* last: a read past them leaves the object */
};

/*
 * Every tag that the firmware knows: those of the wiki and those of
 * Linux's raspberrypi-firmware.h. Sorted.
 */
static const uint32_t known_tags[] = {
	0x00000001, 0x00008010, 0x00008011, 0x00008012, 0x00010001, 0x00010002,
	0x00010003, 0x00010004, 0x00010005, 0x00010006, 0x00010007, 0x00020001,
	0x00020002, 0x00028001, 0x00030001, 0x00030002, 0x00030003, 0x00030004,
	0x00030005, 0x00030006, 0x00030007, 0x00030008, 0x00030009, 0x0003000a,
	0x0003000b, 0x0003000c, 0x0003000d, 0x0003000e, 0x0003000f, 0x00030010,
	0x00030011, 0x00030012, 0x00030014, 0x00030020, 0x00030021, 0x00030030,
	0x00030041, 0x00030043, 0x00030045, 0x00030046, 0x00030047, 0x00030048,
	0x00030049, 0x00030050, 0x00030058, 0x00030066, 0x00030087, 0x00034041,
	0x00038001, 0x00038002, 0x00038003, 0x00038009, 0x00038021, 0x00038030,
	0x00038041, 0x00038042, 0x00038043, 0x00038045, 0x00038087, 0x00040001,
	0x00040002, 0x00040003, 0x00040004, 0x00040005, 0x00040006, 0x00040007,
	0x00040008, 0x00040009, 0x0004000a, 0x0004000b, 0x0004000f, 0x00040010,
	0x00044003, 0x00044004, 0x00044005, 0x00044006, 0x00044007, 0x00044009,
	0x0004400a, 0x0004400b, 0x0004400e, 0x00048001, 0x00048003, 0x00048004,
	0x00048005, 0x00048006, 0x00048007, 0x00048009, 0x0004800a, 0x0004800b,
	0x0004800e, 0x0004800f, 0x00048010, 0x0004801f, 0x00048020, 0x00050001,
	0x00060001,
};

static bool known(uint32_t tag)
{
	for (size_t i = 0; i < ARRAY_SIZE(known_tags); i++)
		if (known_tags[i] == tag)
			return true;
	return false;
}

static void stop(Raspi3bProperty *p, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void stop(Raspi3bProperty *p, const char *fmt, ...)
{
	char why[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	machine_stop(p->machine, MACHINE_STOP_ERROR, "firmware: %s", why);
}

/* The clocks at boot: the defaults of config.txt for a Pi 3. */
static void reset_clocks(Raspi3bProperty *p)
{
	uint32_t core = p->settings.core_freq_fixed ? 250 * MB : 400 * MB;

	for (unsigned i = 0; i < NUM_CLOCKS; i++)
		p->clocks[i] = (Clock){CLOCK_ABSENT, 0, 0, 0};
	p->clocks[CLOCK_EMMC].kind = CLOCK_UNKNOWN;
	p->clocks[CLOCK_PIXEL].kind = CLOCK_UNKNOWN;
	p->clocks[CLOCK_PWM].kind = CLOCK_UNKNOWN;
	p->clocks[CLOCK_EMMC2].kind = CLOCK_UNKNOWN;
	p->clocks[CLOCK_M2MC].kind = CLOCK_UNKNOWN;
	p->clocks[CLOCK_PIXEL_BVB].kind = CLOCK_UNKNOWN;
	/* init_uart_clock; its range is not documented. */
	p->clocks[CLOCK_UART] = (Clock){CLOCK_FIXED, 48 * MB, 0, 0};
	p->clocks[CLOCK_ARM] =
		(Clock){CLOCK_ADJUSTABLE, 1200 * MB, 600 * MB, 1200 * MB};
	p->clocks[CLOCK_CORE] = (Clock){CLOCK_FIXED, core, 250 * MB, core};
	p->clocks[CLOCK_V3D] =
		(Clock){CLOCK_ADJUSTABLE, 400 * MB, 250 * MB, 400 * MB};
	p->clocks[CLOCK_H264] = p->clocks[CLOCK_V3D];
	p->clocks[CLOCK_ISP] = p->clocks[CLOCK_V3D];
	p->clocks[CLOCK_SDRAM] =
		(Clock){CLOCK_ADJUSTABLE, 450 * MB, 400 * MB, 450 * MB};
	/* hevc_freq is "N/A" on the Pi 3: CLOCK_HEVC stays absent. */
}

void raspi3b_property_reset(Raspi3bProperty *p)
{
	reset_clocks(p);
	p->turbo = 0;
	memset(p->power_known, 0, sizeof(p->power_known));
	memset(p->power_on, 0, sizeof(p->power_on));
}

Raspi3bProperty *raspi3b_property_create(Machine *m, uint32_t ram_size)
{
	Raspi3bProperty *p = calloc(1, sizeof(*p));
	static const Raspi3bVcSettings defaults = {
		.total_mem_mb = RASPI3B_TOTAL_MEM_MB,
		.gpu_mem_mb = RASPI3B_GPU_MEM_MB,
		.core_freq_fixed = false,
		.cmdline = "",
	};

	if (p == NULL)
		return NULL;
	p->machine = m;
	p->ram_size = ram_size;
	if (!raspi3b_property_configure(p, &defaults)) {
		free(p);
		return NULL;
	}
	return p;
}

void raspi3b_property_destroy(Raspi3bProperty *p)
{
	free(p->cmdline);
	free(p);
}

bool raspi3b_property_configure(Raspi3bProperty *p,
                                const Raspi3bVcSettings *settings)
{
	size_t len = strlen(settings->cmdline);
	char *cmdline = malloc(len + 1);

	if (cmdline == NULL)
		return false;
	memcpy(cmdline, settings->cmdline, len + 1);
	free(p->cmdline);
	p->cmdline = cmdline;
	p->settings = *settings;
	p->settings.cmdline = cmdline;
	raspi3b_property_reset(p);
	return true;
}

uint32_t raspi3b_property_arm_memory(const Raspi3bProperty *p)
{
	return (p->settings.total_mem_mb - p->settings.gpu_mem_mb) * MIB;
}

void raspi3b_property_mac(uint8_t mac[6])
{
	/* B8-27-EB: the IEEE OUI of the Raspberry Pi Foundation. */
	mac[0] = 0xb8;
	mac[1] = 0x27;
	mac[2] = 0xeb;
	mac[3] = (uint8_t)(RASPI3B_BOARD_SERIAL >> 16);
	mac[4] = (uint8_t)(RASPI3B_BOARD_SERIAL >> 8);
	mac[5] = (uint8_t)RASPI3B_BOARD_SERIAL;
}

/* --- The tags ------------------------------------------------------------- */

static bool needs(Raspi3bProperty *p, const Value *v, uint32_t len)
{
	if (v->request_len >= len)
		return true;
	stop(p,
	     "tag 0x%08" PRIx32 ": a value buffer too short for the request "
	     "is not emulated",
	     v->tag);
	return false;
}

static uint32_t arg(const Value *v, unsigned i)
{
	return load_le32(v->request + 4 * i);
}

static void answer(Value *v, unsigned i, uint32_t word)
{
	store_le32(v->answer + 4 * i, word);
	if (v->answer_len < 4 * (i + 1))
		v->answer_len = 4 * (i + 1);
}

static bool get_firmware_revision(Raspi3bProperty *p, Value *v)
{
	(void)p;
	answer(v, 0, FIRMWARE_REVISION);
	return true;
}

static bool get_board_revision(Raspi3bProperty *p, Value *v)
{
	(void)p;
	answer(v, 0, RASPI3B_BOARD_REVISION);
	return true;
}

static bool get_mac_address(Raspi3bProperty *p, Value *v)
{
	(void)p;
	raspi3b_property_mac(v->answer);
	v->answer_len = 6;
	return true;
}

static bool get_board_serial(Raspi3bProperty *p, Value *v)
{
	(void)p;
	answer(v, 0, (uint32_t)RASPI3B_BOARD_SERIAL);
	answer(v, 1, (uint32_t)(RASPI3B_BOARD_SERIAL >> 32));
	return true;
}

static bool get_arm_memory(Raspi3bProperty *p, Value *v)
{
	answer(v, 0, 0);
	answer(v, 1, raspi3b_property_arm_memory(p));
	return true;
}

static bool get_vc_memory(Raspi3bProperty *p, Value *v)
{
	answer(v, 0, raspi3b_property_arm_memory(p));
	answer(v, 1, p->settings.gpu_mem_mb * MIB);
	return true;
}

static bool get_command_line(Raspi3bProperty *p, Value *v)
{
	size_t len = strlen(p->cmdline);

	if (len > ANSWER_MAX) {
		stop(p, "a command line of %zu bytes is not emulated", len);
		return false;
	}
	memcpy(v->answer, p->cmdline, len);
	v->answer_len = (uint32_t)len;
	return true;
}

static bool get_power_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id;

	if (!needs(p, v, 4))
		return false;
	id = arg(v, 0);
	answer(v, 0, id);
	if (id >= NUM_DEVICES) {
		answer(v, 1, STATE_ABSENT);
		return true;
	}
	if (!p->power_known[id]) {
		stop(p,
		     "the power state of device %" PRIu32 " at boot is not "
		     "documented",
		     id);
		return false;
	}
	answer(v, 1, p->power_on[id] ? STATE_ON : 0);
	return true;
}

static bool get_timing(Raspi3bProperty *p, Value *v)
{
	uint32_t id;

	if (!needs(p, v, 4))
		return false;
	id = arg(v, 0);
	if (id < NUM_DEVICES) {
		stop(p, "the power-up time of device %" PRIu32 " is not documented",
		     id);
		return false;
	}
	answer(v, 0, id);
	answer(v, 1, 0); /* the wiki: 0 for a device that does not exist */
	return true;
}

static bool set_power_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id, state;

	if (!needs(p, v, 8))
		return false;
	id = arg(v, 0);
	state = arg(v, 1);
	if (state & ~(STATE_ON | STATE_WAIT)) {
		stop(p, "power state 0x%" PRIx32 ": reserved bits are set", state);
		return false;
	}
	answer(v, 0, id);
	if (id >= NUM_DEVICES) {
		answer(v, 1, STATE_ABSENT);
		return true;
	}
	p->power_known[id] = true;
	p->power_on[id] = state & STATE_ON;
	answer(v, 1, state & STATE_ON);
	return true;
}

/*
 * The clock of the request, or NULL when it stopped the machine. Clock 0
 * is reserved, and absent like those after the last.
 */
static Clock *clock_of(Raspi3bProperty *p, const Value *v)
{
	uint32_t id = arg(v, 0);
	Clock *clock = &p->clocks[id < NUM_CLOCKS ? id : 0];

	if (clock->kind == CLOCK_UNKNOWN) {
		stop(p, "the rates of clock %" PRIu32 " are not documented", id);
		return NULL;
	}
	return clock;
}

/* Get clock rate, Get max clock rate and Get min clock rate. */
static bool get_clock_rate(Raspi3bProperty *p, Value *v)
{
	const Clock *clock;
	uint32_t rate;

	if (!needs(p, v, 4) || (clock = clock_of(p, v)) == NULL)
		return false;
	if (clock->kind == CLOCK_FIXED && v->tag != 0x00030002 && clock->max == 0) {
		stop(p, "the range of clock %" PRIu32 " is not documented", arg(v, 0));
		return false;
	}
	switch (v->tag) {
	case 0x00030004:
		rate = clock->max;
		break;
	case 0x00030007:
		rate = clock->min;
		break;
	default:
		rate = clock->rate;
		break;
	}
	answer(v, 0, arg(v, 0));
	answer(v, 1, rate);
	return true;
}

static bool set_clock_rate(Raspi3bProperty *p, Value *v)
{
	Clock *clock;
	uint32_t rate;

	if (!needs(p, v, 12) || (clock = clock_of(p, v)) == NULL)
		return false;
	rate = arg(v, 1);
	if (clock->kind == CLOCK_ADJUSTABLE) {
		/* The wiki: the rate may be clamped to the supported range. */
		if (rate < clock->min)
			rate = clock->min;
		if (rate > clock->max)
			rate = clock->max;
		clock->rate = rate;
	} else if (clock->kind == CLOCK_FIXED && rate != clock->rate) {
		stop(p, "changing the rate of clock %" PRIu32 " is not emulated",
		     arg(v, 0));
		return false;
	}
	answer(v, 0, arg(v, 0));
	answer(v, 1, clock->rate);
	return true;
}

static bool get_turbo(Raspi3bProperty *p, Value *v)
{
	if (!needs(p, v, 4))
		return false;
	if (arg(v, 0) != 0) {
		stop(p, "turbo id %" PRIu32 " is not documented", arg(v, 0));
		return false;
	}
	answer(v, 0, 0);
	answer(v, 1, p->turbo);
	return true;
}

static bool set_turbo(Raspi3bProperty *p, Value *v)
{
	if (!needs(p, v, 8))
		return false;
	if (arg(v, 0) != 0) {
		stop(p, "turbo id %" PRIu32 " is not documented", arg(v, 0));
		return false;
	}
	if (arg(v, 1) > 1) {
		stop(p, "turbo level %" PRIu32 " is not documented", arg(v, 1));
		return false;
	}
	/* Turbo raises the GPU clocks, the core clock among them. */
	if (arg(v, 1) && !p->settings.core_freq_fixed) {
		stop(p,
		     "turbo level %" PRIu32 " would raise the core clock to "
		     "400 MHz, which is not emulated",
		     arg(v, 1));
		return false;
	}
	p->turbo = arg(v, 1);
	answer(v, 0, 0);
	answer(v, 1, p->turbo);
	return true;
}

/*
 * SET_SDHOST_CLOCK: the firmware sets SDHOST's clock divider for the SD
 * clock asked, and keeps it right as the core clock changes, which the
 * drivers then leave to it (Raspberry Pi's bcm2835-sdhost.c: the tag
 * that changes nothing leaves ~0 in the answer). How the firmware
 * chooses is not documented: smirror chooses as the driver does itself,
 * the fastest clock not above the one asked, but at most the slowest
 * divider, which it also takes below 100 kHz. The answer gives that clock for
 * the core clock's two speeds, the same twice, as smirror's core clock does not
 * change.
 */
static bool set_sdhost_clock(Raspi3bProperty *p, Value *v)
{
	uint32_t clock, div, rate;

	if (!needs(p, v, 4))
		return false;
	clock = arg(v, 0);
	if (clock < 100000) {
		div = 0x7ff;
	} else {
		div = RASPI3B_CORE_CLOCK_HZ / clock;
		if (div < 2)
			div = 2;
		if (RASPI3B_CORE_CLOCK_HZ / div > clock)
			div++;
		div -= 2;
		if (div > 0x7ff)
			div = 0x7ff;
	}
	if (bus_write(p->machine->bus, RASPI3B_SDHOST_BASE + 0x0c, 4, div) !=
	    BUS_OK) {
		stop(p, "SDHOST is not where the firmware sets its clock");
		return false;
	}
	rate = RASPI3B_CORE_CLOCK_HZ / (div + 2);
	answer(v, 0, clock);
	answer(v, 1, rate);
	answer(v, 2, rate);
	return true;
}

typedef struct Handler {
	uint32_t tag;
	bool (*fn)(Raspi3bProperty *p, Value *v);
} Handler;

static const Handler handlers[] = {
	{0x00000001, get_firmware_revision},
	{0x00010002, get_board_revision},
	{0x00010003, get_mac_address},
	{0x00010004, get_board_serial},
	{0x00010005, get_arm_memory},
	{0x00010006, get_vc_memory},
	{0x00050001, get_command_line},
	{0x00020001, get_power_state},
	{0x00020002, get_timing},
	{0x00028001, set_power_state},
	{0x00030002, get_clock_rate},
	{0x00030004, get_clock_rate},
	{0x00030007, get_clock_rate},
	{0x00038002, set_clock_rate},
	{0x00030009, get_turbo},
	{0x00038009, set_turbo},
	{0x00038042, set_sdhost_clock},
};

/*
 * Answer the tag at offset in buf, whose value buffer is value_size
 * bytes. Returns false when it stopped the machine.
 */
static bool answer_tag(Raspi3bProperty *p, uint8_t *buf, uint32_t offset,
                       uint32_t value_size)
{
	Value *v = &p->value;
	uint32_t tag = load_le32(buf + offset);
	uint32_t code = load_le32(buf + offset + 8);
	const Handler *h = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(handlers); i++)
		if (handlers[i].tag == tag)
			h = &handlers[i];
	if (h == NULL) {
		if (!known(tag))
			return true; /* the wiki: unknown tags are ignored */
		stop(p, "tag 0x%08" PRIx32 " is not emulated", tag);
		return false;
	}
	if (code != 0) {
		stop(p, "a tag request code of 0x%08" PRIx32 " is not documented",
		     code);
		return false;
	}
	v->tag = tag;
	v->request = buf + offset + 12;
	v->request_len = value_size;
	v->answer_len = 0;
	if (!h->fn(p, v))
		return false;
	/* The answer is cut to the buffer; its length says how long it is. */
	memcpy(buf + offset + 12, v->answer,
	       v->answer_len < value_size ? v->answer_len : value_size);
	store_le32(buf + offset + 8, TAG_RESPONSE | v->answer_len);
	return true;
}

/*
 * Answer the tags of buf, size bytes. Returns the code of the buffer's
 * response, or 0 when it stopped the machine.
 */
static uint32_t answer_buffer(Raspi3bProperty *p, uint8_t *buf, uint32_t size)
{
	uint32_t offset = 8;

	while (offset + 4 <= size) {
		uint32_t tag = load_le32(buf + offset);
		uint32_t value_size;

		if (tag == END_TAG)
			return RESPONSE_OK;
		if (offset + 12 > size)
			break;
		value_size = load_le32(buf + offset + 4);
		if (value_size > size - offset - 12)
			break;
		if (!answer_tag(p, buf, offset, value_size))
			return 0;
		/* Values are padded to 32 bits. */
		offset += 12 + ((value_size + 3) & ~3u);
	}
	return RESPONSE_PARSE_ERROR; /* no end tag inside the buffer */
}

bool raspi3b_property_receive(void *ctx, uint32_t message, uint32_t *reply)
{
	Raspi3bProperty *p = ctx;
	uint32_t address = (message & ~0xfu) & BUS_ALIAS_MASK;
	uint8_t head[4];
	uint8_t *buf;
	uint32_t size, code;

	if ((message & 0xf) != RASPI3B_PROPERTY_CHANNEL) {
		stop(p, "mailbox channel %" PRIu32 " is not emulated", message & 0xf);
		return false;
	}
	if (address + 4 > p->ram_size ||
	    bus_read_block(p->machine->bus, address, head, 4) != BUS_OK) {
		stop(p, "a property buffer at 0x%08" PRIx32 " is not in memory",
		     address);
		return false;
	}
	size = load_le32(head);
	if (size > BUFFER_MAX || size > p->ram_size - address) {
		stop(p, "a property buffer of 0x%08" PRIx32 " bytes is not emulated",
		     size);
		return false;
	}
	if (size < 12) {
		stop(p, "a property buffer of %" PRIu32 " bytes is not emulated", size);
		return false;
	}
	buf = malloc(size);
	if (buf == NULL) {
		stop(p, "out of memory for a buffer of %" PRIu32 " bytes", size);
		return false;
	}
	bus_read_block(p->machine->bus, address, buf, size);
	if (load_le32(buf + 4) != REQUEST) {
		stop(p, "a buffer request code of 0x%08" PRIx32 " is not documented",
		     load_le32(buf + 4));
		free(buf);
		return false;
	}
	code = answer_buffer(p, buf, size);
	if (code != 0) {
		store_le32(buf + 4, code);
		bus_write_block(p->machine->bus, address, buf, size);
		*reply = message; /* the wiki: the same buffer address comes back */
	}
	free(buf);
	return code != 0;
}
