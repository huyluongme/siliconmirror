# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The VideoCore's audio service and the audio outputs, in the working
# tree: the new raspi3b_auds, the WAV and ALSA back ends, and the lines
# changed since af1d80a of the property service, the board and the
# front end (-audio).
SNAPSHOT = None
FILES = [
    {"path": "src/machine/raspi3b_auds.c", "lines": "all"},
    {"path": "src/backend/audio/wav.c", "lines": "all"},
    {"path": "src/backend/audio/alsa.c", "lines": "all"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:af1d80a"},
    {"path": "src/machine/raspi3b.c", "lines": "since:af1d80a"},
    {"path": "src/frontend/main.c", "lines": "since:af1d80a"},
]
WORKERS = 3
JOBS = 3
