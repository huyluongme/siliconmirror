/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The Raspberry Pi firmware's devicetree overlays and parameters: what
 * dtoverlay= and dtparam= in config.txt do to the base tree before the
 * kernel gets it.
 *
 * Written from the Raspberry Pi documentation, "Device Trees, overlays,
 * and parameters" (fragments, __fixups__, __local_fixups__, __symbols__,
 * __exports__, __overrides__ with its parameter kinds, the fragment
 * operations, the special properties, the overlay map and the order of
 * the passes), and from the overlays' README. Where the documentation
 * says nothing, rpi_overlay.c says what it does.
 */

#ifndef BOOT_RPI_OVERLAY_H
#define BOOT_RPI_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>

#include "boot/rpi_config.h"
#include "util/fdt.h"

/* How a parameter went. */
typedef enum RpiParamResult {
	RPI_PARAM_APPLIED,
	RPI_PARAM_UNKNOWN, /* the scope has no such parameter */
	RPI_PARAM_FAILED,  /* error says why */
} RpiParamResult;

/*
 * Make overlay fit base before its parameters are set: its phandles
 * move above base's, its references to base's labels (__fixups__) get
 * their phandles. A label of base without a phandle is given one. On
 * failure, writes a message to error and returns false; base may have
 * gained phandles.
 */
bool rpi_overlay_prepare(Fdt *base, Fdt *overlay, char *error,
                         size_t error_size);

/*
 * Set the parameter name to value, NULL when it has none ("dtparam=foo"),
 * as the __overrides__ of scope declare it. scope is base, or an overlay
 * that rpi_overlay_prepare() has prepared for base and that is not
 * merged yet.
 */
RpiParamResult rpi_overlay_param(Fdt *base, Fdt *scope, const char *name,
                                 const char *value, char *error,
                                 size_t error_size);

/*
 * Merge a prepared overlay into base: its fragments that target the
 * overlay itself first, then the others; then base's __symbols__ gains
 * the labels that the overlay exports. On failure, writes a message to
 * error and returns false, with base unchanged.
 */
bool rpi_overlay_merge(Fdt *base, Fdt *overlay, char *error, size_t error_size);

/* What the overlay map says about an overlay. */
typedef enum RpiMapResult {
	RPI_MAP_LOAD,        /* load out: name, or what replaces it */
	RPI_MAP_RENAMED,     /* load out, with a warning: the overlay was renamed */
	RPI_MAP_DEPRECATED,  /* do not load: out holds why */
	RPI_MAP_UNSUPPORTED, /* do not load: not for platform */
	RPI_MAP_LOOP,        /* do not load: the map goes round */
} RpiMapResult;

/*
 * The file to load for the overlay name on platform, such as "bcm2835",
 * as map (overlay_map.dtb, NULL for none) says: out gets the overlay's
 * name, or the reason not to load it, in out_size bytes.
 */
RpiMapResult rpi_overlay_map(Fdt *map, const char *platform, const char *name,
                             char *out, size_t out_size);

/*
 * config.txt's dtoverlay= and dtparam= lines, in their order, as the
 * firmware applies them to base. An overlay's parameters are in scope
 * until the next dtoverlay=, and the overlay is merged then, or at the
 * end; a parameter that the overlay does not declare is base's. What
 * goes wrong goes to warn, and the lines after it still apply.
 */
typedef struct RpiOverlays {
	Fdt *base;
	Fdt *map;             /* overlay_map.dtb, or NULL */
	const char *platform; /* as the map names it: "bcm2835" */
	/* Read the overlay name (no .dtbo): NULL, with why in error, if not. */
	Fdt *(*load)(void *ctx, const char *name, char *error, size_t error_size);
	RpiWarn warn;
	void *ctx;
	/* The overlay in scope, not merged yet; failed: it did not load. */
	Fdt *pending;
	char pending_name[64];
	bool failed;
} RpiOverlays;

/* dtoverlay=line: "name,param=value,...", or "" to end the scope. */
void rpi_overlays_dtoverlay(RpiOverlays *o, const char *line);

/* dtparam=line: "param=value,param,...". */
void rpi_overlays_dtparam(RpiOverlays *o, const char *line);

/* Merge the overlay in scope, if any. */
void rpi_overlays_end(RpiOverlays *o);

#endif /* BOOT_RPI_OVERLAY_H */
