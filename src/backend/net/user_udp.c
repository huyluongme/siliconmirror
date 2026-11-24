/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * UDP in the user network (RFC 768): DHCP to the router's server, and
 * everything else out through the host's sockets, as a NAT would
 * (RFC 4787): each mapping is a socket of the host connected to the
 * destination, so the host picks the outside port, keeps it for the
 * mapping's life ("endpoint independent" in all but the filter), and
 * lets only the destination's answers in.
 */

#define _DEFAULT_SOURCE /* getaddrinfo's AI_NUMERICSERV, struct addrinfo */

#include "backend/net/user_int.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DHCP_SERVER_PORT 67

/*
 * How long a mapping lives without a datagram, in virtual time: RFC 4787
 * REQ-5 asks for at least 2 minutes and recommends 5. Datagrams both ways
 * keep it (REQ-6 asks for the outbound ones, lets the inbound).
 */
#define FLOW_IDLE_NS (300ull * 1000000000)

/* The most data a datagram to the guest has: IPv4's total length. */
#define MAX_DATA (65535 - IP_HEADER - UDP_HEADER)

/*
 * The host's name server: the first "nameserver" line of resolv.conf
 * that names an address, with the keyword at the start of the line; with
 * none, the host itself (resolv.conf(5)).
 */
static void read_resolver(UserNet *u, const NetUserConfig *config)
{
	char line[256], port[8];
	struct addrinfo *found, hints = {
								.ai_family = AF_UNSPEC,
								.ai_socktype = SOCK_DGRAM,
								.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
							};
	FILE *f = fopen(config->resolv_conf, "r");
	const char *address = NULL;

	snprintf(port, sizeof(port), "%u", (unsigned)config->dns_port);
	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		char name[sizeof(line)];

		if (sscanf(line, "nameserver %255s", name) != 1 ||
		    (line[10] != ' ' && line[10] != '\t'))
			continue;
		if (getaddrinfo(name, port, &hints, &found) == 0) {
			memcpy(&u->dns, found->ai_addr, found->ai_addrlen);
			u->dns_len = found->ai_addrlen;
			freeaddrinfo(found);
			address = name;
			break;
		}
	}
	if (f != NULL)
		fclose(f);
	if (address == NULL) {
		struct sockaddr_in *in = (struct sockaddr_in *)&u->dns;

		/* u->dns is still zero, as calloc() made it. */
		in->sin_family = AF_INET;
		in->sin_port = htons(config->dns_port);
		in->sin_addr.s_addr = htonl(USER_LOOPBACK);
		u->dns_len = sizeof(*in);
	}
}

void user_udp_init(UserNet *u, const NetUserConfig *config)
{
	for (unsigned i = 0; i < USER_UDP_FLOWS; i++)
		u->udp[i].fd = -1;
	read_resolver(u, config);
}

static void close_flow(UserFlow *f)
{
	close(f->fd);
	f->fd = -1;
}

void user_udp_close(UserNet *u)
{
	for (unsigned i = 0; i < USER_UDP_FLOWS; i++)
		if (u->udp[i].fd >= 0)
			close_flow(&u->udp[i]);
}

/*
 * The mapping from the guest's address and port to a destination, made
 * if there is none, connected to the host's address for it, to. NULL
 * when the host has no socket for it: the datagram is lost.
 */
static UserFlow *find_flow(UserNet *u, uint32_t guest_ip, uint16_t guest_port,
                           uint32_t ip, uint16_t port,
                           const struct sockaddr *to, socklen_t to_len)
{
	UserFlow *f = NULL;

	for (unsigned i = 0; i < USER_UDP_FLOWS; i++) {
		UserFlow *g = &u->udp[i];

		if (g->fd >= 0 && g->guest_ip == guest_ip &&
		    g->guest_port == guest_port && g->ip == ip && g->port == port)
			return g;
		/* Else an unused one, or the one idle longest. */
		if (f == NULL || (f->fd >= 0 && (g->fd < 0 || g->last < f->last)))
			f = g;
	}
	if (f->fd >= 0)
		close_flow(f);
	f->fd = socket(to->sa_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (f->fd < 0)
		return NULL;
	if (connect(f->fd, to, to_len) < 0) {
		close_flow(f);
		return NULL;
	}
	f->guest_ip = guest_ip;
	f->guest_port = guest_port;
	f->ip = ip;
	f->port = port;
	return f;
}

/*
 * A datagram out through the NAT: to 10.0.2.3's port 53, the host's name
 * server; to 10.0.2.2, the host's loopback; else to dst itself.
 */
static void nat_output(UserNet *u, uint32_t src, uint16_t sport, uint32_t dst,
                       uint16_t dport, const uint8_t *data, size_t len)
{
	struct sockaddr_in in = {
		.sin_family = AF_INET,
		.sin_port = htons(dport),
		.sin_addr.s_addr = htonl(dst == USER_GATEWAY ? USER_LOOPBACK : dst),
	};
	const struct sockaddr *to = (const struct sockaddr *)&in;
	socklen_t to_len = sizeof(in);
	UserFlow *f;

	if (dst == USER_DNS) {
		if (dport != USER_DNS_PORT)
			return;
		to = (const struct sockaddr *)&u->dns;
		to_len = u->dns_len;
	}
	f = find_flow(u, src, sport, dst, dport, to, to_len);
	if (f == NULL)
		return;
	f->last = u->now;
	/*
	 * What the host cannot send is lost, as at a router: a full queue
	 * (EAGAIN, ENOBUFS), no route, or an ICMP error from an earlier
	 * datagram (ECONNREFUSED).
	 */
	(void)!send(f->fd, data, len, 0);
}

void user_udp_input(UserNet *u, const uint8_t eth_src[6], uint32_t src,
                    uint32_t dst, const uint8_t *data, size_t len)
{
	uint16_t dport, length;
	bool broadcast = dst == USER_BROADCAST || dst == (USER_NET | ~USER_MASK);

	if (len < UDP_HEADER)
		return;
	dport = user_get16(data + 2);
	length = user_get16(data + 4);
	/* RFC 768: a checksum of 0 is none. */
	if (length < UDP_HEADER || length > len ||
	    (user_get16(data + 6) != 0 &&
	     user_checksum(user_pseudo_sum(src, dst, IP_UDP, length), data,
	                   length) != 0))
		return;
	/* The router's DHCP server, and nothing else, hears broadcasts. */
	if ((broadcast || dst == USER_GATEWAY) && dport == DHCP_SERVER_PORT)
		user_dhcp_input(u, eth_src, data + UDP_HEADER, length - UDP_HEADER);
	else if (!broadcast)
		nat_output(u, src, user_get16(data), dst, dport, data + UDP_HEADER,
		           length - UDP_HEADER);
}

void user_udp_output(UserNet *u, const uint8_t eth_dst[6], uint32_t src,
                     uint16_t sport, uint32_t dst, uint16_t dport, uint8_t *udp,
                     size_t len)
{
	uint16_t sum;

	user_put16(udp, sport);
	user_put16(udp + 2, dport);
	user_put16(udp + 4, (uint16_t)(UDP_HEADER + len));
	user_put16(udp + 6, 0);
	sum = user_checksum(user_pseudo_sum(src, dst, IP_UDP, UDP_HEADER + len),
	                    udp, UDP_HEADER + len);
	/* RFC 768: a sum of 0 is sent as all ones. */
	user_put16(udp + 6, sum == 0 ? 0xffff : sum);
	user_send_ip(u, eth_dst, IP_UDP, src, dst, udp, UDP_HEADER + len);
}

/*
 * The datagrams a mapping's socket has, while the queue has room for the
 * largest; what does not fit waits on the host.
 */
static void flow_receive(UserNet *u, UserFlow *f)
{
	while (user_queue_free(u) >= USER_MAX_FRAGMENTS) {
		ssize_t got =
			recv(f->fd, u->datagram + UDP_HEADER, MAX_DATA, MSG_TRUNC);

		if (got < 0) {
			/* An ICMP error for an earlier datagram: the next one. */
			if (errno == ECONNREFUSED)
				continue;
			return;
		}
		if (got > MAX_DATA)
			continue; /* too large for IPv4: lost */
		f->last = u->now;
		user_udp_output(u, u->guest_mac, f->ip, f->port, f->guest_ip,
		                f->guest_port, u->datagram, (size_t)got);
	}
}

void user_udp_poll(UserNet *u)
{
	struct pollfd fds[USER_UDP_FLOWS];
	UserFlow *flows[USER_UDP_FLOWS];
	nfds_t n = 0;

	for (unsigned i = 0; i < USER_UDP_FLOWS; i++) {
		UserFlow *f = &u->udp[i];

		if (f->fd < 0)
			continue;
		if (u->now - f->last > FLOW_IDLE_NS) {
			close_flow(f);
		} else {
			fds[n] = (struct pollfd){.fd = f->fd, .events = POLLIN};
			flows[n++] = f;
		}
	}
	if (n == 0 || poll(fds, n, 0) <= 0)
		return;
	for (nfds_t i = 0; i < n; i++)
		if (fds[i].revents != 0)
			flow_receive(u, flows[i]);
}
