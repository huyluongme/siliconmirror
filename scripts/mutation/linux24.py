# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The VideoCore's services, in the working tree: the new SMEM, and the
# lines changed since 81e1e94 of VCHIQ (OPENACK, DATA, CLOSE, the
# messages waiting for slots) and the property service (SMEM's
# creation, GET_GENCMD_RESULT for vcgencmd).
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_smem.c", "lines": "all"},
    {"path": "src/machine/raspi3b_vchiq.c", "lines": "since:81e1e94"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:81e1e94"},
]
WORKERS = 4
JOBS = 3
