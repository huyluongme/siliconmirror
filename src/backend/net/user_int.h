/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The parts of the user network (user.h): user.c has the link, Ethernet,
 * ARP, IPv4 and ICMP; user_dhcp.c the DHCP server; user_udp.c UDP and
 * its NAT through the host's sockets; user_tcp.c TCP, its NAT and the
 * forwards.
 */

#ifndef BACKEND_NET_USER_INT_H
#define BACKEND_NET_USER_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "backend/net/user.h"

/* The addresses, as numbers in host order. */
#define USER_NET 0x0a000200u     /* 10.0.2.0 */
#define USER_MASK 0xffffff00u    /* 255.255.255.0 */
#define USER_GATEWAY 0x0a000202u /* 10.0.2.2 */
#define USER_DNS 0x0a000203u     /* 10.0.2.3 */
#define USER_GUEST 0x0a00020fu   /* 10.0.2.15 */
#define USER_BROADCAST 0xffffffffu
#define USER_LOOPBACK 0x7f000001u /* 127.0.0.1, the host's own */
#define USER_DNS_PORT 53

#define ETH_HEADER 14
#define IP_HEADER 20 /* without options, as the router sends them */
#define UDP_HEADER 8
#define USER_MTU 1500 /* Ethernet's (RFC 894) */

#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17

/*
 * The frames waiting for the guest. A UDP datagram of 65535 bytes is 45
 * fragments; the queue holds a few of them.
 */
#define USER_QUEUE 160
#define USER_MAX_FRAGMENTS 45

/* The UDP mappings at once; the least used goes when one more is needed. */
#define USER_UDP_FLOWS 64

/*
 * A UDP mapping (RFC 4787's "NAT mapping"): the guest's address and port
 * to one destination, through a socket of the host connected to it, so
 * that only that destination's answers come back (an "address and port
 * dependent" filter, section 5).
 */
typedef struct UserFlow {
	int fd; /* -1 when unused */
	uint32_t guest_ip;
	uint16_t guest_port;
	uint32_t ip; /* the destination, as the guest sees it */
	uint16_t port;
	uint64_t last; /* virtual time of the last datagram */
} UserFlow;

/* The TCP connections at once, and each one's buffer each way. */
#define USER_TCP_CONNS 64
#define USER_TCP_BUF 65535 /* the largest window without scaling */

/*
 * The states of RFC 9293 section 3.3.2, of the router's end of a guest's
 * connection; CONNECTING is ours: the guest's SYN came, and the host's
 * connect() to the destination goes on, before the router answers.
 */
typedef enum {
	TCP_CLOSED, /* the slot is free once the host's socket is closed too */
	TCP_CONNECTING,
	TCP_SYN_SENT, /* a forward's connection: the router's SYN is out */
	TCP_SYN_RECEIVED,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSING,
	TCP_TIME_WAIT,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
} UserTcpState;

/*
 * A TCP connection of the guest's, to the router's end, joined to a
 * socket of the host: the bytes from the guest go out on the socket,
 * what comes in on it goes to the guest. The names of the sequence
 * variables are RFC 9293's (section 3.3.1); snd_max is the highest
 * SND.NXT so far, as SND.NXT goes back on a retransmission.
 */
typedef struct UserTcp {
	UserTcpState state;
	int fd;                    /* the host's socket; -1 once closed */
	uint32_t guest_ip, ip;     /* the guest's end, and the other, as the */
	uint16_t guest_port, port; /* guest sees it */

	uint32_t iss, snd_una, snd_nxt, snd_max, snd_wl1;
	uint32_t snd_wnd;
	uint16_t mss; /* the most data in a segment to the guest */
	uint32_t rcv_nxt;
	uint32_t rcv_edge; /* RCV.NXT + RCV.WND as last sent */

	bool guest_fin; /* the guest's FIN came */
	bool host_eof;  /* the host's socket has no more: a FIN to send */
	bool host_shut; /* the guest's FIN went on: shutdown(SHUT_WR) */

	uint64_t timer;   /* when to send again, or TIME-WAIT ends; 0 off */
	unsigned backoff; /* the times the timeout was doubled */
	unsigned silent;  /* the timeouts since the guest last answered */

	/* The guest's SYN, its IP header and 8 bytes, for an ICMP error. */
	uint8_t syn[60 + 8];
	uint8_t syn_header;

	size_t out_len, in_len;
	uint8_t out[USER_TCP_BUF]; /* from the host, from SND.UNA on */
	uint8_t in[USER_TCP_BUF];  /* from the guest, not yet sent on */
} UserTcp;

typedef struct UserNet {
	NetLink link; /* first, so that a NetLink * is a UserNet * */
	uint8_t guest_mac[6];
	uint16_t ip_id; /* the identification of the next datagram sent */
	uint64_t now;   /* the virtual time of the call being made */

	/* The frames for the guest: a ring. */
	uint8_t queue[USER_QUEUE][ETH_HEADER + USER_MTU];
	uint16_t queue_len[USER_QUEUE];
	unsigned head, count;

	/* The host's name server. */
	struct sockaddr_storage dns;
	socklen_t dns_len;

	UserFlow udp[USER_UDP_FLOWS];

	/* The forwards, with the host's listening sockets. */
	unsigned forwards;
	NetUserForward forward[NET_USER_FORWARDS];
	int listen_fd[NET_USER_FORWARDS];
	uint16_t next_port; /* the router's port for the next forward */

	UserTcp tcp[USER_TCP_CONNS];

	uint8_t datagram[65536]; /* a datagram from the host */
} UserNet;

/* The router's own Ethernet address (user.c). */
extern const uint8_t user_mac[6];

/*
 * The Internet checksum (RFC 791 section 3.1, RFC 768) of data, added to
 * sum, the sum of the words before it; and the sum of the pseudo header
 * of UDP and TCP.
 */
uint16_t user_checksum(uint32_t sum, const uint8_t *data, size_t len);
uint32_t user_pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol,
                         size_t len);

/* Free places in the queue for the guest. */
unsigned user_queue_free(const UserNet *u);

/*
 * Send the guest an IPv4 datagram of protocol from src to dst, of len
 * bytes, at least 1, in as many fragments as the MTU needs, in Ethernet
 * frames to eth_dst. When the queue has no room for it all, it is lost.
 */
void user_send_ip(UserNet *u, const uint8_t eth_dst[6], uint8_t protocol,
                  uint32_t src, uint32_t dst, const uint8_t *data, size_t len);

/* A UDP datagram from the guest, its IP header and data (user_udp.c). */
void user_udp_input(UserNet *u, const uint8_t eth_src[6], uint32_t src,
                    uint32_t dst, const uint8_t *data, size_t len);

/*
 * Send the guest a UDP datagram of len bytes of data, which udp holds
 * after UDP_HEADER bytes for the header that this makes.
 */
void user_udp_output(UserNet *u, const uint8_t eth_dst[6], uint32_t src,
                     uint16_t sport, uint32_t dst, uint16_t dport, uint8_t *udp,
                     size_t len);

/* Take what the host's sockets have for the guest; close idle mappings. */
void user_udp_poll(UserNet *u);

/* No mappings yet, and the host's name server from config. */
void user_udp_init(UserNet *u, const NetUserConfig *config);
void user_udp_close(UserNet *u);

/*
 * An ICMP error from the router about a datagram of the guest's, ip,
 * its header of header bytes and len bytes in all: RFC 792's type and
 * code, then the header and 8 bytes of the data.
 */
void user_icmp_error(UserNet *u, const uint8_t eth_dst[6], uint8_t type,
                     uint8_t code, const uint8_t *ip, size_t header,
                     size_t len);

/* Ask on the link who has the address ip (RFC 826). */
void user_arp_request(UserNet *u, uint32_t ip);

/*
 * A TCP segment from the guest, in the datagram ip of len bytes, of
 * which header are the IP header's (user_tcp.c).
 */
void user_tcp_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *ip,
                    size_t header, size_t len);

/* The host's sockets and the timers: what is due for the guest. */
void user_tcp_poll(UserNet *u);

/*
 * No connections yet, and the forwards' sockets listening; false, with
 * a message in error, when the host refuses one.
 */
bool user_tcp_init(UserNet *u, const NetUserConfig *config, char *error,
                   size_t error_size);
void user_tcp_close(UserNet *u);

/* A DHCP message from the guest, the data of its UDP datagram. */
void user_dhcp_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *msg,
                     size_t len);

/* Big-endian access to the frames' fields. */
static inline uint16_t user_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t user_get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
	       p[3];
}

static inline void user_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void user_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

#endif /* BACKEND_NET_USER_INT_H */
