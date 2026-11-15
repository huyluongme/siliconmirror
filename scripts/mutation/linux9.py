# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The BCM2835's clock manager and its place on the board, in the working
# tree: every line of bcm2835_cprman.c, and the lines of raspi3b.c
# changed since 0900eb7.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/misc/bcm2835_cprman.c", "lines": "all"},
    {"path": "src/machine/raspi3b.c", "lines": "since:0900eb7"},
]
WORKERS = 4
JOBS = 3
