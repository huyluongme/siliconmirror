# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The VideoCore's mmal service, in the working tree: the new
# raspi3b_mmal, and the lines changed since d3e76d9 of the property
# service (its creation).
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_mmal.c", "lines": "all"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:d3e76d9"},
]
WORKERS = 4
JOBS = 3
