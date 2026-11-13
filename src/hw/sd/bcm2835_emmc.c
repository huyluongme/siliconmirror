/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's EMMC controller; see bcm2835_emmc.h. */

#include "hw/sd/bcm2835_emmc.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/event.h"
#include "util/muldiv.h"

/* Registers (datasheet 5.4), and the capabilities that it leaves out. */
enum {
	ARG2 = 0x00,
	BLKSIZECNT = 0x04,
	ARG1 = 0x08,
	CMDTM = 0x0c,
	RESP0 = 0x10,
	RESP3 = 0x1c,
	DATA = 0x20,
	STATUS = 0x24,
	CONTROL0 = 0x28,
	CONTROL1 = 0x2c,
	INTERRUPT = 0x30,
	IRPT_MASK = 0x34,
	IRPT_EN = 0x38,
	CONTROL2 = 0x3c,
	CAPABILITIES = 0x40,
	CAPABILITIES_1 = 0x44,
	FORCE_IRPT = 0x50,
	BOOT_TIMEOUT = 0x70,
	DBG_SEL = 0x74,
	EXRDFIFO_CFG = 0x80,
	EXRDFIFO_EN = 0x84,
	TUNE_STEP = 0x88,
	TUNE_STEPS_STD = 0x8c,
	TUNE_STEPS_DDR = 0x90,
	SPI_INT_SPT = 0xf0,
	SLOTISR_VER = 0xfc,
};

/* CMDTM */
#define CMD_INDEX_SHIFT 24
#define CMD_RSPNS_TYPE_SHIFT 16 /* 0: none; 136 bits; 48 bits; 48 with busy */

/* STATUS: lines idle high (the card's pull-ups), no transfer. */
#define CMD_INHIBIT (1u << 0)
#define STATUS_IDLE (0xfu << 25 | 1u << 24 | 0xfu << 20)

/* CONTROL1 */
#define CLK_INTLEN (1u << 0)
#define CLK_STABLE (1u << 1)
#define CLK_EN (1u << 2)
#define CLK_FREQ_MS2_SHIFT 6
#define CLK_FREQ8_SHIFT 8
#define SRST_HC (1u << 24)
#define SRST_CMD (1u << 25)
#define SRST_DATA (1u << 26)

/* INTERRUPT, IRPT_MASK, IRPT_EN and FORCE_IRPT */
#define CMD_DONE (1u << 0)
#define ERR (1u << 15)
#define CTO_ERR (1u << 16)
#define ERRORS 0x017f0000u       /* bits 24 and 22:16 */
#define FLAGS (ERRORS | 0x7037u) /* 14:12, 5:4 and 2:0 */

/* Linux's sdhci-iproc.c, bcm2835_data: caps and caps1. */
#define CAPS 0x01210000u   /* 3.3 V, high speed, blocks of 1024 bytes */
#define CAPS_1 0x00000030u /* driver types A and C */
#define VERSION (2u << 16) /* SDVERSION 2: SD Host Controller 3.00 */

/* Bus times, in SD clocks (SD Physical Layer 4.12). */
#define COMMAND_BITS 48
#define NCR_MAX 64 /* the latest a response may start */
#define NCC 8      /* clocks before the next command */

struct Bcm2835Emmc {
	Machine *machine;
	uint64_t base_hz;
	IrqLine *irq;

	uint32_t arg2, blksizecnt, arg1, cmdtm, resp[4];
	uint32_t control0, control1, control2;
	uint32_t interrupt, irpt_mask, irpt_en;
	uint32_t boot_timeout, dbg_sel, exrdfifo_cfg, exrdfifo_en;
	uint32_t tune_step, tune_steps_std, tune_steps_ddr, spi_int_spt;

	bool busy;        /* a command is on the line */
	uint32_t pending; /* the flags that its end sets */
	Event command_done;
};

static void stop(Bcm2835Emmc *emmc, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(emmc->machine, MACHINE_STOP_ERROR,
	             "bcm2835-emmc: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

static uint32_t interrupt(const Bcm2835Emmc *emmc)
{
	return emmc->interrupt | (emmc->interrupt & ERRORS ? ERR : 0);
}

static void update_irq(Bcm2835Emmc *emmc)
{
	irq_set(emmc->irq, (interrupt(emmc) & emmc->irpt_en) != 0);
}

/* Flags are set only where IRPT_MASK enables them. */
static void set_flags(Bcm2835Emmc *emmc, uint32_t flags)
{
	emmc->interrupt |= flags & emmc->irpt_mask;
	update_irq(emmc);
}

static void command_finished(void *ctx)
{
	Bcm2835Emmc *emmc = ctx;

	emmc->busy = false;
	set_flags(emmc, emmc->pending);
}

/* The SD clock: the base clock, or divided by twice the 10-bit divider. */
static uint64_t sd_clock(const Bcm2835Emmc *emmc)
{
	uint32_t div = (emmc->control1 >> CLK_FREQ8_SHIFT & 0xff) |
	               (emmc->control1 >> CLK_FREQ_MS2_SHIFT & 3) << 8;

	return div ? emmc->base_hz / (2 * div) : emmc->base_hz;
}

/* A command to the card, which does not answer (see the header). */
static void start_command(Bcm2835Emmc *emmc, uint64_t offset, uint32_t v)
{
	unsigned index = v >> CMD_INDEX_SHIFT & 0x3f;
	bool response = (v >> CMD_RSPNS_TYPE_SHIFT & 3) != 0;
	uint64_t clocks;

	if (emmc->busy) {
		stop(emmc, "write to", offset, 4,
		     "is not emulated: the last command has not finished");
		return;
	}
	if ((emmc->control1 & (CLK_INTLEN | CLK_EN)) != (CLK_INTLEN | CLK_EN)) {
		stop(emmc, "write to", offset, 4,
		     "is not emulated: a command without the SD clock");
		return;
	}
	if (index == 5 || index == 52 || index == 53) {
		stop(emmc, "write to", offset, 4,
		     "is not emulated: an SDIO command, which the WiFi chip "
		     "would answer");
		return;
	}
	emmc->cmdtm = v;
	emmc->busy = true;
	emmc->pending = response ? CTO_ERR : CMD_DONE;
	clocks = COMMAND_BITS + (response ? NCR_MAX : NCC);
	event_schedule(&emmc->machine->events, &emmc->command_done,
	               machine_now(emmc->machine) +
	                   mul_div_ceil(clocks, 1000000000, sd_clock(emmc)));
}

static uint64_t emmc_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Emmc *emmc = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(emmc, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case ARG2:
		return emmc->arg2;
	case BLKSIZECNT:
		return emmc->blksizecnt;
	case ARG1:
		return emmc->arg1;
	case CMDTM:
		return emmc->cmdtm;
	case RESP0:
	case RESP0 + 4:
	case RESP0 + 8:
	case RESP3:
		return emmc->resp[(offset - RESP0) / 4];
	case STATUS:
		return STATUS_IDLE | (emmc->busy ? CMD_INHIBIT : 0);
	case CONTROL0:
		return emmc->control0;
	case CONTROL1:
		return emmc->control1 | (emmc->control1 & CLK_INTLEN ? CLK_STABLE : 0);
	case INTERRUPT:
		return interrupt(emmc);
	case IRPT_MASK:
		return emmc->irpt_mask;
	case IRPT_EN:
		return emmc->irpt_en;
	case CONTROL2:
		return emmc->control2;
	case CAPABILITIES:
		return CAPS;
	case CAPABILITIES_1:
		return CAPS_1;
	case BOOT_TIMEOUT:
		return emmc->boot_timeout;
	case DBG_SEL:
		return emmc->dbg_sel;
	case EXRDFIFO_CFG:
		return emmc->exrdfifo_cfg;
	case EXRDFIFO_EN:
		return emmc->exrdfifo_en;
	case TUNE_STEP:
		return emmc->tune_step;
	case TUNE_STEPS_STD:
		return emmc->tune_steps_std;
	case TUNE_STEPS_DDR:
		return emmc->tune_steps_ddr;
	case SPI_INT_SPT:
		return emmc->spi_int_spt;
	case SLOTISR_VER:
		return VERSION | ((interrupt(emmc) & emmc->irpt_en) != 0);
	case DATA:
		stop(emmc, "read of", offset, size,
		     "is not emulated: no card sends data");
		return 0;
	default:
		stop(emmc, "read of", offset, size, "is not a documented register");
		return 0;
	}
}

static void reset_registers(Bcm2835Emmc *emmc)
{
	event_cancel(&emmc->machine->events, &emmc->command_done);
	emmc->arg2 = emmc->blksizecnt = emmc->arg1 = emmc->cmdtm = 0;
	for (int i = 0; i < 4; i++)
		emmc->resp[i] = 0;
	emmc->control0 = emmc->control1 = emmc->control2 = 0;
	emmc->interrupt = emmc->irpt_mask = emmc->irpt_en = 0;
	emmc->boot_timeout = emmc->dbg_sel = 0;
	emmc->exrdfifo_cfg = emmc->exrdfifo_en = 0;
	emmc->tune_step = emmc->tune_steps_std = emmc->tune_steps_ddr = 0;
	emmc->spi_int_spt = 0;
	emmc->busy = false;
	emmc->pending = 0;
	update_irq(emmc);
}

/* CONTROL1: the clock, and the resets, which end at once. */
static void write_control1(Bcm2835Emmc *emmc, uint32_t v)
{
	if (v & SRST_HC) {
		reset_registers(emmc);
		return;
	}
	if (v & SRST_CMD) {
		event_cancel(&emmc->machine->events, &emmc->command_done);
		emmc->busy = false;
	}
	/* SRST_DATA: no data ever moves, nothing to reset. */
	emmc->control1 = v & ~(SRST_HC | SRST_CMD | SRST_DATA | CLK_STABLE);
}

static void emmc_write(void *ctx, uint64_t offset, uint64_t value,
                       unsigned size)
{
	Bcm2835Emmc *emmc = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset % 4 != 0) {
		stop(emmc, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	switch (offset) {
	case ARG2:
		emmc->arg2 = v;
		break;
	case BLKSIZECNT:
		emmc->blksizecnt = v & 0xffff03ffu;
		break;
	case ARG1:
		emmc->arg1 = v;
		break;
	case CMDTM:
		start_command(emmc, offset, v);
		break;
	case CONTROL0:
		emmc->control0 = v;
		break;
	case CONTROL1:
		write_control1(emmc, v);
		break;
	case INTERRUPT:
		emmc->interrupt &= ~(v & FLAGS);
		break;
	case IRPT_MASK:
		emmc->irpt_mask = v;
		break;
	case IRPT_EN:
		emmc->irpt_en = v;
		break;
	case CONTROL2:
		emmc->control2 = v;
		break;
	case FORCE_IRPT:
		emmc->interrupt |= v & FLAGS;
		break;
	case BOOT_TIMEOUT:
		emmc->boot_timeout = v;
		break;
	case DBG_SEL:
		emmc->dbg_sel = v & 1;
		break;
	case EXRDFIFO_CFG:
		emmc->exrdfifo_cfg = v & 7;
		break;
	case EXRDFIFO_EN:
		emmc->exrdfifo_en = v & 1;
		break;
	case TUNE_STEP:
		emmc->tune_step = v & 7;
		break;
	case TUNE_STEPS_STD:
		emmc->tune_steps_std = v & 0x3f;
		break;
	case TUNE_STEPS_DDR:
		emmc->tune_steps_ddr = v & 0x3f;
		break;
	case SPI_INT_SPT:
		emmc->spi_int_spt = v & 0xff;
		break;
	case DATA:
		stop(emmc, "write to", offset, size,
		     "is not emulated: no card takes data");
		return;
	default:
		stop(emmc, "write to", offset, size,
		     "is not a register that software writes");
		return;
	}
	update_irq(emmc);
}

static const BusMmioOps emmc_mmio_ops = {
	.read = emmc_read,
	.write = emmc_write,
};

void bcm2835_emmc_reset(Bcm2835Emmc *emmc)
{
	reset_registers(emmc);
}

Bcm2835Emmc *bcm2835_emmc_create(Machine *m, uint64_t base_hz, IrqLine *irq)
{
	Bcm2835Emmc *emmc;

	if (base_hz == 0)
		return NULL;
	emmc = calloc(1, sizeof(*emmc));
	if (emmc == NULL)
		return NULL;
	emmc->machine = m;
	emmc->base_hz = base_hz;
	emmc->irq = irq;
	event_init(&emmc->command_done, command_finished, emmc);
	reset_registers(emmc);
	return emmc;
}

void bcm2835_emmc_destroy(Bcm2835Emmc *emmc)
{
	event_cancel(&emmc->machine->events, &emmc->command_done);
	free(emmc);
}

BusStatus bcm2835_emmc_map(Bcm2835Emmc *emmc, Bus *bus, const char *name,
                           uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_EMMC_MMIO_SIZE, &emmc_mmio_ops,
	                    emmc);
}
