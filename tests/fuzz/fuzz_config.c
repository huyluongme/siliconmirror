/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Fuzz the config.txt parser (src/boot/rpi_config.c) with a text. */

#include <stddef.h>
#include <stdint.h>

#include "boot/rpi_config.h"

static void warn(void *ctx, const char *message)
{
	(void)ctx;
	(void)message;
}

/* GPIO pins as the input's first byte says. */
static bool gpio_level(void *ctx, unsigned pin)
{
	return (*(const uint8_t *)ctx >> (pin % 8)) & 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const char *const models[] = {"pi3", "pi3+", NULL};
	static const char *const keys[] = {
		"kernel",    "arm_64bit", "enable_uart",    "device_tree",
		"initramfs", "os_prefix", "kernel_address", "uart_2ndstage",
	};
	uint8_t pins = size > 0 ? data[0] : 0;
	const RpiBoard board = {
		.models = models,
		.board_type = 8,
		.serial = 0x12345678,
		.gpio_level = gpio_level,
		.ctx = &pins,
	};
	RpiConfig config = {0};
	uint64_t value;

	if (rpi_config_parse((const char *)data, size, &board, &config, warn,
	                     NULL)) {
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			rpi_config_get(&config, keys[i], i % 2 == 0);
			rpi_config_int(&config, keys[i], false, &value, warn, NULL);
		}
		rpi_config_report_unused(&config, warn, NULL);
	}
	rpi_config_free(&config);
	return 0;
}
