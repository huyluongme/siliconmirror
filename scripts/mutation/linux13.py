# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The firmware's clocks and power domains, the random number generator
# and the temperature sensor, in the working tree: every line of
# bcm2835_rng.c and bcm2835_thermal.c, and the lines of
# raspi3b_property.c and raspi3b.c changed since e46c6cd.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/misc/bcm2835_rng.c", "lines": "all"},
    {"path": "src/hw/misc/bcm2835_thermal.c", "lines": "all"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:e46c6cd"},
    {"path": "src/machine/raspi3b.c", "lines": "since:e46c6cd"},
]
WORKERS = 4
JOBS = 3
