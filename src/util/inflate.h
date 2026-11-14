/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#ifndef SMIRROR_UTIL_INFLATE_H
#define SMIRROR_UTIL_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Decompress a gzip file (RFC 1952) of one member, whose data is DEFLATE
 * (RFC 1951), into at most max bytes. On success, *out is a malloc()ed
 * buffer of *out_size bytes. On failure, error says why: the file is not
 * gzip, is cut short, has wrong data, a wrong CRC or size, more data
 * after its member, or more than max bytes inside.
 */
bool gunzip(const uint8_t *in, size_t size, size_t max, uint8_t **out,
            size_t *out_size, char *error, size_t error_size);

#endif
