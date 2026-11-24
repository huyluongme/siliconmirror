/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * TCP in the user network (RFC 9293), as a NAT that ends the guest's
 * connections itself: the router is the guest's peer, and joins each
 * connection to a socket of the host, whose own TCP speaks to the real
 * destination. A SYN from the guest makes the host connect() first; the
 * router answers it when the host's connection is up (SYN-ACK), refused
 * (RST, as the destination did) or failed otherwise (ICMP host
 * unreachable, RFC 792), so that the guest learns what the destination
 * did. A forward (hostfwd) is the other way: a connection the host's
 * listening socket accepts makes the router send the guest a SYN, from
 * 10.0.2.2 and a port of its own.
 *
 * Kept small, as the link to the guest is one cable, which loses a frame
 * only when the guest drops it:
 * - No window scaling, SACK or timestamps (RFC 7323, RFC 2018): the
 *   router's SYN offers none, so the guest uses none.
 * - The guest's data is taken in order only: a segment after a hole is
 *   dropped, and the guest sends it again.
 * - The retransmission timeout is 1 s, RFC 6298's least (2.4), which a
 *   measured one would not pass on this link; doubled at each timeout
 *   (5.5) up to 60 s (2.5). After a timeout the router sends again from
 *   SND.UNA (go back N).
 * - No congestion control (RFC 5681): the network is the queue to the
 *   guest, which the router never fills.
 * - Urgent data is data.
 */

#define _GNU_SOURCE /* accept4() */

#include "backend/net/user_int.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The control bits (RFC 9293 section 3.1). */
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define TCP_HEADER 20
#define MSS_OPTION 4 /* kind 2, length 4, the size */

/* The router's MSS: the MTU less the IP and TCP headers (section 3.7.1). */
#define TCP_MSS (USER_MTU - IP_HEADER - TCP_HEADER)

/*
 * The guest's MSS when its SYN has no option (section 3.7.1), and the
 * least taken: Linux's is 48 (TCP_MIN_SND_MSS); ours 64, our choice.
 */
#define DEFAULT_MSS 536
#define MIN_MSS 64

#define SECOND 1000000000ull
#define RTO_NS SECOND
#define RTO_MAX_NS (60 * SECOND)

/*
 * The timeouts in a row with no word from the guest before the router
 * gives up: 8, after 1 + 2 + 4 + ... + 60 + 60 = 243 s, more than the
 * 100 s and 3 minutes of section 3.8.3's R2 for data and a SYN.
 */
#define TRIES 8

/* TIME-WAIT: Linux's 60 s (TCP_TIMEWAIT_LEN), not 2 MSL; our choice. */
#define TIME_WAIT_NS (60 * SECOND)

/* Frames of the queue that data leaves free, for ACKs and the others. */
#define QUEUE_RESERVE 16

/* The router's ports for forwards: RFC 6335's dynamic ones. */
#define FIRST_PORT 49152

typedef struct Segment {
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t wnd;
	uint16_t mss; /* a SYN's */
	const uint8_t *data;
	size_t len;
} Segment;

/* a comes before b in the sequence space (section 3.4). */
static bool before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/* The initial sequence number: the 4 microsecond clock of section 3.4.1. */
static uint32_t isn(const UserNet *u)
{
	return (uint32_t)(u->now / 4000);
}

static uint64_t rto(const UserTcp *c)
{
	uint64_t t = RTO_NS << c->backoff;

	return t < RTO_MAX_NS ? t : RTO_MAX_NS;
}

/* Send the guest a segment, with the MSS option when it is a SYN. */
static void segment_out(UserNet *u, const uint8_t eth_dst[6], uint32_t src,
                        uint16_t sport, uint32_t dst, uint16_t dport,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        uint16_t window, const uint8_t *data, size_t len)
{
	uint8_t seg[TCP_HEADER + MSS_OPTION + TCP_MSS];
	size_t header = TCP_HEADER;

	if (flags & SYN) {
		seg[header] = 2;
		seg[header + 1] = MSS_OPTION;
		user_put16(seg + header + 2, TCP_MSS);
		header += MSS_OPTION;
	}
	user_put16(seg, sport);
	user_put16(seg + 2, dport);
	user_put32(seg + 4, seq);
	user_put32(seg + 8, ack);
	seg[12] = (uint8_t)(header / 4 << 4);
	seg[13] = flags;
	user_put16(seg + 14, window);
	user_put16(seg + 16, 0);
	user_put16(seg + 18, 0); /* the urgent pointer */
	memcpy(seg + header, data, len);
	user_put16(seg + 16,
	           user_checksum(user_pseudo_sum(src, dst, IP_TCP, header + len),
	                         seg, header + len));
	user_send_ip(u, eth_dst, IP_TCP, src, dst, seg, header + len);
}

/*
 * The window to offer the guest: the room in the buffer, but its right
 * edge moved on only by a full segment at least, against the silly
 * window syndrome (section 3.8.6.2.2; min(Fr * 65535, 1460) is 1460).
 */
static uint16_t advertise(UserTcp *c)
{
	uint32_t room = (uint32_t)(USER_TCP_BUF - c->in_len);

	if (room - (c->rcv_edge - c->rcv_nxt) >= TCP_MSS)
		c->rcv_edge = c->rcv_nxt + room;
	return (uint16_t)(c->rcv_edge - c->rcv_nxt);
}

/* A segment of c to the guest: seq, flags and len bytes of data. */
static void conn_send(UserNet *u, UserTcp *c, uint32_t seq, uint8_t flags,
                      const uint8_t *data, size_t len)
{
	static const uint8_t unknown[6];

	/* A forward's SYN before the guest spoke: find it; the timer sends. */
	if (memcmp(u->guest_mac, unknown, 6) == 0) {
		user_arp_request(u, c->guest_ip);
		return;
	}
	/* A RST offers no window, as refuse()'s. */
	segment_out(u, u->guest_mac, c->ip, c->port, c->guest_ip, c->guest_port,
	            seq, flags & ACK ? c->rcv_nxt : 0, flags,
	            flags & RST ? 0 : advertise(c), data, len);
}

/*
 * An ACK, or the SYN-ACK again in SYN-RECEIVED. Its sequence number is
 * the next new one, SND.NXT before a timeout took it back (as BSD's
 * snd_max), so that the guest takes it as in its window.
 */
static void send_ack(UserNet *u, UserTcp *c)
{
	if (c->state == TCP_SYN_RECEIVED)
		conn_send(u, c, c->iss, SYN | ACK, c->out, 0);
	else
		conn_send(u, c, c->snd_max, ACK, c->out, 0);
}

/* Close a socket of the host with a RST to its peer (SO_LINGER of 0). */
static void abort_fd(int fd)
{
	struct linger l = {.l_onoff = 1, .l_linger = 0};

	setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
	close(fd);
}

/* End c at once: the host's peer gets a RST; the slot is free. */
static void drop(UserTcp *c)
{
	if (c->fd >= 0)
		abort_fd(c->fd);
	c->fd = -1;
	c->state = TCP_CLOSED;
	c->timer = 0;
}

/* End c with a RST to the guest too (section 3.10.4, ABORT). */
static void reset(UserNet *u, UserTcp *c)
{
	conn_send(u, c, c->snd_max, RST, c->out, 0);
	drop(c);
}

/* Both ways are done on the host's socket: close it. */
static void host_done(UserTcp *c)
{
	if (c->host_shut && c->host_eof && c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

static void time_wait(UserNet *u, UserTcp *c)
{
	c->state = TCP_TIME_WAIT;
	c->timer = u->now + TIME_WAIT_NS;
}

/*
 * RFC 6298 (5.1), (5.2), (5.3): the timer runs while something sent is
 * not acknowledged, or data waits for the guest's window (section
 * 3.8.6.1, the zero window probe), from when it starts.
 */
static void arm(UserNet *u, UserTcp *c)
{
	if ((c->snd_una != c->snd_max || c->out_len > 0) && c->timer == 0)
		c->timer = u->now + rto(c);
}

/*
 * Send the guest what the host's end gave, as far as the guest's window
 * and the queue let, then the FIN once the host's end is done.
 */
static void output(UserNet *u, UserTcp *c)
{
	if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT &&
	    c->state != TCP_FIN_WAIT_1 && c->state != TCP_CLOSING &&
	    c->state != TCP_LAST_ACK)
		return;
	while (user_queue_free(u) > QUEUE_RESERVE) {
		uint32_t sent = c->snd_nxt - c->snd_una;
		size_t n = c->out_len > sent ? c->out_len - sent : 0;
		uint32_t room = c->snd_wnd > sent ? c->snd_wnd - sent : 0;

		if (n > room)
			n = room;
		if (n > c->mss)
			n = c->mss;
		if (n > 0) {
			conn_send(u, c, c->snd_nxt,
			          ACK | (sent + n == c->out_len ? PSH : 0), c->out + sent,
			          n);
			c->snd_nxt += (uint32_t)n;
		} else if (c->host_eof && sent == c->out_len) {
			conn_send(u, c, c->snd_nxt, FIN | ACK, c->out, 0);
			c->snd_nxt++;
			if (c->state == TCP_ESTABLISHED)
				c->state = TCP_FIN_WAIT_1;
			else if (c->state == TCP_CLOSE_WAIT)
				c->state = TCP_LAST_ACK;
		} else {
			break;
		}
		if (before(c->snd_max, c->snd_nxt))
			c->snd_max = c->snd_nxt;
	}
	arm(u, c);
}

/*
 * What the guest sent, on to the host's socket, as far as it takes;
 * then, after the guest's FIN, the host's end is told. false when the
 * host's socket failed and the connection is reset.
 */
static bool host_write(UserNet *u, UserTcp *c)
{
	while (c->in_len > 0) {
		ssize_t n = send(c->fd, c->in, c->in_len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EAGAIN)
				break;
			reset(u, c);
			return false;
		}
		c->in_len -= (size_t)n;
		memmove(c->in, c->in + n, c->in_len);
	}
	if (c->in_len == 0 && c->guest_fin && !c->host_shut) {
		shutdown(c->fd, SHUT_WR);
		c->host_shut = true;
	}
	host_done(c);
	return true;
}

/*
 * What the host's socket has, as much as the buffer takes; output(), at
 * the end of the poll, sends it. host_write(), next, closes the socket
 * when its end too is done.
 */
static void host_read(UserNet *u, UserTcp *c)
{
	ssize_t n = recv(c->fd, c->out + c->out_len, USER_TCP_BUF - c->out_len, 0);

	if (n > 0)
		c->out_len += (size_t)n;
	else if (n == 0)
		c->host_eof = true;
	else if (errno != EAGAIN)
		reset(u, c);
}

/* The host's socket of c has taken a connect(): answer the guest's SYN. */
static void connected(UserNet *u, UserTcp *c, int error)
{
	if (error == 0) {
		c->state = TCP_SYN_RECEIVED;
		c->iss = isn(u);
		c->snd_una = c->iss;
		c->snd_nxt = c->snd_max = c->iss + 1;
		send_ack(u, c);
		arm(u, c);
		return;
	}
	/* Section 3.10.7.1: <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>. */
	if (error == ECONNREFUSED)
		conn_send(u, c, 0, RST | ACK, c->out, 0);
	else /* destination unreachable, host unreachable; RFC 792: 8 bytes */
		user_icmp_error(u, u->guest_mac, 3, 1, c->syn, c->syn_header,
		                sizeof(c->syn));
	drop(c);
}

static UserTcp *find(UserNet *u, uint32_t guest_ip, uint16_t guest_port,
                     uint32_t ip, uint16_t port)
{
	for (unsigned i = 0; i < USER_TCP_CONNS; i++) {
		UserTcp *c = &u->tcp[i];

		if (c->state != TCP_CLOSED && c->guest_ip == guest_ip &&
		    c->guest_port == guest_port && c->ip == ip && c->port == port)
			return c;
	}
	return NULL;
}

/*
 * A slot for a new connection, all zero, its socket for the caller to
 * set: a free one, else the TIME-WAIT one that ends first; NULL when
 * there is none.
 */
static UserTcp *new_conn(UserNet *u)
{
	UserTcp *found = NULL;

	for (unsigned i = 0; i < USER_TCP_CONNS; i++) {
		UserTcp *c = &u->tcp[i];

		if (c->state == TCP_CLOSED && c->fd < 0) {
			found = c;
			break;
		}
		if (c->state == TCP_TIME_WAIT &&
		    (found == NULL || c->timer < found->timer))
			found = c;
	}
	if (found == NULL)
		return NULL;
	drop(found);
	memset(found, 0, offsetof(UserTcp, out));
	return found;
}

/* The guest's MSS option (section 3.2), within MIN_MSS and TCP_MSS. */
static uint16_t parse_mss(const uint8_t *opt, size_t len)
{
	uint16_t mss = DEFAULT_MSS;
	size_t i = 0;

	while (i < len && opt[i] != 0) { /* 0 ends the list */
		if (opt[i] == 1) {           /* no operation */
			i++;
			continue;
		}
		if (i + 1 >= len || opt[i + 1] < 2 || opt[i + 1] > len - i)
			break; /* malformed: the rest is not read */
		if (opt[i] == 2 && opt[i + 1] == MSS_OPTION)
			mss = user_get16(opt + i + 2);
		i += opt[i + 1];
	}
	if (mss < MIN_MSS)
		return MIN_MSS;
	return mss < TCP_MSS ? mss : TCP_MSS;
}

/*
 * A SYN from the guest for a new connection: the host's socket connects
 * to the destination, 10.0.2.2 being the host itself and 10.0.2.3 its
 * name server; the answer waits for it. With no slot or socket, the SYN
 * is lost, as at a full NAT; the guest sends it again.
 */
static void open_out(UserNet *u, const uint8_t *ip, size_t header,
                     const Segment *s, uint16_t sport, uint16_t dport)
{
	uint32_t dst = user_get32(ip + 16);
	struct sockaddr_in in = {
		.sin_family = AF_INET,
		.sin_port = htons(dport),
		.sin_addr.s_addr = htonl(dst == USER_GATEWAY ? USER_LOOPBACK : dst),
	};
	const struct sockaddr *to = (const struct sockaddr *)&in;
	socklen_t to_len = sizeof(in);
	UserTcp *c = new_conn(u);

	if (dst == USER_DNS) {
		to = (const struct sockaddr *)&u->dns;
		to_len = u->dns_len;
	}
	if (c == NULL)
		return;
	c->fd =
		socket(to->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (c->fd < 0)
		return;
	c->state = TCP_CONNECTING;
	c->guest_ip = user_get32(ip + 12);
	c->guest_port = sport;
	c->ip = dst;
	c->port = dport;
	c->rcv_nxt = s->seq + 1;
	c->rcv_edge = c->rcv_nxt + USER_TCP_BUF;
	c->mss = s->mss;
	c->syn_header = (uint8_t)header;
	memcpy(c->syn, ip, header + 8);
	/* poll() tells when it is done, on the host's loopback too. */
	if (connect(c->fd, to, to_len) != 0 && errno != EINPROGRESS)
		connected(u, c, errno);
}

/*
 * A segment for no connection (section 3.10.7.1, CLOSED): a RST, unless
 * it is one.
 */
static void refuse(UserNet *u, const uint8_t eth_src[6], uint32_t src,
                   uint16_t sport, uint32_t dst, uint16_t dport,
                   const Segment *s)
{
	uint32_t len = (uint32_t)s->len + !!(s->flags & SYN) + !!(s->flags & FIN);

	if (s->flags & RST)
		return;
	if (s->flags & ACK)
		segment_out(u, eth_src, dst, dport, src, sport, s->ack, 0, RST, 0,
		            s->data, 0);
	else
		segment_out(u, eth_src, dst, dport, src, sport, 0, s->seq + len,
		            RST | ACK, 0, s->data, 0);
}

/* Section 3.10.7.3: the guest's answer to a forward's SYN. */
static void syn_sent(UserNet *u, UserTcp *c, const Segment *s)
{
	bool ack_ok = (s->flags & ACK) && s->ack == c->snd_max;

	/*
	 * A RST ends it if its ACK is acceptable; a bad ACK gets a RST; a SYN
	 * alone (a simultaneous open) is dropped, data in it too.
	 */
	if (s->flags & RST) {
		if (ack_ok)
			drop(c);
	} else if ((s->flags & ACK) && !ack_ok) {
		conn_send(u, c, s->ack, RST, c->out, 0);
	} else if ((s->flags & SYN) && ack_ok) {
		c->state = TCP_ESTABLISHED;
		c->rcv_nxt = s->seq + 1;
		c->rcv_edge = c->rcv_nxt + USER_TCP_BUF;
		c->snd_una = s->ack;
		c->snd_wnd = s->wnd;
		c->snd_wl1 = s->seq;
		c->mss = s->mss;
		c->silent = 0;
		c->backoff = 0;
		c->timer = 0;
		send_ack(u, c);
	}
}

/*
 * Section 3.10.7.4, first: whether the segment is in the window. One at
 * RCV.NXT is, with a window of 0 too, for its ACK and RST; its data and
 * FIN are then cut off, as outside the window.
 */
static bool acceptable(const UserTcp *c, const Segment *s)
{
	uint32_t wnd = c->rcv_edge - c->rcv_nxt;
	uint32_t len = (uint32_t)s->len + !!(s->flags & SYN) + !!(s->flags & FIN);

	return s->seq == c->rcv_nxt || s->seq - c->rcv_nxt < wnd ||
	       (len > 0 && s->seq + len - 1 - c->rcv_nxt < wnd);
}

/*
 * Section 3.10.7.4, fifth: the ACK field. false when the segment goes no
 * further: dropped, or the connection closed.
 */
static bool ack_arrives(UserNet *u, UserTcp *c, const Segment *s)
{
	if (c->state == TCP_SYN_RECEIVED) {
		if (s->ack != c->snd_max) {
			conn_send(u, c, s->ack, RST, c->out, 0);
			return false;
		}
		c->state = TCP_ESTABLISHED;
		c->snd_una = s->ack;
		c->snd_wnd = s->wnd;
		c->snd_wl1 = s->seq;
		c->backoff = 0;
		c->timer = 0;
		return true;
	}
	if (before(c->snd_max, s->ack)) { /* acknowledges what was not sent */
		send_ack(u, c);
		return false;
	}
	if (before(c->snd_una, s->ack)) {
		uint32_t acked = s->ack - c->snd_una;
		size_t data = acked < c->out_len ? acked : c->out_len;

		c->out_len -= data;
		memmove(c->out, c->out + data, c->out_len);
		c->snd_una = s->ack;
		if (before(c->snd_nxt, s->ack))
			c->snd_nxt = s->ack;
		/* RFC 6298 (5.3): the timer starts again, now. */
		c->backoff = 0;
		c->timer = 0;
		arm(u, c);
	}
	/*
	 * The window, from a segment no older than the last that set it
	 * (SND.WL1). RFC 9293's SND.WL2 =< SEG.ACK always holds here: SND.WL2
	 * is an ACK no later than SND.UNA, which this one is not before.
	 */
	if (!before(s->ack, c->snd_una) && !before(s->seq, c->snd_wl1)) {
		c->snd_wnd = s->wnd;
		c->snd_wl1 = s->seq;
	}
	/* All acknowledged: in these states, the router's FIN too. */
	if (c->snd_una == c->snd_max) {
		if (c->state == TCP_FIN_WAIT_1) {
			c->state = TCP_FIN_WAIT_2;
		} else if (c->state == TCP_CLOSING) {
			time_wait(u, c);
		} else if (c->state == TCP_LAST_ACK) {
			c->state = TCP_CLOSED; /* the slot is free when the socket is */
		}
	}
	return true;
}

/*
 * Section 3.10.7.4, seventh and eighth: the data and the FIN, taken in
 * order only, and acknowledged.
 */
static void text(UserNet *u, UserTcp *c, const Segment *s)
{
	uint32_t wnd = c->rcv_edge - c->rcv_nxt;
	uint32_t skip = c->rcv_nxt - s->seq; /* what came before */
	size_t len = s->len;
	bool fin = (s->flags & FIN) != 0;

	if (before(c->rcv_nxt, s->seq)) { /* after a hole */
		send_ack(u, c);
		return;
	}
	len -= skip;
	if (len + fin > wnd) {
		len = wnd;
		fin = false;
	}
	memcpy(c->in + c->in_len, s->data + skip, len);
	c->in_len += len;
	c->rcv_nxt += (uint32_t)len;
	if (fin) {
		c->rcv_nxt++;
		c->guest_fin = true;
		if (c->state == TCP_ESTABLISHED)
			c->state = TCP_CLOSE_WAIT;
		else if (c->state == TCP_FIN_WAIT_1)
			c->state = TCP_CLOSING;
		else /* FIN-WAIT-2 */
			time_wait(u, c);
	}
	if (host_write(u, c))
		send_ack(u, c);
}

/*
 * A segment for c, in a state other than CLOSED (section 3.10.7). What
 * it lets the router send goes at the next poll, which comes before the
 * guest takes a frame.
 */
static void arrives(UserNet *u, UserTcp *c, const Segment *s)
{
	if (c->state == TCP_CONNECTING) {
		/* No answer yet: the SYN again waits too; a RST ends it. */
		if (s->flags & RST)
			drop(c);
		return;
	}
	if (c->state == TCP_SYN_SENT) {
		syn_sent(u, c, s);
		return;
	}
	if (!acceptable(c, s)) {
		if (!(s->flags & RST))
			send_ack(u, c);
		return;
	}
	c->silent = 0;
	if (s->flags & RST) {
		/* RFC 5961 section 3.2: at RCV.NXT exactly, else a challenge. */
		if (s->seq == c->rcv_nxt)
			drop(c);
		else
			send_ack(u, c);
		return;
	}
	if (s->flags & SYN) { /* RFC 5961 section 4: a challenge ACK */
		send_ack(u, c);
		return;
	}
	if (!(s->flags & ACK) || !ack_arrives(u, c, s))
		return;
	if (!c->guest_fin && (s->len > 0 || (s->flags & FIN)))
		text(u, c, s);
}

void user_tcp_input(UserNet *u, const uint8_t eth_src[6], const uint8_t *ip,
                    size_t header, size_t len)
{
	const uint8_t *tcp = ip + header;
	size_t tcp_len = len - header, offset;
	uint32_t src = user_get32(ip + 12), dst = user_get32(ip + 16);
	uint16_t sport, dport;
	Segment s;
	UserTcp *c;

	if (tcp_len < TCP_HEADER)
		return;
	offset = (size_t)(tcp[12] >> 4) * 4;
	if (offset < TCP_HEADER || offset > tcp_len ||
	    user_checksum(user_pseudo_sum(src, dst, IP_TCP, tcp_len), tcp,
	                  tcp_len) != 0)
		return;
	sport = user_get16(tcp);
	dport = user_get16(tcp + 2);
	s.seq = user_get32(tcp + 4);
	s.ack = user_get32(tcp + 8);
	s.flags = tcp[13];
	s.wnd = user_get16(tcp + 14);
	s.mss = parse_mss(tcp + TCP_HEADER, offset - TCP_HEADER);
	s.data = tcp + offset;
	s.len = tcp_len - offset;
	c = find(u, src, sport, dst, dport);
	if (c != NULL)
		arrives(u, c, &s);
	else if ((s.flags & (SYN | ACK | RST | FIN)) == SYN &&
	         (dst != USER_DNS || dport == USER_DNS_PORT))
		open_out(u, ip, header, &s, sport, dport);
	else
		refuse(u, eth_src, src, sport, dst, dport, &s);
}

/* The router's next port for a forward's connection to the guest's end. */
static uint16_t next_port(UserNet *u, uint32_t guest_ip, uint16_t guest_port)
{
	uint16_t port;

	do {
		port = u->next_port;
		u->next_port = port == 65535 ? FIRST_PORT : port + 1;
	} while (find(u, guest_ip, guest_port, USER_GATEWAY, port) != NULL);
	return port;
}

/*
 * The connections forward i's socket has: each makes a SYN to the guest.
 * With no slot, the host's peer gets a RST.
 */
static void accept_forward(UserNet *u, unsigned i)
{
	const NetUserForward *f = &u->forward[i];
	int fd;

	while ((fd = accept4(u->listen_fd[i], NULL, NULL,
	                     SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
		UserTcp *c = new_conn(u);

		if (c == NULL) {
			abort_fd(fd);
			continue;
		}
		c->fd = fd;
		c->guest_ip = f->guest_ip;
		c->guest_port = f->guest_port;
		c->ip = USER_GATEWAY;
		c->port = next_port(u, f->guest_ip, f->guest_port);
		c->state = TCP_SYN_SENT;
		c->iss = isn(u);
		c->snd_una = c->iss;
		c->snd_nxt = c->snd_max = c->iss + 1;
		conn_send(u, c, c->iss, SYN, c->out, 0);
		arm(u, c);
	}
}

/* Something on the host's socket of c. */
static void host_event(UserNet *u, UserTcp *c)
{
	if (c->state == TCP_CONNECTING) {
		int error = 0;
		socklen_t len = sizeof(error);

		getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len);
		connected(u, c, error);
		return;
	}
	if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
		host_read(u, c);
	/* What waits for the host's socket; the guest hears when the edge moves. */
	if (c->fd >= 0 && host_write(u, c) && !c->guest_fin) {
		uint32_t edge = c->rcv_edge;

		advertise(c);
		if (c->rcv_edge != edge)
			send_ack(u, c);
	}
}

/* What to wait for on the host's socket of c. */
static short events(const UserTcp *c)
{
	short ev = 0;

	if (c->state == TCP_CONNECTING)
		return POLLOUT;
	if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
	    !c->host_eof && c->out_len < USER_TCP_BUF)
		ev |= POLLIN;
	if (c->in_len > 0)
		ev |= POLLOUT;
	return ev;
}

/*
 * A timeout (RFC 6298 (5.4) to (5.6)): send again from SND.UNA, or probe
 * a shut window with one byte; the end of TIME-WAIT; or, after TRIES
 * with no word from the guest, the end.
 */
static void expire(UserNet *u, UserTcp *c)
{
	if (c->state == TCP_TIME_WAIT) {
		drop(c);
		return;
	}
	if (c->silent == TRIES) {
		reset(u, c);
		return;
	}
	c->silent++;
	c->backoff++;
	c->timer = u->now + rto(c);
	if (c->state == TCP_SYN_SENT) {
		conn_send(u, c, c->iss, SYN, c->out, 0);
		return;
	}
	if (c->state == TCP_SYN_RECEIVED) {
		send_ack(u, c);
		return;
	}
	c->snd_nxt = c->snd_una;
	output(u, c);
	if (c->snd_nxt == c->snd_una && c->out_len > 0) {
		conn_send(u, c, c->snd_nxt, ACK, c->out, 1);
		c->snd_nxt++;
		if (before(c->snd_max, c->snd_nxt))
			c->snd_max = c->snd_nxt;
	}
}

void user_tcp_poll(UserNet *u)
{
	struct pollfd fds[USER_TCP_CONNS + NET_USER_FORWARDS];
	UserTcp *polled[USER_TCP_CONNS];
	nfds_t n = 0, conns;

	for (unsigned i = 0; i < USER_TCP_CONNS; i++) {
		UserTcp *c = &u->tcp[i];
		short ev = c->fd < 0 ? 0 : events(c);

		if (ev != 0) {
			fds[n] = (struct pollfd){.fd = c->fd, .events = ev};
			polled[n++] = c;
		}
	}
	conns = n;
	for (unsigned i = 0; i < u->forwards; i++)
		fds[n++] = (struct pollfd){.fd = u->listen_fd[i], .events = POLLIN};
	/* The connections first: a new one may take a TIME-WAIT's slot. */
	if (n > 0 && poll(fds, n, 0) > 0) {
		for (nfds_t i = 0; i < conns; i++)
			if (fds[i].revents != 0)
				host_event(u, polled[i]);
		for (unsigned i = 0; i < u->forwards; i++)
			if (fds[conns + i].revents != 0)
				accept_forward(u, i);
	}
	for (unsigned i = 0; i < USER_TCP_CONNS; i++) {
		UserTcp *c = &u->tcp[i];

		if (c->timer != 0 && u->now >= c->timer)
			expire(u, c);
		output(u, c);
	}
}

bool user_tcp_init(UserNet *u, const NetUserConfig *config, char *error,
                   size_t error_size)
{
	for (unsigned i = 0; i < USER_TCP_CONNS; i++)
		u->tcp[i].fd = -1;
	u->next_port = FIRST_PORT;
	for (unsigned i = 0; i < config->forwards; i++) {
		const NetUserForward *f = &config->forward[i];
		struct sockaddr_in a = {
			.sin_family = AF_INET,
			.sin_port = htons(f->host_port),
			.sin_addr.s_addr = htonl(f->host_ip),
		};
		int on = 1;
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

		if (fd >= 0) {
			u->forward[u->forwards] = *f;
			u->listen_fd[u->forwards++] = fd;
		}
		if (fd < 0 ||
		    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
		    bind(fd, (const struct sockaddr *)&a, sizeof(a)) != 0 ||
		    listen(fd, SOMAXCONN) != 0) {
			char host[INET_ADDRSTRLEN];

			inet_ntop(AF_INET, &a.sin_addr, host, sizeof(host));
			snprintf(error, error_size, "user: hostfwd %s:%u: %s", host,
			         (unsigned)f->host_port, strerror(errno));
			return false;
		}
	}
	return true;
}

void user_tcp_close(UserNet *u)
{
	for (unsigned i = 0; i < u->forwards; i++)
		close(u->listen_fd[i]);
	for (unsigned i = 0; i < USER_TCP_CONNS; i++)
		drop(&u->tcp[i]);
}
