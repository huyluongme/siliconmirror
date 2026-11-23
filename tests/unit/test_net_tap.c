/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the TAP link (src/backend/net/tap.c). A TAP device
 * needs root to make, so the link runs over one end of a SOCK_SEQPACKET
 * socket pair instead, which keeps each frame whole as TAP does; the
 * test holds the other end, as the host's network stack.
 */

#define _GNU_SOURCE /* SOCK_NONBLOCK, posix_openpt */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_tun.h>
#include <net/if.h>

#include "backend/net/tap.h"
#include "test.h"

static uint8_t frame[NET_MAX_FRAME], got[NET_MAX_FRAME];

typedef struct Pair {
	NetLink *link;
	int fd;   /* the link's end */
	int host; /* the host's end */
} Pair;

static Pair open_pair(void)
{
	int sv[2];
	char error[200];
	Pair p = {NULL, -1, -1};

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv) == 0);
	/* The link makes its end not wait itself. */
	CHECK(fcntl(sv[0], F_SETFL, 0) == 0);
	p.link = net_tap_open_fd(sv[0], error, sizeof(error));
	CHECK(p.link != NULL);
	CHECK((fcntl(sv[0], F_GETFL) & O_NONBLOCK) != 0);
	p.fd = sv[0];
	p.host = sv[1];
	return p;
}

static void close_pair(Pair *p)
{
	net_destroy(p->link);
	CHECK(fcntl(p->fd, F_GETFD) == -1); /* the link closed its end */
	close(p->host);
}

/* The guest's frames reach the host whole, one a write. */
static void test_send(void)
{
	Pair p = open_pair();

	for (int i = 0; i < 1514; i++)
		frame[i] = (uint8_t)i;
	CHECK(p.link->ops->send(p.link, 123, frame, 60));
	CHECK(p.link->ops->send(p.link, 456, frame, 1514));
	CHECK_EQ(read(p.host, got, sizeof(got)), 60);
	CHECK(memcmp(got, frame, 60) == 0);
	CHECK_EQ(read(p.host, got, sizeof(got)), 1514);
	CHECK(memcmp(got, frame, 1514) == 0);
	CHECK_EQ(read(p.host, got, sizeof(got)), -1); /* no more */
	close_pair(&p);
}

/* The host's frames, one at a time; none yet is no failure. */
static void test_receive(void)
{
	Pair p = open_pair();
	size_t len = 99;

	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 0);
	memset(frame, 0x5a, 1000);
	CHECK_EQ(write(p.host, frame, 42), 42);
	CHECK_EQ(write(p.host, frame, 1000), 1000);
	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 42);
	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 1000);
	CHECK(memcmp(got, frame, 1000) == 0);
	len = 99;
	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 0);

	/* The longest frame the host passes, whole. */
	int size = NET_MAX_FRAME + 1000;
	CHECK(setsockopt(p.host, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
	memset(frame, 0x33, sizeof(frame));
	CHECK_EQ(write(p.host, frame, NET_MAX_FRAME), NET_MAX_FRAME);
	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, NET_MAX_FRAME);
	CHECK_EQ(got[NET_MAX_FRAME - 1], 0x33);
	close_pair(&p);
}

/* A full queue on the host loses frames, as a busy link does. */
static void test_send_full(void)
{
	Pair p = open_pair();
	unsigned sent = 0, read_back = 0;

	memset(frame, 1, 1500);
	for (unsigned i = 0; i < 10000; i++)
		if (p.link->ops->send(p.link, 0, frame, 1500))
			sent++;
	CHECK_EQ(sent, 10000);
	while (read(p.host, got, sizeof(got)) == 1500 && read_back < 10000)
		read_back++;
	CHECK(read_back > 0);
	CHECK(read_back < 10000);
	close_pair(&p);
}

/* Closed: the link's descriptor goes with it. */
static bool closed(int fd)
{
	return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

/*
 * What is not a failure: a frame the host refuses (EINVAL, as an
 * eventfd gives for a write that is not 8 bytes), a read of nothing (the
 * other end gone), a descriptor whose flags are 0 (a pipe's read end).
 */
static void test_not_failures(void)
{
	char error[200];
	Pair p = open_pair();
	size_t len = 99;
	int fd, pipe_fds[2];
	NetLink *n;

	close(p.host);
	p.host = -1;
	errno = EBADF;
	CHECK(p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 0);
	net_destroy(p.link);
	CHECK(closed(p.fd));

	fd = eventfd(0, 0);
	CHECK(fd >= 0);
	n = net_tap_open_fd(fd, error, sizeof(error));
	CHECK(n != NULL);
	CHECK(n->ops->send(n, 0, frame, 60));
	net_destroy(n);
	CHECK(closed(fd));

	CHECK(pipe(pipe_fds) == 0);
	CHECK_EQ(fcntl(pipe_fds[0], F_GETFL), 0);
	n = net_tap_open_fd(pipe_fds[0], error, sizeof(error));
	CHECK(n != NULL);
	len = 99;
	CHECK(n->ops->receive(n, 0, got, &len));
	CHECK_EQ(len, 0);
	net_destroy(n);
	close(pipe_fds[1]);
}

/* The host's errors: what failed, and why. */
static void test_errors(void)
{
	char error[200];
	Pair p = open_pair();
	size_t len;
	int fd;

	signal(SIGPIPE, SIG_IGN);
	close(p.host);
	p.host = -1;
	CHECK(!p.link->ops->send(p.link, 0, frame, 60));
	CHECK(strstr(p.link->error, "cannot send on the TAP device: ") != NULL);
	net_destroy(p.link);

	/* A read the host refuses: a directory. */
	fd = open(".", O_RDONLY | O_DIRECTORY);
	CHECK(fd >= 0);
	p.link = net_tap_open_fd(fd, error, sizeof(error));
	CHECK(p.link != NULL);
	CHECK(!p.link->ops->receive(p.link, 0, got, &len));
	CHECK_EQ(len, 0);
	CHECK(strstr(p.link->error, "cannot receive from the TAP device: ") !=
	      NULL);
	net_destroy(p.link);

	/* A terminal's master whose other end is gone: EIO. */
	fd = posix_openpt(O_RDWR | O_NOCTTY);
	CHECK(fd >= 0);
	CHECK(grantpt(fd) == 0 && unlockpt(fd) == 0);
	close(open(ptsname(fd), O_RDWR | O_NOCTTY));
	p.link = net_tap_open_fd(fd, error, sizeof(error));
	CHECK(p.link != NULL);
	CHECK(!p.link->ops->receive(p.link, 0, got, &len));
	CHECK(strcmp(p.link->error, "cannot receive from the TAP device: "
	                            "Input/output error") == 0);
	net_destroy(p.link);

	/* No such file descriptor. */
	CHECK(net_tap_open_fd(-1, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "cannot make the TAP device not wait: ") != NULL);
}

/* Names that no interface has, before the device is opened. */
static void test_names(void)
{
	char error[200];

	CHECK(net_tap_open("", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "'' is not an interface name") == 0);
	CHECK(net_tap_open("0123456789abcdef", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "'0123456789abcdef' is not an interface name") == 0);
}

/*
 * The kernel's answers, where /dev/net/tun opens and the user is not
 * root: no right to make a device (EPERM), an interface that is not a
 * TAP device (EINVAL). The device's descriptor is closed after each, and
 * may be 0.
 */
static void test_tun(void)
{
	char error[300];
	struct ifreq ifr;
	int fd = open("/dev/net/tun", O_RDWR);
	bool allowed;

	if (fd < 0) {
		printf("test_tun: skipped, no /dev/net/tun\n");
		return;
	}
	/* The test's own try, not the code's: may this user make one? */
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strcpy(ifr.ifr_name, "smirrortest0");
	allowed = ioctl(fd, TUNSETIFF, &ifr) == 0 || errno != EPERM;
	close(fd); /* a device made goes with it */
	if (allowed) {
		printf("test_tun: skipped, allowed to make TAP devices\n");
		return;
	}
	close(0); /* /dev/net/tun gets descriptor 0 */
	CHECK(net_tap_open("smirrortest0", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot attach to smirrortest0: not allowed; make it "
	                    "yours once with 'sudo ip tuntap add dev "
	                    "smirrortest0 mode tap user $USER'") == 0);
	CHECK(closed(0));
	CHECK(net_tap_open("lo", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot attach to lo: Invalid argument") == 0);
	CHECK(closed(0));
}

int main(void)
{
	RUN_TEST(test_send);
	RUN_TEST(test_receive);
	RUN_TEST(test_send_full);
	RUN_TEST(test_not_failures);
	RUN_TEST(test_errors);
	RUN_TEST(test_names);
	RUN_TEST(test_tun);
	return test_summary();
}
