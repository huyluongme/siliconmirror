# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# SDHOST's BLOCK_IRPT, the LAN9514's checksum offload engines and the
# TSENS clock, in the working tree: the lines of bcm2835_sdhost.c,
# lan95xx.c and bcm2835_cprman.c changed since d1ca2ce.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/sd/bcm2835_sdhost.c", "lines": "since:d1ca2ce"},
    {"path": "src/hw/net/lan95xx.c", "lines": "since:d1ca2ce"},
    {"path": "src/hw/misc/bcm2835_cprman.c", "lines": "since:d1ca2ce"},
]
WORKERS = 4
JOBS = 3
