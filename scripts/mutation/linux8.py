# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The 1-byte accesses to the mini UART that Linux's 8250_bcm2835aux
# driver makes, in the working tree: the lines changed since b7aec3d.
SNAPSHOT = None
FILES = [{"path": "src/hw/char/bcm2835_aux.c", "lines": "since:b7aec3d"}]
WORKERS = 4
JOBS = 3
