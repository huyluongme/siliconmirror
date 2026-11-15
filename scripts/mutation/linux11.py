# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The DMA controller, SDHOST's DREQ and their place on the board, in the
# working tree: every line of bcm2835_dma.c, and the lines of
# bcm2835_sdhost.c and raspi3b.c changed since fa75442.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/dma/bcm2835_dma.c", "lines": "all"},
    {"path": "src/hw/sd/bcm2835_sdhost.c", "lines": "since:fa75442"},
    {"path": "src/machine/raspi3b.c", "lines": "since:fa75442"},
]
WORKERS = 4
JOBS = 3
