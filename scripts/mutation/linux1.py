# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The firmware for Linux (step A of roadmap item 4): gzip kernels, the
# initramfs after image_size, and the firmware's build tags, in the
# working tree: the lines changed since affd279.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:affd279"} for p in [
	"src/util/crc32.c",
	"src/util/inflate.c",
	"src/machine/raspi3b_firmware.c",
	"src/machine/raspi3b_property.c",
]]
WORKERS = 4
JOBS = 3
