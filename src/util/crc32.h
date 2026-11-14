/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#ifndef SMIRROR_UTIL_CRC32_H
#define SMIRROR_UTIL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * The CRC-32 of gzip (RFC 1952, section 8) and Ethernet: the polynomial
 * 0x04C11DB7 with its bits reversed (0xEDB88320), from 0xFFFFFFFF, and
 * the result inverted. The CRC of "123456789" is 0xCBF43926.
 */
uint32_t crc32(const void *data, size_t size);

#endif
