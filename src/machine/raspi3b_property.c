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
#include "machine/raspi3b_vchiq.h"
#include "util/byteorder.h"
#include "util/macros.h"

#define MB 1000000u
#define MIB (1024u * 1024u)

/*
 * The firmware is the start.elf of raspberrypi/firmware commit 896fc67,
 * of 12 November 2026. Its strings say how it was built:
 * "VC_BUILD_ID_TIME: Nov  2 2026", "VC_BUILD_ID_TIME: 13:42:00",
 * "VC_BUILD_ID_VARIANT: start" and its Git hash, "VC_BUILD_ID_VERSION:
 * 13c4219f16dbe58c5b9946086b474baf572512bb (clean)".
 *
 * Its revision is the time it was built, in seconds since 1970, which
 * Linux prints as a date (drivers/firmware/raspberrypi.c). Its variant
 * is 1, start, in Linux's list: unknown, start, start_x, start_db,
 * start_cd. Linux prints the hash as five words in hexadecimal.
 */
#define FIRMWARE_REVISION 0x6ae89328u /* 2026-11-02 13:42:00 UTC */
#define FIRMWARE_VARIANT 1
static const uint32_t firmware_hash[5] = {0x13c4219f, 0x16dbe58c, 0x5b994608,
                                          0x6b474baf, 0x572512bb};

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
	CLOCK_VEC,
	CLOCK_DISP,
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
 * The power domains of the firmware's newer tags, 0 (I2C0) to 22 (ARM),
 * as Linux's dt-bindings/power/raspberrypi-power.h numbers them.
 */
#define NUM_DOMAINS 23
#define DOMAIN_ARM 22

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

/*
 * The GPIO expander of the Pi 3 Model B: 8 lines that only the firmware
 * drives, which it numbers 128 to 135. Their names, from the board's
 * device tree: BT_ON, WL_ON, STATUS_LED, LAN_RUN, HDMI_HPD_N, CAM_GPIO0,
 * CAM_GPIO1, PWR_LOW_N. Neither the wiki nor Raspberry Pi's documentation
 * describes the tags; Linux's gpio-raspberrypi-exp.c gives their values,
 * and that the firmware answers 0 in the GPIO's place for success.
 */
#define EXP_BASE 128
#define EXP_LINES 8

typedef struct ExpLine {
	bool output;
	bool state; /* driven if an output */
	bool level; /* outside, as an input reads it */
} ExpLine;

/*
 * How the lines start. gpioinfo on a Pi 3B+ and a Pi 3A+, whose expanders
 * have BT_ON, WL_ON, LAN_RUN and CAM_GPIO0 and 1 at the same places
 * (Raspberry Pi forums, topic 330552; seamusdemora's PiFormulae, "Testing
 * libgpiod ver 2.1"), shows them as outputs, LAN_RUN on the 3B+ that has
 * a LAN chip, and their unused lines as inputs. STATUS_LED, the activity
 * LED, is an output too; HDMI_HPD_N and PWR_LOW_N, a hot-plug detect and
 * a power monitor, are inputs. The levels are smirror's choice, after
 * what each line does: Bluetooth off until Linux turns it on, WiFi and
 * the LAN9514 powered, the LED and the camera off, no display on HDMI,
 * a good power supply.
 */
static const ExpLine exp_boot[EXP_LINES] = {
	{true, false, false}, /* BT_ON */
	{true, true, false},  /* WL_ON */
	{true, false, false}, /* STATUS_LED */
	{true, true, false},  /* LAN_RUN */
	{false, false, true}, /* HDMI_HPD_N: no display */
	{true, false, false}, /* CAM_GPIO0 */
	{true, false, false}, /* CAM_GPIO1 */
	{false, false, true}, /* PWR_LOW_N: the power is good */
};

/*
 * The display. With no HDMI display connected, a Pi 3 Model B shows its
 * picture on the composite output of its AV jack (Raspberry Pi
 * documentation, config.txt, "Composite video mode": enable_tvout is 1
 * on it, and HDMI is off when no HDMI display is detected), in NTSC
 * (sdtv_mode 0), 480 lines of 720 pixels. The firmware's overscan for
 * that SD mode is 32 pixels on each edge, and the console's frame buffer
 * is the display less the overscan, 656 by 416, of 16 bits per pixel
 * (framebuffer_width, framebuffer_height, framebuffer_depth). A real Pi 3 Model
 * B+ without a display says the same in its boot log: "FB found 1 display(s)",
 * "Registered framebuffer for display 0, size 656x416". max_framebuffers is 1
 * before the Pi 4: one display.
 *
 * disable_overscan=1 takes the firmware's overscan away (Raspberry Pi
 * documentation, legacy config.txt options, "disable_overscan": "the
 * default values of overscan that are set by the firmware"): the frame
 * buffer is then the whole display, 720 by 480, as a real Pi 3 Model B
 * gives it in its command line, "bcm2708_fb.fbwidth=720
 * bcm2708_fb.fbheight=480" (raspberrypi/linux issue 5320).
 *
 * smirror shows no picture: it keeps the settings of the frame buffer,
 * and where it is.
 */
#define DISPLAY_WIDTH 720
#define DISPLAY_HEIGHT 480
#define OVERSCAN 32
#define FB_DEPTH 16
#define FB_SIDE_MAX 4096 /* smirror's limit on a width or a height */
#define FB_ALIGN 4096    /* where the firmware puts its buffers */
#define FB_TAGS_MAX 32   /* frame buffer tags in one buffer */
#define PALETTE_SIZE 256
#define UNSET 0xffffffffu /* a setting nothing documents at boot */

/*
 * The bus address of the RAM, as the VideoCore gives it to the Arm: the
 * alias 0xC0000000 (dma-ranges in the board's device tree).
 */
#define BUS_RAM 0xc0000000u

/* The settings of the display, in the order of the tags' values. */
enum {
	D_WIDTH, /* physical: what the display shows */
	D_HEIGHT,
	D_VWIDTH, /* virtual: the buffer */
	D_VHEIGHT,
	D_DEPTH, /* bits per pixel */
	D_ORDER, /* pixel order: 0 BGR, 1 RGB */
	D_ALPHA, /* alpha mode: 0 enabled, 1 reversed, 2 ignored */
	D_X,     /* virtual offset */
	D_Y,
	D_TOP, /* overscan */
	D_BOTTOM,
	D_LEFT,
	D_RIGHT,
	D_PITCH, /* bytes from a line to the next */
	D_BASE,  /* the buffer: its bus address, 0 when there is none */
	D_SIZE,  /* and its bytes */
	NUM_D
};

/*
 * What the Arm sets and can read back. A palette and blanking the screen
 * change only the picture, which smirror does not show.
 */
typedef struct Display {
	uint32_t v[NUM_D];
} Display;

/* A frame buffer tag of the buffer being answered. */
typedef struct FbTag {
	uint32_t tag;
	uint32_t offset;     /* in the buffer */
	uint32_t value_size; /* its value buffer's */
} FbTag;

struct Raspi3bProperty {
	Machine *machine;
	uint32_t ram_size;
	Raspi3bVcSettings settings;
	char *cmdline;
	uint32_t turbo;
	bool power_known[NUM_DEVICES]; /* set by the Arm since boot */
	ExpLine exp[EXP_LINES];
	bool power_on[NUM_DEVICES];
	bool clock_known[NUM_CLOCKS]; /* a clock's state: documented, or set */
	bool clock_on[NUM_CLOCKS];
	bool domain_known[NUM_DOMAINS]; /* set by the Arm, or documented */
	bool domain_on[NUM_DOMAINS];
	Display display;
	FbTag fb_tags[FB_TAGS_MAX]; /* those of the buffer being answered */
	Raspi3bVchiq *vchiq;        /* the VideoCore's end of VCHIQ */
	unsigned num_fb_tags;
	Value value;              /* the tag being answered */
	Clock clocks[NUM_CLOCKS]; /* last: a read past them leaves the object */
};

/*
 * Every tag that the firmware knows: those of the wiki and those of
 * raspberrypi-firmware.h in Raspberry Pi's Linux, at commit cff533a, the
 * kernel of firmware release 1.20260915. Sorted.
 */
static const uint32_t known_tags[] = {
	0x00000001, 0x00000002, 0x00000003, 0x00008010, 0x00008011, 0x00008012,
	0x00010001, 0x00010002, 0x00010003, 0x00010004, 0x00010005, 0x00010006,
	0x00010007, 0x00020001, 0x00020002, 0x00028001, 0x00030001, 0x00030002,
	0x00030003, 0x00030004, 0x00030005, 0x00030006, 0x00030007, 0x00030008,
	0x00030009, 0x0003000a, 0x0003000b, 0x0003000c, 0x0003000d, 0x0003000e,
	0x0003000f, 0x00030010, 0x00030011, 0x00030012, 0x00030014, 0x00030020,
	0x00030021, 0x00030023, 0x00030030, 0x00030041, 0x00030043, 0x00030045,
	0x00030046, 0x00030047, 0x00030048, 0x00030049, 0x00030050, 0x00030058,
	0x00030064, 0x00030066, 0x00030087, 0x0003008a, 0x00034041, 0x00038001,
	0x00038002, 0x00038003, 0x00038009, 0x00038021, 0x00038030, 0x00038041,
	0x00038042, 0x00038043, 0x00038045, 0x00038049, 0x00038064, 0x00038087,
	0x0003808a, 0x00040001, 0x00040002, 0x00040003, 0x00040004, 0x00040005,
	0x00040006, 0x00040007, 0x00040008, 0x00040009, 0x0004000a, 0x0004000b,
	0x0004000c, 0x0004000d, 0x0004000e, 0x0004000f, 0x00040010, 0x00040013,
	0x00040014, 0x00040016, 0x00040017, 0x00040018, 0x00044003, 0x00044004,
	0x00044005, 0x00044006, 0x00044007, 0x00044009, 0x0004400a, 0x0004400b,
	0x0004400c, 0x0004400d, 0x0004400e, 0x00048001, 0x00048003, 0x00048004,
	0x00048005, 0x00048006, 0x00048007, 0x00048008, 0x00048009, 0x0004800a,
	0x0004800b, 0x0004800c, 0x0004800d, 0x0004800e, 0x0004800f, 0x00048010,
	0x00048013, 0x00048015, 0x00048017, 0x00048019, 0x0004801f, 0x00048020,
	0x00050001, 0x00060001,
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
	/*
	 * Firmware 7fdcd00e and later sets the Arasan controller's EMMC
	 * clock to 200 MHz (U-Boot's bcm2835_sdhci.c history); its range is
	 * not documented.
	 */
	p->clocks[CLOCK_EMMC] = (Clock){CLOCK_FIXED, 200 * MB, 0, 0};
	p->clocks[CLOCK_PWM].kind = CLOCK_UNKNOWN;
	/*
	 * EMMC2, M2MC and PIXEL_BVB are clocks of blocks of the Pi 4's
	 * BCM2711 (its second EMMC controller, its HDMI's pixel clock, its
	 * M2MC), which the BCM2837 does not have: they stay absent, as
	 * DISP, the BCM2712's.
	 */
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
	/*
	 * HDMI's pixel clock: 86.4 MHz, the firmware's minimum that a Pi 3
	 * Model B+ measures when no HDMI mode is set (raspberrypi/linux
	 * issue #7564), up to 154 MHz, that of the highest mode the Pi 3
	 * supports, 1920 by 1200 at 60 Hz with reduced blanking (Raspberry
	 * Pi documentation, legacy video options; VESA's CVT).
	 */
	p->clocks[CLOCK_PIXEL] =
		(Clock){CLOCK_ADJUSTABLE, 86400000, 86400000, 154 * MB};
	/*
	 * The composite output's VEC runs at the one rate that it needs,
	 * 108 MHz (Linux's vc4_vec.c).
	 */
	p->clocks[CLOCK_VEC] = (Clock){CLOCK_FIXED, 108 * MB, 108 * MB, 108 * MB};
	/*
	 * Running at boot: those of the firmware and of the devices it set
	 * up, and V3D, ISP and the VEC, which Linux finds on and turns off
	 * on a Pi 3 Model B+. Nothing documents H.264's and PWM's state.
	 */
	for (unsigned i = 0; i < NUM_CLOCKS; i++) {
		p->clock_on[i] = p->clocks[i].kind != CLOCK_ABSENT;
		p->clock_known[i] = i != CLOCK_H264 && i != CLOCK_PWM;
	}
}

static void reset_display(Display *d, uint32_t edge);

/* The firmware's overscan on each edge of the display. */
static uint32_t overscan(const Raspi3bProperty *p)
{
	return p->settings.overscan_disabled ? 0 : OVERSCAN;
}

void raspi3b_property_reset(Raspi3bProperty *p)
{
	reset_clocks(p);
	reset_display(&p->display, overscan(p));
	raspi3b_vchiq_reset(p->vchiq);
	p->turbo = 0;
	memset(p->power_known, 0, sizeof(p->power_known));
	memset(p->power_on, 0, sizeof(p->power_on));
	/* A domain's state counts once known. */
	memset(p->domain_known, 0, sizeof(p->domain_known));
	p->domain_known[DOMAIN_ARM] = true; /* the Arm runs */
	p->domain_on[DOMAIN_ARM] = true;
	memcpy(p->exp, exp_boot, sizeof(p->exp));
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
	p->vchiq = raspi3b_vchiq_create(m, ram_size);
	if (p->vchiq == NULL || !raspi3b_property_configure(p, &defaults)) {
		raspi3b_property_destroy(p);
		return NULL;
	}
	return p;
}

void raspi3b_property_destroy(Raspi3bProperty *p)
{
	if (p->vchiq != NULL)
		raspi3b_vchiq_destroy(p->vchiq);
	free(p->cmdline);
	free(p);
}

Raspi3bVchiq *raspi3b_property_vchiq(Raspi3bProperty *p)
{
	return p->vchiq;
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

void raspi3b_property_fb_size(const Raspi3bProperty *p, uint32_t *width,
                              uint32_t *height)
{
	*width = DISPLAY_WIDTH - 2 * overscan(p);
	*height = DISPLAY_HEIGHT - 2 * overscan(p);
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

static bool get_firmware_variant(Raspi3bProperty *p, Value *v)
{
	(void)p;
	answer(v, 0, FIRMWARE_VARIANT);
	return true;
}

static bool get_firmware_hash(Raspi3bProperty *p, Value *v)
{
	(void)p;
	for (unsigned i = 0; i < ARRAY_SIZE(firmware_hash); i++)
		answer(v, i, firmware_hash[i]);
	return true;
}

/*
 * Whether the board was short of power or too hot, and slowed down: bits
 * 0 to 3 now, 16 to 19 since boot (Raspberry Pi documentation, vcgencmd
 * get_throttled). The board emulated has a good power supply and never
 * gets hot: 0. Linux asks with 0xffff in the value; its meaning is not
 * documented, and nothing that it could clear is ever set here.
 */
static bool get_throttled(Raspi3bProperty *p, Value *v)
{
	(void)p;
	answer(v, 0, 0);
	return true;
}

/*
 * Where the Arm keeps the virtual GPIOs, a page of its memory that the
 * firmware reads: on the Pi 3 Model B, the activity LED (Linux's
 * gpio-bcm-virt.c). The answer is 0 for success. smirror has no LED to
 * light, so it reads nothing there.
 */
static bool set_gpiovirtbuf(Raspi3bProperty *p, Value *v)
{
	if (!needs(p, v, 4))
		return false;
	answer(v, 0, 0);
	return true;
}

/*
 * NOTIFY_REBOOT, of no value: Linux is about to reboot or halt (its
 * drivers/firmware/raspberrypi.c, from a reboot notifier and at
 * shutdown). Linux's commit b80ec7c0 says why it tells the firmware: the
 * GPIO expander must be as at power on after a reboot, or the boot ROM's
 * network boot fails on a Pi 3B+, so the firmware puts it back. The rest
 * is the reset's to restore.
 */
static bool notify_reboot(Raspi3bProperty *p, Value *v)
{
	(void)v;
	memcpy(p->exp, exp_boot, sizeof(p->exp));
	return true;
}

/* The expander's line of the request, or NULL when it stopped. */
static ExpLine *exp_line(Raspi3bProperty *p, const Value *v, uint32_t len)
{
	uint32_t gpio;

	if (!needs(p, v, len))
		return NULL;
	gpio = arg(v, 0);
	if (gpio - EXP_BASE >= EXP_LINES) {
		stop(p,
		     "tag 0x%08" PRIx32 ": GPIO %" PRIu32 " is not emulated: smirror "
		     "has the expander's, 128 to 135",
		     v->tag, gpio);
		return NULL;
	}
	return &p->exp[gpio - EXP_BASE];
}

static bool exp_state(const ExpLine *line)
{
	return line->output ? line->state : line->level;
}

/* {gpio, direction, polarity, term_en, term_pull_up}: 1 is an output. */
static bool get_gpio_config(Raspi3bProperty *p, Value *v)
{
	const ExpLine *line = exp_line(p, v, 4);

	if (line == NULL)
		return false;
	answer(v, 0, 0);
	answer(v, 1, line->output);
	answer(v, 2, 0); /* polarity */
	answer(v, 3, 0); /* no termination */
	answer(v, 4, 0);
	return true;
}

/*
 * {gpio, direction, polarity, term_en, term_pull_up, state}: Linux keeps
 * the polarity it reads, 0, and turns termination off, when term_pull_up
 * does not count. Anything else is not emulated.
 */
static bool set_gpio_config(Raspi3bProperty *p, Value *v)
{
	ExpLine *line = exp_line(p, v, 24);
	uint32_t direction, polarity, term_en, state;

	if (line == NULL)
		return false;
	direction = arg(v, 1);
	polarity = arg(v, 2);
	term_en = arg(v, 3);
	state = arg(v, 5);
	if (direction > 1 || polarity != 0 || term_en != 0) {
		stop(p,
		     "GPIO %" PRIu32 ": direction %" PRIu32 ", polarity %" PRIu32
		     " or termination %" PRIu32 " is not emulated",
		     arg(v, 0), direction, polarity, term_en);
		return false;
	}
	line->output = direction == 1;
	if (line->output)
		line->state = state != 0;
	answer(v, 0, 0);
	return true;
}

/* {gpio, state}. */
static bool get_gpio_state(Raspi3bProperty *p, Value *v)
{
	const ExpLine *line = exp_line(p, v, 4);

	if (line == NULL)
		return false;
	answer(v, 0, 0);
	answer(v, 1, exp_state(line));
	return true;
}

/* {gpio, state}, of an output: what setting an input does is not known. */
static bool set_gpio_state(Raspi3bProperty *p, Value *v)
{
	ExpLine *line = exp_line(p, v, 8);

	if (line == NULL)
		return false;
	if (!line->output) {
		stop(p, "GPIO %" PRIu32 ": setting an input is not emulated",
		     arg(v, 0));
		return false;
	}
	line->state = arg(v, 1) != 0;
	answer(v, 0, 0);
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

/*
 * Every clock that exists, as the wiki asks: its parent, then its id.
 * The firmware's clocks come from PLLs that are not among them: each is
 * a root, of parent 0.
 */
static bool get_clocks(Raspi3bProperty *p, Value *v)
{
	unsigned n = 0;

	for (uint32_t id = 1; id < NUM_CLOCKS; id++) {
		if (p->clocks[id].kind == CLOCK_ABSENT)
			continue;
		answer(v, n++, 0);
		answer(v, n++, id);
	}
	return true;
}

/*
 * The clock of a state's request, 0 for one that does not exist; the
 * answer gives its id, and bit 1 of the state, 'absent', for clock 0.
 */
static uint32_t clock_state_of(Raspi3bProperty *p, Value *v)
{
	uint32_t id = arg(v, 0);

	answer(v, 0, id);
	if (id >= NUM_CLOCKS || p->clocks[id].kind == CLOCK_ABSENT) {
		answer(v, 1, STATE_ABSENT);
		return 0;
	}
	return id;
}

static bool get_clock_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id;

	if (!needs(p, v, 4))
		return false;
	if ((id = clock_state_of(p, v)) == 0)
		return true;
	if (!p->clock_known[id]) {
		stop(p, "the state of clock %" PRIu32 " at boot is not documented", id);
		return false;
	}
	answer(v, 1, p->clock_on[id]);
	return true;
}

/*
 * The clocks that smirror's devices run on keep running: turning one of
 * them off stops the machine. The others change nothing else.
 */
static bool set_clock_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id, state;

	if (!needs(p, v, 8))
		return false;
	state = arg(v, 1);
	if (state & ~STATE_ON) {
		stop(p, "clock state 0x%" PRIx32 ": reserved bits are set", state);
		return false;
	}
	if ((id = clock_state_of(p, v)) == 0)
		return true;
	if (state == 0 &&
	    (id == CLOCK_EMMC || id == CLOCK_UART || id == CLOCK_ARM ||
	     id == CLOCK_CORE || id == CLOCK_SDRAM)) {
		stop(p,
		     "turning clock %" PRIu32 " off is not emulated: devices run on "
		     "it",
		     id);
		return false;
	}
	p->clock_on[id] = state;
	p->clock_known[id] = true;
	answer(v, 1, state);
	return true;
}

/* The domain of the request, or false when it stopped the machine. */
static bool domain_of(Raspi3bProperty *p, const Value *v, uint32_t len)
{
	if (!needs(p, v, len))
		return false;
	if (arg(v, 0) >= NUM_DOMAINS) {
		stop(p, "power domain %" PRIu32 " is not documented", arg(v, 0));
		return false;
	}
	return true;
}

/*
 * The power domains of the newer tags. Linux asks for the ARM's, which is
 * on, to learn whether the firmware has them. Turning a domain on or off
 * changes nothing else in the emulation, but for the ARM's, which stops
 * the machine.
 */
static bool get_domain_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id;

	if (!domain_of(p, v, 4))
		return false;
	id = arg(v, 0);
	if (!p->domain_known[id]) {
		stop(p,
		     "the state of power domain %" PRIu32 " at boot is not "
		     "documented",
		     id);
		return false;
	}
	answer(v, 0, id);
	answer(v, 1, p->domain_on[id]);
	return true;
}

static bool set_domain_state(Raspi3bProperty *p, Value *v)
{
	uint32_t id, state;

	if (!domain_of(p, v, 8))
		return false;
	id = arg(v, 0);
	state = arg(v, 1);
	if (state > 1) {
		stop(p, "power domain state %" PRIu32 " is not documented", state);
		return false;
	}
	if (id == DOMAIN_ARM && state == 0) {
		stop(p, "turning the ARM's power domain off is not emulated");
		return false;
	}
	p->domain_known[id] = true;
	p->domain_on[id] = state;
	answer(v, 0, id);
	answer(v, 1, state);
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

/* --- The frame buffer ----------------------------------------------------- */

/* Bits 15:12 of a frame buffer tag: get, test (0x4) or set (0x8). */
#define FB_KIND(tag) ((tag) & 0xf000u)
#define FB_GET 0x0000u
#define FB_TEST 0x4000u

#define TAG_ALLOCATE 0x00040001u
#define TAG_RELEASE 0x00048001u
#define TAG_BLANK 0x00040002u
#define TAG_SET_PITCH 0x00048008u
#define TAG_SET_PALETTE 0x0004800bu
#define TAG_NUM_DISPLAYS 0x00040013u
#define TAG_DISPLAY_SETTINGS 0x00040014u
#define TAG_SET_DISPLAY_NUM 0x00048013u

/*
 * The settings that tags get, test and set: 0x00040000, 0x00044000 and
 * 0x00048000 plus their id, with the values of the wiki. The pitch has
 * a get tag, and a set tag that Linux's bcm2708_fb uses; no test tag.
 */
typedef struct FbSetting {
	uint32_t id;
	unsigned first, count; /* the display's values */
	const char *name;
} FbSetting;

static const FbSetting fb_settings[] = {
	{0x003, D_WIDTH, 2, "physical size"}, {0x004, D_VWIDTH, 2, "virtual size"},
	{0x005, D_DEPTH, 1, "depth"},         {0x006, D_ORDER, 1, "pixel order"},
	{0x007, D_ALPHA, 1, "alpha mode"},    {0x008, D_PITCH, 1, "pitch"},
	{0x009, D_X, 2, "virtual offset"},    {0x00a, D_TOP, 4, "overscan"},
};

/* The setting of a known tag, or NULL. */
static const FbSetting *fb_setting(uint32_t tag)
{
	if ((tag & 0xffff0000u) != 0x00040000u)
		return NULL;
	for (size_t i = 0; i < ARRAY_SIZE(fb_settings); i++)
		if (fb_settings[i].id == (tag & 0xfffu))
			return &fb_settings[i];
	return NULL;
}

/* Whether a known tag is answered with the frame buffer's. */
static bool fb_tag(uint32_t tag)
{
	switch (tag) {
	case TAG_ALLOCATE:
	case TAG_RELEASE:
	case TAG_BLANK:
	case TAG_SET_PALETTE:
	case TAG_NUM_DISPLAYS:
	case TAG_DISPLAY_SETTINGS:
	case TAG_SET_DISPLAY_NUM:
		return true;
	default:
		return fb_setting(tag) != NULL;
	}
}

/*
 * The pitch: smirror's choice, as nothing documents the firmware's, is
 * a line of the virtual width's pixels, without padding.
 */
static uint32_t natural_pitch(const Display *d)
{
	return d->v[D_VWIDTH] * d->v[D_DEPTH] / 8;
}

/* The display at boot: the console's frame buffer, not yet allocated. */
static void reset_display(Display *d, uint32_t edge)
{
	memset(d, 0, sizeof(*d));
	d->v[D_WIDTH] = DISPLAY_WIDTH - 2 * edge;
	d->v[D_HEIGHT] = DISPLAY_HEIGHT - 2 * edge;
	d->v[D_VWIDTH] = d->v[D_WIDTH];
	d->v[D_VHEIGHT] = d->v[D_HEIGHT];
	d->v[D_DEPTH] = FB_DEPTH;
	d->v[D_ORDER] = UNSET;
	d->v[D_ALPHA] = UNSET;
	d->v[D_TOP] = edge;
	d->v[D_BOTTOM] = edge;
	d->v[D_LEFT] = edge;
	d->v[D_RIGHT] = edge;
	d->v[D_PITCH] = natural_pitch(d);
}

/* Prepare p->value for the tag t of buf. */
static Value *begin(Raspi3bProperty *p, uint8_t *buf, uint32_t offset,
                    uint32_t value_size)
{
	Value *v = &p->value;

	v->tag = load_le32(buf + offset);
	v->request = buf + offset + 12;
	v->request_len = value_size;
	v->answer_len = 0;
	return v;
}

/* Write the answer in p->value to the tag at offset in buf. */
static void put_answer(Raspi3bProperty *p, uint8_t *buf, uint32_t offset,
                       uint32_t value_size)
{
	const Value *v = &p->value;

	/* The answer is cut to the buffer; its length says how long it is. */
	memcpy(buf + offset + 12, v->answer,
	       v->answer_len < value_size ? v->answer_len : value_size);
	store_le32(buf + offset + 8, TAG_RESPONSE | v->answer_len);
}

/* The wiki: offset 0-255, length 1-256; 1 answers an invalid palette. */
static bool palette_valid(const Value *v)
{
	uint32_t offset = arg(v, 0), length = arg(v, 1);

	return length >= 1 && length <= PALETTE_SIZE &&
	       offset <= PALETTE_SIZE - length;
}

/* Take the values of a set or test tag into next. */
static bool fb_request(Raspi3bProperty *p, Display *next, const Value *v)
{
	const FbSetting *s = fb_setting(v->tag);

	if (s != NULL) {
		if (FB_KIND(v->tag) == FB_GET)
			return true;
		if (!needs(p, v, 4 * s->count))
			return false;
		for (unsigned i = 0; i < s->count; i++)
			next->v[s->first + i] = arg(v, i);
		return true;
	}
	switch (v->tag) {
	case TAG_BLANK:
		if (!needs(p, v, 4))
			return false;
		if (arg(v, 0) > 1) {
			stop(p, "blank state 0x%" PRIx32 ": reserved bits are set",
			     arg(v, 0));
			return false;
		}
		return true;
	case TAG_SET_PALETTE:
		if (!needs(p, v, 8))
			return false;
		return !palette_valid(v) || needs(p, v, 4 * (2 + arg(v, 1)));
	case TAG_ALLOCATE:
		return needs(p, v, 4);
	case TAG_DISPLAY_SETTINGS:
	case TAG_SET_DISPLAY_NUM:
		if (!needs(p, v, 4))
			return false;
		if (arg(v, 0) != 0) {
			stop(p,
			     "tag 0x%08" PRIx32 ": display %" PRIu32 " is not emulated: "
			     "smirror has one display, 0",
			     v->tag, arg(v, 0));
			return false;
		}
		return true;
	default:
		return true; /* release, the number of displays: no request */
	}
}

/*
 * Check the settings of next, which were those of old before the tags
 * changed them; give next its pitch. What nothing documents stops: a
 * size of 0 or larger than smirror's limit, a depth that config.txt does
 * not give, a pixel order or an alpha mode that the wiki does not list,
 * a display that leaves the buffer or is all overscan, a pitch shorter
 * than a line.
 */
static bool fb_check(Raspi3bProperty *p, Display *next, const Display *old,
                     bool pitch_set)
{
	const uint32_t *d = next->v;
	uint32_t pitch;

	for (unsigned i = D_WIDTH; i <= D_VHEIGHT; i++) {
		if (d[i] == 0 || d[i] > FB_SIDE_MAX) {
			stop(p,
			     "a frame buffer of %" PRIu32 " by %" PRIu32 ", %" PRIu32
			     " by %" PRIu32 " virtual, is not emulated",
			     d[D_WIDTH], d[D_HEIGHT], d[D_VWIDTH], d[D_VHEIGHT]);
			return false;
		}
	}
	if (d[D_DEPTH] != 8 && d[D_DEPTH] != 16 && d[D_DEPTH] != 24 &&
	    d[D_DEPTH] != 32) {
		stop(p, "a depth of %" PRIu32 " bits per pixel is not emulated",
		     d[D_DEPTH]);
		return false;
	}
	if (d[D_ORDER] > 1 && d[D_ORDER] != UNSET) {
		stop(p, "pixel order %" PRIu32 " is not in the wiki", d[D_ORDER]);
		return false;
	}
	if (d[D_ALPHA] > 2 && d[D_ALPHA] != UNSET) {
		stop(p, "alpha mode %" PRIu32 " is not in the wiki", d[D_ALPHA]);
		return false;
	}
	if (d[D_WIDTH] > d[D_VWIDTH] || d[D_X] > d[D_VWIDTH] - d[D_WIDTH] ||
	    d[D_HEIGHT] > d[D_VHEIGHT] || d[D_Y] > d[D_VHEIGHT] - d[D_HEIGHT]) {
		stop(p,
		     "a virtual offset of (%" PRIu32 ", %" PRIu32 ") is not emulated: "
		     "the display leaves the buffer",
		     d[D_X], d[D_Y]);
		return false;
	}
	if ((uint64_t)d[D_TOP] + d[D_BOTTOM] >= DISPLAY_HEIGHT ||
	    (uint64_t)d[D_LEFT] + d[D_RIGHT] >= DISPLAY_WIDTH) {
		stop(p,
		     "an overscan of %" PRIu32 ", %" PRIu32 ", %" PRIu32 ", %" PRIu32
		     " is not emulated: nothing is left of the display",
		     d[D_TOP], d[D_BOTTOM], d[D_LEFT], d[D_RIGHT]);
		return false;
	}
	pitch = natural_pitch(next);
	if (pitch_set) {
		if (d[D_PITCH] < pitch || d[D_PITCH] > FB_SIDE_MAX * 4) {
			stop(p, "a pitch of %" PRIu32 " bytes is not emulated", d[D_PITCH]);
			return false;
		}
	} else if (d[D_VWIDTH] != old->v[D_VWIDTH] ||
	           d[D_DEPTH] != old->v[D_DEPTH]) {
		next->v[D_PITCH] = pitch;
	}
	return true;
}

/*
 * Whether the firmware allocates as asked: the wiki leaves to it which
 * alignments it supports; smirror takes 0 and the powers of 2. A buffer
 * of the Arm's own is supported, below.
 */
static bool alignment_supported(const Value *v)
{
	uint32_t align = arg(v, 0);

	return (v->request_len >= 8 && arg(v, 1) != 0) ||
	       (align & (align - 1)) == 0;
}

/*
 * Allocate the buffer of next, of need bytes. A request of two words,
 * the second not 0, gives a buffer of the Arm's memory, its bus address
 * and size: Linux's bcm2708_fb tries it first, and takes the old way
 * when the answer is another buffer; nothing documents how the firmware
 * tells it from an alignment. The firmware takes it, if large enough. An
 * alignment gets a buffer at the top of the VideoCore's memory that the
 * Arm sees, below the peripherals.
 */
static bool fb_allocate(Raspi3bProperty *p, Display *next, const Value *v,
                        uint32_t need)
{
	uint32_t arm = raspi3b_property_arm_memory(p);
	uint32_t align = arg(v, 0);
	uint32_t size = v->request_len >= 8 ? arg(v, 1) : 0;
	uint64_t top = (uint64_t)p->settings.total_mem_mb * MIB;
	uint64_t base;

	if (size != 0) {
		uint32_t at = align & BUS_ALIAS_MASK;

		if (size < need || (uint64_t)at + size > arm) {
			stop(p,
			     "a frame buffer of %" PRIu32 " bytes at 0x%08" PRIx32
			     " is not emulated: it must hold %" PRIu32
			     " bytes, in the Arm's memory",
			     size, align, need);
			return false;
		}
		next->v[D_BASE] = align;
		next->v[D_SIZE] = size;
		return true;
	}
	if (top > RASPI3B_RAM_SIZE)
		top = RASPI3B_RAM_SIZE;
	if (align < FB_ALIGN)
		align = FB_ALIGN;
	base = (top - need) & ~(uint64_t)(align - 1);
	if (top < (uint64_t)arm + need || base < arm) {
		stop(p,
		     "a frame buffer of %" PRIu32 " bytes is not emulated: it does "
		     "not fit in the VideoCore's memory",
		     need);
		return false;
	}
	next->v[D_BASE] = BUS_RAM | (uint32_t)base;
	next->v[D_SIZE] = need;
	return true;
}

/* Answer the frame buffer tag v from the settings of d. */
static bool fb_answer(Raspi3bProperty *p, const Display *d, Value *v)
{
	const FbSetting *s = fb_setting(v->tag);

	if (s != NULL) {
		for (unsigned i = 0; i < s->count; i++) {
			if (d->v[s->first + i] == UNSET) {
				stop(p, "the %s at boot is not documented", s->name);
				return false;
			}
			answer(v, i, d->v[s->first + i]);
		}
		return true;
	}
	switch (v->tag) {
	case TAG_ALLOCATE:
		answer(v, 0, d->v[D_BASE]);
		answer(v, 1, d->v[D_SIZE]);
		break;
	case TAG_BLANK:
		answer(v, 0, arg(v, 0));
		break;
	case TAG_SET_PALETTE:
		answer(v, 0, !palette_valid(v));
		break;
	case TAG_NUM_DISPLAYS:
		answer(v, 0, 1);
		break;
	case TAG_DISPLAY_SETTINGS:
		/*
		 * Linux's struct vc4_display_settings_t: the display, its size,
		 * depth, pitch, virtual size and offset, and the buffer's bus
		 * address, in a word as the VideoCore's addresses are.
		 */
		answer(v, 0, 0);
		answer(v, 1, d->v[D_WIDTH]);
		answer(v, 2, d->v[D_HEIGHT]);
		answer(v, 3, d->v[D_DEPTH]);
		answer(v, 4, d->v[D_PITCH]);
		answer(v, 5, d->v[D_VWIDTH]);
		answer(v, 6, d->v[D_VHEIGHT]);
		answer(v, 7, d->v[D_X]);
		answer(v, 8, d->v[D_Y]);
		answer(v, 9, d->v[D_BASE]);
		break;
	case TAG_SET_DISPLAY_NUM:
		answer(v, 0, 0);
		break;
	default:
		break; /* release: no answer */
	}
	return true;
}

/*
 * Answer the frame buffer tags of buf as the wiki says, in one operation:
 * the values of set and test tags replace the settings in a copy; the
 * copy is checked, its buffer allocated or released; every tag is
 * answered from the copy, which becomes the settings unless the tags
 * only tested them. Without an allocation, settings that need a larger
 * buffer than the one allocated change nothing. Twice the same tag, or
 * test tags with others, which the wiki forbids, stop the machine.
 */
static bool fb_finish(Raspi3bProperty *p, uint8_t *buf)
{
	Display next = p->display;
	const FbTag *allocate = NULL;
	bool tests = false, others = false, pitch_set = false, release = false;
	uint32_t need;

	for (unsigned i = 0; i < p->num_fb_tags; i++) {
		const FbTag *t = &p->fb_tags[i];

		for (unsigned j = 0; j < i; j++) {
			if (p->fb_tags[j].tag == t->tag) {
				stop(p, "tag 0x%08" PRIx32 " twice in a buffer is not emulated",
				     t->tag);
				return false;
			}
		}
		if (FB_KIND(t->tag) == FB_TEST)
			tests = true;
		else
			others = true;
		if (t->tag == TAG_ALLOCATE)
			allocate = t;
		release |= t->tag == TAG_RELEASE;
		pitch_set |= t->tag == TAG_SET_PITCH;
	}
	if (tests && others) {
		stop(p, "test tags with get or set tags in a buffer are not emulated");
		return false;
	}
	if (allocate != NULL && release) {
		stop(p, "allocating and releasing the frame buffer at once is not "
		        "emulated");
		return false;
	}
	for (unsigned i = 0; i < p->num_fb_tags; i++) {
		const FbTag *t = &p->fb_tags[i];

		if (!fb_request(p, &next, begin(p, buf, t->offset, t->value_size)))
			return false;
	}
	if (!fb_check(p, &next, &p->display, pitch_set))
		return false;
	need = next.v[D_PITCH] * next.v[D_VHEIGHT];
	if (allocate != NULL) {
		const Value *v = begin(p, buf, allocate->offset, allocate->value_size);

		if (!alignment_supported(v))
			allocate = NULL; /* the wiki: the buffer does not change */
		else if (!fb_allocate(p, &next, v, need))
			return false;
	}
	if (release) {
		next.v[D_BASE] = 0;
		next.v[D_SIZE] = 0;
	} else if (allocate == NULL && p->display.v[D_SIZE] != 0 &&
	           need > p->display.v[D_SIZE]) {
		next = p->display;
	}
	for (unsigned i = 0; i < p->num_fb_tags; i++) {
		const FbTag *t = &p->fb_tags[i];

		if (!fb_answer(p, &next, begin(p, buf, t->offset, t->value_size)))
			return false;
		put_answer(p, buf, t->offset, t->value_size);
	}
	if (!tests)
		p->display = next;
	return true;
}

/*
 * VCHIQ_INIT: the VideoCore starts its end of VCHIQ on the memory at the
 * request's bus address, and answers 0 (Linux's vchiq_arm.c takes any
 * other answer for a failure).
 */
static bool vchiq_init(Raspi3bProperty *p, Value *v)
{
	if (!needs(p, v, 4) || !raspi3b_vchiq_init(p->vchiq, arg(v, 0)))
		return false;
	answer(v, 0, 0);
	return true;
}

typedef struct Handler {
	uint32_t tag;
	bool (*fn)(Raspi3bProperty *p, Value *v);
} Handler;

static const Handler handlers[] = {
	{0x00000001, get_firmware_revision},
	{0x00000002, get_firmware_variant},
	{0x00000003, get_firmware_hash},
	{0x00030046, get_throttled},
	{0x00048020, set_gpiovirtbuf},
	{0x00030041, get_gpio_state},
	{0x00038041, set_gpio_state},
	{0x00030043, get_gpio_config},
	{0x00038043, set_gpio_config},
	{0x00030048, notify_reboot},
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
	{0x00010007, get_clocks},
	{0x00030001, get_clock_state},
	{0x00038001, set_clock_state},
	{0x00030030, get_domain_state},
	{0x00038030, set_domain_state},
	{0x00048010, vchiq_init},
};

/*
 * Answer the tag at offset in buf, whose value buffer is value_size
 * bytes. Returns false when it stopped the machine.
 */
static bool answer_tag(Raspi3bProperty *p, uint8_t *buf, uint32_t offset,
                       uint32_t value_size)
{
	uint32_t tag = load_le32(buf + offset);
	uint32_t code = load_le32(buf + offset + 8);
	const Handler *h = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(handlers); i++)
		if (handlers[i].tag == tag)
			h = &handlers[i];
	if (!known(tag))
		return true; /* the wiki: unknown tags are ignored */
	if (h == NULL && !fb_tag(tag)) {
		stop(p, "tag 0x%08" PRIx32 " is not emulated", tag);
		return false;
	}
	/*
	 * The wiki: b31 clear for a request, b30-b0 reserved. U-Boot puts
	 * the request's length there (BCM2835_MBOX_INIT_TAG), and the
	 * firmware answers it: the reserved bits are ignored.
	 */
	if (code & TAG_RESPONSE) {
		stop(p, "a tag request code of 0x%08" PRIx32 " is not documented",
		     code);
		return false;
	}
	if (h == NULL) {
		/* Answered with the others, once the buffer is read. */
		if (p->num_fb_tags == FB_TAGS_MAX) {
			stop(p,
			     "more than %u frame buffer tags in a buffer are not "
			     "emulated",
			     FB_TAGS_MAX);
			return false;
		}
		p->fb_tags[p->num_fb_tags++] = (FbTag){tag, offset, value_size};
		return true;
	}
	if (!h->fn(p, begin(p, buf, offset, value_size)))
		return false;
	put_answer(p, buf, offset, value_size);
	return true;
}

/*
 * Answer the tags of buf, size bytes. Returns the code of the buffer's
 * response, or 0 when it stopped the machine.
 */
static uint32_t answer_buffer(Raspi3bProperty *p, uint8_t *buf, uint32_t size)
{
	uint32_t offset = 8;

	p->num_fb_tags = 0;
	while (offset + 4 <= size) {
		uint32_t tag = load_le32(buf + offset);
		uint32_t value_size;

		if (tag == END_TAG)
			return fb_finish(p, buf) ? RESPONSE_OK : 0;
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
