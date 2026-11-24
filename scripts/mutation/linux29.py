# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Networking to the host, parts 2b and 2c, in the working tree: TCP in
# the user network and its forwards (user_tcp.c), and the lines changed
# since 332ec07 of the router (ICMP errors, the ARP request, TCP's
# routing, the hostfwd option) and of its UDP (the shared DNS port).
SNAPSHOT = None
FILES = [
    {"path": "src/backend/net/user_tcp.c", "lines": "all"},
    {"path": "src/backend/net/user.c", "lines": "since:332ec07"},
    {"path": "src/backend/net/user_udp.c", "lines": "since:332ec07"},
]
WORKERS = 3
JOBS = 3
