/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A user-mode network (-net user): the board's cable goes into a small
 * router inside smirror, which reaches the outside through the host's
 * own sockets, so that no root is needed. The idea and the addresses are
 * those of QEMU's user networking (-netdev user, from its documentation,
 * general knowledge; none of its code was read): the network 10.0.2.0/24,
 *
 *     10.0.2.2   the router: ARP, ping, the DHCP server; UDP and TCP
 *                to it go to the host's own loopback, 127.0.0.1
 *     10.0.2.3   the name server: UDP and TCP to its port 53 go to the
 *                host's first name server in /etc/resolv.conf
 *     10.0.2.15  the address the DHCP server gives the board
 *
 * UDP and TCP to any other address leave through sockets of the host, as
 * from a router that does NAT (RFC 4787, RFC 5382); the router ends the
 * guest's TCP itself and passes the bytes on (user_tcp.c). For what the
 * router does not route (IPv6, IP fragments from the guest, other
 * protocols), frames are dropped, as by a router that filters them.
 *
 * Options, after "user," and between commas (QEMU's syntax, from its
 * documentation): hostfwd=[tcp]:[hostaddr]:hostport-[guestaddr]:guestport,
 * as many as NET_USER_FORWARDS, passes TCP connections to the host's
 * hostaddr:hostport on to the guest's guestaddr:guestport, from 10.0.2.2.
 * guestaddr is 10.0.2.15 when left out; hostaddr 127.0.0.1, so that only
 * the host itself reaches the guest (QEMU's documentation does not say;
 * our choice, as a Raspberry Pi OS card has a known password).
 */

#ifndef BACKEND_NET_USER_H
#define BACKEND_NET_USER_H

#include <stddef.h>
#include <stdint.h>

#include "core/net.h"

/* A forward: TCP to the host's address and port goes to the guest's. */
typedef struct NetUserForward {
	uint32_t host_ip; /* as numbers in host order */
	uint16_t host_port;
	uint32_t guest_ip;
	uint16_t guest_port;
} NetUserForward;

#define NET_USER_FORWARDS 16

/* The router's settings; tests change them. */
typedef struct NetUserConfig {
	const char *resolv_conf; /* where the host's name server is */
	uint16_t dns_port;       /* the port it listens on */
	unsigned forwards;
	NetUserForward forward[NET_USER_FORWARDS];
} NetUserConfig;

/*
 * The user network, with options, the text after "user," in -net ("" for
 * none), or NULL with a message in error.
 */
NetLink *net_user_open(const char *options, char *error, size_t error_size);

/* The same with settings other than /etc/resolv.conf, port 53, no forward. */
NetLink *net_user_open_config(const NetUserConfig *config, char *error,
                              size_t error_size);

#endif /* BACKEND_NET_USER_H */
