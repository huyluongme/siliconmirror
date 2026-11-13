/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * An SD memory card, in SD mode, as a host controller sees it through
 * its commands, responses and data blocks. The controller times the bus;
 * the card answers at once.
 *
 * Written from the SD Physical Layer Simplified Specification, version
 * 9.10: card identification (4.2), data transfer (4.3), the switch
 * function (4.3.10), the commands (4.7.4) and the state transitions
 * (4.8), the responses (4.9), the card status and the SD status (4.10),
 * and the registers (chapter 5).
 *
 * The card is of Physical Layer version 3.01, with the capacity of its
 * image: SDSC up to 2 GB, SDHC up to 32 GB, SDXC above, as real cards
 * are made. Its image must be a multiple of 512 KB, the unit of C_SIZE.
 * It supports the 1-bit and 4-bit buses, default and high speed, block
 * reads and writes, CMD23 and erase. It runs at 3.3 V only (no UHS-I),
 * has no write protection and no DSR. Commands that the specification
 * makes mandatory but smirror does not emulate stop the machine: card
 * locking (CMD42), PROGRAM_CSD (CMD27), GEN_CMD (CMD56) and the security
 * commands.
 */

#ifndef HW_SD_SD_CARD_H
#define HW_SD_SD_CARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/block.h"
#include "core/machine.h"

#define SD_BLOCK_SIZE 512

typedef enum SdResponseType {
	SD_NO_RESPONSE, /* also for a command the card ignores */
	SD_R1,
	SD_R1B, /* R1, then busy on DAT0 */
	SD_R2,  /* 136 bits: CID or CSD */
	SD_R3,  /* OCR */
	SD_R6,  /* published RCA */
	SD_R7,  /* interface condition */
} SdResponseType;

typedef struct SdResponse {
	SdResponseType type;

	/*
	 * R2: bits 127:0 of the response, words[0] the highest, which are
	 * bits 127:1 of the register and the end bit. Others: words[0] is
	 * bits 39:8 of the response, the argument field.
	 */
	uint32_t words[4];
} SdResponse;

typedef struct SdCard SdCard;

/*
 * A card with the contents of storage, whose size must be a multiple of
 * 512 KB up to 2 TB. It takes the time from m, and stops m on what it
 * does not emulate. The card starts without power. On failure, writes a
 * message to error and returns NULL.
 */
SdCard *sd_card_create(Machine *m, BlockDevice *storage, char *error,
                       size_t error_size);
void sd_card_destroy(SdCard *card);

/* Power the card on or off. Powered on, it is in the idle state. */
void sd_card_power(SdCard *card, bool on);

/* The command index, with arg; the response goes to *response. */
void sd_card_command(SdCard *card, unsigned index, uint32_t arg,
                     SdResponse *response);

/*
 * The card's next data block for the host, at most SD_BLOCK_SIZE bytes,
 * into buf. Returns its length, or 0 when the card sends no data.
 */
size_t sd_card_read(SdCard *card, uint8_t *buf);

/*
 * A data block from the host, of len bytes. Returns whether the card
 * took it (a positive CRC status); a card that does not expect data, or
 * finds an error, takes no more.
 */
bool sd_card_write(SdCard *card, const uint8_t *buf, size_t len);

/* Whether the card expects a data block from the host. */
bool sd_card_receiving(const SdCard *card);

/* 1 or 4, as ACMD6 set it. */
unsigned sd_card_bus_width(const SdCard *card);

/* Whether CMD6 switched the card to high speed. */
bool sd_card_high_speed(const SdCard *card);

/* The CRC7 of len bytes (4.5): x^7 + x^3 + 1. */
uint8_t sd_crc7(const uint8_t *data, size_t len);

#endif /* HW_SD_SD_CARD_H */
