# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The VideoCore's end of VCHIQ and the doorbells, in the working tree:
# every line of raspi3b_vchiq.c and bcm2835_doorbell.c, and the lines of
# raspi3b_property.c and raspi3b.c changed since b16cf1c.
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_vchiq.c", "lines": "all"},
    {"path": "src/hw/misc/bcm2835_doorbell.c", "lines": "all"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:b16cf1c"},
    {"path": "src/machine/raspi3b.c", "lines": "since:b16cf1c"},
]
WORKERS = 4
JOBS = 3
