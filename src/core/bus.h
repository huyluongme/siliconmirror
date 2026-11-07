/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Memory bus: the guest's physical address space.
 *
 * The bus holds regions that never overlap. A RAM region is backed by
 * host memory and stores values in little-endian byte order. An MMIO
 * region forwards every access to a device through callbacks.
 *
 * Accesses are 1, 2, 4 or 8 bytes wide and may be unaligned. An access
 * must fit inside one region; alignment rules are up to the CPU and the
 * devices, not the bus.
 */

#ifndef CORE_BUS_H
#define CORE_BUS_H

#include <stddef.h>
#include <stdint.h>

typedef struct Bus Bus;

typedef enum BusStatus {
	BUS_OK = 0,
	BUS_ERR_UNMAPPED, /* no region at the address */
	BUS_ERR_BOUNDARY, /* the access runs past the end of its region */
	BUS_ERR_OVERLAP,  /* a new region overlaps an existing one */
	BUS_ERR_INVALID,  /* invalid argument, such as an empty region */
	BUS_ERR_NOMEM,    /* out of host memory */
} BusStatus;

/*
 * Device callbacks for an MMIO region. ctx is the pointer given to
 * bus_map_mmio(), normally the device's own state. offset is relative to
 * the start of the region and size is 1, 2, 4 or 8. Only the low size
 * bytes of a value count: the bus clears the other bits of written and
 * returned values. A NULL read callback makes reads return 0; a NULL
 * write callback ignores writes.
 */
typedef struct BusMmioOps {
	uint64_t (*read)(void *ctx, uint64_t offset, unsigned size);
	void (*write)(void *ctx, uint64_t offset, uint64_t value, unsigned size);
} BusMmioOps;

/* Create an empty bus. Returns NULL when out of memory. */
Bus *bus_create(void);

/* Free the bus and its RAM. bus may be NULL. */
void bus_destroy(Bus *bus);

/*
 * Map size bytes of zeroed RAM at base. name is used in messages and must
 * stay valid while the bus exists; a string literal is the usual choice.
 */
BusStatus bus_map_ram(Bus *bus, const char *name, uint64_t base, uint64_t size);

/*
 * Map a device at base. Accesses call ops, passing ctx as the first
 * argument. name and ops must stay valid while the bus exists.
 */
BusStatus bus_map_mmio(Bus *bus, const char *name, uint64_t base, uint64_t size,
                       const BusMmioOps *ops, void *ctx);

/*
 * Read or write size bytes at addr; size must be 1, 2, 4 or 8. On error
 * nothing is accessed and bus_read() sets *value to 0.
 */
BusStatus bus_read(Bus *bus, uint64_t addr, unsigned size, uint64_t *value);
BusStatus bus_write(Bus *bus, uint64_t addr, unsigned size, uint64_t value);

/*
 * Copy len bytes between buf and the bus, for loaders and debuggers. The
 * range may span several adjacent regions; MMIO regions are accessed one
 * byte at a time. On error, the bytes before the failing address have
 * already been copied.
 */
BusStatus bus_read_block(Bus *bus, uint64_t addr, void *buf, size_t len);
BusStatus bus_write_block(Bus *bus, uint64_t addr, const void *buf, size_t len);

/* A short description of status, for messages. */
const char *bus_status_str(BusStatus status);

#endif /* CORE_BUS_H */
