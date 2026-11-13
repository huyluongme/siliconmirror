/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Block devices: the interface between a device that stores data, such
 * as an SD card, and the host side that holds the data, such as an image
 * file (src/backend/block/).
 *
 * The device sees an array of bytes of a fixed size, read and written at
 * any offset; it decides itself what sectors or blocks it shows the
 * guest. A read or a write fails only when the host does: the device
 * then reports an error to the guest, or stops the machine.
 */

#ifndef CORE_BLOCK_H
#define CORE_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BlockDevice BlockDevice;

typedef struct BlockDeviceOps {
	/* Read len bytes at offset into buf. The range is inside the device. */
	bool (*read)(BlockDevice *b, uint64_t offset, void *buf, size_t len);

	/* Write len bytes from buf at offset, inside the device. */
	bool (*write)(BlockDevice *b, uint64_t offset, const void *buf, size_t len);

	/* Free the device. */
	void (*destroy)(BlockDevice *b);
} BlockDeviceOps;

/* Each back end embeds a BlockDevice in its own state struct. */
struct BlockDevice {
	const BlockDeviceOps *ops;
	uint64_t size; /* bytes */
};

/* Whether len bytes at offset are inside b. */
static inline bool block_in_range(const BlockDevice *b, uint64_t offset,
                                  uint64_t len)
{
	return offset <= b->size && len <= b->size - offset;
}

/* Read, failing for a range outside the device. */
static inline bool block_read(BlockDevice *b, uint64_t offset, void *buf,
                              size_t len)
{
	return block_in_range(b, offset, len) && b->ops->read(b, offset, buf, len);
}

static inline bool block_write(BlockDevice *b, uint64_t offset, const void *buf,
                               size_t len)
{
	return block_in_range(b, offset, len) && b->ops->write(b, offset, buf, len);
}

static inline void block_destroy(BlockDevice *b)
{
	if (b != NULL)
		b->ops->destroy(b);
}

#endif /* CORE_BLOCK_H */
