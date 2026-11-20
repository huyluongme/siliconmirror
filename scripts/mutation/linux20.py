# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The firmware's reboot notice, in the working tree: the lines of
# raspi3b_property.c changed since 4949f0e.
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_property.c", "lines": "since:4949f0e"},
]
WORKERS = 4
JOBS = 3
