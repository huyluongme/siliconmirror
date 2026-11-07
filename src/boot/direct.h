/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Direct boot: load an image file given on the command line (-kernel)
 * into guest memory, the way the Raspberry Pi firmware loads kernel8.img.
 * The board then starts its CPUs at the image, in the state that its
 * firmware would leave them in.
 */

#ifndef BOOT_DIRECT_H
#define BOOT_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/bus.h"

/*
 * Copy the file at path, as it is, to address. The image must fit below
 * limit, so that it cannot spill into the devices mapped above RAM. On
 * failure, writes a message that starts with the path to error and
 * returns false; part of the image may have been copied.
 */
bool boot_direct_load(Bus *bus, const char *path, uint64_t address,
                      uint64_t limit, char *error, size_t error_size);

#endif /* BOOT_DIRECT_H */
