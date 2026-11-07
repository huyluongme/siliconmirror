/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "core/bus.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/byteorder.h"

typedef struct BusRegion {
	const char *name;
	uint64_t base;
	uint64_t size;
	uint8_t *ram; /* host memory for RAM, NULL for MMIO */
	const BusMmioOps *ops;
	void *ctx;
} BusRegion;

struct Bus {
	BusRegion *regions; /* sorted by base address */
	size_t count;
	size_t capacity;
};

/*
 * Accesses are 1, 2, 4 or 8 bytes wide. A macro rather than a function:
 * it is only used in assert(), which disappears when NDEBUG is defined.
 */
#define VALID_SIZE(size)                                                       \
	((size) == 1 || (size) == 2 || (size) == 4 || (size) == 8)

static uint64_t size_mask(unsigned size)
{
	return size == 8 ? UINT64_MAX : (UINT64_C(1) << (8 * size)) - 1;
}

/* Address of the last byte of a region. Regions never wrap around. */
static uint64_t region_last(const BusRegion *r)
{
	return r->base + (r->size - 1);
}

/* Return the region that contains addr, or NULL. */
static BusRegion *find_region(Bus *bus, uint64_t addr)
{
	size_t lo = 0;
	size_t hi = bus->count;

	/* Binary search for the first region that starts above addr. */
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (bus->regions[mid].base <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return NULL;

	BusRegion *r = &bus->regions[lo - 1];

	return addr <= region_last(r) ? r : NULL;
}

/*
 * Check that a new region is valid and free. On success, *index is where
 * it belongs in the sorted region array.
 */
static BusStatus check_new_region(const Bus *bus, uint64_t base, uint64_t size,
                                  size_t *index)
{
	if (size == 0 || size - 1 > UINT64_MAX - base)
		return BUS_ERR_INVALID;

	uint64_t last = base + (size - 1);
	size_t i = 0;

	while (i < bus->count && bus->regions[i].base < base)
		i++;
	if (i > 0 && region_last(&bus->regions[i - 1]) >= base)
		return BUS_ERR_OVERLAP;
	if (i < bus->count && bus->regions[i].base <= last)
		return BUS_ERR_OVERLAP;

	*index = i;
	return BUS_OK;
}

static BusStatus insert_region(Bus *bus, size_t index, const BusRegion *region)
{
	if (bus->count == bus->capacity) {
		size_t capacity = bus->capacity ? bus->capacity * 2 : 8;
		BusRegion *regions = realloc(bus->regions, capacity * sizeof(*regions));

		if (regions == NULL)
			return BUS_ERR_NOMEM;
		bus->regions = regions;
		bus->capacity = capacity;
	}

	memmove(&bus->regions[index + 1], &bus->regions[index],
	        (bus->count - index) * sizeof(*bus->regions));
	bus->regions[index] = *region;
	bus->count++;
	return BUS_OK;
}

Bus *bus_create(void)
{
	return calloc(1, sizeof(Bus));
}

void bus_destroy(Bus *bus)
{
	if (bus == NULL)
		return;
	for (size_t i = 0; i < bus->count; i++)
		free(bus->regions[i].ram);
	free(bus->regions);
	free(bus);
}

BusStatus bus_map_ram(Bus *bus, const char *name, uint64_t base, uint64_t size)
{
	size_t index;
	BusStatus status = check_new_region(bus, base, size, &index);

	if (status != BUS_OK)
		return status;
#if UINT64_MAX > SIZE_MAX
	if (size > SIZE_MAX)
		return BUS_ERR_NOMEM;
#endif

	BusRegion region = {
		.name = name,
		.base = base,
		.size = size,
		.ram = calloc(1, (size_t)size),
	};

	if (region.ram == NULL)
		return BUS_ERR_NOMEM;

	status = insert_region(bus, index, &region);
	if (status != BUS_OK)
		free(region.ram);
	return status;
}

BusStatus bus_map_mmio(Bus *bus, const char *name, uint64_t base, uint64_t size,
                       const BusMmioOps *ops, void *ctx)
{
	if (ops == NULL)
		return BUS_ERR_INVALID;

	size_t index;
	BusStatus status = check_new_region(bus, base, size, &index);

	if (status != BUS_OK)
		return status;

	BusRegion region = {
		.name = name,
		.base = base,
		.size = size,
		.ops = ops,
		.ctx = ctx,
	};

	return insert_region(bus, index, &region);
}

/* Find the region for a size-byte access at addr. */
static BusStatus find_access_region(Bus *bus, uint64_t addr, unsigned size,
                                    BusRegion **region)
{
	BusRegion *r = find_region(bus, addr);

	if (r == NULL)
		return BUS_ERR_UNMAPPED;
	if (size - 1 > region_last(r) - addr)
		return BUS_ERR_BOUNDARY;

	*region = r;
	return BUS_OK;
}

BusStatus bus_read(Bus *bus, uint64_t addr, unsigned size, uint64_t *value)
{
	assert(VALID_SIZE(size));
	*value = 0;

	BusRegion *r;
	BusStatus status = find_access_region(bus, addr, size, &r);

	if (status != BUS_OK)
		return status;

	uint64_t offset = addr - r->base;

	if (r->ram != NULL)
		*value = load_le(r->ram + offset, size);
	else if (r->ops->read != NULL)
		*value = r->ops->read(r->ctx, offset, size) & size_mask(size);
	return BUS_OK;
}

BusStatus bus_write(Bus *bus, uint64_t addr, unsigned size, uint64_t value)
{
	assert(VALID_SIZE(size));

	BusRegion *r;
	BusStatus status = find_access_region(bus, addr, size, &r);

	if (status != BUS_OK)
		return status;

	uint64_t offset = addr - r->base;

	value &= size_mask(size);
	if (r->ram != NULL)
		store_le(r->ram + offset, size, value);
	else if (r->ops->write != NULL)
		r->ops->write(r->ctx, offset, value, size);
	return BUS_OK;
}

/* Copy n bytes between buf and region r, starting at offset. */
static void copy_in_region(BusRegion *r, uint64_t offset, uint8_t *buf,
                           size_t n, bool write)
{
	if (r->ram != NULL) {
		if (write)
			memcpy(r->ram + offset, buf, n);
		else
			memcpy(buf, r->ram + offset, n);
		return;
	}

	for (size_t i = 0; i < n; i++) {
		if (write && r->ops->write != NULL)
			r->ops->write(r->ctx, offset + i, buf[i], 1);
		else if (!write)
			buf[i] = r->ops->read != NULL
			             ? (uint8_t)r->ops->read(r->ctx, offset + i, 1)
			             : 0;
	}
}

static BusStatus copy_block(Bus *bus, uint64_t addr, uint8_t *buf, size_t len,
                            bool write)
{
	while (len > 0) {
		BusRegion *r = find_region(bus, addr);

		if (r == NULL)
			return BUS_ERR_UNMAPPED;

		uint64_t offset = addr - r->base;
		uint64_t left_in_region = r->size - offset;
		size_t n = len < left_in_region ? len : (size_t)left_in_region;

		copy_in_region(r, offset, buf, n, write);
		buf += n;
		len -= n;

		/* The range must not wrap around the top of the address space. */
		if (len > 0 && region_last(r) == UINT64_MAX)
			return BUS_ERR_UNMAPPED;
		addr += n;
	}
	return BUS_OK;
}

BusStatus bus_read_block(Bus *bus, uint64_t addr, void *buf, size_t len)
{
	return copy_block(bus, addr, buf, len, false);
}

BusStatus bus_write_block(Bus *bus, uint64_t addr, const void *buf, size_t len)
{
	/* copy_block() only reads from buf when writing to the bus. */
	return copy_block(bus, addr, (uint8_t *)buf, len, true);
}

const char *bus_status_str(BusStatus status)
{
	switch (status) {
	case BUS_OK:
		return "success";
	case BUS_ERR_UNMAPPED:
		return "no region at this address";
	case BUS_ERR_BOUNDARY:
		return "access runs past the end of its region";
	case BUS_ERR_OVERLAP:
		return "region overlaps an existing region";
	case BUS_ERR_INVALID:
		return "invalid argument";
	case BUS_ERR_NOMEM:
		return "out of memory";
	}
	return "unknown bus status";
}
