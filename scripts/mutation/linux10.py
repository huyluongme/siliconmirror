# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The SDIO commands that no WiFi chip answers, and the serial aliases of
# the command line, in the working tree: the lines changed since cc58b9e.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/sd/bcm2835_emmc.c", "lines": "since:cc58b9e"},
    {"path": "src/machine/raspi3b_firmware.c", "lines": "since:cc58b9e"},
]
WORKERS = 4
JOBS = 3
