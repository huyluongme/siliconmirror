/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the SD memory card (src/hw/sd/sd_card.c). Expected
 * values come from the SD Physical Layer Simplified Specification 9.10;
 * section numbers are its.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/block.h"
#include "core/event.h"
#include "core/machine.h"
#include "hw/sd/sd_card.h"
#include "test.h"
#include "util/macros.h"

#define KIB 1024ull
#define MIB (1024 * KIB)
#define GIB (1024 * MIB)
#define MS 1000000

/* Card states (Table 4-42), as CURRENT_STATE shows them. */
enum { IDLE, READY, IDENT, STBY, TRAN, DATA, RCV, PRG, DIS };

#define OUT_OF_RANGE (1u << 31)
#define ADDRESS_ERROR (1u << 30)
#define BLOCK_LEN_ERROR (1u << 29)
#define ERASE_SEQ_ERROR (1u << 28)
#define ERASE_PARAM (1u << 27)
#define ILLEGAL_COMMAND (1u << 22)
#define READY_FOR_DATA (1u << 8)
#define APP_CMD (1u << 5)
#define STATE(s) ((uint32_t)(s) << 9)

/*
 * A card's storage of any size without the memory: byte n reads as a
 * pattern of n, until a block is written; written blocks are kept.
 */
#define MAX_WRITTEN 64      /* blocks written on a large card */
#define DENSE_MAX (8 * MIB) /* smaller cards are kept whole */

typedef struct Storage {
	BlockDevice dev;
	uint8_t *dense; /* a small card's bytes */
	struct Written {
		uint64_t at;
		uint8_t data[512];
	} *blocks;
	unsigned count;
	bool fail;
	bool discard; /* writes succeed and keep nothing */
} Storage;

static uint8_t pattern(uint64_t n)
{
	return (uint8_t)(n * 7 + (n >> 9));
}

static uint8_t *written(Storage *s, uint64_t block)
{
	for (unsigned i = 0; i < s->count; i++)
		if (s->blocks[i].at == block)
			return s->blocks[i].data;
	return NULL;
}

static bool storage_read(BlockDevice *b, uint64_t offset, void *buf, size_t len)
{
	Storage *s = (Storage *)b;
	uint8_t *out = buf;

	if (s->fail)
		return false;
	if (s->dense != NULL) {
		memcpy(out, s->dense + offset, len);
		return true;
	}
	for (size_t i = 0; i < len; i++) {
		uint64_t n = offset + i;
		uint8_t *block = written(s, n / 512);

		out[i] = block != NULL ? block[n % 512] : pattern(n);
	}
	return true;
}

static bool storage_write(BlockDevice *b, uint64_t offset, const void *buf,
                          size_t len)
{
	Storage *s = (Storage *)b;
	const uint8_t *in = buf;

	if (s->fail)
		return false;
	if (s->discard)
		return true;
	if (s->dense != NULL) {
		memcpy(s->dense + offset, in, len);
		return true;
	}
	for (size_t i = 0; i < len; i++) {
		uint64_t n = offset + i;
		uint8_t *block = written(s, n / 512);

		if (block == NULL) {
			CHECK(s->count < MAX_WRITTEN);
			s->blocks[s->count].at = n / 512;
			block = s->blocks[s->count++].data;
			for (unsigned k = 0; k < 512; k++)
				block[k] = pattern(n / 512 * 512 + k);
		}
		block[n % 512] = in[i];
	}
	return true;
}

static void storage_destroy(BlockDevice *b)
{
	(void)b;
}

static const BlockDeviceOps storage_ops = {
	.read = storage_read,
	.write = storage_write,
	.destroy = storage_destroy,
};

typedef struct Setup {
	Machine *m;
	Storage storage;
	SdCard *card;
} Setup;

static void setup(Setup *s, uint64_t size)
{
	char error[128] = "";

	memset(s, 0, sizeof(*s));
	s->m = machine_create();
	CHECK(s->m != NULL);
	s->storage.dev = (BlockDevice){.ops = &storage_ops, .size = size};
	s->storage.blocks = calloc(MAX_WRITTEN, sizeof(*s->storage.blocks));
	CHECK(s->storage.blocks != NULL);
	if (size <= DENSE_MAX) {
		s->storage.dense = malloc(size);
		CHECK(s->storage.dense != NULL);
		for (uint64_t n = 0; n < size; n++)
			s->storage.dense[n] = pattern(n);
	}
	s->card = sd_card_create(s->m, &s->storage.dev, error, sizeof(error));
	CHECK(s->card != NULL);
	sd_card_power(s->card, true);
}

static void teardown(Setup *s)
{
	sd_card_destroy(s->card);
	machine_destroy(s->m);
	free(s->storage.blocks);
	free(s->storage.dense);
}

static SdResponse cmd(Setup *s, unsigned index, uint32_t arg)
{
	SdResponse r;

	sd_card_command(s->card, index, arg, &r);
	return r;
}

/* An R1 response's card status, checking the type. */
static uint32_t r1(Setup *s, unsigned index, uint32_t arg)
{
	SdResponse r = cmd(s, index, arg);

	CHECK(r.type == SD_R1 || r.type == SD_R1B);
	return r.words[0];
}

static uint32_t acmd(Setup *s, uint16_t rca, unsigned index, uint32_t arg)
{
	CHECK(r1(s, 55, (uint32_t)rca << 16) & APP_CMD);
	return r1(s, index, arg);
}

static void advance(Setup *s, uint64_t ns)
{
	event_queue_run_until(&s->m->events, s->m->events.now + ns);
}

/*
 * Identify the card and select it, as a host of version 2.00 or later
 * does (4.2). Returns its RCA; the card is in the transfer state.
 */
static uint16_t identify(Setup *s)
{
	SdResponse r;
	uint16_t rca;

	cmd(s, 0, 0);
	r = cmd(s, 8, 0x1aa);
	CHECK(r.type == SD_R7);
	r1(s, 55, 0);
	r = cmd(s, 41, 0x40ff8000);
	CHECK(r.type == SD_R3);
	advance(s, MS);
	r1(s, 55, 0);
	r = cmd(s, 41, 0x40ff8000);
	CHECK(r.words[0] & (1u << 31));
	CHECK_EQ(cmd(s, 2, 0).type, SD_R2);
	r = cmd(s, 3, 0);
	CHECK_EQ(r.type, SD_R6);
	rca = (uint16_t)(r.words[0] >> 16);
	CHECK(rca != 0);
	CHECK_EQ(cmd(s, 7, (uint32_t)rca << 16).type, SD_R1B);
	CHECK_EQ(r1(s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));
	return rca;
}

/* The CRC7 examples of 4.5. */
static void test_crc7(void)
{
	static const uint8_t cmd0[] = {0x40, 0, 0, 0, 0};
	static const uint8_t cmd17[] = {0x51, 0, 0, 0, 0};
	static const uint8_t response17[] = {0x11, 0, 0, 0x09, 0};

	CHECK_EQ(sd_crc7(cmd0, 5), 0x4a);       /* 1001010 */
	CHECK_EQ(sd_crc7(cmd17, 5), 0x2a);      /* 0101010 */
	CHECK_EQ(sd_crc7(response17, 5), 0x33); /* 0110011 */
}

/* Sizes: whole 512 KB, up to 2 TB. */
static void test_create(void)
{
	static const uint64_t bad[] = {0, 511 * KIB, 1 * MIB + 512,
	                               2048 * GIB + 512 * KIB};
	Machine *m = machine_create();
	Storage storage = {.dev = {.ops = &storage_ops}};
	char error[160];

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		storage.dev.size = bad[i];
		CHECK(sd_card_create(m, &storage.dev, error, sizeof(error)) == NULL);
		CHECK(strstr(error, "an SD card holds a multiple of 512 KB, up to "
		                    "2 TB, not ") != NULL);
		CHECK(strstr(error, "truncate -s") != NULL);
	}
	storage.dev.size = 2048 * GIB;
	SdCard *card = sd_card_create(m, &storage.dev, error, sizeof(error));

	CHECK(card != NULL);
	sd_card_destroy(card);
	machine_destroy(m);
}

/* Without power, and in the inactive state, the card says nothing. */
static void test_power(void)
{
	Setup s;
	SdResponse r;

	setup(&s, 1 * MIB);
	sd_card_power(s.card, false);
	r = cmd(&s, 8, 0x1aa);
	CHECK_EQ(r.type, SD_NO_RESPONSE);
	CHECK_EQ(sd_card_read(s.card, (uint8_t[512]){0}), 0);
	CHECK(!sd_card_write(s.card, (uint8_t[512]){0}, 512));
	sd_card_power(s.card, true);
	CHECK_EQ(cmd(&s, 8, 0x1aa).type, SD_R7);

	/* ACMD41 with no voltage in common: inactive (4.2.2). */
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x80).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 55, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 0, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 8, 0x1aa).type, SD_NO_RESPONSE); /* CMD0 did not wake it */
	sd_card_power(s.card, false);
	sd_card_power(s.card, true);
	CHECK_EQ(cmd(&s, 8, 0x1aa).type, SD_R7);

	/* CMD15 also makes it inactive. */
	uint16_t rca = identify(&s);

	CHECK_EQ(cmd(&s, 15, 0x12340000).type, SD_NO_RESPONSE); /* not it */
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));
	cmd(&s, 15, (uint32_t)rca << 16);
	CHECK_EQ(cmd(&s, 13, (uint32_t)rca << 16).type, SD_NO_RESPONSE);
	teardown(&s);
}

/* Identification: CMD8, ACMD41, CMD2, CMD3 (4.2.2, 4.2.3). */
static void test_identification(void)
{
	Setup s;
	SdResponse r;
	uint32_t status;

	setup(&s, 1 * MIB);
	/* CMD8 echoes the voltage and the check pattern; 2.7-3.6 V only. */
	r = cmd(&s, 8, 0x2aa);
	CHECK_EQ(r.type, SD_NO_RESPONSE);
	r = cmd(&s, 8, 0xfffff1a5);
	CHECK_EQ(r.type, SD_R7);
	CHECK_EQ(r.words[0], 0x1a5);
	/* CMD55 in idle: RCA 0; APP_CMD set. */
	status = r1(&s, 55, 0);
	CHECK_EQ(status, STATE(IDLE) | READY_FOR_DATA | APP_CMD);
	/* A query: the OCR, busy; nothing starts. */
	r = cmd(&s, 41, 0);
	CHECK_EQ(r.type, SD_R3);
	CHECK_EQ(r.words[0], 0x00ff8000);
	r1(&s, 55, 0);
	r = cmd(&s, 41, 0x40300000);
	CHECK_EQ(r.words[0], 0x00ff8000); /* busy for 1 ms */
	advance(&s, MS - 1);
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x40300000).words[0], 0x00ff8000);
	CHECK_EQ(cmd(&s, 2, 0).type, SD_NO_RESPONSE); /* still idle: illegal */
	advance(&s, 1);
	r1(&s, 55, 0); /* ILLEGAL_COMMAND for CMD2 shows here */
	r = cmd(&s, 41, 0x40300000);
	CHECK_EQ(r.words[0], 0x80ff8000);              /* ready; SDSC: CCS 0 */
	CHECK_EQ(cmd(&s, 41, 0).type, SD_NO_RESPONSE); /* not after CMD55 */

	/* CMD2: the CID, its CRC and end bit in the last byte. */
	r = cmd(&s, 2, 0);
	CHECK_EQ(r.type, SD_R2);
	{
		uint8_t cid[16];

		for (int i = 0; i < 16; i++)
			cid[i] = (uint8_t)(r.words[i / 4] >> (24 - 8 * (i % 4)));
		CHECK_EQ(cid[0], 0x00);                  /* MID */
		CHECK(memcmp(cid + 1, "SM", 2) == 0);    /* OID */
		CHECK(memcmp(cid + 3, "SMSD1", 5) == 0); /* PNM */
		CHECK_EQ(cid[8], 0x10);                  /* PRV 1.0 */
		CHECK_EQ(cid[9] << 24 | cid[10] << 16 | cid[11] << 8 | cid[12],
		         0x5ca1ab1e);
		CHECK_EQ((cid[13] & 0xf) << 8 | cid[14], 26 << 4 | 11); /* MDT */
		CHECK_EQ(cid[15], sd_crc7(cid, 15) << 1 | 1);
	}
	/* CMD3: the RCA, and status bits 23, 22, 19 and 12:0 (Table 4-39). */
	CHECK_EQ(cmd(&s, 2, 0).type, SD_NO_RESPONSE); /* illegal in ident */
	r = cmd(&s, 3, 0);
	CHECK_EQ(r.type, SD_R6);
	CHECK_EQ(r.words[0], 0xaaaa0000u | 0x4000 | STATE(IDENT) | READY_FOR_DATA);
	r = cmd(&s, 3, 0); /* again, in stand-by */
	CHECK_EQ(r.words[0], 0xaaaa0000u | STATE(STBY) | READY_FOR_DATA);
	/* CMD4: no DSR, no response. */
	CHECK_EQ(cmd(&s, 4, 0x04040000).type, SD_NO_RESPONSE);
	CHECK_EQ(r1(&s, 13, 0xaaaa0000) & ILLEGAL_COMMAND, 0);
	teardown(&s);
}

/* High capacity needs CMD8 and HCS (4.2.3); then CCS is 1. */
static void test_high_capacity(void)
{
	Setup s;
	SdResponse r;

	setup(&s, 4 * GIB);
	cmd(&s, 8, 0x1aa);
	r1(&s, 55, 0);
	cmd(&s, 41, 0x00ff8000); /* no HCS */
	advance(&s, 10 * MS);
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x40ff8000).words[0], 0x00ff8000); /* busy */
	cmd(&s, 0, 0);
	r1(&s, 55, 0);
	cmd(&s, 41, 0x40ff8000); /* no CMD8 since CMD0 */
	advance(&s, 10 * MS);
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x40ff8000).words[0], 0x00ff8000);
	cmd(&s, 0, 0);
	cmd(&s, 8, 0x1aa);
	r1(&s, 55, 0);
	cmd(&s, 41, 0x40ff8000);
	advance(&s, MS);
	r1(&s, 55, 0);
	r = cmd(&s, 41, 0x40ff8000);
	CHECK_EQ(r.words[0], 0xc0ff8000);
	teardown(&s);

	/* An SDSC card answers CCS 0, with HCS or not. */
	setup(&s, 2 * GIB);
	cmd(&s, 8, 0x1aa);
	r1(&s, 55, 0);
	cmd(&s, 41, 0x40ff8000);
	advance(&s, MS);
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x40ff8000).words[0], 0x80ff8000);
	teardown(&s);
}

/* The CSD of each capacity (5.3.2, 5.3.3). */
static void test_csd(void)
{
	static const struct {
		uint64_t size;
		unsigned structure, read_bl_len;
		uint32_t c_size;
	} cards[] = {
		{1 * MIB, 0, 9, 3}, /* 4 units of 256 KB */
		{1 * GIB, 0, 9, 4095},    {1 * GIB + 512 * KIB, 0, 10, 2048},
		{2 * GIB, 0, 10, 4095},   {2 * GIB + 512 * KIB, 1, 9, 4096},
		{64 * GIB, 1, 9, 131071},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cards); i++) {
		Setup s;
		uint16_t rca;
		SdResponse r;
		uint8_t csd[16];

		setup(&s, cards[i].size);
		rca = identify(&s);
		cmd(&s, 7, 0); /* deselect: to stand-by */
		r = cmd(&s, 9, (uint32_t)rca << 16);
		CHECK_EQ(r.type, SD_R2);
		for (int k = 0; k < 16; k++)
			csd[k] = (uint8_t)(r.words[k / 4] >> (24 - 8 * (k % 4)));
		CHECK_EQ(csd[0] >> 6, cards[i].structure);
		CHECK_EQ(csd[1], 0x0e);                     /* TAAC */
		CHECK_EQ(csd[3], 0x32);                     /* TRAN_SPEED: 25 MHz */
		CHECK_EQ(csd[4] << 4 | csd[5] >> 4, 0x5b5); /* CCC */
		CHECK_EQ(csd[5] & 0xf, cards[i].read_bl_len);
		if (cards[i].structure == 0) {
			CHECK_EQ(csd[6] >> 7, 1); /* READ_BL_PARTIAL */
			CHECK_EQ((csd[6] & 3) << 10 | csd[7] << 2 | csd[8] >> 6,
			         cards[i].c_size);
			CHECK_EQ((csd[9] & 3) << 1 | csd[10] >> 7, 7); /* C_SIZE_MULT */
		} else {
			CHECK_EQ((csd[7] & 0x3f) << 16 | csd[8] << 8 | csd[9],
			         cards[i].c_size);
		}
		CHECK_EQ(csd[10] & 0x40, 0x40);             /* ERASE_BLK_EN */
		CHECK_EQ((csd[12] & 3) << 2 | csd[13] >> 6, /* WRITE_BL_LEN */
		         cards[i].read_bl_len);
		CHECK_EQ(csd[15], sd_crc7(csd, 15) << 1 | 1);
		/* CMD10 gives the CID; not addressed, nothing. */
		CHECK_EQ(cmd(&s, 10, (uint32_t)rca << 16).type, SD_R2);
		CHECK_EQ(cmd(&s, 9, 0x12340000).type, SD_NO_RESPONSE);
		teardown(&s);
	}
}

/* Illegal commands and application commands (4.3.9, 4.6.1). */
static void test_commands(void)
{
	Setup s;
	uint16_t rca;
	uint32_t status;

	setup(&s, 1 * MIB);
	rca = identify(&s);
	/* An illegal command: no response; the next response tells. */
	CHECK_EQ(cmd(&s, 2, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 5, 0).type, SD_NO_RESPONSE); /* SDIO */
	CHECK_EQ(cmd(&s, 41, 0).type, SD_NO_RESPONSE);
	status = r1(&s, 13, (uint32_t)rca << 16);
	CHECK_EQ(status, ILLEGAL_COMMAND | STATE(TRAN) | READY_FOR_DATA);
	status = r1(&s, 13, (uint32_t)rca << 16);
	CHECK_EQ(status, STATE(TRAN) | READY_FOR_DATA);
	/* CMD13 to another card: nothing. */
	CHECK_EQ(cmd(&s, 13, 0x12340000).type, SD_NO_RESPONSE);

	/* CMD55 twice: both say APP_CMD; then a defined ACMD says it too. */
	CHECK(r1(&s, 55, (uint32_t)rca << 16) & APP_CMD);
	CHECK(r1(&s, 55, (uint32_t)rca << 16) & APP_CMD);
	CHECK(r1(&s, 42, 1) & APP_CMD);
	/* An undefined ACMD is the regular command, without APP_CMD. */
	r1(&s, 55, (uint32_t)rca << 16);
	status = r1(&s, 16, 512);
	CHECK_EQ(status & APP_CMD, 0);
	/* Only the next command is an ACMD. */
	r1(&s, 55, (uint32_t)rca << 16);
	r1(&s, 16, 512);
	CHECK_EQ(cmd(&s, 42, 1).type, SD_NO_RESPONSE); /* CMD42, not ACMD42 */
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);       /* is not emulated */
	s.m->stop = MACHINE_STOP_NONE;
	r1(&s, 13, (uint32_t)rca << 16);
	/* CMD55 to another card: no response, and no ACMD. */
	CHECK_EQ(cmd(&s, 55, 0x12340000).type, SD_NO_RESPONSE);
	CHECK_EQ(r1(&s, 23, 4) & APP_CMD, 0); /* CMD23, not ACMD23 */
	/* An illegal ACMD is an illegal command (4.3.9.1 (5)). */
	r1(&s, 55, (uint32_t)rca << 16);
	CHECK_EQ(cmd(&s, 41, 0).type, SD_NO_RESPONSE);
	CHECK(r1(&s, 13, (uint32_t)rca << 16) & ILLEGAL_COMMAND);
	r1(&s, 55, (uint32_t)rca << 16);
	cmd(&s, 7, 0); /* undefined: CMD7, to stand-by */
	r1(&s, 55, (uint32_t)rca << 16);
	CHECK_EQ(cmd(&s, 6, 2).type, SD_NO_RESPONSE); /* ACMD6 needs tran */
	CHECK_EQ(cmd(&s, 13, 0).type, SD_NO_RESPONSE);
	r1(&s, 55, (uint32_t)rca << 16);
	for (unsigned i = 0; i < 6; i++) {
		static const unsigned acmds[] = {13, 22, 23, 42, 51, 6};

		r1(&s, 55, (uint32_t)rca << 16);
		CHECK_EQ(cmd(&s, acmds[i], 0).type, SD_NO_RESPONSE);
	}
	teardown(&s);
}

/* Selecting and deselecting (CMD7). */
static void test_select(void)
{
	Setup s;
	uint16_t rca;

	setup(&s, 1 * MIB);
	rca = identify(&s);
	/* Selecting again in tran: illegal. */
	CHECK_EQ(cmd(&s, 7, (uint32_t)rca << 16).type, SD_NO_RESPONSE);
	CHECK(r1(&s, 13, (uint32_t)rca << 16) & ILLEGAL_COMMAND);
	/* Another card's RCA, or 0: this one leaves, without a word. */
	CHECK_EQ(cmd(&s, 7, 0x12340000).type, SD_NO_RESPONSE);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16), STATE(STBY) | READY_FOR_DATA);
	CHECK_EQ(cmd(&s, 7, 0).type, SD_NO_RESPONSE); /* stays */
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16), STATE(STBY) | READY_FOR_DATA);
	CHECK_EQ(cmd(&s, 7, (uint32_t)rca << 16).type, SD_R1B);
	/* From data: back to stand-by too. */
	r1(&s, 18, 0);
	cmd(&s, 7, 0);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(STBY));
	/* Not in idle, ready or ident. */
	cmd(&s, 0, 0);
	CHECK_EQ(cmd(&s, 7, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 13, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(cmd(&s, 15, 0).type, SD_NO_RESPONSE);
	teardown(&s);
}

static bool same_as_storage(Setup *s, const uint8_t *buf, uint64_t at,
                            size_t len)
{
	uint8_t expected[512];

	storage_read(&s->storage.dev, at, expected, len);
	return memcmp(buf, expected, len) == 0;
}

/* Reads (4.3.3): one block, several, partial blocks on SDSC. */
static void test_read(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512];
	uint32_t status;

	setup(&s, 1 * MIB);
	rca = identify(&s);
	CHECK_EQ(r1(&s, 16, 512), STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 17, 1024), STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(DATA));
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK(same_as_storage(&s, buf, 1024, 512));
	CHECK_EQ(sd_card_read(s.card, buf), 0); /* one block only */
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));

	/* Partial blocks inside 512 bytes (Table 4-4). */
	r1(&s, 16, 100);
	r1(&s, 17, 412);
	CHECK_EQ(sd_card_read(s.card, buf), 100);
	CHECK(same_as_storage(&s, buf, 412, 100));
	status = r1(&s, 17, 413); /* crosses 512 */
	CHECK_EQ(status, ADDRESS_ERROR | STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	status = r1(&s, 17, 1 * MIB - 99); /* past the end */
	CHECK_EQ(status & OUT_OF_RANGE, OUT_OF_RANGE);
	CHECK_EQ(r1(&s, 17, 1 * MIB), OUT_OF_RANGE | STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 16, 0), BLOCK_LEN_ERROR | STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 16, 513) & BLOCK_LEN_ERROR, BLOCK_LEN_ERROR);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & 0xffff0000, 0); /* cleared */
	r1(&s, 16, 512);

	/* Several blocks, until CMD12. */
	r1(&s, 18, 2048);
	for (unsigned i = 0; i < 3; i++) {
		CHECK_EQ(sd_card_read(s.card, buf), 512);
		CHECK(same_as_storage(&s, buf, 2048 + 512 * i, 512));
	}
	CHECK_EQ(cmd(&s, 12, 0).type, SD_R1B);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));
	CHECK_EQ(cmd(&s, 12, 0).type, SD_NO_RESPONSE); /* illegal in tran */
	r1(&s, 13, (uint32_t)rca << 16);

	/* To the end: OUT_OF_RANGE comes with CMD12. */
	r1(&s, 18, 1 * MIB - 512);
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	status = r1(&s, 12, 0);
	CHECK_EQ(status, OUT_OF_RANGE | STATE(DATA) | READY_FOR_DATA);

	/* CMD23 counts the blocks: no CMD12. */
	r1(&s, 23, 2);
	r1(&s, 18, 0);
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(DATA));
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));
	/* CMD23 counts for the next transfer only. */
	r1(&s, 18, 0);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(sd_card_read(s.card, buf), 512);
	r1(&s, 12, 0);
	/* Reads fail on the host: the machine stops. */
	s.storage.fail = true;
	r1(&s, 17, 0);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "sd-card: cannot read the image at 0x0"));
	teardown(&s);

	/* High capacity: block addresses, blocks of 512 bytes. */
	setup(&s, 4 * GIB);
	rca = identify(&s);
	r1(&s, 16, 100); /* for CMD42 only */
	r1(&s, 17, 2);
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK(same_as_storage(&s, buf, 1024, 512));
	r1(&s, 17, (uint32_t)(4 * GIB / 512 - 1));
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK(same_as_storage(&s, buf, 4 * GIB - 512, 512));
	CHECK_EQ(r1(&s, 17, (uint32_t)(4 * GIB / 512)) & OUT_OF_RANGE,
	         OUT_OF_RANGE);
	(void)rca;
	teardown(&s);
}

/* Writes (4.3.4): whole blocks, aligned on SDSC. */
static void test_write(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512], back[512];
	SdResponse r;

	for (unsigned i = 0; i < 512; i++)
		buf[i] = (uint8_t)(i ^ 0x5a);
	setup(&s, 1 * MIB);
	rca = identify(&s);
	CHECK(!sd_card_receiving(s.card));
	CHECK(!sd_card_write(s.card, buf, 512)); /* not expected */
	r1(&s, 24, 512);
	CHECK(sd_card_receiving(s.card));
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(RCV));
	CHECK(sd_card_write(s.card, buf, 512));
	CHECK(!sd_card_receiving(s.card));
	storage_read(&s.storage.dev, 512, back, 512);
	CHECK(memcmp(buf, back, 512) == 0);
	CHECK(same_as_storage(&s, back, 512, 512));
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));
	/* ACMD22: the blocks written. */
	acmd(&s, rca, 22, 0);
	CHECK_EQ(sd_card_read(s.card, back), 4);
	CHECK(memcmp(back, "\0\0\0\1", 4) == 0);

	/* Misaligned, the wrong block length, the wrong size. */
	CHECK_EQ(r1(&s, 24, 100) & ADDRESS_ERROR, ADDRESS_ERROR);
	CHECK(!sd_card_receiving(s.card));
	r1(&s, 16, 100);
	CHECK_EQ(r1(&s, 24, 512) & BLOCK_LEN_ERROR, BLOCK_LEN_ERROR);
	r1(&s, 16, 512);
	r1(&s, 24, 0);
	CHECK(!sd_card_write(s.card, buf, 100));
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16),
	         BLOCK_LEN_ERROR | STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 24, 1 * MIB) & OUT_OF_RANGE, OUT_OF_RANGE);

	/* Several blocks, until CMD12, which comes in the receive state. */
	r1(&s, 25, 4096);
	for (unsigned i = 0; i < 3; i++) {
		buf[0] = (uint8_t)i;
		CHECK(sd_card_write(s.card, buf, 512));
	}
	r = cmd(&s, 12, 0);
	CHECK_EQ(r.type, SD_R1B);
	CHECK_EQ(r.words[0], STATE(RCV) | READY_FOR_DATA);
	for (unsigned i = 0; i < 3; i++) {
		storage_read(&s.storage.dev, 4096 + 512 * i, back, 512);
		CHECK_EQ(back[0], i);
		CHECK_EQ(back[1], buf[1]);
	}
	acmd(&s, rca, 22, 0);
	CHECK_EQ(sd_card_read(s.card, back), 4);
	CHECK(memcmp(back, "\0\0\0\3", 4) == 0);

	/* CMD23 ends the write after its count. */
	r1(&s, 23, 2);
	r1(&s, 25, 8192);
	CHECK(sd_card_write(s.card, buf, 512));
	CHECK(sd_card_write(s.card, buf, 512));
	CHECK(!sd_card_write(s.card, buf, 512));
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16) & STATE(15), STATE(TRAN));

	/* Past the end: OUT_OF_RANGE, and the card takes no more. */
	r1(&s, 25, 1 * MIB - 512);
	CHECK(sd_card_write(s.card, buf, 512));
	CHECK(!sd_card_write(s.card, buf, 512));
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16),
	         OUT_OF_RANGE | STATE(TRAN) | READY_FOR_DATA);

	/* Writes fail on the host: the machine stops. */
	s.storage.fail = true;
	r1(&s, 24, 0);
	CHECK(!sd_card_write(s.card, buf, 512));
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	CHECK(strstr(s.m->stop_message, "sd-card: cannot write the image at 0x0"));
	teardown(&s);

	/* High capacity: block addresses. */
	setup(&s, 4 * GIB);
	identify(&s);
	r1(&s, 24, 3);
	CHECK(sd_card_write(s.card, buf, 512));
	storage_read(&s.storage.dev, 3 * 512, back, 512);
	CHECK(memcmp(back, buf, 512) == 0);
	teardown(&s);
}

/* Erase (4.3.5): from the start block to the end block, as zeros. */
static void test_erase(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512];
	uint32_t status;

	setup(&s, 1 * MIB);
	rca = identify(&s);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16), STATE(TRAN) | READY_FOR_DATA);
	r1(&s, 32, 1100);
	r1(&s, 33, 2047);
	{
		SdResponse r = cmd(&s, 38, 0);

		CHECK_EQ(r.type, SD_R1B);
		CHECK_EQ(r.words[0], STATE(TRAN) | READY_FOR_DATA);
	}
	for (unsigned b = 1; b <= 4; b++) {
		storage_read(&s.storage.dev, 512 * b, buf, 512);
		if (b == 1 || b == 4) {
			CHECK(same_as_storage(&s, buf, 512 * b, 512));
			CHECK(buf[3] != 0 || buf[4] != 0);
		} else {
			CHECK(memcmp(buf, (uint8_t[512]){0}, 512) == 0);
		}
	}
	/* Sequence errors. */
	status = r1(&s, 38, 0);
	CHECK_EQ(status, ERASE_SEQ_ERROR | STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 33, 512) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	r1(&s, 32, 512);
	CHECK_EQ(r1(&s, 38, 0) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	r1(&s, 32, 4096);
	r1(&s, 33, 512);
	CHECK_EQ(r1(&s, 38, 0) & ERASE_PARAM, ERASE_PARAM);
	CHECK_EQ(r1(&s, 32, 1 * MIB), OUT_OF_RANGE | STATE(TRAN) | READY_FOR_DATA);
	/* A new start forgets the end; a failed erase forgets the start. */
	r1(&s, 32, 0);
	r1(&s, 33, 512);
	r1(&s, 32, 1024);
	CHECK_EQ(r1(&s, 38, 0) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	r1(&s, 32, 0);
	CHECK_EQ(r1(&s, 38, 0) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	CHECK_EQ(r1(&s, 33, 512) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	r1(&s, 32, 0);
	CHECK_EQ(r1(&s, 33, 1 * MIB) & OUT_OF_RANGE, OUT_OF_RANGE);
	CHECK_EQ(r1(&s, 38, 0) & ERASE_SEQ_ERROR, ERASE_SEQ_ERROR);
	/* A whole card, in large pieces, up to its last block. */
	r1(&s, 32, 0);
	r1(&s, 33, 1 * MIB - 1);
	r1(&s, 38, 1);
	storage_read(&s.storage.dev, 1 * MIB - 512, buf, 512);
	CHECK(memcmp(buf, (uint8_t[512]){0}, 512) == 0);
	(void)rca;
	s.storage.fail = true;
	r1(&s, 32, 0);
	r1(&s, 33, 0);
	r1(&s, 38, 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	teardown(&s);
}

/*
 * CMD0 clears the errors still to report; a register read stopped by
 * CMD12 leaves nothing for the next block read.
 */
static void test_errors_and_aborts(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512];

	setup(&s, 1 * MIB);
	rca = identify(&s);
	CHECK_EQ(cmd(&s, 2, 0).type, SD_NO_RESPONSE); /* illegal in tran */
	cmd(&s, 0, 0);
	cmd(&s, 8, 0x1aa);
	CHECK_EQ(r1(&s, 55, 0) & ILLEGAL_COMMAND, 0);
	rca = identify(&s);
	acmd(&s, rca, 51, 0); /* the SCR, never read */
	r1(&s, 12, 0);
	CHECK_EQ(r1(&s, 17, 0), STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK(same_as_storage(&s, buf, 0, 512));
	teardown(&s);
}

/* CMD6, the switch function (4.3.10): high speed in group 1. */
static void test_switch(void)
{
	Setup s;
	uint16_t rca;
	uint8_t st[64];
	SdResponse r;

	setup(&s, 1 * MIB);
	rca = identify(&s);
	/* Mode 0: check high speed; nothing changes. */
	r1(&s, 6, 0x00fffff1);
	CHECK_EQ(sd_card_read(s.card, st), 64);
	CHECK_EQ(st[0] << 8 | st[1], 100);      /* 511:496: 100 mA */
	CHECK_EQ(st[2] << 8 | st[3], 0x8001);   /* group 6 */
	CHECK_EQ(st[12] << 8 | st[13], 0x8003); /* group 1: 0, 1 and 15 */
	CHECK_EQ(st[14], 0x00);                 /* groups 6 and 5 */
	CHECK_EQ(st[16], 0x01);                 /* groups 2 and 1 */
	CHECK_EQ(st[17], 1);                    /* version */
	CHECK(!sd_card_high_speed(s.card));
	/* Mode 1: switch. */
	r1(&s, 6, 0x80fffff1);
	CHECK_EQ(sd_card_read(s.card, st), 64);
	CHECK_EQ(st[16], 0x01);
	CHECK(sd_card_high_speed(s.card));
	/* No change asked: the current function. */
	r1(&s, 6, 0x80ffffff);
	sd_card_read(s.card, st);
	CHECK_EQ(st[16], 0x01);
	CHECK(sd_card_high_speed(s.card));
	/* An unsupported function: 0xf everywhere, and no switch. */
	r1(&s, 6, 0x80ffff1f);
	sd_card_read(s.card, st);
	CHECK_EQ(st[14], 0xff);
	CHECK_EQ(st[16], 0xff);
	CHECK(sd_card_high_speed(s.card));
	r1(&s, 6, 0x80fffff2);
	sd_card_read(s.card, st);
	CHECK_EQ(st[16], 0xff);
	r1(&s, 6, 0x80fffff0);
	sd_card_read(s.card, st);
	CHECK(!sd_card_high_speed(s.card));
	r1(&s, 6, 0x80fffff1);
	sd_card_read(s.card, st);
	/* The CSD's TRAN_SPEED follows: 50 MHz. */
	cmd(&s, 7, 0);
	r = cmd(&s, 9, (uint32_t)rca << 16);
	CHECK_EQ((r.words[0] & 0xff), 0x5a);
	/* CMD0 goes back to the default speed. */
	cmd(&s, 0, 0);
	CHECK(!sd_card_high_speed(s.card));
	teardown(&s);
}

/* ACMD6, ACMD13, ACMD51 and the others of class 8. */
static void test_app_commands(void)
{
	static const struct {
		uint64_t size;
		uint8_t security;
	} cards[] = {{1 * MIB, 0x25},
	             {4 * GIB, 0x35},
	             {32 * GIB, 0x35}, /* SDHC up to 32 GB */
	             {32 * GIB + 512 * KIB, 0x45},
	             {64 * GIB, 0x45}};

	for (size_t i = 0; i < ARRAY_SIZE(cards); i++) {
		Setup s;
		uint16_t rca;
		uint8_t buf[64];

		setup(&s, cards[i].size);
		rca = identify(&s);
		CHECK_EQ(sd_card_bus_width(s.card), 1);
		CHECK_EQ(acmd(&s, rca, 6, 2), STATE(TRAN) | READY_FOR_DATA | APP_CMD);
		CHECK_EQ(sd_card_bus_width(s.card), 4);
		acmd(&s, rca, 6, 1); /* reserved: no change */
		CHECK_EQ(sd_card_bus_width(s.card), 4);
		acmd(&s, rca, 13, 0);
		CHECK_EQ(sd_card_read(s.card, buf), 64);
		CHECK_EQ(buf[0] >> 6, 2);  /* 4 bits */
		CHECK_EQ(buf[8], 4);       /* SPEED_CLASS */
		CHECK_EQ(buf[10] >> 4, 9); /* AU_SIZE */
		acmd(&s, rca, 6, 0);
		CHECK_EQ(sd_card_bus_width(s.card), 1);
		acmd(&s, rca, 13, 0);
		sd_card_read(s.card, buf);
		CHECK_EQ(buf[0] >> 6, 0);
		CHECK_EQ(acmd(&s, rca, 51, 0) & APP_CMD, APP_CMD);
		CHECK_EQ(sd_card_read(s.card, buf), 8);
		CHECK(
			memcmp(buf,
		           (uint8_t[]){0x02, cards[i].security, 0x80, 0x02, 0, 0, 0, 0},
		           8) == 0);
		CHECK_EQ(sd_card_read(s.card, buf), 0);
		CHECK_EQ(acmd(&s, rca, 23, 100),
		         STATE(TRAN) | READY_FOR_DATA | APP_CMD);
		CHECK_EQ(acmd(&s, rca, 42, 0), STATE(TRAN) | READY_FOR_DATA | APP_CMD);
		/* CMD0 sets the bus width back to 1 bit. */
		acmd(&s, rca, 6, 2);
		cmd(&s, 0, 0);
		CHECK_EQ(sd_card_bus_width(s.card), 1);
		teardown(&s);
	}
}

/* What the card must have but smirror does not emulate stops it. */
static void test_stops(void)
{
	static const struct {
		bool app;
		unsigned index;
		const char *message;
	} stops[] = {
		{false, 42, "sd-card: LOCK_UNLOCK (CMD42) is not emulated"},
		{false, 27, "sd-card: PROGRAM_CSD (CMD27) is not emulated"},
		{false, 56, "sd-card: GEN_CMD (CMD56) is not emulated"},
		{true, 18, "a security command, after CMD55 (CMD18) is not"},
		{true, 25, "(CMD25) is not emulated"},
		{true, 26, "(CMD26) is not emulated"},
		{true, 38, "(CMD38) is not emulated"},
		{true, 43, "(CMD43) is not emulated"},
		{true, 44, "(CMD44) is not emulated"},
		{true, 45, "(CMD45) is not emulated"},
		{true, 46, "(CMD46) is not emulated"},
		{true, 47, "(CMD47) is not emulated"},
		{true, 48, "(CMD48) is not emulated"},
		{true, 49, "(CMD49) is not emulated"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(stops); i++) {
		Setup s;
		uint16_t rca;

		setup(&s, 1 * MIB);
		rca = identify(&s);
		if (stops[i].app)
			r1(&s, 55, (uint32_t)rca << 16);
		CHECK_EQ(cmd(&s, stops[i].index, 0).type, SD_NO_RESPONSE);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}
}

/* The bytes of a 16-byte register in an R2 response. */
static void r2_bytes(const SdResponse *r, uint8_t out[16])
{
	for (int i = 0; i < 16; i++)
		out[i] = (uint8_t)(r->words[i / 4] >> (24 - 8 * (i % 4)));
}

/*
 * The registers, byte for byte, as the fields of Tables 5-2, 5-4 and 5-16
 * place them: the CID, the CSD of each kind of card, the SD status and
 * CMD6's status.
 */
static void test_registers(void)
{
	static const uint8_t cid[16] = {0x00, 0x53, 0x4d, 0x53, 0x4d, 0x53,
	                                0x44, 0x31, 0x10, 0x5c, 0xa1, 0xab,
	                                0x1e, 0x01, 0xab, 0x03};
	static const struct {
		uint64_t size;
		uint8_t csd[16], csd_high_speed[16];
	} cards[] = {
		{1 * MIB,
	     {0x00, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x80, 0x00, 0xff, 0xff, 0xff,
	      0x80, 0x0a, 0x40, 0x00, 0xe1},
	     {0x00, 0x0e, 0x00, 0x5a, 0x5b, 0x59, 0x80, 0x00, 0xff, 0xff, 0xff,
	      0x80, 0x0a, 0x40, 0x00, 0x37}},
		{1 * GIB + 512 * KIB,
	     {0x00, 0x0e, 0x00, 0x32, 0x5b, 0x5a, 0x82, 0x00, 0x3f, 0xff, 0xff,
	      0x80, 0x0a, 0x80, 0x00, 0x91},
	     {0}},
		{4 * GIB,
	     {0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x00, 0x1f, 0xff, 0x7f,
	      0x80, 0x0a, 0x40, 0x00, 0xc3},
	     {0}},
		{64 * GIB,
	     {0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x01, 0xff, 0xff, 0x7f,
	      0x80, 0x0a, 0x40, 0x00, 0x17},
	     {0}},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cards); i++) {
		Setup s;
		uint16_t rca;
		uint8_t reg[16], st[64], expected[64];

		setup(&s, cards[i].size);
		rca = identify(&s);
		cmd(&s, 7, 0);
		{
			SdResponse r = cmd(&s, 10, (uint32_t)rca << 16);

			r2_bytes(&r, reg);
			CHECK(memcmp(reg, cid, 16) == 0);
			r = cmd(&s, 9, (uint32_t)rca << 16);
			r2_bytes(&r, reg);
			CHECK(memcmp(reg, cards[i].csd, 16) == 0);
		}
		cmd(&s, 7, (uint32_t)rca << 16);

		/* The SD status: the bus width, class 10, AU of 4 MB. */
		for (unsigned width = 1; width <= 4; width += 3) {
			acmd(&s, rca, 6, width == 4 ? 2 : 0);
			acmd(&s, rca, 13, 0);
			CHECK_EQ(sd_card_read(s.card, st), 64);
			memset(expected, 0, 64);
			expected[0] = width == 4 ? 0x80 : 0;
			expected[8] = 4;
			expected[10] = 0x90;
			CHECK(memcmp(st, expected, 64) == 0);
		}

		if (i == 0) {
			SdResponse r;

			r1(&s, 6, 0x80fffff1);
			sd_card_read(s.card, st);
			cmd(&s, 7, 0);
			r = cmd(&s, 9, (uint32_t)rca << 16);
			r2_bytes(&r, reg);
			CHECK(memcmp(reg, cards[i].csd_high_speed, 16) == 0);
		}
		teardown(&s);
	}
}

/* CMD6's status for arg: the function of each group, or 0xf. */
static void check_switch(Setup *s, uint32_t arg, const uint8_t result[3],
                         bool high_speed)
{
	static const uint8_t head[14] = {0x00, 0x64, 0x80, 0x01, 0x80, 0x01, 0x80,
	                                 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x03};
	uint8_t st[64], expected[64] = {0};

	CHECK_EQ(r1(s, 6, arg), STATE(TRAN) | READY_FOR_DATA);
	CHECK_EQ(sd_card_read(s->card, st), 64);
	memcpy(expected, head, sizeof(head));
	memcpy(expected + 14, result, 3);
	expected[17] = 1;
	if (memcmp(st, expected, 64) != 0)
		fprintf(stderr, "CMD6 0x%08x: %02x %02x %02x\n", (unsigned)arg, st[14],
		        st[15], st[16]);
	CHECK(memcmp(st, expected, 64) == 0);
	CHECK_EQ(sd_card_high_speed(s->card), high_speed);
}

/* CMD6's arguments, function by function and group by group. */
static void test_switch_args(void)
{
	Setup s;

	setup(&s, 1 * MIB);
	identify(&s);
	/* No change asked, at default speed: function 0 now. */
	check_switch(&s, 0x00ffffff, (const uint8_t[]){0x00, 0x00, 0x00}, false);
	/* Function 0 in some groups, 0xf in others. */
	check_switch(&s, 0x0000ff01, (const uint8_t[]){0x00, 0x00, 0x01}, false);
	/* An unsupported function in group 6. */
	check_switch(&s, 0x801ffff1, (const uint8_t[]){0xff, 0xff, 0xff}, false);
	/* Reserved bits 30:24 do not count. */
	check_switch(&s, 0x0ffffff1, (const uint8_t[]){0x00, 0x00, 0x01}, false);
	check_switch(&s, 0x40fffff1, (const uint8_t[]){0x00, 0x00, 0x01}, false);
	check_switch(&s, 0x80fffff1, (const uint8_t[]){0x00, 0x00, 0x01}, true);
	check_switch(&s, 0x00ffffff, (const uint8_t[]){0x00, 0x00, 0x01}, true);
	teardown(&s);
}

/* ACMD6 with the reserved widths, 1 and 3: no change. */
static void test_bus_width_args(void)
{
	Setup s;
	uint16_t rca;

	setup(&s, 1 * MIB);
	CHECK_EQ(sd_card_bus_width(s.card), 1); /* before power too */
	rca = identify(&s);
	acmd(&s, rca, 6, 1);
	CHECK_EQ(sd_card_bus_width(s.card), 1);
	acmd(&s, rca, 6, 3);
	CHECK_EQ(sd_card_bus_width(s.card), 1);
	acmd(&s, rca, 6, 2);
	acmd(&s, rca, 6, 3);
	CHECK_EQ(sd_card_bus_width(s.card), 4);
	teardown(&s);

	{
		Machine *m = machine_create();
		Storage st = {.dev = {.ops = &storage_ops, .size = 1 * MIB}};
		char error[64];
		SdCard *card = sd_card_create(m, &st.dev, error, sizeof(error));

		CHECK(card != NULL);
		CHECK_EQ(sd_card_bus_width(card), 1); /* no power yet */
		CHECK(!sd_card_high_speed(card));
		sd_card_destroy(card);
		machine_destroy(m);
	}
}

/* ACMD41's argument: a query does not start the card. */
static void test_op_cond_args(void)
{
	Setup s;
	SdResponse r;

	setup(&s, 1 * MIB);
	cmd(&s, 8, 0x1aa);
	r1(&s, 55, 0);
	r = cmd(&s, 41, 0x00000001); /* reserved bit 0: still a query */
	CHECK_EQ(r.type, SD_R3);
	CHECK_EQ(r.words[0], 0x00ff8000);
	advance(&s, 2 * MS);
	r1(&s, 55, 0);
	r = cmd(&s, 41, 0);
	CHECK_EQ(r.words[0], 0x00ff8000); /* the query started nothing */
	r1(&s, 55, 0);
	cmd(&s, 41, 0x00ff8000);
	advance(&s, MS);
	r1(&s, 55, 0);
	CHECK_EQ(cmd(&s, 41, 0x00ff8000).words[0], 0x80ff8000);
	teardown(&s);
}

/* What CMD0 and a power cycle set back. */
static void test_reset_state(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512];

	setup(&s, 1 * MIB);
	rca = identify(&s);
	/* An error, a count, an erase range, then CMD0. */
	cmd(&s, 2, 0);
	r1(&s, 23, 1);
	r1(&s, 32, 0);
	r1(&s, 33, 512);
	cmd(&s, 0, 0);
	/* RCA 0 again: CMD55 with 0 is for this card; no error left. */
	CHECK_EQ(r1(&s, 55, 0), STATE(IDLE) | READY_FOR_DATA | APP_CMD);
	CHECK_EQ(cmd(&s, 41, 0).type, SD_R3);
	rca = identify(&s);
	CHECK_EQ(r1(&s, 38, 0), ERASE_SEQ_ERROR | STATE(TRAN) | READY_FOR_DATA);
	r1(&s, 18, 0); /* open: no count left */
	for (unsigned i = 0; i < 3; i++)
		CHECK_EQ(sd_card_read(s.card, buf), 512);
	r1(&s, 12, 0);
	/* CMD0 right after CMD55: the next command is not an ACMD. */
	r1(&s, 55, (uint32_t)rca << 16);
	cmd(&s, 0, 0);
	CHECK_EQ(cmd(&s, 41, 0).type, SD_NO_RESPONSE);
	CHECK_EQ(r1(&s, 55, 0),
	         ILLEGAL_COMMAND | STATE(IDLE) | READY_FOR_DATA | APP_CMD);
	/* A power cycle after CMD55: no ACMD either. */
	sd_card_power(s.card, false);
	sd_card_power(s.card, true);
	CHECK_EQ(cmd(&s, 41, 0).type, SD_NO_RESPONSE);
	teardown(&s);
}

/*
 * CMD18 to the end, and partial blocks that end up across 512 bytes: the
 * card aborts, reports the error once, and waits for CMD12 (4.3.3).
 */
static void test_read_abort(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512];

	setup(&s, 1 * MIB);
	rca = identify(&s);
	r1(&s, 18, 1 * MIB - 512);
	CHECK_EQ(sd_card_read(s.card, buf), 512);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16),
	         OUT_OF_RANGE | STATE(DATA) | READY_FOR_DATA);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(r1(&s, 12, 0), STATE(DATA) | READY_FOR_DATA);

	/* Blocks of 100 bytes: five fit in the first 512, the sixth does not. */
	r1(&s, 16, 100);
	r1(&s, 18, 0);
	for (unsigned i = 0; i < 5; i++) {
		CHECK_EQ(sd_card_read(s.card, buf), 100);
		CHECK(same_as_storage(&s, buf, 100 * i, 100));
	}
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16),
	         ADDRESS_ERROR | STATE(DATA) | READY_FOR_DATA);
	CHECK_EQ(sd_card_read(s.card, buf), 0);
	CHECK_EQ(r1(&s, 12, 0), STATE(DATA) | READY_FOR_DATA);
	CHECK_EQ(r1(&s, 13, (uint32_t)rca << 16), STATE(TRAN) | READY_FOR_DATA);
	/* Blocks of 128 bytes stay inside: no error. */
	r1(&s, 16, 128);
	r1(&s, 18, 0);
	for (unsigned i = 0; i < 9; i++)
		CHECK_EQ(sd_card_read(s.card, buf), 128);
	CHECK_EQ(r1(&s, 12, 0), STATE(DATA) | READY_FOR_DATA);
	teardown(&s);
}

/* ACMD22 counts in 32 bits: 0x01010101 blocks, on a card of 16 GB. */
static void test_written_count(void)
{
	Setup s;
	uint16_t rca;
	uint8_t buf[512] = {0};

	setup(&s, 16 * GIB);
	s.storage.discard = true;
	rca = identify(&s);
	r1(&s, 25, 0);
	for (uint32_t i = 0; i < 0x01010101; i++)
		if (!sd_card_write(s.card, buf, 512)) {
			CHECK(false);
			break;
		}
	r1(&s, 12, 0);
	CHECK_EQ(acmd(&s, rca, 22, 0), STATE(TRAN) | READY_FOR_DATA | APP_CMD);
	CHECK_EQ(sd_card_read(s.card, buf), 4);
	CHECK(memcmp(buf, "\1\1\1\1", 4) == 0);
	teardown(&s);
}

/* --- The state transition table (Table 4-35) ---------------------------- */

#define INA 15
#define ILLEGAL (-1)
#define STOPS (-2) /* not emulated: the machine stops */

/*
 * What the card does with a command in a state, for the classes it has
 * (0, 2, 4, 5, 7, 8 and 10) and without UHS-I: the next state, or
 * ILLEGAL; *type gets the response.
 */
static int expected_next(bool app, unsigned index, int state,
                         SdResponseType *type)
{
	*type = SD_NO_RESPONSE;
	if (app) {
		switch (index) {
		case 6:
		case 23:
		case 42:
			*type = SD_R1;
			return state == TRAN ? TRAN : ILLEGAL;
		case 13:
		case 22:
		case 51:
			*type = SD_R1;
			return state == TRAN ? DATA : ILLEGAL;
		case 41:
			*type = SD_R3;
			return state == IDLE ? IDLE : ILLEGAL; /* still busy */
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
			return STOPS;
		default:
			break; /* a regular command */
		}
	}
	switch (index) {
	case 0:
		return IDLE;
	case 2:
		*type = SD_R2;
		return state == READY ? IDENT : ILLEGAL;
	case 3:
		*type = SD_R6;
		return state == IDENT || state == STBY ? STBY : ILLEGAL;
	case 4:
		return state == STBY ? STBY : ILLEGAL;
	case 6:
		*type = SD_R1;
		return state == TRAN ? DATA : ILLEGAL;
	case 7: /* addressed */
		*type = SD_R1B;
		return state == STBY ? TRAN : ILLEGAL;
	case 8:
		*type = SD_R7;
		return state == IDLE ? IDLE : ILLEGAL;
	case 9:
	case 10:
		*type = SD_R2;
		return state == STBY ? STBY : ILLEGAL;
	case 12:
		*type = SD_R1B;
		return state == DATA || state == RCV ? TRAN : ILLEGAL;
	case 13:
		*type = SD_R1;
		return state >= STBY && state <= RCV ? state : ILLEGAL;
	case 15:
		return state >= STBY && state <= RCV ? INA : ILLEGAL;
	case 16:
	case 23:
	case 32:
	case 33:
		*type = SD_R1;
		return state == TRAN ? TRAN : ILLEGAL;
	case 38:
		*type = SD_R1B;
		return state == TRAN ? TRAN : ILLEGAL;
	case 17:
	case 18:
		*type = SD_R1;
		return state == TRAN ? DATA : ILLEGAL;
	case 24:
	case 25:
		*type = SD_R1;
		return state == TRAN ? RCV : ILLEGAL;
	case 27:
	case 42:
	case 56:
		return state == TRAN ? STOPS : ILLEGAL;
	case 55:
		*type = SD_R1;
		return state == IDLE || (state >= STBY && state <= RCV) ? state
		                                                        : ILLEGAL;
	default:
		return ILLEGAL; /* reserved, or of a class the card lacks */
	}
}

/* A card, powered again, brought to state. */
static void enter(Setup *s, int state)
{
	sd_card_power(s->card, false);
	sd_card_power(s->card, true);
	if (state == IDLE)
		return;
	cmd(s, 8, 0x1aa);
	r1(s, 55, 0);
	cmd(s, 41, 0x00ff8000);
	advance(s, MS);
	r1(s, 55, 0);
	CHECK(cmd(s, 41, 0x00ff8000).words[0] & (1u << 31));
	if (state == READY)
		return;
	cmd(s, 2, 0);
	if (state == IDENT)
		return;
	CHECK_EQ(cmd(s, 3, 0).words[0] >> 16, 0xaaaa);
	if (state == STBY)
		return;
	if (state == INA) {
		cmd(s, 15, 0xaaaa0000);
		return;
	}
	cmd(s, 7, 0xaaaa0000);
	if (state == DATA)
		r1(s, 17, 0);
	if (state == RCV)
		r1(s, 24, 0);
}

/*
 * Check that the card is in state, and whether the last command was
 * illegal, with a command that the state allows.
 */
static void check_state(Setup *s, int state, bool illegal)
{
	uint32_t flag = illegal ? ILLEGAL_COMMAND : 0;
	SdResponse r;

	switch (state) {
	case IDLE:
		CHECK_EQ(r1(s, 55, 0), flag | STATE(IDLE) | READY_FOR_DATA | APP_CMD);
		break;
	case READY:
		CHECK_EQ(cmd(s, 2, 0).type, SD_R2);
		r = cmd(s, 3, 0);
		CHECK_EQ(r.words[0], 0xaaaa0000 | (illegal ? 0x4000 : 0) |
		                         STATE(IDENT) | READY_FOR_DATA);
		break;
	case IDENT:
		r = cmd(s, 3, 0);
		CHECK_EQ(r.type, SD_R6);
		CHECK_EQ(r.words[0], 0xaaaa0000 | (illegal ? 0x4000 : 0) |
		                         STATE(IDENT) | READY_FOR_DATA);
		break;
	case INA:
		CHECK_EQ(cmd(s, 8, 0x1aa).type, SD_NO_RESPONSE);
		CHECK_EQ(cmd(s, 0, 0).type, SD_NO_RESPONSE);
		CHECK_EQ(cmd(s, 8, 0x1aa).type, SD_NO_RESPONSE);
		break;
	default:
		CHECK_EQ(r1(s, 13, 0xaaaa0000), flag | STATE(state) | READY_FOR_DATA);
		break;
	}
}

/* The argument of index in a test of the table: a valid one. */
static uint32_t table_arg(bool app, unsigned index, int state)
{
	uint32_t rca =
		state == IDLE || state == READY || state == IDENT ? 0 : 0xaaaa0000;

	if (app && index == 41)
		return 0x00ff8000;
	switch (index) {
	case 6:
		return app ? 2 : 0x00fffff1;
	case 8:
		return 0x1aa;
	case 16:
		return 512;
	case 17:
	case 18:
	case 24:
	case 25:
	case 32:
	case 33:
	case 38:
		return 0;
	case 23:
		return 1;
	default:
		return rca;
	}
}

/* Every command in every state that a card can stay in. */
static void test_state_table(void)
{
	static const int states[] = {IDLE, READY, IDENT, STBY,
	                             TRAN, DATA,  RCV,   INA};
	Setup s;

	setup(&s, 1 * MIB);
	for (int app = 0; app <= 1; app++) {
		for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
			int state = states[i];
			SdResponseType unused;

			/* An ACMD needs CMD55 first, where CMD55 is allowed. */
			if (app && expected_next(false, 55, state, &unused) == ILLEGAL)
				continue;
			for (unsigned index = 0; index < 64; index++) {
				SdResponseType type;
				int next = expected_next(app, index, state, &type);
				uint32_t arg = table_arg(app, index, state);
				SdResponse r;

				if (next == STOPS)
					continue; /* test_stops */
				enter(&s, state);
				if (app)
					r1(&s, 55, state == IDLE ? 0 : 0xaaaa0000);
				r = cmd(&s, index, arg);
				if (state == INA) {
					next = INA;
					type = SD_NO_RESPONSE;
				}
				if (r.type != (next == ILLEGAL ? SD_NO_RESPONSE : type) ||
				    s.m->stop != MACHINE_STOP_NONE)
					fprintf(stderr, "%sCMD%u in state %d: response %d\n",
					        app ? "A" : "", index, state, r.type);
				CHECK_EQ(r.type, next == ILLEGAL ? SD_NO_RESPONSE : type);
				CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
				s.m->stop = MACHINE_STOP_NONE;
				if (r.type == SD_R1 || r.type == SD_R1B) {
					/* APP_CMD: CMD55, and the ACMDs that the card defines. */
					bool app_cmd =
						index == 55 ||
						(app && (index == 6 || index == 13 || index == 22 ||
					             index == 23 || index == 42 || index == 51));
					/* CMD33 and CMD38 without CMD32 first. */
					uint32_t error =
						index == 33 || index == 38 ? ERASE_SEQ_ERROR : 0;

					CHECK_EQ(r.words[0], error | STATE(state) | READY_FOR_DATA |
					                         (app_cmd ? APP_CMD : 0));
				}
				if (index == 55 && next != ILLEGAL)
					continue; /* the next command would be an ACMD */
				check_state(&s, next == ILLEGAL ? state : next,
				            next == ILLEGAL && state != INA);
			}
		}
	}

	/* CMD7 to another card: who leaves, who stays, who may not. */
	for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
		int state = states[i], next;
		SdResponse r;

		if (state == INA)
			continue;
		enter(&s, state);
		r = cmd(&s, 7, 0x12340000);
		CHECK_EQ(r.type, SD_NO_RESPONSE);
		next = state == STBY || state == TRAN || state == DATA ? STBY : ILLEGAL;
		check_state(&s, next == ILLEGAL ? state : next, next == ILLEGAL);
	}
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_crc7);
	RUN_TEST(test_create);
	RUN_TEST(test_power);
	RUN_TEST(test_identification);
	RUN_TEST(test_high_capacity);
	RUN_TEST(test_csd);
	RUN_TEST(test_commands);
	RUN_TEST(test_select);
	RUN_TEST(test_read);
	RUN_TEST(test_write);
	RUN_TEST(test_erase);
	RUN_TEST(test_errors_and_aborts);
	RUN_TEST(test_switch);
	RUN_TEST(test_app_commands);
	RUN_TEST(test_stops);
	RUN_TEST(test_registers);
	RUN_TEST(test_switch_args);
	RUN_TEST(test_bus_width_args);
	RUN_TEST(test_op_cond_args);
	RUN_TEST(test_reset_state);
	RUN_TEST(test_read_abort);
	RUN_TEST(test_written_count);
	RUN_TEST(test_state_table);
	return test_summary();
}
