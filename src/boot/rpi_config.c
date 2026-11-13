/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* config.txt of the Raspberry Pi firmware; see rpi_config.h. */

#include "boot/rpi_config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"

#define LINE_LIMIT 98          /* characters a line; the rest is ignored */
#define MAX_INCLUDE_DEPTH 8    /* our limit: deeper includes are refused */
#define MAX_FILE (1024 * 1024) /* our limit on the size of a file */

/*
 * Kinds of filter: one of a kind replaces the last; a setting applies
 * when every kind in force matches.
 */
typedef enum FilterKind {
	FILTER_MODEL, /* [pi3], [cm4], [board-type=8], [none] */
	FILTER_SERIAL,
	FILTER_GPIO,
	FILTER_EDID,
	FILTER_TRYBOOT,
	FILTER_HDMI,
	FILTER_EXPRESSION,
	FILTER_UNKNOWN,
	NUM_FILTER_KINDS
} FilterKind;

typedef struct Parser {
	const RpiBoard *board;
	RpiConfig *config;
	RpiWarn warn;
	void *warn_ctx;
	FatVolume *fat; /* for include; NULL for text alone */
	bool in_force[NUM_FILTER_KINDS];
	bool matches[NUM_FILTER_KINDS];
	char *error;
	size_t error_size;
	bool failed; /* the file system is damaged */
} Parser;

static void note(Parser *p, const char *file, unsigned line, const char *fmt,
                 ...) PRINTF_FORMAT(4, 5);

static void note(Parser *p, const char *file, unsigned line, const char *fmt,
                 ...)
{
	char message[256];
	int n = snprintf(message, sizeof(message), "%s:%u: ", file, line);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(message + n, sizeof(message) - (size_t)n, fmt, ap);
	va_end(ap);
	if (p->warn != NULL)
		p->warn(p->warn_ctx, message);
}

static char *copy(const char *s, size_t len)
{
	char *c = malloc(len + 1);

	if (c != NULL) {
		memcpy(c, s, len);
		c[len] = '\0';
	}
	return c;
}

/* Parse a whole integer: decimal, or hexadecimal after 0x. */
static bool parse_uint(const char *s, uint64_t *value)
{
	int base = 10;
	uint64_t v = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	if (*s == '\0')
		return false;
	for (; *s != '\0'; s++) {
		int d;

		if (isdigit((unsigned char)*s))
			d = *s - '0';
		else if (base == 16 && isxdigit((unsigned char)*s))
			d = tolower((unsigned char)*s) - 'a' + 10;
		else
			return false;
		if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
			return false; /* too large */
		v = v * (uint64_t)base + (uint64_t)d;
	}
	*value = v;
	return true;
}

static bool is_model(const char *name)
{
	static const char *const models[] = {
		"pi1", "pi2", "pi3",  "pi3+", "pi4",  "pi5", "pi400", "pi500", "cm0",
		"cm1", "cm3", "cm3+", "cm4",  "cm4s", "cm5", "pi0",   "pi0w",  "pi02",
	};

	for (size_t i = 0; i < ARRAY_SIZE(models); i++)
		if (strcmp(name, models[i]) == 0)
			return true;
	return false;
}

static bool board_has_model(const RpiBoard *board, const char *name)
{
	for (const char *const *m = board->models; *m != NULL; m++)
		if (strcmp(*m, name) == 0)
			return true;
	return false;
}

/*
 * An expression filter, "variable" then = VALUE, & MASK, & MASK = VALUE,
 * < VALUE or > VALUE. Returns false when name is not one.
 */
static bool expression(Parser *p, const char *f, const char *file,
                       unsigned line, bool *match)
{
	static const char *const variables[] = {
		"boot_arg1", "bootvar0", "boot_count", "boot_partition", "partition",
	};
	size_t len = strcspn(f, "=&<>");
	char name[32], op = f[len];
	const char *rest = f + len + 1;
	uint64_t var = 0, a, b = 0;
	bool known = false;

	if (op == '\0' || len == 0 || len >= sizeof(name))
		return false;
	memcpy(name, f, len);
	name[len] = '\0';
	/* The customer OTP rows of the board are not programmed: 0. */
	if (strncmp(name, "cust_otp", 8) == 0 && name[8] >= '0' && name[8] <= '7' &&
	    name[9] == '\0') {
		known = true;
	} else if (strcmp(name, "partition") == 0) {
		known = true; /* PM_RSTS asks for no partition at power on */
	} else {
		bool variable = false;

		for (size_t i = 0; i < ARRAY_SIZE(variables); i++)
			variable |= strcmp(name, variables[i]) == 0;
		if (!variable)
			return false;
	}
	if (op == '&' && strchr(rest, '=') != NULL) {
		char mask[24];
		size_t mlen = strcspn(rest, "=");

		if (mlen >= sizeof(mask))
			return false;
		memcpy(mask, rest, mlen);
		mask[mlen] = '\0';
		if (!parse_uint(mask, &a) || !parse_uint(rest + mlen + 1, &b))
			return false;
		*match = known && (var & a) == b;
	} else {
		if (!parse_uint(rest, &a))
			return false;
		*match = known && (op == '='   ? var == a
		                   : op == '&' ? (var & a) != 0
		                   : op == '<' ? var < a
		                               : var > a);
	}
	if (!known)
		note(p, file, line,
		     "[%s]: %s is not set on a Raspberry Pi 3: the settings "
		     "after it are not applied",
		     f, name);
	return true;
}

static void filter(Parser *p, const char *f, const char *file, unsigned line)
{
	FilterKind kind = FILTER_UNKNOWN;
	bool match = false;
	uint64_t value;

	if (strcmp(f, "all") == 0) {
		memset(p->in_force, 0, sizeof(p->in_force));
		return;
	}
	if (strcmp(f, "none") == 0) {
		kind = FILTER_MODEL;
	} else if (is_model(f)) {
		kind = FILTER_MODEL;
		match = board_has_model(p->board, f);
	} else if (strncmp(f, "board-type=", 11) == 0 &&
	           parse_uint(f + 11, &value)) {
		kind = FILTER_MODEL;
		match = value == p->board->board_type;
	} else if (f[0] == '0' && (f[1] == 'x' || f[1] == 'X') &&
	           parse_uint(f, &value)) {
		kind = FILTER_SERIAL;
		match = value == p->board->serial;
	} else if (strncmp(f, "gpio", 4) == 0 && isdigit((unsigned char)f[4])) {
		char *end;
		unsigned long pin = strtoul(f + 4, &end, 10);

		if ((end[0] == '=' && (end[1] == '0' || end[1] == '1') &&
		     end[2] == '\0')) {
			kind = FILTER_GPIO;
			match = p->board->gpio_level != NULL && pin <= 53 &&
			        p->board->gpio_level(p->board->ctx, (unsigned)pin) ==
			            (end[1] == '1');
		}
	} else if (strncmp(f, "EDID=", 5) == 0) {
		kind = FILTER_EDID; /* no display is connected */
	} else if (strcmp(f, "tryboot") == 0) {
		kind = FILTER_TRYBOOT; /* nothing asked for a try boot */
	} else if (strncmp(f, "HDMI:", 5) == 0) {
		kind = FILTER_HDMI;
		note(p, file, line,
		     "[%s] is for the Raspberry Pi 4: the settings after it are "
		     "not applied",
		     f);
	} else if (expression(p, f, file, line, &match)) {
		kind = FILTER_EXPRESSION;
	}
	if (kind == FILTER_UNKNOWN)
		note(p, file, line,
		     "[%s] is not a known filter: the settings after it are not "
		     "applied",
		     f);
	p->in_force[kind] = true;
	p->matches[kind] = match;
}

static bool applies(const Parser *p)
{
	for (int k = 0; k < NUM_FILTER_KINDS; k++)
		if (p->in_force[k] && !p->matches[k])
			return false;
	return true;
}

static bool add(Parser *p, const char *key, size_t key_len, const char *value,
                size_t value_len, const char *file, unsigned line,
                bool included)
{
	RpiConfig *c = p->config;
	RpiSetting *grown = realloc(c->settings, (c->count + 1) * sizeof(*grown));
	RpiSetting *s;

	if (grown == NULL)
		return false;
	c->settings = grown;
	s = &c->settings[c->count];
	*s = (RpiSetting){.line = line, .included = included};
	s->key = copy(key, key_len);
	s->value = copy(value, value_len);
	s->file = copy(file, strlen(file));
	if (s->key == NULL || s->value == NULL || s->file == NULL) {
		free(s->key);
		free(s->value);
		free(s->file);
		return false;
	}
	c->count++;
	return true;
}

static bool parse_file(Parser *p, const char *file, const char *text,
                       size_t len, unsigned depth);

/* Read file from the boot partition, and parse it. */
static bool include(Parser *p, const char *name, const char *from,
                    unsigned line, unsigned depth)
{
	FatFile f;
	char *text;
	bool ok;
	int r;

	if (p->fat == NULL) {
		note(p, from, line, "include %s: there is no file system", name);
		return true;
	}
	if (depth >= MAX_INCLUDE_DEPTH) {
		note(p, from, line,
		     "include %s: files include each other more than %d deep", name,
		     MAX_INCLUDE_DEPTH);
		return true;
	}
	r = fat_find(p->fat, name, &f, p->error, p->error_size);
	if (r < 0) {
		p->failed = true;
		return false;
	}
	if (r == 0 || f.directory) {
		note(p, from, line, "include %s: there is no such file", name);
		return true;
	}
	if (f.size > MAX_FILE) {
		note(p, from, line, "include %s: the file is too large", name);
		return true;
	}
	text = malloc(f.size ? f.size : 1);
	if (text == NULL)
		return false;
	if (!fat_read(p->fat, &f, 0, text, f.size, p->error, p->error_size)) {
		free(text);
		p->failed = true;
		return false;
	}
	ok = parse_file(p, name, text, f.size, depth + 1);
	free(text);
	return ok;
}

/* One line, without its end of line. Returns false when memory runs out. */
static bool parse_line(Parser *p, const char *file, unsigned line,
                       const char *s, size_t len, unsigned depth)
{
	char *hash;
	char buf[LINE_LIMIT + 1];
	char *start, *end, *eq;

	if (len > LINE_LIMIT)
		len = LINE_LIMIT;
	memcpy(buf, s, len);
	buf[len] = '\0';
	hash = strchr(buf, '#');
	if (hash != NULL)
		*hash = '\0';
	start = buf;
	while (isspace((unsigned char)*start))
		start++;
	end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if (*start == '\0')
		return true;
	if (*start == '[') {
		char *close = strchr(start, ']');

		if (close == NULL) {
			note(p, file, line, "a filter without its ]");
			return true;
		}
		*close = '\0';
		filter(p, start + 1, file, line);
		return true;
	}
	if (!applies(p))
		return true;
	if (strncmp(start, "include", 7) == 0 && isspace((unsigned char)start[7])) {
		char *name = start + 8;

		while (isspace((unsigned char)*name))
			name++;
		return include(p, name, file, line, depth);
	}
	if (strncmp(start, "initramfs", 9) == 0 &&
	    isspace((unsigned char)start[9])) {
		char *rest = start + 10;

		while (isspace((unsigned char)*rest))
			rest++;
		return add(p, "initramfs", 9, rest, strlen(rest), file, line,
		           depth > 0);
	}
	eq = strchr(start, '=');
	if (eq == NULL || eq == start) {
		note(p, file, line, "\"%s\" is not a setting", start);
		return true;
	}
	end = eq;
	while (end > start && isspace((unsigned char)end[-1]))
		end--;
	eq++;
	while (isspace((unsigned char)*eq))
		eq++;
	return add(p, start, (size_t)(end - start), eq, strlen(eq), file, line,
	           depth > 0);
}

static bool parse_file(Parser *p, const char *file, const char *text,
                       size_t len, unsigned depth)
{
	unsigned line = 0;
	size_t pos = 0;

	while (pos < len) {
		size_t n = 0;

		/* A \r before the \n is a blank, which parse_line() trims. */
		while (pos + n < len && text[pos + n] != '\n')
			n++;
		line++;
		if (!parse_line(p, file, line, text + pos, n, depth))
			return false;
		pos += n + 1;
	}
	return true;
}

static bool run(Parser *p, const char *text, size_t len)
{
	p->config->settings = NULL;
	p->config->count = 0;
	if (parse_file(p, "config.txt", text, len, 0))
		return true;
	if (!p->failed)
		snprintf(p->error, p->error_size, "out of memory");
	rpi_config_free(p->config);
	return false;
}

bool rpi_config_parse(const char *text, size_t len, const RpiBoard *board,
                      RpiConfig *config, RpiWarn warn, void *warn_ctx)
{
	char error[64];
	Parser p = {.board = board,
	            .config = config,
	            .warn = warn,
	            .warn_ctx = warn_ctx,
	            .error = error,
	            .error_size = sizeof(error)};

	return run(&p, text, len);
}

bool rpi_config_read(FatVolume *fat, const RpiBoard *board, RpiConfig *config,
                     RpiWarn warn, void *warn_ctx, char *error,
                     size_t error_size)
{
	Parser p = {.board = board,
	            .config = config,
	            .warn = warn,
	            .warn_ctx = warn_ctx,
	            .fat = fat,
	            .error = error,
	            .error_size = error_size};
	FatFile f;
	char *text;
	bool ok;
	int r = fat_find(fat, "config.txt", &f, error, error_size);

	config->settings = NULL;
	config->count = 0;
	if (r < 0)
		return false;
	if (r == 0 || f.directory)
		return true; /* no config.txt: the defaults */
	if (f.size > MAX_FILE) {
		snprintf(error, error_size, "config.txt is larger than %d bytes",
		         MAX_FILE);
		return false;
	}
	text = malloc(f.size ? f.size : 1);
	if (text == NULL) {
		snprintf(error, error_size, "out of memory");
		return false;
	}
	if (!fat_read(fat, &f, 0, text, f.size, error, error_size)) {
		free(text);
		return false;
	}
	ok = run(&p, text, f.size);
	free(text);
	return ok;
}

void rpi_config_free(RpiConfig *config)
{
	for (size_t i = 0; i < config->count; i++) {
		free(config->settings[i].key);
		free(config->settings[i].value);
		free(config->settings[i].file);
	}
	free(config->settings);
	config->settings = NULL;
	config->count = 0;
}

const char *rpi_config_get(RpiConfig *config, const char *key,
                           bool bootloader_only)
{
	const char *value = NULL;

	for (size_t i = 0; i < config->count; i++) {
		RpiSetting *s = &config->settings[i];

		if (strcmp(s->key, key) != 0 || (bootloader_only && s->included))
			continue;
		s->used = true;
		value = s->value;
	}
	return value;
}

bool rpi_config_int(RpiConfig *config, const char *key, bool bootloader_only,
                    uint64_t *value, RpiWarn warn, void *warn_ctx)
{
	const char *text = rpi_config_get(config, key, bootloader_only);
	uint64_t v;

	if (text == NULL)
		return false;
	if (!parse_uint(text, &v)) {
		char message[160];

		snprintf(message, sizeof(message),
		         "config.txt: %s=%s: not a number, so not applied", key, text);
		if (warn != NULL)
			warn(warn_ctx, message);
		return false;
	}
	*value = v;
	return true;
}

void rpi_config_report_unused(const RpiConfig *config, RpiWarn warn,
                              void *warn_ctx)
{
	for (size_t i = 0; i < config->count; i++) {
		const RpiSetting *s = &config->settings[i];
		char message[256];

		if (s->used || warn == NULL)
			continue;
		snprintf(message, sizeof(message), "%s:%u: %s=%s is not applied",
		         s->file, s->line, s->key, s->value);
		warn(warn_ctx, message);
	}
}
