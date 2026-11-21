# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# What the firmware tells Linux, and the load/store classes newer than
# Armv8.0, in the working tree: the lines changed since 10d6079.
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_firmware.c", "lines": "since:10d6079"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:10d6079"},
    {"path": "src/cpu/arm64/load_store.c", "lines": "since:10d6079"},
]
WORKERS = 4
JOBS = 3
