# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# The survivors of 8b's full run, against the tests of the working tree:
# run with the old log as SURVIVORS.
SNAPSHOT = None
FILES = [{"path": p, "lines": "since:2cc0884"} for p in [
	"src/core/char_stream.h",
	"src/core/machine.c",
	"src/core/machine.h",
	"src/frontend/main.c",
	"src/hw/char/bcm2835_aux.c",
	"src/hw/char/bcm2835_aux.h",
	"src/hw/char/pl011.c",
	"src/hw/char/pl011.h",
	"src/hw/gpio/bcm2835_gpio.c",
	"src/hw/gpio/bcm2835_gpio.h",
	"src/hw/intc/bcm2835_ic.h",
	"src/machine/board.h",
	"src/machine/raspi3b.c",
	"src/machine/raspi3b.h",
	"src/machine/raspi3b_serial.c",
	"src/machine/raspi3b_serial.h",
]]
WORKERS = 4
JOBS = 3
