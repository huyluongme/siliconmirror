# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The AXI bridges of the power manager, in the working tree: the lines
# of bcm2835_pm.c and raspi3b.c changed since 39dc9d5.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/misc/bcm2835_pm.c", "lines": "since:39dc9d5"},
    {"path": "src/machine/raspi3b.c", "lines": "since:39dc9d5"},
]
WORKERS = 4
JOBS = 3
