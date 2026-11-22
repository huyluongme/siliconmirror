# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Raspberry Pi OS to its login prompt, in the working tree: the new BSC
# and HVS, and the lines changed since 3f49a7b of VCHIQ (its messages),
# the property service (overscan, NOTIFY_DISPLAY_DONE), the firmware
# (KMS and the display's settings) and the board (the BSCs' shared
# interrupt, the HVS, the doorbell).
SNAPSHOT = None
FILES = [
    {"path": "src/hw/i2c/bcm2835_bsc.c", "lines": "all"},
    {"path": "src/hw/display/bcm2835_hvs.c", "lines": "all"},
    {"path": "src/machine/raspi3b_vchiq.c", "lines": "since:3f49a7b"},
    {"path": "src/machine/raspi3b_property.c", "lines": "since:3f49a7b"},
    {"path": "src/machine/raspi3b_firmware.c", "lines": "since:3f49a7b"},
    {"path": "src/machine/raspi3b.c", "lines": "since:3f49a7b"},
]
WORKERS = 4
JOBS = 3
