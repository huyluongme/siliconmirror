/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A network link that records the guest's frames in a pcap file
 * (-net pcap:PATH), for Wireshark or tcpdump, as if the cable were
 * plugged into a port that only listens: no frame comes back.
 *
 * The file follows draft-ietf-opsawg-pcap-04: timestamps in nanoseconds
 * (magic 0xA1B23C4D), LINKTYPE_ETHERNET without the FCS, little-endian.
 * A frame's time is the virtual time it was sent, from 1970-01-01 at
 * power on, so that a run gives the same file each time. Each frame is
 * written as it is sent, so the file is whole however smirror ends.
 */

#ifndef BACKEND_NET_PCAP_H
#define BACKEND_NET_PCAP_H

#include <stddef.h>

#include "core/net.h"

/*
 * The link, or NULL with a message in error when PATH cannot be
 * created.
 */
NetLink *net_pcap_open(const char *path, char *error, size_t error_size);

#endif /* BACKEND_NET_PCAP_H */
