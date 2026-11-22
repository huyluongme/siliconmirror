# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

# Device models, grouped by kind. See docs/architecture.md.
add_library(hw STATIC
	char/bcm2835_aux.c
	char/pl011.c
	dma/bcm2835_dma.c
	display/bcm2835_hvs.c
	gpio/bcm2835_gpio.c
	i2c/bcm2835_bsc.c
	intc/bcm2835_ic.c
	intc/bcm2836_local.c
	misc/bcm2835_mbox.c
	misc/bcm2835_mphi.c
	misc/bcm2835_cprman.c
	misc/bcm2835_doorbell.c
	misc/bcm2835_pm.c
	misc/bcm2835_rng.c
	misc/bcm2835_thermal.c
	net/lan95xx.c
	sd/bcm2835_emmc.c
	sd/bcm2835_sdhost.c
	sd/sd_card.c
	usb/dwc2.c
	usb/usb.c
	usb/usb_hub.c)

target_link_libraries(hw PUBLIC core)
