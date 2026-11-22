/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A pcap file of the guest's frames (draft-ietf-opsawg-pcap-04: the
 * file header in section 4, the packet records in section 5).
 */

#define _POSIX_C_SOURCE 200809L /* O_CLOEXEC */

#include "backend/net/pcap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/byteorder.h"

#define MAGIC_NS 0xa1b23c4du /* seconds and nanoseconds */
#define VERSION_MAJOR 2
#define VERSION_MINOR 4
#define LINKTYPE_ETHERNET 1 /* no FCS: the P bit is clear */
#define FILE_HEADER 24
#define RECORD_HEADER 16
#define NS_PER_S 1000000000u

typedef struct PcapLink {
	NetLink link; /* first, so that a NetLink * is a PcapLink * */
	int fd;
} PcapLink;

static bool write_all(PcapLink *p, const uint8_t *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(p->fd, buf, len);

		if (n <= 0) {
			snprintf(p->link.error, sizeof(p->link.error),
			         "cannot write the pcap file: %s", strerror(errno));
			return false;
		}
		buf += n;
		len -= (size_t)n;
	}
	return true;
}

static bool pcap_send(NetLink *n, uint64_t now, const uint8_t *frame,
                      size_t len)
{
	PcapLink *p = (PcapLink *)n;
	uint8_t record[RECORD_HEADER];

	store_le32(record, (uint32_t)(now / NS_PER_S));
	store_le32(record + 4, (uint32_t)(now % NS_PER_S));
	store_le32(record + 8, (uint32_t)len);  /* captured */
	store_le32(record + 12, (uint32_t)len); /* on the wire */
	return write_all(p, record, sizeof(record)) && write_all(p, frame, len);
}

static bool pcap_receive(NetLink *n, uint8_t *frame, size_t *len)
{
	(void)n;
	(void)frame;
	*len = 0;
	return true;
}

static void pcap_destroy(NetLink *n)
{
	PcapLink *p = (PcapLink *)n;

	close(p->fd);
	free(p);
}

static const NetLinkOps pcap_ops = {
	.send = pcap_send,
	.receive = pcap_receive,
	.destroy = pcap_destroy,
};

NetLink *net_pcap_open(const char *path, char *error, size_t error_size)
{
	PcapLink *p = calloc(1, sizeof(*p));
	uint8_t header[FILE_HEADER] = {0}; /* the two reserved fields 0 */

	if (p == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	p->link.ops = &pcap_ops;
	p->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (p->fd < 0) {
		snprintf(error, error_size, "cannot create %s: %s", path,
		         strerror(errno));
		free(p);
		return NULL;
	}
	store_le32(header, MAGIC_NS);
	store_le16(header + 4, VERSION_MAJOR);
	store_le16(header + 6, VERSION_MINOR);
	store_le32(header + 16, NET_MAX_FRAME); /* SnapLen: every frame whole */
	store_le32(header + 20, LINKTYPE_ETHERNET);
	if (!write_all(p, header, sizeof(header))) {
		snprintf(error, error_size, "%s", p->link.error);
		pcap_destroy(&p->link);
		return NULL;
	}
	return &p->link;
}
