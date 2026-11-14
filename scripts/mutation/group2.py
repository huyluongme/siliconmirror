# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Group 2 (8f, 8g) and what came after, in the working tree: the new
# files whole, and the lines changed since group 1 (92f0ca2) in the
# others.
SNAPSHOT = None
NEW = [
	"src/hw/misc/bcm2835_pm.c",
	"src/hw/net/lan95xx.c",
	"src/hw/sd/bcm2835_emmc.c",
	"src/hw/usb/dwc2.c",
	"src/hw/usb/usb.c",
	"src/hw/usb/usb_hub.c",
]
CHANGED = [
	"src/core/machine.c",
	"src/machine/raspi3b.c",
	"src/machine/raspi3b_firmware.c",
	"src/machine/raspi3b_property.c",
	"src/util/byteorder.h",
]
FILES = ([{"path": p, "lines": "all"} for p in NEW] +
         [{"path": p, "lines": "since:92f0ca2"} for p in CHANGED])
WORKERS = 4
JOBS = 3
