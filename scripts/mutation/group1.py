# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Group 1 (8c, 8d, 8e) in the working tree: the new files whole, and the
# lines changed since 8b (35c86d2) in the others.
SNAPSHOT = None
NEW = [
	"src/backend/block/file.c",
	"src/boot/fat.c",
	"src/boot/mbr.c",
	"src/boot/rpi_config.c",
	"src/core/block.h",
	"src/hw/misc/bcm2835_mbox.c",
	"src/hw/misc/bcm2835_mbox.h",
	"src/hw/sd/bcm2835_sdhost.c",
	"src/hw/sd/bcm2835_sdhost.h",
	"src/hw/sd/sd_card.c",
	"src/hw/sd/sd_card.h",
	"src/machine/raspi3b_firmware.c",
	"src/machine/raspi3b_property.c",
	"src/machine/raspi3b_property.h",
	"src/util/fdt.c",
	"src/util/fdt.h",
]
CHANGED = [
	"src/boot/direct.c",
	"src/frontend/main.c",
	"src/hw/intc/bcm2835_ic.h",
	"src/machine/board.h",
	"src/machine/raspi3b.c",
	"src/machine/raspi3b.h",
	"src/util/byteorder.h",
]
FILES = ([{"path": p, "lines": "all"} for p in NEW] +
         [{"path": p, "lines": "since:35c86d2"} for p in CHANGED])
WORKERS = 4
JOBS = 3
