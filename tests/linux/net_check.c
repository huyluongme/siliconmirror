/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Check the capture of the Linux network session (net.txt): the pcap
 * file that smirror's -net pcap: wrote of what Linux sent.
 *
 * Usage: net_check CARD CAPTURE.pcap
 *
 * Without CARD, the session did not run: exit status 77. The file must
 * have the header of draft-ietf-opsawg-pcap-04 section 4 that
 * backend/net/pcap.c writes, then records (section 5) in time order,
 * each a whole frame of at least 60 bytes from the board's address. At
 * least one must be the ARP request (RFC 826) of ping: who has 10.0.2.2,
 * from 10.0.2.15 at b8:27:eb:ff:ee:03, to the broadcast address.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/byteorder.h"

static const uint8_t board[6] = {0xb8, 0x27, 0xeb, 0xff, 0xee, 0x03};
static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t own_ip[4] = {10, 0, 2, 15};
static const uint8_t asked_ip[4] = {10, 0, 2, 2};

static int fail(const char *why)
{
	fprintf(stderr, "net_check: %s\n", why);
	return 1;
}

/* The ARP request of ping, as Linux sends it on Ethernet. */
static bool is_request(const uint8_t *f, uint32_t len)
{
	return len >= 42 && memcmp(f, broadcast, 6) == 0 &&
	       load_be16(f + 12) == 0x0806 && load_be16(f + 14) == 1 &&
	       load_be16(f + 16) == 0x0800 && f[18] == 6 && f[19] == 4 &&
	       load_be16(f + 20) == 1 && memcmp(f + 22, board, 6) == 0 &&
	       memcmp(f + 28, own_ip, 4) == 0 && memcmp(f + 38, asked_ip, 4) == 0;
}

int main(int argc, char **argv)
{
	static uint8_t frame[65549];
	uint8_t header[24], record[16];
	uint64_t last = 0;
	unsigned frames = 0, requests = 0;
	FILE *f;

	if (argc != 3) {
		fputs("usage: net_check CARD CAPTURE.pcap\n", stderr);
		return 2;
	}
	if (access(argv[1], R_OK) != 0)
		return 77;
	f = fopen(argv[2], "rb");
	if (f == NULL)
		return fail("cannot open the capture");
	if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
	    load_le32(header) != 0xa1b23c4du || load_le16(header + 4) != 2 ||
	    load_le16(header + 6) != 4 || load_le32(header + 8) != 0 ||
	    load_le32(header + 12) != 0 || load_le32(header + 16) != 65549 ||
	    load_le32(header + 20) != 1) {
		fclose(f);
		return fail("not the file header of -net pcap:");
	}
	while (fread(record, 1, sizeof(record), f) == sizeof(record)) {
		uint64_t t =
			(uint64_t)load_le32(record) * 1000000000u + load_le32(record + 4);
		uint32_t len = load_le32(record + 8);

		if (load_le32(record + 4) >= 1000000000u || t < last ||
		    len != load_le32(record + 12) || len < 60 || len > sizeof(frame) ||
		    fread(frame, 1, len, f) != len ||
		    memcmp(frame + 6, board, 6) != 0) {
			fclose(f);
			return fail("a record that is not a frame of the board's");
		}
		last = t;
		frames++;
		requests += is_request(frame, len);
	}
	fclose(f);
	if (requests == 0)
		return fail("no ARP request for 10.0.2.2");
	printf("net_check: %u frames, %u ARP requests for 10.0.2.2\n", frames,
	       requests);
	return 0;
}
