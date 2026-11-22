/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Frames between the host's form and the cable's (IEEE 802.3 clause 3:
 * at least 64 bytes with the FCS, the data padded; the FCS, a CRC-32 of
 * the rest, sent least significant byte first).
 */

#include "core/net.h"

#include <string.h>

#include "util/byteorder.h"
#include "util/crc32.h"

#define NET_MIN_FRAME (NET_MIN_WIRE - NET_FCS_SIZE)

size_t net_to_wire(const uint8_t *frame, size_t len, uint8_t *wire)
{
	memcpy(wire, frame, len);
	if (len < NET_MIN_FRAME) {
		memset(wire + len, 0, NET_MIN_FRAME - len);
		len = NET_MIN_FRAME;
	}
	store_le32(wire + len, crc32(wire, len));
	return len + NET_FCS_SIZE;
}

size_t net_from_wire(const uint8_t *wire, size_t len)
{
	if (len < NET_MIN_WIRE)
		return 0;
	len -= NET_FCS_SIZE;
	if (crc32(wire, len) != load_le32(wire + len))
		return 0;
	return len;
}
