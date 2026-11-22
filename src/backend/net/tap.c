/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A network link to a Linux TAP device, after the kernel's tuntap.txt:
 * open /dev/net/tun, attach to the interface with TUNSETIFF, IFF_TAP for
 * Ethernet frames and IFF_NO_PI for no packet information before them.
 * Each read() then gives one frame and each write() sends one, without
 * their FCS.
 */

#define _DEFAULT_SOURCE /* struct ifreq's ifr_name */

#include "backend/net/tap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#endif

typedef struct TapLink {
	NetLink link; /* first, so that a NetLink * is a TapLink * */
	int fd;
} TapLink;

static bool fail(TapLink *t, const char *what)
{
	snprintf(t->link.error, sizeof(t->link.error), "%s: %s", what,
	         strerror(errno));
	return false;
}

static bool tap_send(NetLink *n, uint64_t now, const uint8_t *frame, size_t len)
{
	TapLink *t = (TapLink *)n;

	(void)now;
	if (write(t->fd, frame, len) >= 0)
		return true;
	/*
	 * Lost, not a failure: no room in the host's queue, the interface
	 * down (EIO from Linux's tun driver, from general knowledge), or a
	 * frame the host refuses. Not waiting, no call is interrupted (EINTR).
	 * Any other error stops the machine.
	 */
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO ||
	    errno == EINVAL)
		return true;
	return fail(t, "cannot send on the TAP device");
}

static bool tap_receive(NetLink *n, uint8_t *frame, size_t *len)
{
	TapLink *t = (TapLink *)n;
	ssize_t got = read(t->fd, frame, NET_MAX_FRAME);

	*len = 0;
	if (got >= 0) {
		*len = (size_t)got;
		return true;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return true;
	return fail(t, "cannot receive from the TAP device");
}

static void tap_destroy(NetLink *n)
{
	TapLink *t = (TapLink *)n;

	close(t->fd);
	free(t);
}

static const NetLinkOps tap_ops = {
	.send = tap_send,
	.receive = tap_receive,
	.destroy = tap_destroy,
};

NetLink *net_tap_open_fd(int fd, char *error, size_t error_size)
{
	TapLink *t;
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		snprintf(error, error_size, "cannot make the TAP device not wait: %s",
		         strerror(errno));
		close(fd);
		return NULL;
	}
	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		snprintf(error, error_size, "out of memory");
		close(fd);
		return NULL;
	}
	t->link.ops = &tap_ops;
	t->fd = fd;
	return &t->link;
}

#ifdef __linux__
NetLink *net_tap_open(const char *ifname, char *error, size_t error_size)
{
	struct ifreq ifr;
	int fd;

	if (strlen(ifname) == 0 || strlen(ifname) >= sizeof(ifr.ifr_name)) {
		snprintf(error, error_size, "'%s' is not an interface name", ifname);
		return NULL;
	}
	fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		snprintf(error, error_size, "cannot open /dev/net/tun: %s",
		         strerror(errno));
		return NULL;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strcpy(ifr.ifr_name, ifname);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		int e = errno;

		close(fd);
		if (e == EPERM)
			snprintf(error, error_size,
			         "cannot attach to %s: not allowed; make it yours once "
			         "with 'sudo ip tuntap add dev %s mode tap user $USER'",
			         ifname, ifname);
		else
			snprintf(error, error_size, "cannot attach to %s: %s", ifname,
			         strerror(e));
		return NULL;
	}
	return net_tap_open_fd(fd, error, error_size);
}
#else
NetLink *net_tap_open(const char *ifname, char *error, size_t error_size)
{
	snprintf(error, error_size, "%s: TAP devices are Linux only", ifname);
	return NULL;
}
#endif
