/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the pcap link (src/backend/net/pcap.c), against
 * draft-ietf-opsawg-pcap-04: the file header (section 4) and the packet
 * records (section 5).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "backend/net/pcap.h"
#include "test.h"
#include "util/byteorder.h"

#define PCAP "test_net_pcap.pcap"

static uint8_t file[4096];

static long read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(file, 1, sizeof(file), f);
	fclose(f);
	return (long)n;
}

static NetLink *open_pcap(void)
{
	char error[200];
	NetLink *n = net_pcap_open(PCAP, error, sizeof(error));

	CHECK(n != NULL);
	return n;
}

/* The header alone, once opened: nanoseconds, Ethernet without FCS. */
static void test_header(void)
{
	static const uint8_t header[24] = {
		0x4d, 0x3c, 0xb2, 0xa1, 2,    0, 4, 0, 0, 0, 0, 0,
		0,    0,    0,    0,    0x0d, 0, 1, 0, 1, 0, 0, 0,
	};
	NetLink *n = open_pcap();

	CHECK_EQ(read_file(PCAP), 24);
	CHECK(memcmp(file, header, 24) == 0);
	CHECK_EQ(load_le32(file + 16), NET_MAX_FRAME);
	net_destroy(n);
	CHECK_EQ(read_file(PCAP), 24);
	unlink(PCAP);
}

/* Each frame, as sent, with its virtual time; nothing comes back. */
static void test_records(void)
{
	NetLink *n = open_pcap();
	uint8_t frame[100], got[16];
	size_t len = 7;

	for (int i = 0; i < 100; i++)
		frame[i] = (uint8_t)(200 - i);
	CHECK(n->ops->send(n, 1500000000u, frame, 60));
	CHECK_EQ(read_file(PCAP), 24 + 16 + 60); /* written at once */
	CHECK(n->ops->send(n, 4294967296ull * 1000000000u + 7, frame, 100));
	CHECK(n->ops->receive(n, 0, got, &len));
	CHECK_EQ(len, 0);
	net_destroy(n);

	CHECK_EQ(read_file(PCAP), 24 + 16 + 60 + 16 + 100);
	CHECK_EQ(load_le32(file + 24), 1);
	CHECK_EQ(load_le32(file + 28), 500000000u);
	CHECK_EQ(load_le32(file + 32), 60);
	CHECK_EQ(load_le32(file + 36), 60);
	CHECK(memcmp(file + 40, frame, 60) == 0);
	/* Seconds keep their 32 bits. */
	CHECK_EQ(load_le32(file + 100), 0);
	CHECK_EQ(load_le32(file + 104), 7);
	CHECK_EQ(load_le32(file + 108), 100);
	CHECK_EQ(load_le32(file + 112), 100);
	CHECK(memcmp(file + 116, frame, 100) == 0);

	/* Opening again starts the file again. */
	n = open_pcap();
	net_destroy(n);
	CHECK_EQ(read_file(PCAP), 24);
	unlink(PCAP);
}

/* The host's errors: what failed, and why. */
static void test_errors(void)
{
	char error[200];
	struct rlimit old, lim;
	uint8_t frame[60] = {0};
	NetLink *n;

	CHECK(net_pcap_open("no/such/dir.pcap", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot create no/such/dir.pcap: No such file or "
	                    "directory") == 0);
	CHECK(net_pcap_open("/dev/full", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot write the pcap file: No space left on "
	                    "device") == 0);

	/*
	 * A file that cannot grow: the record fails, or the frame after 30
	 * bytes, 59 or 1.
	 */
	static const unsigned rooms[] = {0, 16 + 30, 16 + 59, 16 + 1};

	signal(SIGXFSZ, SIG_IGN);
	CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
	lim = old;
	for (unsigned i = 0; i < 4; i++) {
		n = open_pcap();
		lim.rlim_cur = 24 + rooms[i];
		CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
		errno = 0;
		CHECK(!n->ops->send(n, 0, frame, 60));
		CHECK(strcmp(n->error, "cannot write the pcap file: File too "
		                       "large") == 0);
		CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
		net_destroy(n);
	}
	unlink(PCAP);
}

/* The file may get descriptor 0; it is closed with the link. */
static void test_descriptor(void)
{
	NetLink *n;

	close(0);
	n = open_pcap();
	CHECK(fcntl(0, F_GETFD) != -1);
	net_destroy(n);
	CHECK(fcntl(0, F_GETFD) == -1 && errno == EBADF);
	unlink(PCAP);
}

int main(void)
{
	RUN_TEST(test_header);
	RUN_TEST(test_records);
	RUN_TEST(test_errors);
	RUN_TEST(test_descriptor);
	return test_summary();
}
