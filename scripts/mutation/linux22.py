# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The firmware's overlays and parameters in the working tree: the new
# rpi_overlay.c, and the lines of fdt.c and of the firmware changed since
# d133690.
SNAPSHOT = None
FILES = [
    {"path": "src/boot/rpi_overlay.c", "lines": "all"},
    {"path": "src/util/fdt.c", "lines": "since:d133690"},
    {"path": "src/machine/raspi3b_firmware.c", "lines": "since:d133690"},
]
WORKERS = 4
JOBS = 3
