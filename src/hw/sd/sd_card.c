/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* An SD memory card; see sd_card.h. Section numbers are the spec's. */

#include "hw/sd/sd_card.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"

#define KIB 1024ull
#define MIB (1024 * KIB)
#define GIB (1024 * MIB)

#define C_SIZE_UNIT (512 * KIB) /* CSD 2.0: (C_SIZE + 1) * 512 KB */
#define SDSC_MAX (2 * GIB)
#define SDHC_MAX (32 * GIB)
#define SDXC_MAX (2048 * GIB)

/*
 * The card is ready 1 ms after the first ACMD41 that starts it: the
 * specification allows up to 1 s (4.2.3); real cards take some
 * milliseconds. Our choice.
 */
#define INIT_TIME_NS 1000000

/* The card's RCA, published by CMD3; any but 0 will do. Our choice. */
#define CARD_RCA 0xaaaa

/* Card states, as CURRENT_STATE codes them (Table 4-42). */
typedef enum State {
	IDLE,
	READY,
	IDENT,
	STBY,
	TRAN,
	DATA,
	RCV,
	PRG,
	DIS,
	INA = 15, /* not reported: the card no longer answers */
} State;

/* Card status bits (Table 4-42). */
#define OUT_OF_RANGE (1u << 31)
#define ADDRESS_ERROR (1u << 30)
#define BLOCK_LEN_ERROR (1u << 29)
#define ERASE_SEQ_ERROR (1u << 28)
#define ERASE_PARAM (1u << 27)
#define ILLEGAL_COMMAND (1u << 22)
#define ERASE_RESET (1u << 13)
#define STATE_SHIFT 9
#define READY_FOR_DATA (1u << 8)
#define APP_CMD (1u << 5)

/* OCR (Table 5-1): 2.7 to 3.6 V, CCS and the power-up status. */
#define OCR_VOLTAGES 0x00ff8000u
#define OCR_CCS (1u << 30)
#define OCR_READY (1u << 31)

/* What the card sends in a data block, besides its memory. */
typedef struct RegisterData {
	uint8_t bytes[64];
	size_t len; /* 0: none */
} RegisterData;

struct SdCard {
	Machine *machine;
	BlockDevice *storage;
	uint64_t capacity;  /* bytes */
	bool high_capacity; /* SDHC or SDXC: block addresses */
	uint8_t cid[16], csd[16], scr[8];

	bool powered;
	State state;
	uint16_t rca;
	bool if_cond;         /* CMD8 since power on or CMD0 */
	bool initializing;    /* ACMD41 started the power-up routine */
	uint64_t ready_at;    /* when it ends */
	bool hcs;             /* the host supports high capacity */
	uint32_t errors;      /* status bits for the next response */
	bool app_cmd;         /* the next command is an ACMD */
	uint32_t block_len;   /* CMD16 */
	unsigned bus_width;   /* 1 or 4 */
	bool high_speed;      /* CMD6, function group 1 */
	uint32_t block_count; /* CMD23 for the next transfer; 0: open */
	bool erase_start_set, erase_end_set;
	uint64_t erase_start, erase_end; /* bytes */
	uint32_t well_written;           /* blocks of the last write, for ACMD22 */

	/* The transfer in progress, in the data or receive state. */
	RegisterData reg;   /* a register to send instead of memory */
	uint64_t address;   /* bytes */
	uint32_t remaining; /* blocks; UINT32_MAX for no limit */
	bool multiple;      /* CMD18 or CMD25 */
};

static void stop(SdCard *card, const char *what, unsigned index)
{
	machine_stop(card->machine, MACHINE_STOP_ERROR,
	             "sd-card: %s (CMD%u) is not emulated", what, index);
}

/* --- Registers ------------------------------------------------------------ */

/* Set bits msb:lsb of a register of size bytes, bit 0 the last byte's. */
static void set_bits(uint8_t *reg, size_t size, unsigned msb, unsigned lsb,
                     uint64_t value)
{
	for (unsigned bit = lsb; bit <= msb; bit++) {
		size_t byte = size - 1 - bit / 8;
		uint8_t mask = (uint8_t)(1u << (bit % 8));

		if ((value >> (bit - lsb)) & 1)
			reg[byte] |= mask;
		else
			reg[byte] &= (uint8_t)~mask;
	}
}

uint8_t sd_crc7(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;

	for (size_t i = 0; i < len; i++) {
		for (int bit = 7; bit >= 0; bit--) {
			unsigned in = ((crc >> 6) ^ (data[i] >> bit)) & 1;

			crc = (uint8_t)((crc << 1) & 0x7f);
			if (in)
				crc ^= 0x09;
		}
	}
	return crc;
}

/* The CRC7 of a CID or CSD in bits 7:1, and the 1 of bit 0. */
static void seal(uint8_t reg[16])
{
	reg[15] = (uint8_t)(sd_crc7(reg, 15) << 1 | 1);
}

/*
 * CID (Table 5-2). The manufacturer and OEM IDs are licensed by SD-3C;
 * smirror's card uses none of theirs: 0 and "SM". Our choices.
 */
static void make_cid(SdCard *card)
{
	uint8_t *c = card->cid;

	memset(c, 0, 16);
	set_bits(c, 16, 127, 120, 0x00);                 /* MID */
	set_bits(c, 16, 119, 104, 'S' << 8 | 'M');       /* OID */
	memcpy(c + 3, "SMSD1", 5);                       /* PNM */
	set_bits(c, 16, 63, 56, 0x10);                   /* PRV 1.0 */
	set_bits(c, 16, 55, 24, 0x5ca1ab1e);             /* PSN */
	set_bits(c, 16, 19, 8, (2026 - 2000) << 4 | 11); /* MDT 2026-11 */
	seal(c);
}

/* CSD: version 1.0 for SDSC (5.3.2), version 2.0 above (5.3.3). */
static void make_csd(SdCard *card)
{
	uint8_t *c = card->csd;

	memset(c, 0, 16);
	set_bits(c, 16, 119, 112, 0x0e); /* TAAC: 1 ms */
	set_bits(c, 16, 103, 96, card->high_speed ? 0x5a : 0x32);
	/* Classes 0, 2, 4, 5, 7, 8 and 10. */
	set_bits(c, 16, 95, 84, 0x5b5);
	set_bits(c, 16, 46, 46, 1);    /* ERASE_BLK_EN */
	set_bits(c, 16, 45, 39, 0x7f); /* SECTOR_SIZE */
	set_bits(c, 16, 28, 26, 2);    /* R2W_FACTOR */
	if (card->high_capacity) {
		set_bits(c, 16, 127, 126, 1); /* CSD_STRUCTURE */
		set_bits(c, 16, 83, 80, 9);   /* READ_BL_LEN */
		set_bits(c, 16, 69, 48, card->capacity / C_SIZE_UNIT - 1);
		set_bits(c, 16, 25, 22, 9); /* WRITE_BL_LEN */
	} else {
		/*
		 * (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN
		 * bytes: up to 1 GB with blocks of 512 bytes, up to 2 GB with
		 * 1024, as 4.3.2 asks.
		 */
		unsigned bl_len = card->capacity > GIB ? 10 : 9;
		uint64_t unit = (1ull << bl_len) * 512; /* C_SIZE_MULT 7 */

		set_bits(c, 16, 83, 80, bl_len);
		set_bits(c, 16, 79, 79, 1); /* READ_BL_PARTIAL */
		set_bits(c, 16, 73, 62, card->capacity / unit - 1);
		set_bits(c, 16, 61, 50, 0xfff); /* currents: the largest */
		set_bits(c, 16, 49, 47, 7);     /* C_SIZE_MULT */
		set_bits(c, 16, 25, 22, bl_len);
	}
	seal(c);
}

/* SCR (Table 5-17): version 3.0X, 1 and 4 bits, CMD23. */
static void make_scr(SdCard *card)
{
	uint8_t *s = card->scr;
	unsigned security = card->capacity > SDHC_MAX ? 4
	                    : card->high_capacity     ? 3
	                                              : 2;

	memset(s, 0, 8);
	set_bits(s, 8, 59, 56, 2);        /* SD_SPEC */
	set_bits(s, 8, 54, 52, security); /* SD_SECURITY */
	set_bits(s, 8, 51, 48, 0x5);      /* SD_BUS_WIDTHS: 1 and 4 */
	set_bits(s, 8, 47, 47, 1);        /* SD_SPEC3 */
	set_bits(s, 8, 36, 32, 0x2);      /* CMD_SUPPORT: CMD23 */
}

/* --- Life ----------------------------------------------------------------- */

static void reset(SdCard *card)
{
	card->state = IDLE;
	card->rca = 0;
	card->if_cond = false;
	card->initializing = false;
	card->errors = 0;
	card->app_cmd = false;
	card->block_len = SD_BLOCK_SIZE;
	card->bus_width = 1;
	card->high_speed = false;
	card->block_count = 0;
	card->erase_start_set = card->erase_end_set = false;
	make_csd(card);
}

SdCard *sd_card_create(Machine *m, BlockDevice *storage, char *error,
                       size_t error_size)
{
	SdCard *card;

	if (storage->size == 0 || storage->size % C_SIZE_UNIT != 0 ||
	    storage->size > SDXC_MAX) {
		snprintf(error, error_size,
		         "an SD card holds a multiple of 512 KB, up to 2 TB, not "
		         "%" PRIu64 " bytes: truncate -s to resize the image",
		         storage->size);
		return NULL;
	}
	card = calloc(1, sizeof(*card));
	if (card == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	card->machine = m;
	card->storage = storage;
	card->capacity = storage->size;
	card->high_capacity = card->capacity > SDSC_MAX;
	make_cid(card);
	make_scr(card);
	reset(card);
	return card;
}

void sd_card_destroy(SdCard *card)
{
	free(card);
}

void sd_card_power(SdCard *card, bool on)
{
	card->powered = on;
	reset(card);
}

unsigned sd_card_bus_width(const SdCard *card)
{
	return card->bus_width;
}

bool sd_card_high_speed(const SdCard *card)
{
	return card->high_speed;
}

bool sd_card_receiving(const SdCard *card)
{
	return card->powered && card->state == RCV;
}

/* --- Responses ------------------------------------------------------------ */

/*
 * An R1 response: the errors found since the last response and by this
 * command, which the response then clears (types B and C), and the state
 * the card was in when the command came.
 */
static void r1(SdCard *card, SdResponse *response, SdResponseType type,
               State before, uint32_t found, bool app)
{
	response->type = type;
	response->words[0] = card->errors | found |
	                     (uint32_t)before << STATE_SHIFT | READY_FOR_DATA |
	                     (app ? APP_CMD : 0);
	card->errors = 0;
}

/* R2: bits 127:1 of reg, then the end bit, which reg[15] holds as 1. */
static void r2(SdResponse *response, const uint8_t reg[16])
{
	response->type = SD_R2;
	for (int i = 0; i < 4; i++)
		response->words[i] = (uint32_t)reg[4 * i] << 24 |
		                     (uint32_t)reg[4 * i + 1] << 16 |
		                     (uint32_t)reg[4 * i + 2] << 8 | reg[4 * i + 3];
}

static void illegal(SdCard *card, SdResponse *response)
{
	card->errors |= ILLEGAL_COMMAND;
	response->type = SD_NO_RESPONSE;
}

/* --- Data ----------------------------------------------------------------- */

/* The byte address of a read or write argument. */
static uint64_t byte_address(const SdCard *card, uint32_t arg)
{
	return card->high_capacity ? (uint64_t)arg * SD_BLOCK_SIZE : arg;
}

/* Start sending a register of len bytes. */
static void send_register(SdCard *card, const uint8_t *bytes, size_t len)
{
	memcpy(card->reg.bytes, bytes, len);
	card->reg.len = len;
	card->state = DATA;
}

/* Whether len bytes at address cross the end of a 512-byte block. */
static bool crosses(uint64_t address, uint32_t len)
{
	return address / SD_BLOCK_SIZE != (address + len - 1) / SD_BLOCK_SIZE;
}

/*
 * The checks of a read or a write at address of the block length
 * (4.3.3, 4.3.4): the error bits, 0 when it may start.
 */
static uint32_t check_transfer(const SdCard *card, uint64_t address, bool write)
{
	uint32_t len = card->high_capacity ? SD_BLOCK_SIZE : card->block_len;

	if (address >= card->capacity || len > card->capacity - address)
		return OUT_OF_RANGE;
	if (write && (len != SD_BLOCK_SIZE))
		return BLOCK_LEN_ERROR;
	/*
	 * A block must stay inside its 512 bytes (Table 4-4): a partial
	 * block, or a write's whole block, which must then be aligned.
	 */
	if (crosses(address, len))
		return ADDRESS_ERROR;
	return 0;
}

size_t sd_card_read(SdCard *card, uint8_t *buf)
{
	uint32_t len;

	if (!card->powered || card->state != DATA)
		return 0;
	if (card->reg.len != 0) {
		len = (uint32_t)card->reg.len;
		memcpy(buf, card->reg.bytes, len);
		card->state = TRAN;
		return len;
	}
	if (card->remaining == 0)
		return 0; /* aborted: waiting for CMD12 */
	len = card->high_capacity ? SD_BLOCK_SIZE : card->block_len;
	/*
	 * CMD18 past the end, or to a partial block that would cross 512
	 * bytes: the card aborts, and waits for CMD12, which reports the
	 * error (4.3.3).
	 */
	if (card->address >= card->capacity ||
	    len > card->capacity - card->address) {
		card->errors |= OUT_OF_RANGE;
		card->remaining = 0;
		return 0;
	}
	if (crosses(card->address, len)) {
		card->errors |= ADDRESS_ERROR;
		card->remaining = 0;
		return 0;
	}
	if (!block_read(card->storage, card->address, buf, len)) {
		machine_stop(card->machine, MACHINE_STOP_ERROR,
		             "sd-card: cannot read the image at 0x%" PRIx64,
		             card->address);
		return 0;
	}
	card->address += len;
	if (card->remaining != UINT32_MAX)
		card->remaining--;
	/* One block, or CMD23's count, ends the transfer; else CMD12 does. */
	if (!card->multiple || card->remaining == 0)
		card->state = TRAN;
	return len;
}

bool sd_card_write(SdCard *card, const uint8_t *buf, size_t len)
{
	if (!card->powered || card->state != RCV)
		return false;
	if (len != SD_BLOCK_SIZE) {
		card->errors |= BLOCK_LEN_ERROR;
		card->state = TRAN;
		return false;
	}
	if (card->address >= card->capacity ||
	    len > card->capacity - card->address) {
		card->errors |= OUT_OF_RANGE;
		card->state = TRAN;
		return false;
	}
	if (!block_write(card->storage, card->address, buf, len)) {
		machine_stop(card->machine, MACHINE_STOP_ERROR,
		             "sd-card: cannot write the image at 0x%" PRIx64,
		             card->address);
		return false;
	}
	card->address += len;
	card->well_written++;
	if (card->remaining != UINT32_MAX)
		card->remaining--;
	/* Programmed at once: back to receive, or to transfer when done. */
	if (!card->multiple || card->remaining == 0)
		card->state = TRAN;
	return true;
}

/*
 * Erase the write blocks from erase_start to erase_end: they read as 0,
 * as DATA_STAT_AFTER_ERASE in the SCR says.
 */
static void erase(SdCard *card)
{
	static const uint8_t zeros[64 * KIB];
	uint64_t at = card->erase_start / SD_BLOCK_SIZE * SD_BLOCK_SIZE;
	uint64_t end = (card->erase_end / SD_BLOCK_SIZE + 1) * SD_BLOCK_SIZE;

	while (at < end) {
		size_t len =
			end - at < sizeof(zeros) ? (size_t)(end - at) : sizeof(zeros);

		if (!block_write(card->storage, at, zeros, len)) {
			machine_stop(card->machine, MACHINE_STOP_ERROR,
			             "sd-card: cannot write the image at 0x%" PRIx64, at);
			return;
		}
		at += len;
	}
}

/* --- Commands ------------------------------------------------------------- */

/* CMD6 (4.3.10): mode 1 switches; groups 2 to 6 have function 0 only. */
static void switch_function(SdCard *card, uint32_t arg)
{
	uint8_t status[64] = {0};
	unsigned want = arg & 0xf; /* group 1 */
	bool ok = true;
	unsigned result;

	for (unsigned group = 1; group < 6; group++) {
		unsigned f = (arg >> (4 * group)) & 0xf;

		ok &= f == 0 || f == 0xf;
	}
	ok &= want == 0 || want == 1 || want == 0xf;
	result = !ok ? 0xf : want == 0xf ? (card->high_speed ? 1u : 0u) : want;
	set_bits(status, 64, 511, 496, 100); /* 100 mA at most */
	for (unsigned group = 0; group < 6; group++)
		set_bits(status, 64, 415 + 16 * group, 400 + 16 * group,
		         group == 0 ? 0x8003 : 0x8001); /* function 15 too */
	for (unsigned group = 1; group < 6; group++)
		set_bits(status, 64, 379 + 4 * group, 376 + 4 * group, ok ? 0 : 0xf);
	set_bits(status, 64, 379, 376, result);
	set_bits(status, 64, 375, 368, 1); /* version 1: busy bits valid */
	if (ok && (arg >> 31) && want != 0xf) {
		card->high_speed = want == 1;
		make_csd(card); /* TRAN_SPEED follows the speed */
	}
	send_register(card, status, sizeof(status));
}

/* ACMD13: the SD status (4.10.2). */
static void sd_status(SdCard *card)
{
	uint8_t status[64] = {0};

	set_bits(status, 64, 511, 510, card->bus_width == 4 ? 2 : 0);
	set_bits(status, 64, 447, 440, 4); /* SPEED_CLASS: class 10 */
	set_bits(status, 64, 431, 428, 9); /* AU_SIZE: 4 MB */
	send_register(card, status, sizeof(status));
}

/* ACMD41 (4.2.3): power up, then report the OCR. */
static void send_op_cond(SdCard *card, uint32_t arg, SdResponse *response)
{
	uint64_t now = machine_now(card->machine);
	uint32_t window = arg & OCR_VOLTAGES;
	uint32_t ocr = OCR_VOLTAGES;

	/* A window of only voltages the card lacks, LVR among them (5.1). */
	if ((arg & 0x00ffff80u) != 0 && window == 0) {
		card->state = INA; /* no voltage in common */
		response->type = SD_NO_RESPONSE;
		return;
	}
	if (window != 0 && !card->initializing) {
		card->initializing = true;
		card->ready_at = now + INIT_TIME_NS;
		card->hcs = card->if_cond && (arg & OCR_CCS);
	}
	/* High capacity needs a host that says it supports it. */
	if (card->initializing && now >= card->ready_at &&
	    (!card->high_capacity || card->hcs)) {
		ocr |= OCR_READY;
		if (card->if_cond && card->high_capacity)
			ocr |= OCR_CCS;
		card->state = READY;
	}
	response->type = SD_R3;
	response->words[0] = ocr;
}

/* Whether an addressed command's RCA is this card's. */
static bool addressed(const SdCard *card, uint32_t arg)
{
	return (arg >> 16) == card->rca;
}

/* The application commands that this card defines (4.3.9). */
static bool app_command(SdCard *card, unsigned index, uint32_t arg,
                        SdResponse *response)
{
	State before = card->state;

	switch (index) {
	case 6: /* SET_BUS_WIDTH */
		if (before != TRAN)
			break;
		if ((arg & 3) == 0 || (arg & 3) == 2)
			card->bus_width = (arg & 3) ? 4 : 1;
		r1(card, response, SD_R1, before, 0, true);
		return true;
	case 13: /* SD_STATUS */
		if (before != TRAN)
			break;
		r1(card, response, SD_R1, before, 0, true);
		sd_status(card);
		return true;
	case 22: { /* SEND_NUM_WR_BLOCKS */
		uint8_t count[4] = {
			(uint8_t)(card->well_written >> 24),
			(uint8_t)(card->well_written >> 16),
			(uint8_t)(card->well_written >> 8),
			(uint8_t)card->well_written,
		};

		if (before != TRAN)
			break;
		r1(card, response, SD_R1, before, 0, true);
		send_register(card, count, sizeof(count));
		return true;
	}
	case 23: /* SET_WR_BLK_ERASE_COUNT: a hint, nothing to do */
	case 42: /* SET_CLR_CARD_DETECT: the pull-up on DAT3 */
		if (before != TRAN)
			break;
		r1(card, response, SD_R1, before, 0, true);
		return true;
	case 41:
		if (before != IDLE)
			break;
		send_op_cond(card, arg, response);
		return true;
	case 51: /* SEND_SCR */
		if (before != TRAN)
			break;
		r1(card, response, SD_R1, before, 0, true);
		send_register(card, card->scr, sizeof(card->scr));
		return true;
	case 18:
	case 25:
	case 26:
	case 38:
	case 43:
	case 44:
	case 45:
	case 46:
	case 47:
	case 48:
	case 49:
		stop(card, "a security command, after CMD55", index);
		response->type = SD_NO_RESPONSE;
		return true;
	default:
		return false; /* not defined: a regular command */
	}
	illegal(card, response);
	return true;
}

/* Start a read or a write of CMD17, 18, 24 or 25. */
static void transfer(SdCard *card, uint32_t arg, bool write, bool multiple,
                     SdResponse *response)
{
	uint64_t address = byte_address(card, arg);
	uint32_t found = check_transfer(card, address, write);

	r1(card, response, SD_R1, TRAN, found, false);
	if (found)
		return; /* rejected: no data, still in transfer */
	card->address = address;
	card->multiple = multiple;
	card->remaining = !multiple           ? 1
	                  : card->block_count ? card->block_count
	                                      : UINT32_MAX;
	card->block_count = 0;
	card->reg.len = 0;
	if (write)
		card->well_written = 0;
	card->state = write ? RCV : DATA;
}

void sd_card_command(SdCard *card, unsigned index, uint32_t arg,
                     SdResponse *response)
{
	State before = card->state;
	bool app = card->app_cmd;

	memset(response, 0, sizeof(*response));
	if (!card->powered || before == INA)
		return;
	card->app_cmd = false;
	if (app && app_command(card, index, arg, response))
		return;
	switch (index) {
	case 0: /* GO_IDLE_STATE */
		reset(card);
		return;
	case 2: /* ALL_SEND_CID */
		if (before != READY)
			break;
		card->state = IDENT;
		r2(response, card->cid);
		return;
	case 3: /* SEND_RELATIVE_ADDR */
		if (before != IDENT && before != STBY)
			break;
		card->rca = CARD_RCA;
		card->state = STBY;
		r1(card, response, SD_R6, before, 0, false);
		/* R6: the RCA, then status bits 23, 22, 19 and 12:0. */
		response->words[0] =
			(uint32_t)card->rca << 16 | (response->words[0] >> 8 & 0xc000) |
			(response->words[0] >> 6 & 0x2000) | (response->words[0] & 0x1fff);
		return;
	case 4: /* SET_DSR: the card has no DSR */
		if (before != STBY)
			break;
		return;
	case 6: /* SWITCH_FUNC */
		if (before != TRAN)
			break;
		r1(card, response, SD_R1, before, 0, false);
		switch_function(card, arg);
		return;
	case 7: /* SELECT/DESELECT_CARD */
		/*
		 * The card programs at once: it is never left in prg, so never
		 * in dis, the states that CMD7 also moves between.
		 */
		if (addressed(card, arg) && card->rca != 0) {
			if (before != STBY)
				break;
			card->state = TRAN;
			r1(card, response, SD_R1B, before, 0, false);
			return;
		}
		if (before == TRAN || before == DATA)
			card->state = STBY;
		else if (before != STBY)
			break;
		return; /* not addressed: no response */
	case 8:     /* SEND_IF_COND */
		if (before != IDLE)
			break;
		/* 2.7 to 3.6 V only (Table 4-41); the PCIe bits stay 0. */
		if (((arg >> 8) & 0xf) != 1)
			return;
		card->if_cond = true;
		response->type = SD_R7;
		response->words[0] = arg & 0xfff;
		return;
	case 9:  /* SEND_CSD */
	case 10: /* SEND_CID */
		if (before != STBY)
			break;
		if (addressed(card, arg))
			r2(response, index == 9 ? card->csd : card->cid);
		return;
	case 12: /* STOP_TRANSMISSION */
		if (before != DATA && before != RCV)
			break;
		r1(card, response, SD_R1B, before, 0, false);
		card->state = TRAN; /* through programming, at once */
		return;
	case 13: /* SEND_STATUS */
		if (before < STBY || before > DIS)
			break;
		if (addressed(card, arg))
			r1(card, response, SD_R1, before, 0, false);
		return;
	case 15: /* GO_INACTIVE_STATE */
		if (before < STBY || before > DIS)
			break;
		if (addressed(card, arg))
			card->state = INA;
		return;
	case 16: /* SET_BLOCKLEN */
		if (before != TRAN)
			break;
		if (arg == 0 || arg > SD_BLOCK_SIZE) {
			r1(card, response, SD_R1, before, BLOCK_LEN_ERROR, false);
			return;
		}
		card->block_len = arg; /* SDHC and SDXC: for CMD42 only */
		r1(card, response, SD_R1, before, 0, false);
		return;
	case 17: /* READ_SINGLE_BLOCK */
	case 18: /* READ_MULTIPLE_BLOCK */
	case 24: /* WRITE_BLOCK */
	case 25: /* WRITE_MULTIPLE_BLOCK */
		if (before != TRAN)
			break;
		transfer(card, arg, index >= 24, index == 18 || index == 25, response);
		return;
	case 23: /* SET_BLOCK_COUNT */
		if (before != TRAN)
			break;
		card->block_count = arg;
		r1(card, response, SD_R1, before, 0, false);
		return;
	case 27:
		if (before == TRAN)
			stop(card, "PROGRAM_CSD", index);
		break;
	case 32: /* ERASE_WR_BLK_START */
	case 33: /* ERASE_WR_BLK_END */
		if (before != TRAN)
			break;
		if (byte_address(card, arg) >= card->capacity) {
			r1(card, response, SD_R1, before, OUT_OF_RANGE, false);
			return;
		}
		if (index == 32) {
			card->erase_start = byte_address(card, arg);
			card->erase_start_set = true;
			card->erase_end_set = false;
		} else if (card->erase_start_set) {
			card->erase_end = byte_address(card, arg);
			card->erase_end_set = true;
		} else {
			r1(card, response, SD_R1, before, ERASE_SEQ_ERROR, false);
			return;
		}
		r1(card, response, SD_R1, before, 0, false);
		return;
	case 38: /* ERASE: every argument erases, as discard may */
		if (before != TRAN)
			break;
		if (!card->erase_end_set || card->erase_end < card->erase_start) {
			uint32_t found =
				!card->erase_end_set ? ERASE_SEQ_ERROR : ERASE_PARAM;

			card->erase_start_set = card->erase_end_set = false;
			r1(card, response, SD_R1B, before, found, false);
			return;
		}
		r1(card, response, SD_R1B, before, 0, false);
		card->erase_start_set = card->erase_end_set = false;
		erase(card);
		return;
	case 42:
		if (before == TRAN)
			stop(card, "LOCK_UNLOCK", index);
		break;
	case 55: /* APP_CMD */
		if (before == READY || before == IDENT || before == INA)
			break;
		if (!addressed(card, arg))
			return;
		card->app_cmd = true;
		r1(card, response, SD_R1, before, 0, true);
		return;
	case 56:
		if (before == TRAN)
			stop(card, "GEN_CMD", index);
		break;
	default:
		break;
	}
	illegal(card, response);
}
