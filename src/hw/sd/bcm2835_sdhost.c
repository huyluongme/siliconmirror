/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The BCM2835's SDHOST; see bcm2835_sdhost.h. */

#include "hw/sd/bcm2835_sdhost.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "core/event.h"
#include "util/byteorder.h"
#include "util/muldiv.h"

/* Registers, with the drivers' names. */
enum {
	SDCMD = 0x00,
	SDARG = 0x04,
	SDTOUT = 0x08,
	SDCDIV = 0x0c,
	SDRSP0 = 0x10,
	SDRSP3 = 0x1c,
	SDHSTS = 0x20,
	SDVDD = 0x30,
	SDEDM = 0x34,
	SDHCFG = 0x38,
	SDHBCT = 0x3c,
	SDDATA = 0x40,
	SDHBLC = 0x50,
};

#define CMD_INDEX 0x3fu
#define CMD_READ 0x40u
#define CMD_WRITE 0x80u
#define CMD_LONG 0x200u
#define CMD_NO_RESPONSE 0x400u
#define CMD_BUSYWAIT 0x800u
#define CMD_FAIL 0x4000u
#define CMD_NEW 0x8000u
#define CMD_KNOWN                                                              \
	(CMD_NEW | CMD_FAIL | CMD_BUSYWAIT | CMD_NO_RESPONSE | CMD_LONG |          \
	 CMD_WRITE | CMD_READ | CMD_INDEX)

#define HSTS_DATA_FLAG 0x001u
#define HSTS_FIFO_ERROR 0x008u
#define HSTS_CRC7_ERROR 0x010u
#define HSTS_CRC16_ERROR 0x020u
#define HSTS_CMD_TIME_OUT 0x040u
#define HSTS_REW_TIME_OUT 0x080u
#define HSTS_SDIO_IRPT 0x100u
#define HSTS_BLOCK_IRPT 0x200u
#define HSTS_BUSY_IRPT 0x400u
#define HSTS_CLEARABLE 0x7f8u /* write 1 to clear */

#define HCFG_WIDE_EXT_BUS 0x004u
#define HCFG_SLOW_CARD 0x008u
#define HCFG_DATA_IRPT_EN 0x010u
#define HCFG_SDIO_IRPT_EN 0x020u
#define HCFG_BLOCK_IRPT_EN 0x100u
#define HCFG_BUSY_IRPT_EN 0x400u
#define HCFG_KNOWN 0x53fu

#define EDM_FIFO_SHIFT 4
#define EDM_WRITE_THRESHOLD_SHIFT 9
#define EDM_READ_THRESHOLD_SHIFT 14
#define EDM_THRESHOLDS (0x3ffu << EDM_WRITE_THRESHOLD_SHIFT)
#define EDM_FORCE_DATA_MODE (1u << 19)

/* States of SDEDM's FSM field. */
enum {
	FSM_IDENTMODE = 0x0,
	FSM_DATAMODE = 0x1,
	FSM_READDATA = 0x2,
	FSM_WRITEDATA = 0x3,
	FSM_READWAIT = 0x4,
	FSM_WRITEWAIT1 = 0x7,
	FSM_POWERDOWN = 0x8,
	FSM_WRITESTART1 = 0xa,
};

#define CDIV_MASK 0x7ffu
#define CDIV_RESET 507 /* bcm2835-sdhost.c's comment on SDCDIV */

/*
 * Bus timings, in SD clocks: a command or a short response is 48 bits,
 * a long one 136 (4.9); a response comes Ncr = 2 clocks after its
 * command, the first data block Nac = 2 clocks after the response, and
 * a written block's CRC status Ncrc = 2 clocks after it: the shortest
 * times of 4.12. A block ends with 16 bits of CRC and an end bit. The
 * card's busy time after a block or an R1b command is our choice.
 */
#define COMMAND_BITS 48
#define LONG_BITS 136
#define NCR 2
#define NCR_MAX 64
#define NAC 2
#define BLOCK_END_BITS 17
#define CRC_STATUS_BITS (2 + 5 + 1)
#define BUSY_CLOCKS 8

typedef enum Transfer { XFER_NONE, XFER_READ, XFER_WRITE } Transfer;

struct Bcm2835Sdhost {
	Machine *machine;
	uint64_t core_hz;
	Bcm2835SdhostSlot slot;
	IrqLine *irq;

	uint32_t cmd, arg, tout, cdiv, rsp[4], hsts, vdd, edm, hcfg, hbct, hblc;
	bool data_mode; /* the card has its RCA: the bus is in data mode */

	/* What the command in flight shows when it is done. */
	SdResponse response;
	uint32_t pending_hsts;
	bool pending_fail;

	uint32_t fifo[BCM2835_SDHOST_FIFO_WORDS];
	unsigned fifo_head, fifo_count;

	Transfer xfer;
	uint8_t block[SD_BLOCK_SIZE];
	uint32_t pos;         /* bytes of the block through the FIFO */
	uint32_t blocks_left; /* after this one */
	bool stalled;         /* the clock waits for room or data */
	bool block_error;     /* a CRC16 error at the end of the block */
	bool card_busy;       /* after a written block */

	Event command_done; /* the response, or its timeout */
	Event word_done;    /* a data word crossed the bus */
	Event busy_done;    /* the card released DAT0 */
	Event data_timeout; /* no data came within SDTOUT */
};

static void stop(Bcm2835Sdhost *host, const char *access, uint64_t offset,
                 unsigned size, const char *why)
{
	machine_stop(host->machine, MACHINE_STOP_ERROR,
	             "bcm2835-sdhost: %u-byte %s offset 0x%02" PRIx64 " %s", size,
	             access, offset, why);
}

/* --- Time ----------------------------------------------------------------- */

static uint64_t sd_clock(const Bcm2835Sdhost *host)
{
	uint32_t divisor = (host->hcfg & HCFG_SLOW_CARD) || !host->data_mode
	                       ? host->cdiv + 2
	                       : (host->cdiv & 7) + 2;

	return host->core_hz / divisor;
}

static uint64_t at_clocks(const Bcm2835Sdhost *host, uint64_t clocks)
{
	return machine_now(host->machine) +
	       mul_div_ceil(clocks, 1000000000, sd_clock(host));
}

static unsigned data_lines(const Bcm2835Sdhost *host)
{
	return host->hcfg & HCFG_WIDE_EXT_BUS ? 4 : 1;
}

/* Clocks for one 32-bit word of data. */
static uint64_t word_clocks(const Bcm2835Sdhost *host)
{
	return 32 / data_lines(host);
}

/* --- FIFO and interrupt --------------------------------------------------- */

static uint32_t threshold(const Bcm2835Sdhost *host, unsigned shift)
{
	uint32_t t = (host->edm >> shift) & 0x1f;

	return t ? t : 1;
}

/*
 * DATA_FLAG: on a read, the FIFO holds the read threshold of SDEDM, or
 * the rest of the block; on a write, it has room for the write
 * threshold while words are still due.
 */
static bool data_flag(const Bcm2835Sdhost *host)
{
	if (host->xfer == XFER_READ ||
	    (host->xfer == XFER_NONE && host->fifo_count != 0))
		return host->fifo_count >= threshold(host, EDM_READ_THRESHOLD_SHIFT) ||
		       (host->fifo_count != 0 &&
		        (host->pos >= host->hbct || host->xfer == XFER_NONE));
	if (host->xfer == XFER_WRITE)
		return BCM2835_SDHOST_FIFO_WORDS - host->fifo_count >=
		       threshold(host, EDM_WRITE_THRESHOLD_SHIFT);
	return false;
}

static uint32_t hsts(const Bcm2835Sdhost *host)
{
	return host->hsts | (data_flag(host) ? HSTS_DATA_FLAG : 0);
}

static void update_irq(Bcm2835Sdhost *host)
{
	uint32_t s = hsts(host), c = host->hcfg;

	irq_set(host->irq,
	        ((s & HSTS_BUSY_IRPT) && (c & HCFG_BUSY_IRPT_EN)) ||
	            ((s & HSTS_BLOCK_IRPT) && (c & HCFG_BLOCK_IRPT_EN)) ||
	            ((s & HSTS_SDIO_IRPT) && (c & HCFG_SDIO_IRPT_EN)) ||
	            ((s & HSTS_DATA_FLAG) && (c & HCFG_DATA_IRPT_EN)));
}

static void fifo_push(Bcm2835Sdhost *host, uint32_t word)
{
	host->fifo[(host->fifo_head + host->fifo_count) %
	           BCM2835_SDHOST_FIFO_WORDS] = word;
	host->fifo_count++;
}

static uint32_t fifo_pop(Bcm2835Sdhost *host)
{
	uint32_t word = host->fifo[host->fifo_head];

	host->fifo_head = (host->fifo_head + 1) % BCM2835_SDHOST_FIFO_WORDS;
	host->fifo_count--;
	return word;
}

/* --- Data ----------------------------------------------------------------- */

static void end_transfer(Bcm2835Sdhost *host)
{
	host->xfer = XFER_NONE;
	host->stalled = false;
	event_cancel(&host->machine->events, &host->word_done);
	event_cancel(&host->machine->events, &host->data_timeout);
	event_cancel(&host->machine->events, &host->busy_done);
	host->card_busy = false;
}

static bool card_here(const Bcm2835Sdhost *host)
{
	return host->slot.card != NULL && (host->slot.connected == NULL ||
	                                   host->slot.connected(host->slot.ctx));
}

/*
 * The next block of a read, from the card: its first word comes Nac
 * clocks, a start bit and a word's time from now. Without a block, the
 * read times out after SDTOUT clocks.
 */
static void next_read_block(Bcm2835Sdhost *host)
{
	size_t len = 0;

	memset(host->block, 0, sizeof(host->block));
	if (card_here(host))
		len = sd_card_read(host->slot.card, host->block);
	host->pos = 0;
	if (len == 0) {
		event_schedule(&host->machine->events, &host->data_timeout,
		               at_clocks(host, host->tout));
		return;
	}
	/* The wrong length or bus width: the CRC does not match. */
	host->block_error = len != host->hbct ||
	                    sd_card_bus_width(host->slot.card) != data_lines(host);
	event_schedule(&host->machine->events, &host->word_done,
	               at_clocks(host, NAC + 1 + word_clocks(host)));
}

/* A block is done: its interrupt, then the next, or the end. */
static void block_done(Bcm2835Sdhost *host)
{
	host->hsts |= HSTS_BLOCK_IRPT;
	if (host->block_error)
		host->hsts |= HSTS_CRC16_ERROR;
	if (host->blocks_left == 0) {
		host->xfer = XFER_NONE;
	} else {
		host->blocks_left--;
		host->pos = 0;
		if (host->xfer == XFER_READ)
			next_read_block(host);
		else
			host->stalled = true; /* until a word comes */
	}
	update_irq(host);
}

static void start_write_word(Bcm2835Sdhost *host, uint64_t clocks)
{
	host->stalled = false;
	event_schedule(&host->machine->events, &host->word_done,
	               at_clocks(host, clocks));
}

/* A word went over the bus: into the FIFO, or out of it. */
static void word_finished(void *ctx)
{
	Bcm2835Sdhost *host = ctx;

	if (host->xfer == XFER_READ) {
		if (host->fifo_count == BCM2835_SDHOST_FIFO_WORDS) {
			host->stalled = true; /* until the Arm reads */
			return;
		}
		fifo_push(host, load_le32(host->block + host->pos));
		host->pos += 4;
		if (host->pos < host->hbct)
			event_schedule(&host->machine->events, &host->word_done,
			               at_clocks(host, word_clocks(host)));
		else
			event_schedule(&host->machine->events, &host->busy_done,
			               at_clocks(host, BLOCK_END_BITS));
		update_irq(host);
		return;
	}
	/* A write: the word left the FIFO for the card. */
	store_le32(host->block + host->pos, fifo_pop(host));
	host->pos += 4;
	if (host->pos >= host->hbct) {
		bool taken = card_here(host) &&
		             sd_card_write(host->slot.card, host->block, host->hbct);

		host->block_error =
			!taken || sd_card_bus_width(host->slot.card) != data_lines(host);
		host->card_busy = true;
		event_schedule(
			&host->machine->events, &host->busy_done,
			at_clocks(host, BLOCK_END_BITS + CRC_STATUS_BITS + BUSY_CLOCKS));
	} else if (host->fifo_count != 0) {
		start_write_word(host, word_clocks(host));
	} else {
		host->stalled = true; /* until the Arm writes */
	}
	update_irq(host);
}

/* The end of a block's CRC, or of the card's busy time. */
static void busy_finished(void *ctx)
{
	Bcm2835Sdhost *host = ctx;

	if (host->xfer == XFER_NONE) {
		/* An R1b command: the command is done now. */
		host->cmd &= ~CMD_NEW;
		host->hsts |= HSTS_BUSY_IRPT;
		update_irq(host);
		return;
	}
	host->card_busy = false;
	block_done(host);
	if (host->xfer == XFER_WRITE && host->fifo_count != 0)
		start_write_word(host, NAC + 1 + word_clocks(host));
}

static void data_timed_out(void *ctx)
{
	Bcm2835Sdhost *host = ctx;

	host->hsts |= HSTS_REW_TIME_OUT;
	end_transfer(host);
	update_irq(host);
}

/* --- Commands ------------------------------------------------------------- */

static void command_finished(void *ctx)
{
	Bcm2835Sdhost *host = ctx;
	const SdResponse *r = &host->response;

	host->hsts |= host->pending_hsts;
	if (host->pending_fail) {
		host->cmd = (host->cmd & ~CMD_NEW) | CMD_FAIL;
		update_irq(host);
		return;
	}
	if (r->type == SD_R2) {
		for (int i = 0; i < 4; i++)
			host->rsp[i] = r->words[3 - i];
	} else if (r->type != SD_NO_RESPONSE && !(host->cmd & CMD_NO_RESPONSE)) {
		host->rsp[0] = r->words[0];
		if (r->type == SD_R6)
			host->data_mode = true; /* identification is over */
	}
	if ((host->cmd & (CMD_READ | CMD_WRITE)) && host->hbct == 0) {
		host->cmd &= ~CMD_NEW;
		stop(host, "write to", SDCMD, 4,
		     "is not emulated: a transfer with SDHBCT 0");
		return;
	}
	if (host->cmd & CMD_READ) {
		host->xfer = XFER_READ;
		host->blocks_left = host->hblc ? host->hblc - 1 : 0;
		next_read_block(host);
	} else if (host->cmd & CMD_WRITE) {
		host->xfer = XFER_WRITE;
		host->blocks_left = host->hblc ? host->hblc - 1 : 0;
		host->pos = 0;
		host->stalled = true;
		if (host->fifo_count != 0)
			start_write_word(host, NAC + 1 + word_clocks(host));
	}
	if (host->cmd & CMD_BUSYWAIT) {
		event_schedule(&host->machine->events, &host->busy_done,
		               at_clocks(host, BUSY_CLOCKS));
		return; /* NEW clears when the card is no longer busy */
	}
	host->cmd &= ~CMD_NEW;
	update_irq(host);
}

/*
 * Send a command. The card takes it at once; the response, or the
 * timeout, comes after the bits' time.
 */
static void start_command(Bcm2835Sdhost *host, uint32_t value)
{
	unsigned index = value & CMD_INDEX;
	SdResponse *r = &host->response;
	uint64_t clocks = COMMAND_BITS;
	bool long_response = value & CMD_LONG;

	host->cmd = value & ~CMD_FAIL;
	host->pending_hsts = 0;
	host->pending_fail = false;
	memset(r, 0, sizeof(*r));
	end_transfer(host);
	if (card_here(host))
		sd_card_command(host->slot.card, index, host->arg, r);
	if (index == 0 || index == 15)
		host->data_mode = false;
	if (value & CMD_NO_RESPONSE) {
		clocks += 8; /* Ncc, before the next command */
	} else if (r->type == SD_NO_RESPONSE) {
		clocks += NCR_MAX;
		host->pending_fail = true;
		host->pending_hsts = HSTS_CMD_TIME_OUT;
	} else if (long_response != (r->type == SD_R2)) {
		/* The response is not as long as expected: its CRC fails. */
		clocks += NCR + (long_response ? LONG_BITS : COMMAND_BITS);
		host->pending_fail = true;
		host->pending_hsts = HSTS_CRC7_ERROR;
	} else {
		clocks += NCR + (long_response ? LONG_BITS : COMMAND_BITS);
	}
	event_schedule(&host->machine->events, &host->command_done,
	               at_clocks(host, clocks));
}

/* --- Registers ------------------------------------------------------------ */

static uint32_t edm(const Bcm2835Sdhost *host)
{
	unsigned fsm;

	if (host->vdd == 0)
		fsm = FSM_POWERDOWN;
	else if (host->xfer == XFER_READ)
		fsm = host->stalled ? FSM_READWAIT : FSM_READDATA;
	else if (host->xfer == XFER_WRITE)
		fsm = host->card_busy                   ? FSM_WRITEWAIT1
		      : host->pos == 0 && host->stalled ? FSM_WRITESTART1
		                                        : FSM_WRITEDATA;
	else
		fsm = host->data_mode ? FSM_DATAMODE : FSM_IDENTMODE;
	return (host->edm & EDM_THRESHOLDS) | host->fifo_count << EDM_FIFO_SHIFT |
	       fsm;
}

static uint64_t sdhost_read(void *ctx, uint64_t offset, unsigned size)
{
	Bcm2835Sdhost *host = ctx;

	if (size != 4 || offset % 4 != 0) {
		stop(host, "read of", offset, size, "is not a 32-bit register read");
		return 0;
	}
	switch (offset) {
	case SDCMD:
		return host->cmd;
	case SDARG:
		return host->arg;
	case SDTOUT:
		return host->tout;
	case SDCDIV:
		return host->cdiv;
	case SDRSP0:
	case SDRSP0 + 4:
	case SDRSP0 + 8:
	case SDRSP3:
		return host->rsp[(offset - SDRSP0) / 4];
	case SDHSTS:
		return hsts(host);
	case SDVDD:
		return host->vdd;
	case SDEDM:
		return edm(host);
	case SDHCFG:
		return host->hcfg;
	case SDHBCT:
		return host->hbct;
	case SDHBLC:
		return host->hblc;
	case SDDATA: {
		uint32_t word;

		if (host->fifo_count == 0) {
			stop(host, "read of", offset, size,
			     "is not emulated: the FIFO is empty");
			return 0;
		}
		word = fifo_pop(host);
		if (host->xfer == XFER_READ && host->stalled) {
			host->stalled = false; /* the clock runs again */
			event_schedule(&host->machine->events, &host->word_done,
			               at_clocks(host, word_clocks(host)));
		}
		update_irq(host);
		return word;
	}
	default:
		stop(host, "read of", offset, size,
		     "is not a register the drivers "
		     "use");
		return 0;
	}
}

static void sdhost_write(void *ctx, uint64_t offset, uint64_t value,
                         unsigned size)
{
	Bcm2835Sdhost *host = ctx;
	uint32_t v = (uint32_t)value;

	if (size != 4 || offset % 4 != 0) {
		stop(host, "write to", offset, size, "is not a 32-bit register write");
		return;
	}
	switch (offset) {
	case SDCMD:
		if (v & ~CMD_KNOWN) {
			stop(host, "write to", offset, size,
			     "sets bits that the drivers do not use");
			return;
		}
		if ((v & CMD_NEW) && (host->cmd & CMD_NEW)) {
			stop(host, "write to", offset, size,
			     "is not emulated: the last command has not finished");
			return;
		}
		if ((v & CMD_NEW) && host->vdd == 0) {
			stop(host, "write to", offset, size,
			     "is not emulated: SDVDD has the power off");
			return;
		}
		/* The drivers stop a transfer with CMD12, and send nothing else. */
		if ((v & CMD_NEW) && host->xfer != XFER_NONE && (v & CMD_INDEX) != 12) {
			stop(host, "write to", offset, size,
			     "is not emulated: a command other than CMD12 while data "
			     "moves");
			return;
		}
		if (v & CMD_NEW)
			start_command(host, v);
		else
			host->cmd = v;
		break;
	case SDARG:
		host->arg = v;
		break;
	case SDTOUT:
		host->tout = v;
		break;
	case SDCDIV:
		host->cdiv = v & CDIV_MASK;
		break;
	case SDHSTS:
		host->hsts &= ~(v & HSTS_CLEARABLE);
		break;
	case SDVDD:
		host->vdd = v & 1;
		if (host->vdd == 0)
			end_transfer(host);
		break;
	case SDEDM:
		if (v & ~(EDM_THRESHOLDS | EDM_FORCE_DATA_MODE | 0x1ffu)) {
			stop(host, "write to", offset, size,
			     "sets bits that the drivers do not use");
			return;
		}
		host->edm = v & EDM_THRESHOLDS;
		if (v & EDM_FORCE_DATA_MODE) {
			end_transfer(host);
			host->fifo_count = 0;
			host->data_mode = true;
		}
		break;
	case SDHCFG:
		if (v & ~HCFG_KNOWN) {
			stop(host, "write to", offset, size,
			     "sets bits that the drivers do not use");
			return;
		}
		host->hcfg = v;
		break;
	case SDHBCT:
		if (v > SD_BLOCK_SIZE || v % 4 != 0) {
			stop(host, "write to", offset, size,
			     "is not emulated: a block that is not whole words of "
			     "up to 512 bytes");
			return;
		}
		host->hbct = v;
		break;
	case SDHBLC:
		host->hblc = v;
		break;
	case SDDATA:
		if (host->fifo_count == BCM2835_SDHOST_FIFO_WORDS) {
			stop(host, "write to", offset, size,
			     "is not emulated: the FIFO is full");
			return;
		}
		fifo_push(host, v);
		if (host->xfer == XFER_WRITE && host->stalled && !host->card_busy)
			start_write_word(host, (host->pos == 0 ? NAC + 1 : 0) +
			                           word_clocks(host));
		break;
	default:
		stop(host, "write to", offset, size,
		     "is not a register the drivers "
		     "use");
		return;
	}
	update_irq(host);
}

static const BusMmioOps sdhost_mmio_ops = {
	.read = sdhost_read,
	.write = sdhost_write,
};

void bcm2835_sdhost_reset(Bcm2835Sdhost *host)
{
	end_transfer(host);
	event_cancel(&host->machine->events, &host->command_done);
	host->cmd = host->arg = host->tout = 0;
	host->cdiv = CDIV_RESET;
	memset(host->rsp, 0, sizeof(host->rsp));
	host->hsts = host->vdd = host->edm = host->hcfg = 0;
	host->hbct = SD_BLOCK_SIZE;
	host->hblc = 0;
	host->data_mode = false;
	host->pending_hsts = 0;
	host->pending_fail = false;
	host->fifo_head = host->fifo_count = 0;
	update_irq(host);
}

Bcm2835Sdhost *bcm2835_sdhost_create(Machine *m, uint64_t core_hz,
                                     const Bcm2835SdhostSlot *slot,
                                     IrqLine *irq)
{
	Bcm2835Sdhost *host;

	if (core_hz == 0)
		return NULL;
	host = calloc(1, sizeof(*host));
	if (host == NULL)
		return NULL;
	host->machine = m;
	host->core_hz = core_hz;
	host->slot = *slot;
	host->irq = irq;
	event_init(&host->command_done, command_finished, host);
	event_init(&host->word_done, word_finished, host);
	event_init(&host->busy_done, busy_finished, host);
	event_init(&host->data_timeout, data_timed_out, host);
	bcm2835_sdhost_reset(host);
	return host;
}

void bcm2835_sdhost_destroy(Bcm2835Sdhost *host)
{
	EventQueue *q = &host->machine->events;

	event_cancel(q, &host->command_done);
	event_cancel(q, &host->word_done);
	event_cancel(q, &host->busy_done);
	event_cancel(q, &host->data_timeout);
	free(host);
}

BusStatus bcm2835_sdhost_map(Bcm2835Sdhost *host, Bus *bus, const char *name,
                             uint64_t base)
{
	return bus_map_mmio(bus, name, base, BCM2835_SDHOST_MMIO_SIZE,
	                    &sdhost_mmio_ops, host);
}
