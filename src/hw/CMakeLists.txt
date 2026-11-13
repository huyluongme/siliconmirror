# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Device models, grouped by kind. See docs/architecture.md.
add_library(hw STATIC
	char/bcm2835_aux.c
	char/pl011.c
	gpio/bcm2835_gpio.c
	intc/bcm2835_ic.c
	intc/bcm2836_local.c
	misc/bcm2835_mbox.c
	sd/bcm2835_sdhost.c
	sd/sd_card.c)

target_link_libraries(hw PUBLIC core)
