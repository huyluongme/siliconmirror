/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A user-mode network (-net user): the board's cable goes into a small
 * router inside smirror, which reaches the outside through the host's
 * own sockets, so that no root is needed. The idea and the addresses are
 * those of QEMU's user networking (-netdev user, from its documentation,
 * general knowledge; none of its code was read): the network 10.0.2.0/24,
 *
 *     10.0.2.2   the router: ARP, ping, the DHCP server; UDP to it goes
 *                to the host's own loopback, 127.0.0.1
 *     10.0.2.3   the name server: UDP to its port 53 goes to the host's
 *                first name server in /etc/resolv.conf
 *     10.0.2.15  the address the DHCP server gives the board
 *
 * UDP to any other address leaves through a socket of the host, as from
 * a router that does NAT (RFC 4787). TCP comes in a later part; until
 * then, and for what the router does not route (IPv6, IP fragments from
 * the guest, other protocols), frames are dropped, as by a router that
 * filters them.
 */

#ifndef BACKEND_NET_USER_H
#define BACKEND_NET_USER_H

#include <stddef.h>
#include <stdint.h>

#include "core/net.h"

/* The router's settings; tests change them. */
typedef struct NetUserConfig {
	const char *resolv_conf; /* where the host's name server is */
	uint16_t dns_port;       /* the port it listens on */
} NetUserConfig;

/*
 * The user network, with options, the text after "user," in -net (none
 * yet: "" is the only one taken), or NULL with a message in error.
 */
NetLink *net_user_open(const char *options, char *error, size_t error_size);

/* The same with settings other than /etc/resolv.conf and port 53. */
NetLink *net_user_open_config(const NetUserConfig *config, char *error,
                              size_t error_size);

#endif /* BACKEND_NET_USER_H */
