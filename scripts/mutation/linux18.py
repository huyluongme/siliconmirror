# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The PHY registers 7 to 15 of the LAN9514, in the working tree: the
# lines of lan95xx.c changed since fd79469.
SNAPSHOT = None
FILES = [
    {"path": "src/hw/net/lan95xx.c", "lines": "since:fd79469"},
]
WORKERS = 4
JOBS = 3
