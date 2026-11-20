/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The BCM2835's clock manager; see bcm2835_cprman.h. The register
 * offsets and fields not in "BCM2835 ARM Peripherals" are those of
 * Linux's drivers/clk/bcm/clk-bcm2835.c.
 *
 * A PLL multiplies the crystal by NDIV + FRAC / 2^20 (A2W_PLLx_CTRL bits
 * 9:0, A2W_PLLx_FRAC bits 19:0), twice that with the feedback
 * pre-divider of A2W_PLLx_ANA1 (bit 14, bit 11 for PLLH), and divides
 * by PDIV (A2W_PLLx_CTRL bits 14:12). It runs while it is powered
 * (A2W_PLLx_CTRL bit 16, PWRDN, clear) and out of analog reset (CM_PLLx
 * bit 8, ANARST, clear), and CM_LOCK shows it locked. Each channel
 * divides its PLL by bits 7:0 of its A2W register, 256 for 0; bit 8
 * disables it, and its HOLD bit in CM_PLLx holds it.
 *
 * A clock generator runs while ENAB is set and KILL clear, and divides
 * its source by DIVI, or with MASH by DIVI + DIVF / 4096, as many of
 * those bits as the generator has.
 *
 * What nothing documents, this model chooses:
 *  - A write without the password does nothing, as in the power manager.
 *  - A PLL locks at once, and BUSY follows ENAB and KILL at once: no
 *    document gives the time a PLL takes to lock, or what the last cycle
 *    of a stopping clock takes.
 *  - A new channel divisor takes effect when it is written, LOAD or not.
 *  - Control registers keep bits 10:0 but BUSY, as the general purpose
 *    clocks have them; the others keep bits 23:0, below the password.
 *  - The firmware's state: what a Raspberry Pi 3 Model B shows in
 *    Linux's clk_summary (PLLC and PLLD at 2 GHz; PLLC_CORE0 at 1 GHz;
 *    PLLD_PER and PLLD_CORE at 500 MHz; UART at 47999625 Hz, GP1 at
 *    25 MHz and SDRAM at PLLD_CORE / 3 from them; GP2 at 32768 Hz from
 *    the crystal), and TSENS at 1.92 MHz, the crystal / 10, as a
 *    Raspberry Pi Model B's shows it (the firmware runs the temperature
 *    sensor); with them, the rates of config.txt's defaults: the ARM
 *    at 1.2 GHz from PLLB at 2.4 GHz, the VPU at 250 MHz as enable_uart
 *    sets it, H264 and ISP at 400 MHz, and EMMC at 200 MHz from
 *    PLLD_PER. As Linux sets them, the PLLs above 1.75 GHz use the
 *    pre-divider, and running PLLs have PRST_DISABLE set. PLLA and PLLH
 *    are powered down and in reset, and the other channels and clocks
 *    are off.
 */

#include "hw/misc/bcm2835_cprman.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PASSWORD 0x5a000000u
#define PASSWORD_MASK 0xff000000u
#define DATA_MASK 0x00ffffffu
#define NUM_WORDS (BCM2835_CPRMAN_MMIO_SIZE / 4)

/* Clock generators. */
#define CTL_MASK 0x0000077fu /* bits 10:0, but BUSY */
#define CTL_SRC 0x0000000fu
#define CTL_ENAB (1u << 4)
#define CTL_KILL (1u << 5)
#define CTL_BUSY (1u << 7)
#define CTL_MASH_SHIFT 9
#define DIV_FRAC_BITS 12

/* PLLs and their channels. */
#define CM_ANARST (1u << 8)
#define A2W_NDIV 0x000003ffu
#define A2W_PDIV_SHIFT 12
#define A2W_PDIV 0x7u
#define A2W_PWRDN (1u << 16)
#define A2W_PRST_DISABLE (1u << 17)
#define FRAC_MASK 0x000fffffu
#define FRAC_BITS 20
#define CHANNEL_DIV 0x000000ffu
#define CHANNEL_DISABLE (1u << 8)
#define ANA1_PREDIV (1u << 14)
#define ANA1_PREDIV_PLLH (1u << 11)

enum {
	REG_TCNTCTL = 0x0c0,
	REG_TCNTCNT = 0x0c4,
	REG_OSCCOUNT = 0x100,
	REG_LOCK = 0x114,
	REG_DSI0HSCK = 0x120,
	REG_XOSC_CTRL = 0x1190,
};

typedef enum Kind {
	KIND_NONE = 0,
	KIND_CTL,
	KIND_LOCK,
	KIND_COUNTER, /* the frequency counter */
	KIND_PLAIN    /* keeps bits 23:0 and does nothing else */
} Kind;

typedef enum Pll { PLLA, PLLB, PLLC, PLLD, PLLH, NUM_PLLS } Pll;

typedef struct PllInfo {
	uint16_t cm, ctrl, ana, frac; /* ana: ANA0; ANA1 follows */
	uint32_t prediv;
} PllInfo;

static const PllInfo plls[NUM_PLLS] = {
	[PLLA] = {0x104, 0x1100, 0x1010, 0x1200, ANA1_PREDIV},
	[PLLB] = {0x170, 0x11e0, 0x10f0, 0x12e0, ANA1_PREDIV},
	[PLLC] = {0x108, 0x1120, 0x1030, 0x1220, ANA1_PREDIV},
	[PLLD] = {0x10c, 0x1140, 0x1050, 0x1240, ANA1_PREDIV},
	[PLLH] = {0x110, 0x1160, 0x1070, 0x1260, ANA1_PREDIV_PLLH},
};

typedef enum Channel {
	NO_CHANNEL, /* ground, a test input or a DSI PHY: no rate here */
	PLLA_DSI0,
	PLLA_CORE,
	PLLA_PER,
	PLLA_CCP2,
	PLLB_ARM,
	PLLC_CORE2,
	PLLC_CORE1,
	PLLC_PER,
	PLLC_CORE0,
	PLLD_DSI0,
	PLLD_CORE,
	PLLD_PER,
	PLLD_DSI1,
	PLLH_AUX,
	PLLH_RCAL,
	PLLH_PIX,
	NUM_CHANNELS
} Channel;

typedef struct ChannelInfo {
	Pll pll;
	uint16_t a2w;
	uint32_t hold; /* its bit in CM_PLLx, 0 for none */
} ChannelInfo;

static const ChannelInfo channels[NUM_CHANNELS] = {
	[PLLA_DSI0] = {PLLA, 0x1300, 1u << 1},
	[PLLA_CORE] = {PLLA, 0x1400, 1u << 5},
	[PLLA_PER] = {PLLA, 0x1500, 1u << 7},
	[PLLA_CCP2] = {PLLA, 0x1600, 1u << 3},
	[PLLB_ARM] = {PLLB, 0x13e0, 1u << 1},
	[PLLC_CORE2] = {PLLC, 0x1320, 1u << 5},
	[PLLC_CORE1] = {PLLC, 0x1420, 1u << 3},
	[PLLC_PER] = {PLLC, 0x1520, 1u << 7},
	[PLLC_CORE0] = {PLLC, 0x1620, 1u << 1},
	[PLLD_DSI0] = {PLLD, 0x1340, 1u << 1},
	[PLLD_CORE] = {PLLD, 0x1440, 1u << 5},
	[PLLD_PER] = {PLLD, 0x1540, 1u << 7},
	[PLLD_DSI1] = {PLLD, 0x1640, 1u << 3},
	[PLLH_AUX] = {PLLH, 0x1360, 0},
	[PLLH_RCAL] = {PLLH, 0x1460, 0},
	[PLLH_PIX] = {PLLH, 0x1560, 0},
};

/* The sources of a generator, 0 to 15: 1 is the crystal. */
#define SRC_OSC 1u
#define NUM_SOURCES 16

typedef enum Parents { PARENTS_PER, PARENTS_VPU } Parents;

static const Channel parents[][NUM_SOURCES] = {
	[PARENTS_PER] = {[4] = PLLA_PER, PLLC_PER, PLLD_PER, PLLH_AUX},
	[PARENTS_VPU] = {[4] = PLLA_CORE,
                     PLLC_CORE0,
                     PLLD_CORE,
                     PLLH_AUX,
                     PLLC_CORE1,
                     PLLC_CORE2},
};

/* A generator: its control register, and its divisor 4 bytes on. */
typedef struct Generator {
	const char *name;
	uint16_t ctl;
	Parents parents;
	unsigned int_bits, frac_bits;
} Generator;

/* The generators of the modelled devices, in Bcm2835CprmanClock order. */
static const Generator watched[BCM2835_CPRMAN_NUM_CLOCKS] = {
	[BCM2835_CPRMAN_UART] = {"UART", 0x0f0, PARENTS_PER, 10, 12},
	[BCM2835_CPRMAN_EMMC] = {"EMMC", 0x1c0, PARENTS_PER, 4, 8},
	[BCM2835_CPRMAN_VPU] = {"VPU", 0x008, PARENTS_VPU, 12, 8},
};

/* Generators that the firmware's state sets. */
enum {
	REG_PERIICTL = 0x020,
	REG_H264CTL = 0x028,
	REG_ISPCTL = 0x030,
	REG_GP1CTL = 0x078,
	REG_GP2CTL = 0x080,
	REG_TSENSCTL = 0x0e0,
	REG_SDCCTL = 0x1a8,
};

/*
 * The control registers of the other generators that clk-bcm2835.c knows
 * on the BCM2835 family: PERI_IMAGE (a gate, without a divisor), H264,
 * ISP, CAM0, CAM1, DSI0E, DSI0P, DPI, GP0, GP1, GP2, HSM, OTP, PCM, PWM,
 * SLIM, SMI, TEC, TSENS, TIMER, DSI1E, DSI1P, DFT, SDRAM and AVEO.
 */
static const uint16_t others[] = {
	0x020, 0x028, 0x030, 0x040, 0x048, 0x058, 0x060, 0x068, 0x070,
	0x078, 0x080, 0x088, 0x090, 0x098, 0x0a0, 0x0a8, 0x0b0, 0x0c8,
	0x0e0, 0x0e8, 0x158, 0x160, 0x168, 0x1a8, 0x1b8,
};

#define NUM_OTHERS (sizeof(others) / sizeof(others[0]))

struct Bcm2835Cprman {
	Machine *machine;
	uint8_t kind[NUM_WORDS];
	uint32_t regs[NUM_WORDS];
	uint64_t boot_rate[BCM2835_CPRMAN_NUM_CLOCKS];
};

static uint32_t reg(const Bcm2835Cprman *c, uint16_t offset)
{
	return c->regs[offset / 4];
}

static void set(Bcm2835Cprman *c, uint16_t offset, uint32_t value)
{
	c->regs[offset / 4] = value;
}

static bool pll_running(const Bcm2835Cprman *c, Pll p)
{
	return !(reg(c, plls[p].ctrl) & A2W_PWRDN) &&
	       !(reg(c, plls[p].cm) & CM_ANARST);
}

static uint64_t pll_rate(const Bcm2835Cprman *c, Pll p)
{
	uint32_t ctrl = reg(c, plls[p].ctrl);
	uint64_t pdiv = ctrl >> A2W_PDIV_SHIFT & A2W_PDIV;
	uint64_t mul = (uint64_t)(ctrl & A2W_NDIV) << FRAC_BITS |
	               (reg(c, plls[p].frac) & FRAC_MASK);

	if (!pll_running(c, p) || pdiv == 0)
		return 0;
	if (reg(c, plls[p].ana + 4) & plls[p].prediv)
		mul *= 2;
	return (BCM2835_CPRMAN_OSC_HZ * mul >> FRAC_BITS) / pdiv;
}

static uint64_t channel_rate(const Bcm2835Cprman *c, Channel ch)
{
	const ChannelInfo *info = &channels[ch];
	uint32_t a2w = reg(c, info->a2w);
	uint64_t div = a2w & CHANNEL_DIV;

	if ((a2w & CHANNEL_DISABLE) || (reg(c, plls[info->pll].cm) & info->hold))
		return 0;
	return pll_rate(c, info->pll) / (div == 0 ? 256 : div);
}

/*
 * The rate of a modelled device's generator. Its sources are PLL
 * channels; any other, the crystal too, stops the device's clock here,
 * and the write that chooses it stops the machine.
 */
static uint64_t generator_rate(const Bcm2835Cprman *c, const Generator *g)
{
	uint32_t ctl = reg(c, g->ctl), div = reg(c, g->ctl + 4);
	Channel ch = parents[g->parents][ctl & CTL_SRC];
	unsigned drop = DIV_FRAC_BITS - g->frac_bits;
	uint64_t divi = div >> DIV_FRAC_BITS & ((1u << g->int_bits) - 1);
	uint64_t divf = (div & ((1u << DIV_FRAC_BITS) - 1)) >> drop << drop;

	if (!(ctl & CTL_ENAB) || (ctl & CTL_KILL) || ch == NO_CHANNEL || divi == 0)
		return 0;
	if ((ctl >> CTL_MASH_SHIFT & 3) == 0)
		divf = 0; /* without MASH, an integer divider */
	return (channel_rate(c, ch) << DIV_FRAC_BITS) /
	       (divi << DIV_FRAC_BITS | divf);
}

uint64_t bcm2835_cprman_rate(const Bcm2835Cprman *c, Bcm2835CprmanClock clock)
{
	return generator_rate(c, &watched[clock]);
}

static uint32_t read_lock(const Bcm2835Cprman *c)
{
	uint32_t lock = 0;

	for (unsigned p = 0; p < NUM_PLLS; p++)
		if (pll_running(c, p))
			lock |= 1u << (8 + p);
	return lock;
}

static void stop(Bcm2835Cprman *c, const char *access, uint64_t offset,
                 unsigned size, const char *why, const char *name)
{
	machine_stop(c->machine, MACHINE_STOP_ERROR,
	             "bcm2835-cprman: %u-byte %s offset 0x%04" PRIx64 " %s%s", size,
	             access, offset, why, name);
}

/* Whether an access can go on: a 32-bit one to a modelled register. */
static bool accessible(Bcm2835Cprman *c, const char *access, uint64_t offset,
                       unsigned size)
{
	if (size != 4 || offset % 4 != 0) {
		stop(c, access, offset, size, "is not a 32-bit register access", "");
		return false;
	}
	switch (c->kind[offset / 4]) {
	case KIND_NONE:
		stop(c, access, offset, size,
		     "is not emulated: no register of clk-bcm2835 is there", "");
		return false;
	case KIND_COUNTER:
		stop(c, access, offset, size,
		     "is not implemented yet (the frequency counter)", "");
		return false;
	default:
		return true;
	}
}

static uint64_t cprman_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Cprman *c = ctx;
	uint32_t value;

	if (!accessible(c, "read from", offset, size))
		return 0;
	value = c->regs[offset / 4];
	switch (c->kind[offset / 4]) {
	case KIND_CTL:
		if ((value & CTL_ENAB) && !(value & CTL_KILL))
			value |= CTL_BUSY;
		return value;
	case KIND_LOCK:
		return read_lock(c);
	default:
		return value;
	}
}

static void cprman_write(void *ctx, uint64_t offset, uint64_t value,
                         unsigned size)
{
	Bcm2835Cprman *c = ctx;
	uint32_t v = (uint32_t)value, old;

	if (!accessible(c, "write to", offset, size))
		return;
	if (c->kind[offset / 4] == KIND_LOCK) {
		stop(c, "write to", offset, size, "is not emulated: CM_LOCK", "");
		return;
	}
	if ((v & PASSWORD_MASK) != PASSWORD)
		return; /* the password guards every register */
	old = c->regs[offset / 4];
	c->regs[offset / 4] =
		v & (c->kind[offset / 4] == KIND_CTL ? CTL_MASK : DATA_MASK);
	for (unsigned i = 0; i < BCM2835_CPRMAN_NUM_CLOCKS; i++) {
		if (bcm2835_cprman_rate(c, i) != c->boot_rate[i]) {
			c->regs[offset / 4] = old;
			stop(c, "write to", offset, size,
			     "is not implemented yet: it changes the rate of the ",
			     watched[i].name);
			return;
		}
	}
}

static const BusMmioOps cprman_mmio_ops = {
	.read = cprman_read,
	.write = cprman_write,
};

/* A generator's control and divisor registers. */
static void set_generator(Bcm2835Cprman *c, uint16_t ctl_offset, uint32_t ctl,
                          uint32_t div)
{
	set(c, ctl_offset, ctl);
	set(c, ctl_offset + 4, div);
}

static void set_pll(Bcm2835Cprman *c, Pll p, uint32_t ndiv, uint32_t frac)
{
	set(c, plls[p].ctrl, A2W_PRST_DISABLE | 1u << A2W_PDIV_SHIFT | ndiv);
	set(c, plls[p].frac, frac);
	set(c, plls[p].ana + 4, plls[p].prediv);
	set(c, plls[p].cm, 0);
}

void bcm2835_cprman_reset(Bcm2835Cprman *c)
{
	const uint32_t on = CTL_ENAB, mash = 1u << CTL_MASH_SHIFT;

	memset(c->regs, 0, sizeof(c->regs));
	for (unsigned p = 0; p < NUM_PLLS; p++) {
		set(c, plls[p].ctrl, A2W_PWRDN);
		set(c, plls[p].cm, CM_ANARST);
	}
	for (unsigned ch = PLLA_DSI0; ch < NUM_CHANNELS; ch++)
		set(c, channels[ch].a2w, CHANNEL_DISABLE);
	/* 19.2 MHz * 2 * (52 + 87382 / 2^20) = 2000000024 Hz. */
	set_pll(c, PLLC, 52, 87382);
	set_pll(c, PLLD, 52, 87382);
	set_pll(c, PLLB, 62, 1u << 19); /* 2.4 GHz */
	set(c, channels[PLLB_ARM].a2w, 2);
	set(c, channels[PLLC_CORE0].a2w, 2);
	set(c, channels[PLLD_CORE].a2w, 4);
	set(c, channels[PLLD_PER].a2w, 4);
	set(c, REG_XOSC_CTRL, 0x95); /* PLLB, DDR (PLLD), USB and PLLC */
	set_generator(c, watched[BCM2835_CPRMAN_VPU].ctl, on | 5,
	              4 << DIV_FRAC_BITS);
	set_generator(c, REG_H264CTL, on | mash | 5, 0x2800);
	set_generator(c, REG_ISPCTL, on | mash | 5, 0x2800);
	set_generator(c, REG_SDCCTL, on | 6, 3 << DIV_FRAC_BITS);
	set_generator(c, watched[BCM2835_CPRMAN_UART].ctl, on | mash | 6,
	              10 << DIV_FRAC_BITS | 1707);
	set_generator(c, watched[BCM2835_CPRMAN_EMMC].ctl, on | mash | 6, 0x2800);
	set_generator(c, REG_GP1CTL, on | 6, 20 << DIV_FRAC_BITS);
	set_generator(c, REG_GP2CTL, on | mash | SRC_OSC,
	              585 << DIV_FRAC_BITS | 3840);
	set_generator(c, REG_TSENSCTL, on | SRC_OSC, 10 << DIV_FRAC_BITS);
	for (unsigned i = 0; i < BCM2835_CPRMAN_NUM_CLOCKS; i++)
		c->boot_rate[i] = bcm2835_cprman_rate(c, i);
}

static void mark(Bcm2835Cprman *c, uint16_t offset, Kind kind)
{
	c->kind[offset / 4] = (uint8_t)kind;
}

Bcm2835Cprman *bcm2835_cprman_create(Machine *m)
{
	Bcm2835Cprman *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	c->machine = m;
	for (unsigned i = 0; i < BCM2835_CPRMAN_NUM_CLOCKS; i++) {
		mark(c, watched[i].ctl, KIND_CTL);
		mark(c, (uint16_t)(watched[i].ctl + 4), KIND_PLAIN);
	}
	for (unsigned i = 0; i < NUM_OTHERS; i++) {
		mark(c, others[i], KIND_CTL);
		if (others[i] != REG_PERIICTL)
			mark(c, (uint16_t)(others[i] + 4), KIND_PLAIN);
	}
	for (unsigned p = 0; p < NUM_PLLS; p++) {
		mark(c, plls[p].cm, KIND_PLAIN);
		mark(c, plls[p].ctrl, KIND_PLAIN);
		mark(c, plls[p].frac, KIND_PLAIN);
		for (unsigned i = 0; i < 4; i++)
			mark(c, (uint16_t)(plls[p].ana + 4 * i), KIND_PLAIN);
	}
	for (unsigned ch = PLLA_DSI0; ch < NUM_CHANNELS; ch++)
		mark(c, channels[ch].a2w, KIND_PLAIN);
	mark(c, REG_LOCK, KIND_LOCK);
	mark(c, REG_DSI0HSCK, KIND_PLAIN);
	mark(c, REG_XOSC_CTRL, KIND_PLAIN);
	mark(c, REG_OSCCOUNT, KIND_COUNTER);
	mark(c, REG_TCNTCTL, KIND_COUNTER);
	mark(c, REG_TCNTCNT, KIND_COUNTER);
	bcm2835_cprman_reset(c);
	return c;
}

void bcm2835_cprman_destroy(Bcm2835Cprman *c)
{
	free(c);
}

BusStatus bcm2835_cprman_map(Bcm2835Cprman *c, Bus *bus, const char *name,
                             uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_CPRMAN_MMIO_SIZE,
	                    &cprman_mmio_ops, c);
}
