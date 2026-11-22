/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * A network link to a TAP device of the Linux host (-net tap:IFNAME):
 * the guest's frames come out of the host's interface IFNAME, and the
 * frames the host sends on it reach the guest, as if the board's cable
 * were plugged into it. The host routes, bridges or serves DHCP on it
 * as on any interface.
 *
 * The device is made once, by root, and given to the user, so that
 * smirror runs without root (the kernel's tuntap.txt, "Configuration"):
 *
 *     sudo ip tuntap add dev tap0 mode tap user $USER
 *     sudo ip link set tap0 up
 *
 * Linux only; elsewhere opening fails with a message.
 */

#ifndef BACKEND_NET_TAP_H
#define BACKEND_NET_TAP_H

#include <stddef.h>

#include "core/net.h"

/*
 * The link to the TAP device ifname, or NULL with a message in error
 * when it cannot be attached to.
 */
NetLink *net_tap_open(const char *ifname, char *error, size_t error_size);

/*
 * The same over fd, already open, where each read and write is one
 * frame, for tests (a SOCK_SEQPACKET socket). The link closes fd.
 */
NetLink *net_tap_open_fd(int fd, char *error, size_t error_size);

#endif /* BACKEND_NET_TAP_H */
