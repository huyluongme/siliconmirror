# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The MPHI, which dwc_otg kicks for its interrupt, and the registers of
# features the USB core lacks, in the working tree: every line of
# bcm2835_mphi.c, and the lines of dwc2.c and raspi3b.c changed since
# 9fb9ab4.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/misc/bcm2835_mphi.c", "lines": "all"},
    {"path": "src/hw/usb/dwc2.c", "lines": "since:9fb9ab4"},
    {"path": "src/machine/raspi3b.c", "lines": "since:9fb9ab4"},
]
WORKERS = 4
JOBS = 3
