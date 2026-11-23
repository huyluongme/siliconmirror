# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Networking to the host, part 2a, in the working tree: the user
# network (the router: ARP, IPv4, ICMP; the DHCP server; UDP and its
# NAT), and the lines changed since d1f18d2 of the DWC2 (a NAKed bulk
# or control IN tried again), the cable and the links (receive's time),
# and the front end (-net user).
SNAPSHOT = None
FILES = [
    {"path": "src/backend/net/user.c", "lines": "all"},
    {"path": "src/backend/net/user_dhcp.c", "lines": "all"},
    {"path": "src/backend/net/user_udp.c", "lines": "all"},
    {"path": "src/hw/usb/dwc2.c", "lines": "since:d1f18d2"},
    {"path": "src/machine/net_cable.c", "lines": "since:d1f18d2"},
    {"path": "src/backend/net/tap.c", "lines": "since:d1f18d2"},
    {"path": "src/backend/net/pcap.c", "lines": "since:d1f18d2"},
    {"path": "src/frontend/main.c", "lines": "since:d1f18d2"},
]
WORKERS = 3
JOBS = 3
