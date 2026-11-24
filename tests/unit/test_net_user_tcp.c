/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * TCP in the user network (backend/net/user_tcp.c): the guest's segments
 * and the router's answers, byte by byte, against TCP sockets of the
 * test on the host's loopback; the forwards (hostfwd) and their option.
 */

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h> /* SIOCOUTQ (glibc 2.39 has no include of it) */
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend/net/user.h"
#include "net_user_frames.h"
#include "test.h"

#define RESOLV "test_net_user_tcp.resolv"

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define MSS 1460
#define FIRST_PORT 49152
#define CONNS 64 /* the router's connections at once */
/* When the clock of the initial sequence numbers (4 us) reads 0xffffffff. */
#define WRAP (0xffffffffull * 4000)

static const uint8_t none[1];
static const uint8_t mss_1460[4] = {2, 4, 0x05, 0xb4};

/* A frame the test waits for did not come: no use going on. */
static void missing(const char *what, int line)
{
	fprintf(stderr, "test_net_user_tcp.c:%d: no %s\n", line, what);
	exit(1);
}

/* The next frame for the guest, waiting up to a second for the host. */
static size_t wait_frame_at(NetLink *n, int line)
{
	for (int i = 0; i < 2000; i++) {
		size_t len = take(n);

		if (len != 0)
			return len;
		usleep(500);
	}
	missing("frame", line);
	return 0;
}

#define wait_frame(n) wait_frame_at(n, __LINE__)

/* No frame for the guest, after the host had time to send one. */
static void quiet(NetLink *n)
{
	usleep(2000);
	CHECK_EQ(take(n), 0);
}

static NetLink *open_config(NetUserConfig *config)
{
	char error[200];
	NetLink *n;

	if (config->resolv_conf == NULL)
		config->resolv_conf = RESOLV;
	if (config->dns_port == 0)
		config->dns_port = 53;
	n = net_user_open_config(config, error, sizeof(error));
	if (n == NULL) {
		fprintf(stderr, "net_user_open_config: %s\n", error);
		exit(1);
	}
	return n;
}

static NetLink *open_tcp(void)
{
	NetUserConfig config = {0};

	return open_config(&config);
}

static void write_resolv(const char *text)
{
	FILE *f = fopen(RESOLV, "w");

	CHECK(f != NULL);
	fputs(text, f);
	fclose(f);
}

/* A TCP segment from the guest, with options, in frame. */
static size_t tcp_frame(uint32_t src, uint16_t sport, uint32_t dst,
                        uint16_t dport, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint16_t wnd, const uint8_t *opt,
                        size_t optlen, const uint8_t *data, size_t len)
{
	static uint8_t tcp[65536];
	size_t h = 20 + optlen;

	put16(tcp, sport);
	put16(tcp + 2, dport);
	put32(tcp + 4, seq);
	put32(tcp + 8, ack);
	tcp[12] = (uint8_t)(h / 4 << 4);
	tcp[13] = flags;
	put16(tcp + 14, wnd);
	put16(tcp + 16, 0);
	put16(tcp + 18, 0);
	memcpy(tcp + 20, opt, optlen);
	memcpy(tcp + h, data, len);
	put16(tcp + 16, ~sum(pseudo(src, dst, 6, h + len), tcp, h + len));
	return ip_frame(frame, router_mac, 6, src, dst, 64, tcp, h + len);
}

/* A segment from the router, as the test reads it. */
typedef struct Got {
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t wnd;
	size_t len; /* of the data */
	const uint8_t *data;
} Got;

/* A TCP segment from the router in got, from src:sport to the guest. */
static Got parse_tcp(size_t len, uint32_t src, uint16_t sport, uint16_t dport)
{
	const uint8_t *tcp = got + 34;
	size_t tlen = len - 34, h;
	Got g;

	check_ip(len, guest_mac, 6, src, GUEST);
	CHECK_EQ(get16(got + 14 + 6), 0); /* not a fragment */
	CHECK_EQ(sum(pseudo(src, GUEST, 6, tlen), tcp, tlen), 0xffff);
	CHECK_EQ(get16(tcp), sport);
	CHECK_EQ(get16(tcp + 2), dport);
	g.seq = get32(tcp + 4);
	g.ack = get32(tcp + 8);
	h = (size_t)(tcp[12] >> 4) * 4;
	CHECK_EQ(tcp[12] & 0xf, 0);
	g.flags = tcp[13];
	g.wnd = get16(tcp + 14);
	CHECK_EQ(get16(tcp + 18), 0);
	if (g.flags & SYN) {
		CHECK_EQ(h, 24);
		CHECK(memcmp(tcp + 20, mss_1460, 4) == 0);
	} else {
		CHECK_EQ(h, 20);
	}
	g.data = tcp + h;
	g.len = tlen - h;
	if (!(g.flags & ACK))
		CHECK_EQ(g.ack, 0);
	if (g.flags & RST) {
		CHECK_EQ(g.len, 0);
		CHECK_EQ(g.wnd, 0);
	}
	return g;
}

/* The guest's end of a connection, and the host's. */
typedef struct Conn {
	NetLink *n;
	uint32_t dst; /* the router's end, as the guest sees it */
	uint16_t dport, gport;
	uint32_t seq, ack; /* the guest's next, and the router's next */
	uint16_t wnd;      /* the guest's window */
	int host;
	const uint8_t *syn_data; /* in the guest's SYN, syn_len bytes */
	size_t syn_len;
} Conn;

static void send_at(Conn *c, uint32_t seq, uint32_t ack, uint8_t flags,
                    const void *data, size_t len)
{
	send_frame(c->n, tcp_frame(GUEST, c->gport, c->dst, c->dport, seq, ack,
	                           flags, c->wnd, none, 0, data, len));
}

/* The guest sends, at its next sequence number. */
static void gsend(Conn *c, uint8_t flags, const void *data, size_t len)
{
	send_at(c, c->seq, c->ack, flags, data, len);
	c->seq += (uint32_t)len + !!(flags & SYN) + !!(flags & FIN);
}

/* The router's next segment for c: its flags, its data's length. */
static Got expect_at(Conn *c, uint8_t flags, size_t len, int line)
{
	Got g = parse_tcp(wait_frame_at(c->n, line), c->dst, c->dport, c->gport);

	if (g.flags != flags || g.len != len)
		fprintf(stderr, "test_net_user_tcp.c:%d: flags 0x%x len %zu\n", line,
		        g.flags, g.len);
	CHECK_EQ(g.flags, flags);
	CHECK_EQ(g.len, len);
	if (g.flags & SYN)
		c->ack = g.seq;
	CHECK_EQ(g.seq, c->ack);
	if (g.flags & ACK)
		CHECK_EQ(g.ack, c->seq);
	c->ack = g.seq + (uint32_t)g.len + !!(g.flags & SYN) + !!(g.flags & FIN);
	return g;
}

#define expect(c, flags, len) expect_at(c, flags, len, __LINE__)

/*
 * A socket of the test's own, moved above 2: descriptor 0, which main()
 * closes, is left for the router's next socket.
 */
static int high(int fd)
{
	int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);

	CHECK(moved >= 3);
	close(fd);
	return moved;
}

/* The descriptors open in this process. */
static int open_fds(void)
{
	int count = 0;

	for (int fd = 0; fd < 1024; fd++)
		count += fcntl(fd, F_GETFD) != -1;
	return count;
}

/* A listening TCP socket of the test on the loopback, and its port. */
static int listener(uint16_t *port, int backlog)
{
	struct sockaddr_in a = {.sin_family = AF_INET,
	                        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t alen = sizeof(a);
	int fd = high(socket(AF_INET, SOCK_STREAM, 0));

	CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(listen(fd, backlog) == 0);
	CHECK(getsockname(fd, (struct sockaddr *)&a, &alen) == 0);
	*port = ntohs(a.sin_port);
	return fd;
}

/* A listening socket on the IPv6 loopback; -1 when the host has none. */
static int listener6(uint16_t *port)
{
	struct sockaddr_in6 a = {.sin6_family = AF_INET6,
	                         .sin6_addr = IN6ADDR_LOOPBACK_INIT};
	socklen_t alen = sizeof(a);
	int fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
		close(fd);
		return -1;
	}
	CHECK(getsockname(fd, (struct sockaddr *)&a, &alen) == 0);
	*port = ntohs(a.sin6_port);
	return high(fd);
}

/* A port of the loopback that nothing listens on. */
static uint16_t closed_port(void)
{
	uint16_t port;

	close(listener(&port, 1));
	return port;
}

static bool readable(int fd)
{
	struct pollfd p = {.fd = fd, .events = POLLIN};

	return poll(&p, 1, 1000) == 1;
}

static int host_accept(int lfd)
{
	int fd;

	if (!readable(lfd))
		missing("connection", __LINE__);
	fd = accept(lfd, NULL, NULL);
	CHECK(fd >= 0);
	return fd;
}

/* What the host's end reads: text, then nothing more for now. */
static void host_reads(int fd, const char *text)
{
	char buf[256];
	size_t want = strlen(text), have = 0;

	while (have < want) {
		ssize_t n;

		if (!readable(fd))
			missing("data on the host", __LINE__);
		n = recv(fd, buf + have, sizeof(buf) - have, 0);
		CHECK(n > 0);
		if (n <= 0)
			return;
		have += (size_t)n;
	}
	CHECK_EQ(have, want);
	CHECK(memcmp(buf, text, want) == 0);
}

/*
 * The host's end of c reads len bytes, data when it is not NULL; the
 * router, polled meanwhile, has nothing for the guest.
 */
static void host_drains(Conn *c, const uint8_t *data, size_t len)
{
	static uint8_t buf[65536];
	size_t have = 0;
	int waits = 0;

	while (have < len) {
		size_t want = len - have < sizeof(buf) ? len - have : sizeof(buf);
		ssize_t r = recv(c->host, buf, want, MSG_DONTWAIT);

		if (r > 0) {
			if (data != NULL)
				CHECK(memcmp(buf, data + have, (size_t)r) == 0);
			have += (size_t)r;
			continue;
		}
		if (++waits > 20000)
			missing("data on the host", __LINE__);
		CHECK_EQ(take(c->n), 0);
		usleep(50);
	}
}

/* The router's socket joined to the test's socket fd. */
static int router_end(int fd)
{
	struct sockaddr_in mine, peer, a, b;
	socklen_t len = sizeof(mine);

	CHECK(getsockname(fd, (struct sockaddr *)&mine, &len) == 0);
	len = sizeof(peer);
	CHECK(getpeername(fd, (struct sockaddr *)&peer, &len) == 0);
	for (int r = 0; r < 1024; r++) {
		socklen_t la = sizeof(a), lb = sizeof(b);

		if (r != fd && getsockname(r, (struct sockaddr *)&a, &la) == 0 &&
		    getpeername(r, (struct sockaddr *)&b, &lb) == 0 &&
		    a.sin_family == AF_INET && a.sin_port == peer.sin_port &&
		    b.sin_port == mine.sin_port)
			return r;
	}
	missing("socket of the router's", __LINE__);
	return -1;
}

/*
 * The test fills the router's socket joined to host itself, host
 * reading nothing: what the router has for it waits. The bytes put in.
 */
static size_t jam(int host)
{
	static const uint8_t junk[4096];
	int r = router_end(host), small = 4096;
	size_t put = 0;
	ssize_t n;

	CHECK(setsockopt(r, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
	/*
	 * Until it stays full, what it holds the same, over longer than the
	 * host's delayed ACK (40 ms): no room comes back until host reads.
	 */
	for (int i = 0, last = -1;; i++) {
		size_t had = put;
		int queued;

		while ((n = send(r, junk, sizeof(junk), MSG_DONTWAIT)) > 0)
			put += (size_t)n;
		CHECK_EQ(errno, EAGAIN);
		CHECK(ioctl(r, SIOCOUTQ, &queued) == 0);
		if (put == had && queued == last)
			break;
		if (i == 100)
			missing("full socket", __LINE__);
		last = queued;
		usleep(50000);
	}
	return put;
}

/* The host's end reads the end of the stream, or err. */
static void host_ends(int fd, int err)
{
	char c;
	ssize_t n;

	if (!readable(fd))
		missing("end on the host", __LINE__);
	n = recv(fd, &c, 1, 0);
	CHECK_EQ(n, err == 0 ? 0 : -1);
	if (err != 0)
		CHECK_EQ(errno, err);
}

/*
 * The guest connects from gport to dst:dport, with the MSS option opt,
 * and the host's listener lfd takes the connection.
 */
static void connect_opt(Conn *c, NetLink *n, uint32_t dst, uint16_t dport,
                        uint16_t gport, int lfd, const uint8_t *opt,
                        size_t optlen)
{
	Got g;

	c->n = n;
	c->dst = dst;
	c->dport = dport;
	c->gport = gport;
	c->seq = 0xfffffff0u; /* across the wrap of the sequence numbers */
	c->ack = 0;
	if (c->wnd == 0)
		c->wnd = 65535;
	send_frame(n,
	           tcp_frame(GUEST, gport, dst, dport, c->seq, 0, SYN, c->wnd, opt,
	                     optlen, c->syn_len ? c->syn_data : none, c->syn_len));
	c->seq++;
	g = expect(c, SYN | ACK, 0);
	CHECK_EQ(g.seq, (uint32_t)(now / 4000)); /* the 4 us clock */
	CHECK_EQ(g.wnd, 65535);
	gsend(c, ACK, none, 0);
	c->host = lfd < 0 ? -1 : host_accept(lfd);
}

static void connect_out(Conn *c, NetLink *n, uint16_t dport, uint16_t gport,
                        int lfd)
{
	connect_opt(c, n, GATEWAY, dport, gport, lfd, mss_1460, 4);
}

/* The guest resets c, and the router lets it go. */
static void guest_reset(Conn *c)
{
	gsend(c, RST, none, 0);
	close(c->host);
}

/* A segment of no connection: the router's RST, from (dst, dport). */
static Got refused(NetLink *n, uint32_t dst, uint16_t dport, uint16_t gport)
{
	return parse_tcp(wait_frame(n), dst, dport, gport);
}

static void test_life(void)
{
	static uint8_t more[MSS - 5];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	int fds = open_fds(), probe;
	Conn c = {0};
	Got g;

	connect_out(&c, n, port, 40000, lfd);
	gsend(&c, ACK | PSH, "hello", 5);
	g = expect(&c, ACK, 0);
	CHECK_EQ(g.wnd, 65535 - 5); /* the edge moves by a segment at least */
	host_reads(c.host, "hello");
	memset(more, 'm', sizeof(more));
	gsend(&c, ACK, more, sizeof(more));
	CHECK_EQ(expect(&c, ACK, 0).wnd, 65535); /* a segment, exactly */
	host_drains(&c, more, sizeof(more));

	CHECK_EQ(send(c.host, "world", 5, 0), 5);
	g = expect(&c, ACK | PSH, 5);
	CHECK(memcmp(g.data, "world", 5) == 0);
	gsend(&c, ACK, none, 0);
	quiet(n);
	/* Idle a while, then data not acknowledged: again 1 s after. */
	now += 10 * SECOND;
	quiet(n);
	CHECK_EQ(send(c.host, "again", 5, 0), 5);
	expect(&c, ACK | PSH, 5);
	now += SECOND - 1;
	quiet(n);
	now += 1;
	c.ack -= 5;
	expect(&c, ACK | PSH, 5);
	gsend(&c, ACK, none, 0);
	quiet(n);

	/* The host's end closes first: FIN-WAIT-1, FIN-WAIT-2, TIME-WAIT. */
	shutdown(c.host, SHUT_WR);
	expect(&c, FIN | ACK, 0);
	gsend(&c, ACK, none, 0);
	quiet(n);
	gsend(&c, ACK | FIN, "bye", 3);
	expect(&c, ACK, 0);
	host_reads(c.host, "bye");
	host_ends(c.host, 0);
	/* Both ways done: the router's socket is closed; another in its place. */
	CHECK_EQ(open_fds(), fds + 1);
	probe = dup(2);

	/* The FIN again, in TIME-WAIT: acknowledged again; an ACK is not. */
	send_at(&c, c.seq - 4, c.ack, ACK | FIN, "bye", 3);
	expect(&c, ACK, 0);
	send_at(&c, c.seq, c.ack, ACK, none, 0);
	now += 60 * SECOND - 1;
	quiet(n);
	send_at(&c, c.seq - 1, c.ack, ACK | FIN, none, 0);
	expect(&c, ACK, 0);
	now += 1;
	CHECK_EQ(take(n), 0); /* TIME-WAIT ends */
	CHECK(fcntl(probe, F_GETFD) != -1);
	CHECK(fcntl(1, F_GETFD) != -1);
	close(probe);
	send_at(&c, c.seq - 1, c.ack, ACK | FIN, none, 0);
	g = refused(n, GATEWAY, port, 40000);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	/* Nothing more from it, ever. */
	for (int i = 0; i < 20; i++) {
		now += 60 * SECOND;
		CHECK_EQ(take(n), 0);
	}

	close(c.host);
	close(lfd);
	net_destroy(n);
}

static void test_close_wait(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	int fds = open_fds();
	Conn c = {0};
	Got g;

	/* The guest closes first: CLOSE-WAIT, LAST-ACK. */
	connect_out(&c, n, port, 40001, lfd);
	gsend(&c, ACK | FIN, none, 0);
	expect(&c, ACK, 0);
	host_ends(c.host, 0);
	CHECK_EQ(send(c.host, "late", 4, 0), 4);
	g = expect(&c, ACK | PSH, 4);
	CHECK(memcmp(g.data, "late", 4) == 0);
	/* The guest's data after its FIN is not taken, nor answered. */
	gsend(&c, ACK, "x", 1);
	quiet(n);
	c.seq--;
	close(c.host);
	expect(&c, FIN | ACK, 0);
	CHECK_EQ(open_fds(), fds); /* the router's socket is closed */
	gsend(&c, ACK, none, 0);
	quiet(n);
	/* CLOSED: the next segment has no connection. */
	send_at(&c, c.seq, c.ack, ACK, none, 0);
	g = refused(n, GATEWAY, port, 40001);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);

	close(lfd);
	net_destroy(n);
}

static void test_refused(void)
{
	NetLink *n = open_tcp();
	uint16_t port = closed_port();
	Got g;

	send_frame(n, tcp_frame(GUEST, 40002, GATEWAY, port, 777, 0, SYN, 65535,
	                        mss_1460, 4, none, 0));
	g = refused(n, GATEWAY, port, 40002);
	CHECK_EQ(g.flags, RST | ACK);
	CHECK_EQ(g.seq, 0);
	CHECK_EQ(g.ack, 778);
	quiet(n);
	net_destroy(n);
}

static void test_unreachable(void)
{
	NetLink *n;
	size_t len;

	/* The host refuses a TCP connection to a multicast address. */
	write_resolv("nameserver 224.0.0.1\n");
	n = open_tcp();
	send_frame(n, tcp_frame(GUEST, 40003, DNS, 53, 777, 0, SYN, 65535, mss_1460,
	                        4, none, 0));
	len = wait_frame(n);
	check_ip(len, guest_mac, 1, GATEWAY, GUEST);
	CHECK_EQ(len, 34 + 8 + 28);
	CHECK_EQ(got[34], 3); /* destination unreachable */
	CHECK_EQ(got[35], 1); /* host unreachable */
	CHECK_EQ(get32(got + 38), 0);
	CHECK_EQ(sum(0, got + 34, len - 34), 0xffff);
	CHECK(memcmp(got + 42, frame + 14, 28) == 0);
	quiet(n);
	net_destroy(n);
	remove(RESOLV);
}

static void test_dns(void)
{
	NetUserConfig config = {0};
	NetLink *n;
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	Got g;

	write_resolv("nameserver 127.0.0.1\n");
	config.dns_port = port;
	n = open_config(&config);
	connect_opt(&c, n, DNS, 53, 40004, -1, mss_1460, 4);
	c.host = host_accept(lfd);
	gsend(&c, ACK, "q", 1);
	expect(&c, ACK, 0);
	host_reads(c.host, "q");
	guest_reset(&c);

	/* The name server's other ports are closed. */
	send_frame(n, tcp_frame(GUEST, 40005, DNS, 80, 9, 0, SYN, 65535, none, 0,
	                        none, 0));
	g = parse_tcp(take1(n), DNS, 80, 40005);
	CHECK_EQ(g.flags, RST | ACK);
	CHECK_EQ(g.ack, 10);
	close(lfd);
	net_destroy(n);

	/* A name server of IPv6, where the host has it. */
	lfd = listener6(&port);
	if (lfd >= 0) {
		write_resolv("nameserver ::1\n");
		config.dns_port = port;
		n = open_config(&config);
		connect_opt(&c, n, DNS, 53, 40006, -1, mss_1460, 4);
		c.host = host_accept(lfd);
		gsend(&c, ACK, "6", 1);
		expect(&c, ACK, 0);
		host_reads(c.host, "6");
		guest_reset(&c);
		close(lfd);
		net_destroy(n);
	}
	remove(RESOLV);
}

static void test_unknown(void)
{
	NetLink *n = open_tcp();
	Got g;

	/* With ACK: <SEQ=SEG.ACK><CTL=RST>. */
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 1234, ACK, 100, none,
	                        0, none, 0));
	g = parse_tcp(take1(n), OUTSIDE, 80, 40006);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, 1234);
	CHECK_EQ(g.ack, 0);
	CHECK_EQ(g.wnd, 0);
	/* Without: <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>. */
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 0, FIN, 100, none, 0,
	                        (const uint8_t *)"abc", 3));
	g = parse_tcp(take1(n), OUTSIDE, 80, 40006);
	CHECK_EQ(g.flags, RST | ACK);
	CHECK_EQ(g.seq, 0);
	CHECK_EQ(g.ack, 9);
	/* A SYN with FIN is no new connection. */
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 0, SYN | FIN, 100,
	                        none, 0, none, 0));
	g = parse_tcp(take1(n), OUTSIDE, 80, 40006);
	CHECK_EQ(g.flags, RST | ACK);
	CHECK_EQ(g.ack, 7);
	/* Nor is a SYN with ACK, or one with RST, which has no answer. */
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 66, SYN | ACK, 100,
	                        none, 0, none, 0));
	g = parse_tcp(take1(n), OUTSIDE, 80, 40006);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, 66);
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 66, RST | ACK, 100,
	                        none, 0, none, 0));
	send_frame(n, tcp_frame(GUEST, 40006, OUTSIDE, 80, 5, 0, SYN | RST, 100,
	                        none, 0, none, 0));
	quiet(n);
	net_destroy(n);
}

static void test_malformed(void)
{
	NetLink *n = open_tcp();
	uint8_t *tcp = frame + 34, short_tcp[19];
	size_t len;

	/* Each would have a RST; dropped, none comes. */
	len =
		tcp_frame(GUEST, 40007, OUTSIDE, 80, 5, 1, ACK, 100, none, 0, none, 0);
	put16(tcp + 16, get16(tcp + 16) ^ 1); /* the checksum */
	send_frame(n, len);
	tcp_frame(GUEST, 40007, OUTSIDE, 80, 5, 1, ACK, 100, none, 0, none, 0);
	tcp[12] = 4 << 4; /* a header of 16 bytes */
	put16(tcp + 16, 0);
	put16(tcp + 16, ~sum(pseudo(GUEST, OUTSIDE, 6, 20), tcp, 20));
	send_frame(n, len);
	tcp_frame(GUEST, 40007, OUTSIDE, 80, 5, 1, ACK, 100, none, 0, none, 0);
	tcp[12] = 6 << 4; /* a header of 24 bytes, in 20 */
	put16(tcp + 16, 0);
	put16(tcp + 16, ~sum(pseudo(GUEST, OUTSIDE, 6, 20), tcp, 20));
	send_frame(n, len);
	/* 19 bytes of TCP header. */
	memcpy(short_tcp, tcp, sizeof(short_tcp));
	send_frame(n, ip_frame(frame, router_mac, 6, GUEST, OUTSIDE, 64, short_tcp,
	                       sizeof(short_tcp)));
	/* To the link's broadcast address, or another of the link's. */
	send_frame(n, tcp_frame(GUEST, 40007, 0x0a0002ffu, 80, 5, 1, ACK, 100, none,
	                        0, none, 0));
	send_frame(n, tcp_frame(GUEST, 40007, 0x0a000209u, 80, 5, 1, ACK, 100, none,
	                        0, none, 0));
	quiet(n);
	/* A header of 24 bytes is read as one. */
	tcp_frame(GUEST, 40007, OUTSIDE, 80, 5, 1, ACK, 100,
	          (const uint8_t *)"\1\1\1\1", 4, none, 0);
	send_frame(n, 14 + 20 + 24);
	CHECK_EQ(parse_tcp(take1(n), OUTSIDE, 80, 40007).seq, 1);
	net_destroy(n);
}

static void test_time_exceeded(void)
{
	NetLink *n = open_tcp();
	size_t len =
		tcp_frame(GUEST, 40008, OUTSIDE, 80, 5, 0, SYN, 100, none, 0, none, 0);

	frame[14 + 8] = 1; /* the time to live */
	put16(frame + 14 + 10, 0);
	put16(frame + 14 + 10, ~sum(0, frame + 14, 20));
	send_frame(n, len);
	len = take1(n);
	check_ip(len, guest_mac, 1, GATEWAY, GUEST);
	CHECK_EQ(got[34], 11);
	CHECK_EQ(got[35], 0);
	CHECK(memcmp(got + 42, frame + 14, 28) == 0);
	net_destroy(n);
}

/* The MSS the router uses with a guest whose SYN has the options opt. */
static size_t mss_with(const uint8_t *opt, size_t optlen,
                       const uint8_t *syn_data, size_t syn_len)
{
	static uint8_t data[2000];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {.syn_data = syn_data, .syn_len = syn_len};
	Got g;

	connect_opt(&c, n, GATEWAY, port, 40009, lfd, opt, optlen);
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	g = parse_tcp(wait_frame(n), GATEWAY, port, 40009);
	CHECK_EQ(g.flags, ACK);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
	return g.len;
}

static size_t mss_of(const uint8_t *opt, size_t optlen)
{
	return mss_with(opt, optlen, none, 0);
}

static void test_mss(void)
{
	static const uint8_t nop_mss[8] = {1, 1, 2, 4, 0x01, 0x00, 0, 0};
	static const uint8_t small[4] = {2, 4, 0, 30};
	static const uint8_t large[4] = {2, 4, 0x23, 0x28};
	static const uint8_t at_64[4] = {2, 4, 0, 64};
	static const uint8_t ended[8] = {0, 0, 0, 0, 2, 4, 0x01, 0x00};
	static const uint8_t zero_len[8] = {8, 0, 0, 0, 2, 4, 0x01, 0x00};
	static const uint8_t one_len[8] = {8, 1, 0, 0, 2, 4, 0x01, 0x00};
	static const uint8_t past[4] = {3, 5, 0, 0};
	static const uint8_t cut[4] = {1, 1, 1, 2};
	static const uint8_t exact[8] = {1, 1, 1, 1, 2, 4, 0x01, 0x00};
	/* SACK permitted, timestamps, then the MSS. */
	static const uint8_t other[16] = {4, 2, 8, 10, 0, 0, 0,    0,
	                                  0, 0, 0, 0,  2, 4, 0x01, 0};
	static const uint8_t mss_3[8] = {2, 3, 0x01, 0, 0, 0, 0, 0};
	static const uint8_t len_1[8] = {2, 1, 1, 1, 2, 4, 0x01, 0x00};
	static const uint8_t kind_30[8] = {30, 4, 0, 0, 2, 4, 0x01, 0x00};
	/* The MSS option cut by the end of the options; data after them. */
	static const uint8_t short_mss[4] = {1, 1, 2, 4};
	static const uint8_t syn_data[2] = {0x01, 0x00};

	CHECK_EQ(mss_of(none, 0), 536);
	CHECK_EQ(mss_of(nop_mss, 8), 256);
	CHECK_EQ(mss_of(small, 4), 64);
	CHECK_EQ(mss_of(at_64, 4), 64);
	CHECK_EQ(mss_of(large, 4), 1460);
	CHECK_EQ(mss_of(mss_1460, 4), 1460);
	CHECK_EQ(mss_of(ended, 8), 536);
	CHECK_EQ(mss_of(zero_len, 8), 536);
	CHECK_EQ(mss_of(one_len, 8), 536);
	CHECK_EQ(mss_of(past, 4), 536);
	CHECK_EQ(mss_of(cut, 4), 536);
	CHECK_EQ(mss_of(exact, 8), 256);
	CHECK_EQ(mss_of(other, 16), 256);
	CHECK_EQ(mss_of(mss_3, 8), 536);
	CHECK_EQ(mss_of(len_1, 8), 536);
	CHECK_EQ(mss_of(kind_30, 8), 256);
	CHECK_EQ(mss_with(short_mss, 4, syn_data, 2), 536);
}

/*
 * What the host sent comes to the guest, in segments of up to MSS, the
 * last with PSH.
 */
static size_t guest_takes(Conn *c, const uint8_t *data, size_t len)
{
	size_t have = 0;

	while (have < len) {
		Got g = parse_tcp(wait_frame(c->n), c->dst, c->dport, c->gport);
		size_t part = len - have < MSS ? len - have : MSS;

		CHECK_EQ(g.seq, c->ack);
		CHECK_EQ(g.flags, ACK | (have + g.len == len ? PSH : 0));
		CHECK_EQ(g.len, part);
		CHECK(memcmp(g.data, data + have, g.len) == 0);
		have += g.len;
		c->ack += (uint32_t)g.len;
	}
	return have;
}

static void test_window(void)
{
	static uint8_t data[3000], more[6000];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {.wnd = 1000};
	Got g;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 7);
	connect_out(&c, n, port, 40010, lfd);
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	g = expect(&c, ACK, 1000);
	CHECK(memcmp(g.data, data, 1000) == 0);
	quiet(n);
	/* A shut window: a probe of one byte at each timeout. */
	c.wnd = 0;
	gsend(&c, ACK, none, 0);
	quiet(n);
	now += SECOND;
	g = expect(&c, ACK, 1);
	CHECK_EQ(g.data[0], data[1000]);
	c.ack--;
	send_at(&c, c.seq, c.ack, ACK, none, 0); /* not taken */
	now += SECOND;
	quiet(n);
	now += SECOND;
	g = expect(&c, ACK, 1);
	/* The window opens, with the probe taken. */
	c.wnd = 2000;
	gsend(&c, ACK, none, 0);
	g = expect(&c, ACK, 1460);
	CHECK(memcmp(g.data, data + 1001, 1460) == 0);
	g = expect(&c, ACK | PSH, 539);
	CHECK(memcmp(g.data, data + 2461, 539) == 0);
	gsend(&c, ACK, none, 0);
	quiet(n);
	/* One byte, a shut window: probes, each answered, with no end. */
	c.wnd = 0;
	gsend(&c, ACK, none, 0);
	CHECK_EQ(send(c.host, "p", 1, 0), 1);
	quiet(n);
	for (int i = 0; i < 12; i++) {
		now += 60 * SECOND;
		g = expect(&c, ACK, 1);
		CHECK_EQ(g.data[0], 'p');
		if (i < 11)
			c.ack--;
		else
			c.wnd = 3000; /* the last taken, the window open */
		send_at(&c, c.seq, c.ack, ACK, none, 0);
	}
	/* A window not a multiple of the segment: its rest, last. */
	CHECK_EQ(send(c.host, more, sizeof(more), 0), sizeof(more));
	expect(&c, ACK, 1460);
	expect(&c, ACK, 1460);
	expect(&c, ACK, 80);
	quiet(n);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_window_update(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {.wnd = 100};
	Got g;

	connect_out(&c, n, port, 40011, lfd);
	gsend(&c, ACK, "abc", 3);
	expect(&c, ACK, 0);
	gsend(&c, ACK, none, 0); /* SND.WL1 is after "abc" */
	/* Older than SND.WL1: its window of 0 is not taken. */
	c.wnd = 0;
	send_at(&c, c.seq - 3, c.ack, ACK, "abcd", 4);
	c.seq++;
	expect(&c, ACK, 0);
	host_reads(c.host, "abcd");
	CHECK_EQ(send(c.host, "xyz", 3, 0), 3);
	g = expect(&c, ACK | PSH, 3);
	CHECK(memcmp(g.data, "xyz", 3) == 0);
	/* An old ACK: its window is not taken either. */
	c.wnd = 100;
	gsend(&c, ACK, none, 0);
	c.wnd = 0;
	send_at(&c, c.seq, c.ack - 3, ACK, none, 0);
	CHECK_EQ(send(c.host, "q", 1, 0), 1);
	expect(&c, ACK | PSH, 1);
	/* An ACK of what was not sent: answered, dropped, its data too. */
	send_at(&c, c.seq, c.ack + 1, ACK, "no", 2);
	expect(&c, ACK, 0);
	/* At SND.WL1, a new ACK: its window of 0 shuts. */
	gsend(&c, ACK, none, 0);
	CHECK_EQ(send(c.host, "r", 1, 0), 1);
	quiet(n);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_retransmit(void)
{
	static const uint64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60};
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	Got g;

	connect_out(&c, n, port, 40012, lfd);
	CHECK_EQ(send(c.host, "abcdef", 6, 0), 6);
	expect(&c, ACK | PSH, 6);
	/* The guest takes 2 bytes: the timer starts again, at 1 s. */
	now += SECOND / 2;
	send_at(&c, c.seq, c.ack - 4, ACK, none, 0);
	now += SECOND - 1;
	quiet(n);
	now += 1;
	c.ack -= 4;
	g = expect(&c, ACK | PSH, 4);
	CHECK(memcmp(g.data, "cdef", 4) == 0);
	/* Silent guest: doubled each time, then the end. */
	for (size_t i = 1; i < sizeof(waits) / sizeof(waits[0]); i++) {
		now += waits[i] * SECOND - 1;
		quiet(n);
		now += 1;
		c.ack -= 4;
		expect(&c, ACK | PSH, 4);
	}
	now += 60 * SECOND - 1;
	quiet(n);
	now += 1;
	g = parse_tcp(wait_frame(n), GATEWAY, port, 40012);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	host_ends(c.host, ECONNRESET);
	for (int i = 0; i < 20; i++) {
		now += 60 * SECOND;
		CHECK_EQ(take(n), 0);
	}
	close(c.host);
	close(lfd);
	net_destroy(n);
}

static void test_go_back(void)
{
	static uint8_t data[3 * MSS];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	uint32_t start;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 13);
	connect_out(&c, n, port, 40013, lfd);
	start = c.ack;
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	guest_takes(&c, data, sizeof(data));
	/* The first is lost: all three again, from SND.UNA. */
	now += SECOND;
	c.ack = start;
	guest_takes(&c, data, sizeof(data));
	/* An ACK of the third, sent before the timeout: no more again. */
	gsend(&c, ACK, none, 0);
	quiet(n);
	now += 10 * SECOND;
	quiet(n);
	/* The host's FIN after a timeout: from SND.UNA, then the FIN. */
	CHECK_EQ(send(c.host, "z", 1, 0), 1);
	expect(&c, ACK | PSH, 1);
	shutdown(c.host, SHUT_WR);
	expect(&c, FIN | ACK, 0);
	now += 11 * SECOND;
	c.ack -= 2;
	expect(&c, ACK | PSH, 1);
	expect(&c, FIN | ACK, 0);
	/* Only the data acknowledged: the FIN again, alone. */
	send_at(&c, c.seq, c.ack - 1, ACK, none, 0);
	now += SECOND;
	c.ack--;
	expect(&c, FIN | ACK, 0);
	/* An ACK of all, sent before: FIN-WAIT-2, nothing again. */
	gsend(&c, ACK, none, 0);
	now += 100 * SECOND;
	quiet(n);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_guest_data(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};

	uint16_t wnd;

	connect_out(&c, n, port, 40014, lfd);
	/* After a hole: dropped, the ACK again. */
	send_at(&c, c.seq + 10, c.ack, ACK, "late", 4);
	expect(&c, ACK, 0);
	/* Overlapping what came: the new part only. */
	gsend(&c, ACK, "0123456789", 10);
	expect(&c, ACK, 0);
	send_at(&c, c.seq - 3, c.ack, ACK, "789XYZ", 6);
	c.seq += 3;
	wnd = expect(&c, ACK, 0).wnd;
	host_reads(c.host, "0123456789XYZ");
	/* All old: out of the window, answered. */
	send_at(&c, c.seq - 5, c.ack, ACK, "abc", 3);
	expect(&c, ACK, 0);
	/* With no ACK bit: dropped, no answer. */
	send_at(&c, c.seq, c.ack, PSH, "nope", 4);
	quiet(n);
	/* Far out of the window: answered; a RST there is not. */
	send_at(&c, c.seq + 70000, c.ack, ACK, "far", 3);
	expect(&c, ACK, 0);
	send_at(&c, c.seq + 70000, c.ack, RST, none, 0);
	quiet(n);
	/* A pure ACK just past the window is out of it. */
	CHECK_EQ(wnd, 65535 - 13);
	send_at(&c, c.seq + wnd, c.ack, ACK, none, 0);
	expect(&c, ACK, 0);
	/* The last byte of the window is in it. */
	send_at(&c, c.seq + wnd - 1, c.ack, ACK, none, 0);
	quiet(n);
	gsend(&c, ACK, "!", 1);
	expect(&c, ACK, 0);
	host_reads(c.host, "!");
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

/* The guest fills the router's buffer while the host's end reads none. */
static void test_flow(void)
{
	static uint8_t data[1 << 24]; /* more than the host's buffers take */
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	size_t sent = 0, back = 0;
	uint16_t wnd = 65535;
	int small = 2048;
	Got g;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31 + i / 256);
	CHECK(setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
	connect_out(&c, n, port, 40015, lfd);
	while (wnd > 0 && sent < sizeof(data)) {
		size_t part = sizeof(data) - sent;

		if (part > MSS)
			part = MSS;
		if (part > wnd)
			part = wnd;
		gsend(&c, ACK, data + sent, part);
		sent += part;
		g = expect(&c, ACK, 0);
		/* The right edge stays, or moves on by a segment at least. */
		CHECK(g.wnd == wnd - part || g.wnd >= wnd - part + MSS);
		wnd = g.wnd;
	}
	CHECK_EQ(wnd, 0);
	CHECK(sent < sizeof(data));
	/* A byte in a shut window: not taken. */
	send_at(&c, c.seq, c.ack, ACK, data + sent, 1);
	g = expect(&c, ACK, 0);
	CHECK_EQ(g.wnd, 0);
	/* The host reads: window updates, up to the whole buffer less a segment. */
	for (int i = 0; (back < sent || wnd + MSS <= 65535) && i < 100000; i++) {
		uint8_t buf[4096];
		size_t len = take(n);
		ssize_t r = recv(c.host, buf, sizeof(buf), MSG_DONTWAIT);

		if (len != 0) {
			g = parse_tcp(len, GATEWAY, port, 40015);
			CHECK_EQ(g.flags, ACK);
			CHECK(g.wnd >= wnd + MSS);
			wnd = g.wnd;
		}
		if (r > 0) {
			CHECK(memcmp(buf, data + back, (size_t)r) == 0);
			back += (size_t)r;
		} else if (len == 0) {
			usleep(100);
		}
	}
	CHECK_EQ(back, sent);
	CHECK(wnd + MSS > 65535);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_challenge(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	Got g;

	connect_out(&c, n, port, 40016, lfd);
	/* A SYN: a challenge ACK (RFC 5961 section 4), and nothing else. */
	send_at(&c, c.seq, c.ack, SYN, none, 0);
	expect(&c, ACK, 0);
	send_at(&c, c.seq, c.ack + 5, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	quiet(n);
	/* A RST in the window, not at RCV.NXT: a challenge ACK too. */
	send_at(&c, c.seq + 1, c.ack, RST, none, 0);
	expect(&c, ACK, 0);
	send_at(&c, c.seq + 1, c.ack + 5, RST | ACK, none, 0);
	expect(&c, ACK, 0);
	quiet(n);
	gsend(&c, ACK, "ok", 2);
	expect(&c, ACK, 0);
	host_reads(c.host, "ok");
	/* At RCV.NXT: the end, and the host's end is reset; nothing else. */
	send_at(&c, c.seq, c.ack + 5, RST | ACK, none, 0);
	quiet(n);
	host_ends(c.host, ECONNRESET);
	send_at(&c, c.seq, c.ack, ACK, none, 0);
	g = refused(n, GATEWAY, port, 40016);
	CHECK_EQ(g.flags, RST);
	close(c.host);
	close(lfd);
	net_destroy(n);
}

static void abort_host(int fd)
{
	struct linger l = {.l_onoff = 1, .l_linger = 0};

	CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) == 0);
	close(fd);
}

static void test_host_reset(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	Got g;

	/* Found reading. */
	connect_out(&c, n, port, 40017, lfd);
	abort_host(c.host);
	g = parse_tcp(wait_frame(n), GATEWAY, port, 40017);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	send_at(&c, c.seq, c.ack, ACK, none, 0);
	CHECK_EQ(refused(n, GATEWAY, port, 40017).flags, RST);

	/* Found writing what the guest sent. */
	connect_out(&c, n, port, 40018, lfd);
	abort_host(c.host);
	usleep(1000);
	gsend(&c, ACK, "data", 4);
	g = parse_tcp(take1(n), GATEWAY, port, 40018);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	close(lfd);
	net_destroy(n);
}

static void test_simultaneous(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};

	connect_out(&c, n, port, 40019, lfd);
	shutdown(c.host, SHUT_WR);
	expect(&c, FIN | ACK, 0);
	/* The guest's FIN crosses the router's: CLOSING. */
	send_at(&c, c.seq, c.ack - 1, ACK | FIN, none, 0);
	c.seq++;
	expect(&c, ACK, 0);
	host_ends(c.host, 0);
	/* The router's FIN again at the timeout; its ACK: TIME-WAIT. */
	now += SECOND;
	c.ack--;
	expect(&c, FIN | ACK, 0);
	gsend(&c, ACK, none, 0);
	quiet(n);
	now += 60 * SECOND - 1;
	send_at(&c, c.seq - 1, c.ack, ACK | FIN, none, 0);
	expect(&c, ACK, 0);
	now += 1;
	CHECK_EQ(take(n), 0);
	send_at(&c, c.seq - 1, c.ack, ACK | FIN, none, 0);
	CHECK_EQ(refused(n, GATEWAY, port, 40019).flags, RST);
	close(c.host);
	close(lfd);
	net_destroy(n);
}

/* The guest's SYN from gport to the host's port; the SYN-ACK. */
static Got syn(Conn *c, NetLink *n, uint16_t port, uint16_t gport)
{
	Got g;

	*c = (Conn){.n = n,
	            .dst = GATEWAY,
	            .dport = port,
	            .gport = gport,
	            .seq = 500,
	            .wnd = 65535};
	gsend(c, SYN, none, 0);
	g = expect(c, SYN | ACK, 0);
	CHECK_EQ(g.seq, (uint32_t)(now / 4000));
	return g;
}

static void test_syn_received(void)
{
	static const uint64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60};
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 8);
	uint32_t iss;
	Conn c;
	Got g;

	iss = syn(&c, n, port, 40020).seq;
	/* The SYN again: the SYN-ACK again. */
	send_at(&c, 500, 0, SYN, none, 0);
	CHECK_EQ(expect(&c, SYN | ACK, 0).seq, iss);
	/* An ACK of something else: <SEQ=SEG.ACK><CTL=RST>; its data not taken. */
	send_at(&c, c.seq, c.ack + 5, ACK, "zz", 2);
	g = parse_tcp(take1(n), GATEWAY, port, 40020);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack + 5);
	/* A RST at RCV.NXT: the end. */
	send_at(&c, c.seq, 0, RST, none, 0);
	quiet(n);
	send_at(&c, c.seq, c.ack, ACK, none, 0);
	CHECK_EQ(refused(n, GATEWAY, port, 40020).flags, RST);

	/* No ACK: the SYN-ACK again at each timeout, then the end. */
	syn(&c, n, port, 40021);
	for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
		now += waits[i] * SECOND - 1;
		quiet(n);
		now += 1;
		expect(&c, SYN | ACK, 0);
	}
	now += 60 * SECOND;
	g = parse_tcp(take1(n), GATEWAY, port, 40021);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	close(lfd);
	net_destroy(n);
}

static void test_connecting(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 0);
	struct sockaddr_in a = {.sin_family = AF_INET,
	                        .sin_port = 0,
	                        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int fill[3];
	Conn c = {.n = n, .dst = GATEWAY, .gport = 40022, .seq = 70, .wnd = 9};
	Got g;

	/* A full accept queue: the host's connect() waits. */
	a.sin_port = htons(port);
	for (int i = 0; i < 3; i++) {
		fill[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		connect(fill[i], (struct sockaddr *)&a, sizeof(a));
	}
	usleep(2000);
	c.dport = port;
	gsend(&c, SYN, none, 0);
	quiet(n);
	/* The SYN again, or an ACK, waits too. */
	send_at(&c, 69, 0, SYN, none, 0);
	send_at(&c, 70, 1, ACK, none, 0);
	quiet(n);
	/* A RST ends it: the next is for no connection. */
	send_at(&c, 70, 0, RST, none, 0);
	send_at(&c, 70, 1, ACK, none, 0);
	g = refused(n, GATEWAY, port, 40022);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, 1);
	for (int i = 0; i < 3; i++)
		close(fill[i]);
	close(lfd);
	net_destroy(n);
}

/* The guest says who it is: an ARP request for the router, answered. */
static void guest_speaks(NetLink *n)
{
	uint8_t *arp = frame + 14;

	memcpy(frame, bcast_mac, 6);
	memcpy(frame + 6, guest_mac, 6);
	put16(frame + 12, 0x0806);
	put16(arp, 1);
	put16(arp + 2, 0x0800);
	arp[4] = 6;
	arp[5] = 4;
	put16(arp + 6, 1);
	memcpy(arp + 8, guest_mac, 6);
	put32(arp + 14, GUEST);
	memset(arp + 18, 0, 6);
	put32(arp + 24, GATEWAY);
	send_frame(n, 42);
	CHECK_EQ(take1(n), 42);
}

/* A client of the host's, connected to the forward on port. */
static int client(uint16_t port)
{
	struct sockaddr_in a = {.sin_family = AF_INET,
	                        .sin_port = htons(port),
	                        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int fd = high(socket(AF_INET, SOCK_STREAM, 0));

	CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	return fd;
}

static NetLink *open_forward(uint16_t *port, uint16_t guest_port)
{
	NetUserConfig config = {0};

	*port = closed_port();
	config.forwards = 1;
	config.forward[0] =
		(NetUserForward){INADDR_LOOPBACK, *port, GUEST, guest_port};
	return open_config(&config);
}

/* The forward's SYN to the guest's port, from the router's port. */
static void forward_syn(Conn *c, NetLink *n, uint16_t port, uint16_t gport)
{
	Got g;

	*c = (Conn){.n = n,
	            .dst = GATEWAY,
	            .dport = port,
	            .gport = gport,
	            .seq = 9000,
	            .wnd = 65535};
	g = expect(c, SYN, 0);
	CHECK_EQ(g.ack, 0);
	CHECK_EQ(g.wnd, 65535);
}

static void test_forward(void)
{
	static const uint8_t mss_1000[4] = {2, 4, 0x03, 0xe8};
	static uint8_t data[3000];
	uint16_t port;
	NetLink *n = open_forward(&port, 22);
	int host = client(port), host2, host3;
	const uint8_t *arp = got + 14;
	size_t len;
	Conn c;

	/* The guest has not spoken: the router asks who has 10.0.2.15. */
	len = wait_frame(n);
	CHECK_EQ(len, 42);
	CHECK(memcmp(got, bcast_mac, 6) == 0);
	CHECK(memcmp(got + 6, router_mac, 6) == 0);
	CHECK_EQ(get16(got + 12), 0x0806);
	CHECK_EQ(get16(arp), 1);
	CHECK_EQ(get16(arp + 2), 0x0800);
	CHECK_EQ(arp[4], 6);
	CHECK_EQ(arp[5], 4);
	CHECK_EQ(get16(arp + 6), 1);
	CHECK(memcmp(arp + 8, router_mac, 6) == 0);
	CHECK_EQ(get32(arp + 14), GATEWAY);
	for (int i = 18; i < 24; i++)
		CHECK_EQ(arp[i], 0);
	CHECK_EQ(get32(arp + 24), GUEST);
	CHECK_EQ(take(n), 0);
	/* The guest's answer, as Linux sends it; the SYN at the timeout. */
	memcpy(frame, router_mac, 6);
	memcpy(frame + 6, guest_mac, 6);
	put16(frame + 12, 0x0806);
	memcpy(frame + 14, arp, 6);
	put16(frame + 20, 2);
	memcpy(frame + 22, guest_mac, 6);
	put32(frame + 28, GUEST);
	memcpy(frame + 32, router_mac, 6);
	put32(frame + 38, GATEWAY);
	send_frame(n, 42);
	quiet(n);
	now += SECOND;
	forward_syn(&c, n, FIRST_PORT, 22);
	c.seq = 999; /* RCV.NXT 1000: the right edge moves on all the same */
	send_frame(n, tcp_frame(GUEST, 22, GATEWAY, FIRST_PORT, c.seq, c.ack,
	                        SYN | ACK, 65535, mss_1000, 4, none, 0));
	c.seq++;
	CHECK_EQ(expect(&c, ACK, 0).wnd, 65535);
	quiet(n);
	/* Both ways, with the guest's MSS. */
	CHECK_EQ(send(host, data, sizeof(data), 0), sizeof(data));
	expect(&c, ACK, 1000);
	expect(&c, ACK, 1000);
	expect(&c, ACK | PSH, 1000);
	gsend(&c, ACK | PSH, "SSH-2.0", 7);
	expect(&c, ACK, 0);
	host_reads(host, "SSH-2.0");
	/* The next connection has the router's next port. */
	host2 = client(port);
	forward_syn(&c, n, FIRST_PORT + 1, 22);
	/* The guest's port is closed: RST, and the host's client is reset. */
	send_at(&c, 0, c.ack, RST | ACK, none, 0);
	quiet(n);
	host_ends(host2, ECONNRESET);
	close(host2);
	/* The guest's ISN past 2^31: its later window of 0 is taken. */
	host3 = client(port);
	forward_syn(&c, n, FIRST_PORT + 2, 22);
	c.seq = 0x90000000u;
	gsend(&c, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	c.wnd = 0;
	gsend(&c, ACK, none, 0);
	CHECK_EQ(send(host3, "z", 1, 0), 1);
	quiet(n);
	send_at(&c, c.seq, c.ack, RST, none, 0);
	close(host3);
	close(host);
	net_destroy(n);
}

static void test_forward_syn_sent(void)
{
	static const uint64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60};
	uint16_t port;
	NetLink *n = open_forward(&port, 80);
	int host;
	uint32_t iss;
	Conn c;
	Got g;

	guest_speaks(n);
	host = client(port);
	forward_syn(&c, n, FIRST_PORT, 80);
	iss = c.ack - 1;
	/* An ACK of something else: <SEQ=SEG.ACK><CTL=RST>. */
	send_at(&c, 1, c.ack + 1, ACK, none, 0);
	g = parse_tcp(take1(n), GATEWAY, FIRST_PORT, 80);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack + 1);
	CHECK_EQ(g.ack, 0);
	/* With RST too, or a RST with no ACK: dropped. */
	send_at(&c, 1, c.ack + 1, RST | ACK, none, 0);
	send_at(&c, 1, 0, RST, none, 0);
	/*
	 * A SYN alone, or a segment with none of SYN, RST, ACK; an ACK alone,
	 * acceptable; a SYN with the right number but no ACK bit.
	 */
	send_at(&c, 1, 0, SYN, none, 0);
	send_at(&c, 1, 0, PSH, none, 0);
	send_at(&c, 1, c.ack, ACK, none, 0);
	send_at(&c, 1, c.ack, SYN, none, 0);
	quiet(n);
	/* The SYN again at each timeout, then the end. */
	for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
		now += waits[i] * SECOND - 1;
		quiet(n);
		now += 1;
		CHECK_EQ(expect(&c, SYN, 0).seq, iss);
	}
	now += 60 * SECOND;
	g = parse_tcp(take1(n), GATEWAY, FIRST_PORT, 80);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	host_ends(host, ECONNRESET);
	close(host);
	net_destroy(n);
}

static void test_forward_ports(void)
{
	uint16_t port;
	NetLink *n = open_forward(&port, 7);
	int keep;
	Conn c;

	guest_speaks(n);
	keep = client(port);
	forward_syn(&c, n, FIRST_PORT, 7);
	gsend(&c, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	/* Every other port once, each connection refused by the guest. */
	for (unsigned p = FIRST_PORT + 1, failures = test_failures; p <= 65535;
	     p++) {
		int fd = client(port);

		forward_syn(&c, n, (uint16_t)p, 7);
		send_at(&c, 0, c.ack, RST | ACK, none, 0);
		close(fd);
		if (test_failures != (int)failures)
			break;
	}
	/* Then the first again, but in use: the one after it. */
	close(client(port));
	forward_syn(&c, n, FIRST_PORT + 1, 7);
	close(keep);
	net_destroy(n);
}

static void test_slots(void)
{
	uint16_t fport;
	NetLink *n = open_forward(&fport, 22);
	uint16_t port;
	int lfd = listener(&port, 128);
	Conn c[CONNS + 2];
	int host[2], fd, fds, small = 2048;

	CHECK(setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
	for (unsigned i = 0; i < CONNS; i++) {
		syn(&c[i], n, port, (uint16_t)(41000 + i));
		gsend(&c[i], ACK, none, 0);
	}
	for (int i = 0; i < 2; i++)
		host[i] = host_accept(lfd);
	/* All taken: a SYN is lost. */
	c[CONNS] = (Conn){.n = n,
	                  .dst = GATEWAY,
	                  .dport = port,
	                  .gport = 42000,
	                  .seq = 1,
	                  .wnd = 100};
	gsend(&c[CONNS], SYN, none, 0);
	quiet(n);
	/* And a forward's: the host's client is reset. */
	fd = client(fport);
	quiet(n);
	host_ends(fd, ECONNRESET);
	close(fd);
	/*
	 * The first two in TIME-WAIT, the first to end first; the first with
	 * a byte that its host's socket, full, has not taken.
	 */
	jam(host[0]);
	for (int i = 0; i < 2; i++) {
		shutdown(host[i], SHUT_WR);
		expect(&c[i], FIN | ACK, 0);
		gsend(&c[i], ACK | FIN, "x", i == 0);
		expect(&c[i], ACK, 0);
		now += SECOND;
	}
	/* One closes: its slot, free, goes before a TIME-WAIT's. */
	gsend(&c[5], RST, none, 0);
	syn(&c[CONNS], n, port, 42001);
	gsend(&c[CONNS], ACK, none, 0);
	send_at(&c[0], c[0].seq - 1, c[0].ack, ACK | FIN, none, 0);
	expect(&c[0], ACK, 0);
	/* Then the TIME-WAIT that ends first, its socket closed. */
	fds = open_fds();
	syn(&c[CONNS + 1], n, port, 42002);
	CHECK_EQ(open_fds(), fds);
	/* Nothing of the old there: the data of the ACK is taken. */
	gsend(&c[CONNS + 1], ACK, "hi", 2);
	expect(&c[CONNS + 1], ACK, 0);
	send_at(&c[0], c[0].seq - 1, c[0].ack, ACK | FIN, none, 0);
	CHECK_EQ(refused(n, GATEWAY, port, 41000).flags, RST);
	send_at(&c[1], c[1].seq - 1, c[1].ack, ACK | FIN, none, 0);
	expect(&c[1], ACK, 0);
	/* The other TIME-WAIT, in the second slot, ends too. */
	now += 60 * SECOND;
	CHECK_EQ(take(n), 0);
	send_at(&c[1], c[1].seq - 1, c[1].ack, ACK | FIN, none, 0);
	CHECK_EQ(refused(n, GATEWAY, port, 41001).flags, RST);
	for (int i = 0; i < 2; i++)
		close(host[i]);
	close(lfd);
	net_destroy(n);
}

/* An echo request from the guest to the router, in frame. */
static size_t ping_frame(void)
{
	uint8_t icmp[8] = {8, 0, 0, 0, 0, 1, 0, 1};

	put16(icmp + 2, ~sum(0, icmp, 8));
	return ip_frame(frame, router_mac, 1, GUEST, GATEWAY, 64, icmp, 8);
}

static void ping(NetLink *n)
{
	send_frame(n, ping_frame());
}

/* The guest's pings fill the queue to the guest but for left places. */
static void fill_queue(NetLink *n, int left)
{
	for (int i = 0; i < 160 - left; i++)
		ping(n);
}

/* The pings' replies the guest takes, up to another frame, in len. */
static unsigned replies(NetLink *n, size_t *len)
{
	unsigned count = 0;

	while ((*len = take(n)) != 0 && got[14 + 9] == 1)
		count++;
	return count;
}

static void test_queue_room(void)
{
	static uint8_t data[MSS];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	size_t len;
	Got g;

	connect_out(&c, n, port, 40023, lfd);
	/* 16 places left at the poll: the data waits, as 17 pings come. */
	fill_queue(n, 16);
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	usleep(2000);
	take(n);
	for (int i = 0; i < 17; i++)
		ping(n);
	CHECK_EQ(1 + replies(n, &len), 161);
	g = parse_tcp(len, GATEWAY, port, 40023);
	CHECK_EQ(g.len, MSS);
	c.ack = g.seq + MSS;
	gsend(&c, ACK, none, 0);
	/* 17 left at the poll: it goes, before the pings after it. */
	fill_queue(n, 16);
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	usleep(2000);
	take(n);
	take(n);
	for (int i = 0; i < 16; i++)
		ping(n);
	CHECK_EQ(2 + replies(n, &len), 144);
	g = parse_tcp(len, GATEWAY, port, 40023);
	CHECK_EQ(g.len, MSS);
	CHECK_EQ(replies(n, &len), 16);
	CHECK_EQ(len, 0);
	c.ack = g.seq + MSS;
	gsend(&c, ACK, none, 0);
	/* The FIN's timeout, 16 places left: the FIN again once there is room. */
	shutdown(c.host, SHUT_WR);
	expect(&c, FIN | ACK, 0);
	fill_queue(n, 16);
	now += SECOND;
	CHECK_EQ(replies(n, &len), 144);
	g = parse_tcp(len, GATEWAY, port, 40023);
	CHECK_EQ(g.flags, FIN | ACK);
	CHECK_EQ(g.len, 0);
	CHECK_EQ(g.seq, c.ack - 1);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_ack_after_rewind(void)
{
	static uint8_t data[3 * MSS];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	uint32_t start;
	Got g;

	connect_out(&c, n, port, 40024, lfd);
	start = c.ack;
	CHECK_EQ(send(c.host, data, sizeof(data), 0), sizeof(data));
	guest_takes(&c, data, sizeof(data));
	/* The guest's window shrinks to a segment, nothing acknowledged. */
	c.wnd = MSS;
	send_at(&c, c.seq, start, ACK, none, 0);
	quiet(n);
	/* At the timeout, one segment again, from SND.UNA. */
	now += SECOND;
	c.ack = start;
	expect(&c, ACK, MSS);
	/* All three were taken after all: new data goes after them. */
	c.ack = start + 3 * MSS;
	c.wnd = 65535;
	gsend(&c, ACK, none, 0);
	quiet(n);
	CHECK_EQ(send(c.host, "new", 3, 0), 3);
	g = expect(&c, ACK | PSH, 3);
	CHECK(memcmp(g.data, "new", 3) == 0);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

static void test_no_socket(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	int next = dup(lfd);
	struct rlimit old, low;
	Got g;

	/* No file descriptor left for the host's socket: the SYN is lost. */
	close(next);
	CHECK(getrlimit(RLIMIT_NOFILE, &old) == 0);
	low = old;
	low.rlim_cur = (rlim_t)next;
	CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
	send_frame(n, tcp_frame(GUEST, 40025, GATEWAY, port, 1, 0, SYN, 100, none,
	                        0, none, 0));
	CHECK(setrlimit(RLIMIT_NOFILE, &old) == 0);
	quiet(n);
	/* It is no connection: its next segment is refused. */
	send_frame(n, tcp_frame(GUEST, 40025, GATEWAY, port, 2, 5, ACK, 100, none,
	                        0, none, 0));
	g = refused(n, GATEWAY, port, 40025);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, 5);
	close(lfd);
	net_destroy(n);
}

static void test_arp_queue_full(void)
{
	uint16_t port;
	NetLink *n = open_forward(&port, 22);
	unsigned pings = 0;
	size_t len;
	int host;

	/* Pings from a group address fill the queue; it teaches nothing. */
	for (int i = 0; i < 160; i++) {
		len = ping_frame();
		frame[6] |= 1;
		send_frame(n, len);
	}
	/* The forward's ARP request finds no room: lost. */
	host = client(port);
	usleep(2000);
	while ((len = take(n)) != 0) {
		CHECK_EQ(get16(got + 12), 0x0800);
		pings++;
	}
	CHECK_EQ(pings, 160);
	/* Asked again at the timeout. */
	now += SECOND;
	len = take1(n);
	CHECK_EQ(len, 42);
	CHECK_EQ(get16(got + 12), 0x0806);
	close(host);
	net_destroy(n);
}

/* net_user_open(options) fails with the message error. */
static void check_option(const char *options, const char *error)
{
	char got_error[200] = "";

	CHECK(net_user_open(options, got_error, sizeof(got_error)) == NULL);
	if (strcmp(got_error, error) != 0)
		fprintf(stderr, "'%s': '%s'\n", options, got_error);
	CHECK(strcmp(got_error, error) == 0);
}

static void test_options(void)
{
	static const char bad[] = "user: bad hostfwd '%s', expected "
							  "[tcp]:[hostaddr]:hostport-[guestaddr]:guestport";
	static const char *const syntax[] = {
		"tcpx::1-:2", "tc::1-:2",
		":1-:2",      "tcp:1-:2",
		"tcp::0-:2",  "tcp::65536-:2",
		"tcp::1a-:2", "tcp::-:2",
		"tcp::1:2",   "tcp::1-2",
		"tcp::1-:0",  "tcp:1.2.3:1-:2",
		"",           "tcp",
		"tcp:",       "tcp:127.0.0.1.127.0.0.1:1-:2",
		"tcp::1-:/2", "udx::1-:2",
		"tcx::1-:2",  "tcp:111.111.111.1111:1-:2",
	};
	static const char *const not_guest[] = {
		"10.0.2.0:2", "10.0.2.2:2", "10.0.2.3:2", "10.0.2.255:2", "10.0.3.15:2",
	};
	char options[1024], error[300];
	uint16_t port = closed_port(), port2 = closed_port();
	NetLink *n;
	int fd;

	snprintf(options, sizeof(options), "hostfwd=tcp::%u-:65535", port);
	n = net_user_open(options, error, sizeof(error));
	CHECK(n != NULL);
	fd = client(port);
	close(fd);
	net_destroy(n);
	snprintf(options, sizeof(options),
	         "hostfwd=:127.0.0.1:%u-10.0.2.15:29,hostfwd=tcp::%u-10.0.2.1:80",
	         port, port2);
	n = net_user_open(options, error, sizeof(error));
	CHECK(n != NULL);
	close(client(port));
	close(client(port2));
	net_destroy(n);
	CHECK((n = net_user_open("", error, sizeof(error))) != NULL);
	net_destroy(n);

	check_option("foo", "user: unknown option 'foo'");
	check_option("hostfw", "user: unknown option 'hostfw'");
	check_option("hostfwd=tcp::1-:2,", "user: unknown option ''");
	snprintf(options, sizeof(options), "hostfwd=tcp::%u-:22,", port);
	check_option(options, "user: unknown option ''");
	check_option(",", "user: unknown option ''");
	check_option("hostfwd=udp::1-:2", "user: hostfwd of udp is not supported");
	for (size_t i = 0; i < sizeof(syntax) / sizeof(syntax[0]); i++) {
		snprintf(options, sizeof(options), "hostfwd=%s", syntax[i]);
		snprintf(error, sizeof(error), bad, syntax[i]);
		check_option(options, error);
	}
	for (size_t i = 0; i < sizeof(not_guest) / sizeof(not_guest[0]); i++) {
		snprintf(options, sizeof(options), "hostfwd=tcp::1-%s", not_guest[i]);
		snprintf(error, sizeof(error),
		         "user: hostfwd to %s: not an address of the guest's",
		         not_guest[i]);
		check_option(options, error);
	}
	check_option("hostfwd=tcp::1-10.0.2.2:2,hostfwd=tcp::1-:2",
	             "user: hostfwd to 10.0.2.2:2: not an address of the guest's");
	options[0] = '\0';
	for (int i = 0; i < 17; i++)
		strcat(options, i == 0 ? "hostfwd=tcp::1-:2" : ",hostfwd=tcp::1-:2");
	check_option(options, "user: more than 16 hostfwd");
	/* The host refuses: the port taken, an address not the host's. */
	snprintf(options, sizeof(options),
	         "hostfwd=tcp::%u-:22,hostfwd=tcp::%u-:23", port, port);
	snprintf(error, sizeof(error),
	         "user: hostfwd 127.0.0.1:%u: Address already in use", port);
	check_option(options, error);
	check_option("hostfwd=tcp:192.0.2.1:1-:2",
	             "user: hostfwd 192.0.2.1:1: Cannot assign requested address");
}

/* At the wrap of the router's numbers: the SYN-ACK again, then data. */
static void test_syn_ack_again(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c;

	now = WRAP; /* ISS 0xffffffff, its SND.NXT 0 */
	CHECK_EQ(syn(&c, n, port, 40032).seq, 0xffffffffu);
	/* No ACK: the SYN-ACK again at the timeout. */
	now += SECOND;
	c.ack--;
	expect(&c, SYN | ACK, 0);
	gsend(&c, ACK, none, 0);
	c.host = host_accept(lfd);
	CHECK_EQ(send(c.host, "abc", 3, 0), 3);
	CHECK_EQ(expect(&c, ACK | PSH, 3).seq, 0);
	/* Not acknowledged: again 1 s after, the SYN-ACK's backoff gone. */
	now += SECOND - 1;
	quiet(n);
	now += 1;
	c.ack -= 3;
	expect(&c, ACK | PSH, 3);
	guest_reset(&c);
	close(lfd);
	net_destroy(n);
}

/* The guest's RCV.NXT 65000 before the wrap: the whole window still. */
static void test_syn_window(void)
{
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {.n = n,
	          .dst = GATEWAY,
	          .dport = port,
	          .gport = 40037,
	          .seq = 0xffff0217u,
	          .wnd = 65535};

	gsend(&c, SYN, none, 0);
	CHECK_EQ(expect(&c, SYN | ACK, 0).wnd, 65535);
	send_at(&c, c.seq, 0, RST, none, 0);
	quiet(n);
	close(lfd);
	net_destroy(n);
}

/*
 * A forward's SYN sent again; after the guest's SYN-ACK, the timeouts of
 * the data start from 1 s, and count from none.
 */
static void test_forward_silent(void)
{
	static const uint64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60};
	uint16_t port;
	NetLink *n = open_forward(&port, 80);
	int host;
	Conn c;
	Got g;

	now = WRAP;
	guest_speaks(n);
	host = client(port);
	forward_syn(&c, n, FIRST_PORT, 80);
	CHECK_EQ(c.ack, 0); /* ISS 0xffffffff */
	now += SECOND;
	c.ack--;
	expect(&c, SYN, 0);
	gsend(&c, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	CHECK_EQ(send(host, "x", 1, 0), 1);
	expect(&c, ACK | PSH, 1);
	for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
		now += waits[i] * SECOND - 1;
		quiet(n);
		now += 1;
		c.ack--;
		expect(&c, ACK | PSH, 1);
	}
	now += 60 * SECOND;
	g = parse_tcp(take1(n), GATEWAY, FIRST_PORT, 80);
	CHECK_EQ(g.flags, RST);
	CHECK_EQ(g.seq, c.ack);
	host_ends(host, ECONNRESET);
	close(host);
	net_destroy(n);
}

/* A guest whose address is zeros but its last byte is known all the same. */
static void test_mac_zeros(void)
{
	static const uint8_t mac[6] = {0, 0, 0, 0, 0, 1};
	uint16_t port;
	NetLink *n = open_forward(&port, 22);
	size_t len = ping_frame();
	int host;

	memcpy(frame + 6, mac, 6);
	send_frame(n, len);
	CHECK(take(n) != 0);
	host = client(port);
	wait_frame(n);
	CHECK(memcmp(got, mac, 6) == 0);
	CHECK_EQ(get16(got + 12), 0x0800);
	CHECK_EQ(got[14 + 9], 6);
	close(host);
	net_destroy(n);
}

/*
 * The guest sends until the window is to, the host's socket taking
 * nothing: the right edge stays.
 */
static void shrink(Conn *c, uint16_t to)
{
	static uint8_t fill[1000];
	uint16_t wnd = 65535;

	memset(fill, 'f', sizeof(fill));
	while (wnd > to) {
		size_t part = wnd - to < 1000 ? wnd - to : 1000;

		gsend(c, ACK, fill, part);
		wnd = expect(c, ACK, 0).wnd;
		CHECK(wnd < 65535);
	}
	CHECK_EQ(wnd, to);
}

/* Segments on the right edge of the window, with a FIN or not. */
static void test_edge(void)
{
	static uint8_t data[1100];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	size_t junk;
	Conn c = {0}, d = {0};
	uint32_t nxt;

	memset(data, 'e', sizeof(data));
	connect_out(&c, n, port, 40033, lfd);
	CHECK_EQ(router_end(c.host), 0);
	junk = jam(c.host);
	shrink(&c, 1000);
	nxt = c.seq;
	/* From before RCV.NXT to one past the edge: out of the window. */
	send_at(&c, nxt - 5, c.ack, ACK, data, 1006);
	expect(&c, ACK, 0);
	/* Its last byte in, its FIN one past: out of it too. */
	send_at(&c, nxt - 5, c.ack, ACK | FIN, data, 1005);
	expect(&c, ACK, 0);
	/* Data and a FIN that just fit: all taken; CLOSE-WAIT. */
	send_at(&c, nxt - 5, c.ack, ACK | FIN, data, 1004);
	c.seq = nxt + 1000;
	CHECK_EQ(expect(&c, ACK, 0).wnd, 0);
	/*
	 * LAST-ACK, then CLOSED, the socket kept for what waits: a new
	 * connection leaves it alone.
	 */
	shutdown(c.host, SHUT_WR);
	expect(&c, FIN | ACK, 0);
	gsend(&c, ACK, none, 0);
	quiet(n);
	connect_out(&d, n, port, 40034, lfd);
	host_drains(&c, NULL, junk + 65535 - 1000);
	host_drains(&c, data, 999);
	host_ends(c.host, 0);
	close(c.host);

	/* From before RCV.NXT to the edge exactly: in the window. */
	junk = jam(d.host);
	shrink(&d, 1000);
	nxt = d.seq;
	send_at(&d, nxt - 5, d.ack, ACK, data, 1005);
	d.seq = nxt + 1000;
	CHECK_EQ(expect(&d, ACK, 0).wnd, 0);
	/* Data and a FIN in a shut window: none taken, the FIN neither. */
	send_at(&d, d.seq, d.ack, ACK | FIN, data, 1);
	CHECK_EQ(expect(&d, ACK, 0).wnd, 0);
	guest_reset(&d);
	close(lfd);
	net_destroy(n);
}

/* A byte for a forward's full socket: it goes when there is room. */
static void test_one_byte(void)
{
	int hold = dup(2); /* descriptor 0: not for the forward's socket */
	uint16_t port;
	NetLink *n = open_forward(&port, 22);
	size_t junk;
	int host;
	Conn c;

	CHECK_EQ(hold, 0);
	close(hold);
	guest_speaks(n);
	host = client(port);
	forward_syn(&c, n, FIRST_PORT, 22);
	c.host = host;
	gsend(&c, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	CHECK_EQ(router_end(c.host), 0); /* the host's socket, accepted */
	junk = jam(c.host);
	gsend(&c, ACK, "1", 1);
	expect(&c, ACK, 0);
	host_drains(&c, NULL, junk);
	host_drains(&c, (const uint8_t *)"1", 1);
	guest_reset(&c);
	net_destroy(n);
}

/* Both ends send at once, each segment acknowledged with data. */
static void test_both_ways(void)
{
	static uint8_t down[300000], up[15000];
	NetLink *n = open_tcp();
	uint16_t port;
	int lfd = listener(&port, 4);
	Conn c = {0};
	size_t put = 0, down_got = 0, up_sent = 0, up_got = 0;
	bool fin = false;

	for (size_t i = 0; i < sizeof(down); i++)
		down[i] = (uint8_t)(i * 17 + i / 509);
	for (size_t i = 0; i < sizeof(up); i++)
		up[i] = (uint8_t)(i * 5 + i / 251);
	connect_out(&c, n, port, 40035, lfd);
	for (int i = 0; i < 400000 && !fin; i++) {
		uint8_t buf[4096];
		size_t len, part;
		ssize_t r;
		Got g;

		if (put < sizeof(down)) {
			r = send(c.host, down + put, sizeof(down) - put, MSG_DONTWAIT);
			if (r > 0 && (put += (size_t)r) == sizeof(down))
				shutdown(c.host, SHUT_WR);
		}
		r = recv(c.host, buf, sizeof(buf), MSG_DONTWAIT);
		if (r > 0) {
			CHECK(up_got + (size_t)r <= up_sent);
			CHECK(memcmp(buf, up + up_got, (size_t)r) == 0);
			up_got += (size_t)r;
		}
		len = take(n);
		if (len == 0) {
			usleep(50);
			continue;
		}
		g = parse_tcp(len, GATEWAY, port, 40035);
		CHECK_EQ(g.seq, c.ack);
		CHECK(down_got + g.len <= sizeof(down));
		if (down_got + g.len > sizeof(down) || g.seq != c.ack)
			break;
		CHECK(memcmp(g.data, down + down_got, g.len) == 0);
		down_got += g.len;
		c.ack += (uint32_t)g.len;
		fin = (g.flags & FIN) != 0;
		if (g.len == 0 && !fin)
			continue;
		part = sizeof(up) - up_sent < 150 ? sizeof(up) - up_sent : 150;
		c.ack += fin;
		gsend(&c, ACK | (fin ? FIN : 0), up + up_sent, part);
		up_sent += part;
	}
	CHECK(fin);
	CHECK_EQ(down_got, sizeof(down));
	CHECK_EQ(up_sent, sizeof(up));
	expect(&c, ACK, 0);
	host_drains(&c, up + up_got, sizeof(up) - up_got);
	host_ends(c.host, 0);
	close(c.host);
	close(lfd);
	net_destroy(n);
}

/* The router's sockets: one per forward and connection; none after. */
static void test_fds(void)
{
	int before = open_fds();
	NetUserConfig config = {.forwards = 2};
	uint16_t port;
	int lfd, client2;
	NetLink *n;
	Conn out = {0}, in;

	for (int i = 0; i < 2; i++)
		config.forward[i] = (NetUserForward){INADDR_LOOPBACK, closed_port(),
		                                     GUEST, (uint16_t)(22 + i)};
	n = open_config(&config);
	CHECK_EQ(open_fds(), before + 2);
	lfd = listener(&port, 4);
	connect_out(&out, n, port, 40036, lfd);
	/* The second forward, with a connection polled too. */
	client2 = client(config.forward[1].host_port);
	forward_syn(&in, n, FIRST_PORT, 23);
	gsend(&in, SYN | ACK, none, 0);
	expect(&in, ACK, 0);
	CHECK_EQ(open_fds(), before + 7);
	net_destroy(n);
	CHECK_EQ(open_fds(), before + 3);
	close(client2);
	close(out.host);
	close(lfd);
}

/* A forward's port in TIME-WAIT on the host: open again all the same. */
static void test_reopen(void)
{
	uint16_t port;
	NetLink *n = open_forward(&port, 22);
	NetUserConfig config = {.forwards = 1};
	Conn c;

	int host;

	guest_speaks(n);
	host = client(port);
	forward_syn(&c, n, FIRST_PORT, 22);
	c.host = host;
	gsend(&c, SYN | ACK, none, 0);
	expect(&c, ACK, 0);
	/* The guest closes first: the router's socket too, so it waits. */
	gsend(&c, ACK | FIN, none, 0);
	expect(&c, ACK, 0);
	host_ends(c.host, 0);
	close(c.host);
	expect(&c, FIN | ACK, 0);
	gsend(&c, ACK, none, 0);
	net_destroy(n);
	config.forward[0] = (NetUserForward){INADDR_LOOPBACK, port, GUEST, 22};
	net_destroy(open_config(&config));
}

int main(void)
{
	alarm(60);
	close(0); /* the router's sockets get descriptor 0 too */
	now = WRAP;
	remove(RESOLV);
	RUN_TEST(test_life);
	RUN_TEST(test_close_wait);
	RUN_TEST(test_refused);
	RUN_TEST(test_unreachable);
	RUN_TEST(test_dns);
	RUN_TEST(test_unknown);
	RUN_TEST(test_malformed);
	RUN_TEST(test_time_exceeded);
	RUN_TEST(test_mss);
	RUN_TEST(test_window);
	RUN_TEST(test_window_update);
	RUN_TEST(test_retransmit);
	RUN_TEST(test_go_back);
	RUN_TEST(test_guest_data);
	RUN_TEST(test_flow);
	RUN_TEST(test_challenge);
	RUN_TEST(test_host_reset);
	RUN_TEST(test_simultaneous);
	RUN_TEST(test_syn_received);
	RUN_TEST(test_connecting);
	RUN_TEST(test_forward);
	RUN_TEST(test_forward_syn_sent);
	RUN_TEST(test_forward_ports);
	RUN_TEST(test_slots);
	RUN_TEST(test_queue_room);
	RUN_TEST(test_ack_after_rewind);
	RUN_TEST(test_no_socket);
	RUN_TEST(test_arp_queue_full);
	RUN_TEST(test_options);
	RUN_TEST(test_syn_ack_again);
	RUN_TEST(test_syn_window);
	RUN_TEST(test_forward_silent);
	RUN_TEST(test_mac_zeros);
	RUN_TEST(test_edge);
	RUN_TEST(test_one_byte);
	RUN_TEST(test_both_ways);
	RUN_TEST(test_fds);
	RUN_TEST(test_reopen);
	return test_summary();
}
