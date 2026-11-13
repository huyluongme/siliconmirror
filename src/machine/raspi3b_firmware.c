/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* The Raspberry Pi 3 firmware's boot; see raspi3b_firmware.h. */

#include "machine/raspi3b_firmware.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/direct.h"
#include "boot/fat.h"
#include "boot/mbr.h"
#include "boot/rpi_config.h"
#include "machine/raspi3b.h"
#include "util/byteorder.h"
#include "util/fdt.h"
#include "util/macros.h"

/*
 * The boot stub, assembled from raspi3b_stub.s: run `make stub` after
 * changing that file.
 */
static const uint32_t stub[] = {
#include "machine/raspi3b_stub.inc"
};

static_assert(sizeof(stub) == RASPI3B_STUB_SIZE, "the stub has the wrong size");

#define MIB (1024u * 1024u)

/*
 * A 64-bit Linux image starts with a header (Linux,
 * Documentation/arch/arm64/booting.rst): text_offset at byte 8, the
 * magic "ARM\x64" at byte 56.
 */
#define IMAGE_TEXT_OFFSET 8
#define IMAGE_MAGIC_OFFSET 56
#define IMAGE_MAGIC 0x644d5241u /* "ARM\x64", little-endian */
#define IMAGE_HEADER_SIZE 64

/*
 * Where the firmware loads a 64-bit kernel without that header; with
 * it, at its text_offset, 2 MB higher if that would overlap the stub
 * (raspberrypi/firmware issue #1415).
 */
#define KERNEL_ADDRESS_64 0x80000
#define KERNEL_OVERLAP_SHIFT (2 * MIB)

/*
 * The device tree ends just below this address, or below the end of the
 * Arm's memory if that is lower: U-Boot on a real Pi 3 found it at
 * 0x2effb300 (U-Boot mailing list, May 2018). The firmware's own rule is
 * not documented.
 */
#define DTB_END 0x2f000000u
#define DTB_ALIGN 256

/* initramfs followkernel: right after the kernel, at our alignment. */
#define INITRD_ALIGN 4096

#define CMDLINE_MAX 4096 /* the longest command line; ours */

/* The registers that the firmware writes. */
#define GPPUD (RASPI3B_GPIO_BASE + 0x94)
#define GPPUDCLK0 (RASPI3B_GPIO_BASE + 0x98)
#define AUX_ENABLES (RASPI3B_AUX_BASE + 0x04)
#define AUX_MU_LCR_REG (RASPI3B_AUX_BASE + 0x4c)
#define AUX_MU_BAUD_REG (RASPI3B_AUX_BASE + 0x68)

enum { PULL_NONE, PULL_DOWN, PULL_UP };

typedef struct Boot {
	const Raspi3bFirmwareConfig *cfg;
	Machine *m;
	char *error;
	size_t error_size;
	FatVolume *fat;     /* the boot partition, from the card */
	RpiConfig config;   /* its config.txt */
	const char *prefix; /* os_prefix, or "" */
	uint32_t arm_memory;
	uint64_t kernel_start, kernel_end;
	uint64_t initrd_start, initrd_end; /* 0, 0 without an initramfs */
	char *cmdline;                     /* NULL without one */
} Boot;

static bool fail(Boot *b, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static bool fail(Boot *b, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b->error, b->error_size, fmt, ap);
	va_end(ap);
	return false;
}

static void warn(void *ctx, const char *message)
{
	Boot *b = ctx;

	if (b->cfg->warn != NULL)
		b->cfg->warn(b->cfg->warn_ctx, message);
}

static void warnf(Boot *b, const char *fmt, ...) PRINTF_FORMAT(2, 3);

static void warnf(Boot *b, const char *fmt, ...)
{
	char message[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	warn(b, message);
}

/* A setting of config.txt; none with -kernel. */
static const char *get(Boot *b, const char *key, bool bootloader_only)
{
	return b->fat != NULL ? rpi_config_get(&b->config, key, bootloader_only)
	                      : NULL;
}

static bool get_int(Boot *b, const char *key, bool bootloader_only,
                    uint64_t *value)
{
	return b->fat != NULL &&
	       rpi_config_int(&b->config, key, bootloader_only, value, warn, b);
}

/*
 * Read a file of the boot partition into a new buffer: 1 when read, 0
 * when there is no such file, -1 with a message.
 */
static int read_file(Boot *b, const char *name, uint64_t limit, uint8_t **data,
                     size_t *size)
{
	FatFile f;
	int r = fat_find(b->fat, name, &f, b->error, b->error_size);

	if (r <= 0)
		return r;
	if (f.directory)
		return 0;
	if (f.size > limit) {
		fail(b, "%s is larger than 0x%" PRIx64 " bytes", name, limit);
		return -1;
	}
	*data = malloc(f.size ? f.size : 1);
	if (*data == NULL) {
		fail(b, "out of memory");
		return -1;
	}
	if (!fat_read(b->fat, &f, 0, *data, f.size, b->error, b->error_size)) {
		free(*data);
		return -1;
	}
	*size = f.size;
	return 1;
}

static bool exists(Boot *b, const char *name, bool *found)
{
	FatFile f;
	int r = fat_find(b->fat, name, &f, b->error, b->error_size);

	*found = r == 1 && !f.directory;
	return r >= 0;
}

/* The name of a file of the operating system, after os_prefix. */
static void os_file(const Boot *b, const char *name, char *out, size_t size)
{
	snprintf(out, size, "%s%s", b->prefix, name);
}

static bool write_bus(Boot *b, uint64_t address, const void *data, size_t len,
                      const char *what)
{
	BusStatus status = bus_write_block(b->m->bus, address, data, len);

	if (status != BUS_OK)
		return fail(b, "cannot load %s at 0x%" PRIx64 ": %s", what, address,
		            bus_status_str(status));
	return true;
}

/* Whether [a, a_end) and [b, b_end) share a byte. */
static bool overlap(uint64_t a, uint64_t a_end, uint64_t b, uint64_t b_end)
{
	return a < b_end && b < a_end;
}

static bool write_reg(Boot *b, uint64_t address, uint32_t value)
{
	BusStatus status = bus_write(b->m->bus, address, 4, value);

	if (status != BUS_OK)
		return fail(b, "cannot write 0x%" PRIx64 ": %s", address,
		            bus_status_str(status));
	return true;
}

/* --- The SD card ---------------------------------------------------------- */

static bool gpio_level(void *ctx, unsigned pin)
{
	return bcm2835_gpio_level(ctx, pin);
}

/*
 * What the boot ROM and bootcode.bin do: find the boot partition, which
 * must have bootcode.bin; then read config.txt and check the start file.
 */
static bool open_card(Boot *b)
{
	static const char *const models[] = {"pi3", NULL};
	const RpiBoard board = {
		.models = models,
		.board_type = (RASPI3B_BOARD_REVISION >> 4) & 0xff,
		.serial = (uint32_t)RASPI3B_BOARD_SERIAL,
		.gpio_level = gpio_level,
		.ctx = b->cfg->gpio,
	};
	MbrPartition parts[MBR_NUM_PARTITIONS];
	const MbrPartition *boot = NULL;
	const char *start;
	uint64_t value;
	bool found;
	char why[160];

	if (b->cfg->card == NULL)
		return fail(b, "no SD card and no -kernel: the board has nothing to "
		               "boot from");
	if (!mbr_read(b->cfg->card, parts, why, sizeof(why)))
		return fail(b, "the SD card: %s", why);
	for (unsigned i = 0; i < MBR_NUM_PARTITIONS && boot == NULL; i++)
		if (parts[i].type != 0)
			boot = &parts[i];
	if (boot == NULL)
		return fail(b, "the SD card has no partition");
	b->fat = fat_open(b->cfg->card, boot->first_sector * MBR_SECTOR_SIZE,
	                  boot->sectors * MBR_SECTOR_SIZE, why, sizeof(why));
	if (b->fat == NULL)
		return fail(b, "the first partition of the SD card: %s", why);
	if (!exists(b, "bootcode.bin", &found))
		return false;
	if (!found)
		return fail(b, "bootcode.bin is not on the boot partition: the "
		               "board's boot ROM would stop there");
	if (!rpi_config_read(b->fat, &board, &b->config, warn, b, b->error,
	                     b->error_size))
		return false;

	/* The start file: start.elf unless config.txt asks for another. */
	start = get(b, "start_file", true);
	if (start == NULL && get_int(b, "start_debug", true, &value) && value)
		start = "start_db.elf";
	if (start == NULL && get_int(b, "start_x", true, &value) && value)
		start = "start_x.elf";
	if (start == NULL && get_int(b, "gpu_mem", true, &value) && value == 16)
		start = "start_cd.elf"; /* the cut-down firmware */
	if (start == NULL)
		start = "start.elf";
	if (!exists(b, start, &found))
		return false;
	if (!found)
		return fail(b,
		            "%s is not on the boot partition: bootcode.bin "
		            "would stop there",
		            start);
	get(b, "fixup_file", true); /* the start file's pair: nothing to do */
	return true;
}

/* os_prefix, if the kernel and the device tree are found with it. */
static bool choose_prefix(Boot *b, const char *kernel, const char *dtb)
{
	const char *prefix = get(b, "os_prefix", false);
	char name[256];
	bool kernel_found, dtb_found = true;

	if (prefix == NULL || prefix[0] == '\0')
		return true;
	snprintf(name, sizeof(name), "%s%s", prefix, kernel);
	if (!exists(b, name, &kernel_found))
		return false;
	if (dtb != NULL) {
		snprintf(name, sizeof(name), "%s%s", prefix, dtb);
		if (!exists(b, name, &dtb_found))
			return false;
	}
	if (kernel_found && dtb_found)
		b->prefix = prefix;
	else
		warnf(b,
		      "os_prefix=%s: the kernel or the device tree is not "
		      "there, so it is not used",
		      prefix);
	return true;
}

/* --- Memory, console and pins --------------------------------------------- */

/* gpu_mem and total_mem, which bootcode.bin reads from config.txt alone. */
static bool memory_split(Boot *b, Raspi3bVcSettings *s)
{
	uint64_t total = RASPI3B_TOTAL_MEM_MB, gpu = RASPI3B_GPU_MEM_MB;

	if (get_int(b, "total_mem", true, &total)) {
		if (total < 128)
			total = 128; /* the documentation's clamp */
		if (total > RASPI3B_TOTAL_MEM_MB)
			total = RASPI3B_TOTAL_MEM_MB;
	}
	if (!get_int(b, "gpu_mem_1024", true, &gpu))
		get_int(b, "gpu_mem", true, &gpu);
	get(b, "gpu_mem_256", true);
	get(b, "gpu_mem_512", true); /* for boards with less memory */
	if (gpu < 16)
		gpu = 16; /* the documentation's minimum */
	if (gpu + 16 > total)
		return fail(b, "gpu_mem=%" PRIu64 " leaves the Arm less than 16 MB",
		            gpu);
	s->total_mem_mb = (uint32_t)total;
	s->gpu_mem_mb = (uint32_t)gpu;
	b->arm_memory = (uint32_t)(total - gpu) * MIB;
	return true;
}

/* Set the pull of the pins in mask, as the GPPUD sequence of 6.1 does. */
static bool set_pull(Boot *b, uint64_t mask, unsigned pull)
{
	return write_reg(b, GPPUD, pull) &&
	       write_reg(b, GPPUDCLK0, (uint32_t)mask) &&
	       write_reg(b, GPPUDCLK0 + 4, (uint32_t)(mask >> 32)) &&
	       write_reg(b, GPPUD, 0) && write_reg(b, GPPUDCLK0, 0) &&
	       write_reg(b, GPPUDCLK0 + 4, 0);
}

/*
 * The pins as the firmware sets them on the Pi 3 Model B rev 1.2: the
 * pin_config of pins_3b2 in dt-blob.dts. Pins 128 and up are on the GPIO
 * expander that the firmware drives, which is not modelled.
 */
static bool set_pins(Boot *b)
{
	static const struct {
		unsigned first, last;
		Bcm2835GpioFunction function;
		unsigned pull;
	} pins[] = {
		{14, 14, BCM2835_GPIO_ALT0, PULL_NONE}, /* uart0 TXD */
		{15, 15, BCM2835_GPIO_ALT0, PULL_UP},   /* uart0 RXD */
		{34, 39, BCM2835_GPIO_INPUT, PULL_UP},  /* WiFi SDIO */
		{40, 41, BCM2835_GPIO_ALT0, PULL_NONE}, /* PWM: audio */
		{42, 43, BCM2835_GPIO_ALT0, PULL_DOWN}, /* GPCLK: Ethernet, WiFi */
		{44, 47, BCM2835_GPIO_INPUT, PULL_UP},  /* I2C 0, SMPS */
		{48, 53, BCM2835_GPIO_ALT0, PULL_UP},   /* the SD card */
	};
	uint64_t masks[3] = {0};

	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		for (unsigned p = pins[i].first; p <= pins[i].last; p++) {
			bcm2835_gpio_set_function(b->cfg->gpio, p, pins[i].function);
			masks[pins[i].pull] |= 1ull << p;
		}
	}
	return set_pull(b, masks[PULL_NONE], PULL_NONE) &&
	       set_pull(b, masks[PULL_DOWN], PULL_DOWN) &&
	       set_pull(b, masks[PULL_UP], PULL_UP);
}

/*
 * enable_uart=1 with the mini UART as the console, as on the Pi 3
 * (Raspberry Pi documentation, "Configure UARTs"): GPIO 14 and 15 to
 * ALT5, which U-Boot checks (drivers/serial/serial_bcm283x_mu.c), and the
 * mini UART on, with 8-bit characters at 115200 baud: 250 MHz /
 * (8 * (270 + 1)) is 115313 baud. The firmware's own register writes
 * are not documented.
 */
static bool console(Boot *b)
{
	bcm2835_gpio_set_function(b->cfg->gpio, 14, BCM2835_GPIO_ALT5);
	bcm2835_gpio_set_function(b->cfg->gpio, 15, BCM2835_GPIO_ALT5);
	return write_reg(b, AUX_ENABLES, 1) && write_reg(b, AUX_MU_LCR_REG, 3) &&
	       write_reg(b, AUX_MU_BAUD_REG, 270);
}

/* The first line of the command line file, without its end of line. */
static bool read_cmdline(Boot *b)
{
	const char *name = get(b, "cmdline", false);
	char path[256];
	uint8_t *data;
	size_t size, len = 0;
	int r;

	if (b->fat == NULL)
		return true;
	os_file(b, name != NULL ? name : "cmdline.txt", path, sizeof(path));
	r = read_file(b, path, UINT32_MAX, &data, &size);
	if (r < 0)
		return false;
	if (r == 0)
		return true;
	while (len < size && data[len] != '\n' && data[len] != '\r')
		len++;
	while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\t'))
		len--;
	if (len > CMDLINE_MAX) {
		free(data);
		return fail(b, "%s is longer than %d characters", path, CMDLINE_MAX);
	}
	if (len > 0) {
		b->cmdline = malloc(len + 1);
		if (b->cmdline == NULL) {
			free(data);
			return fail(b, "out of memory");
		}
		memcpy(b->cmdline, data, len);
		b->cmdline[len] = '\0';
	}
	free(data);
	return true;
}

/* --- The kernel ----------------------------------------------------------- */

/*
 * The kernel's file name, which must be a 64-bit kernel: kernel= or
 * kernel8.img, with arm_64bit (0 by default on the Pi 3), as the boot
 * options of config.txt describe them.
 */
static bool kernel_name(Boot *b, const char **name)
{
	static const struct {
		const char *name;
		bool is_64;
	} known[] = {
		{"kernel.img", false},     {"kernel7.img", false},
		{"kernel7l.img", false},   {"kernel8.img", true},
		{"kernel_2712.img", true},
	};
	const char *kernel = get(b, "kernel", false);
	uint64_t arm_64bit = 0;
	bool is_64;

	get_int(b, "arm_64bit", false, &arm_64bit);
	is_64 = arm_64bit != 0;
	if (kernel == NULL) {
		kernel = is_64 ? "kernel8.img" : "kernel7.img";
	} else {
		for (size_t i = 0; i < ARRAY_SIZE(known); i++)
			if (strcmp(kernel, known[i].name) == 0)
				is_64 = known[i].is_64;
	}
	if (!is_64)
		return fail(b,
		            "config.txt boots %s as a 32-bit kernel, but smirror "
		            "runs only 64-bit kernels: set arm_64bit=1",
		            kernel);
	*name = kernel;
	return true;
}

/* Where the kernel goes: kernel_address, or by its header. */
static uint64_t kernel_address(Boot *b, const uint8_t *image, size_t size)
{
	uint64_t address;

	if (get_int(b, "kernel_address", false, &address))
		return address;
	if (size < IMAGE_HEADER_SIZE ||
	    load_le32(image + IMAGE_MAGIC_OFFSET) != IMAGE_MAGIC)
		return KERNEL_ADDRESS_64;
	address = load_le64(image + IMAGE_TEXT_OFFSET);
	if (address < RASPI3B_STUB_SIZE)
		address += KERNEL_OVERLAP_SHIFT;
	return address;
}

/* Load the kernel: -kernel's file, or name from the card. */
static bool load_kernel(Boot *b, const char *name, uint64_t *entry)
{
	uint8_t *image;
	size_t size;
	uint64_t address;
	bool ok;

	if (b->fat == NULL) {
		if (!boot_direct_read(b->cfg->kernel, b->arm_memory, &image, &size,
		                      b->error, b->error_size))
			return false;
	} else {
		char path[256];
		int r;

		os_file(b, name, path, sizeof(path));
		r = read_file(b, path, b->arm_memory, &image, &size);
		if (r < 0)
			return false;
		if (r == 0)
			return fail(b, "the kernel, %s, is not on the boot partition",
			            path);
		if (size == 0) {
			free(image);
			return fail(b, "the kernel, %s, is empty", path);
		}
	}
	if (size >= 2 && image[0] == 0x1f && image[1] == 0x8b) {
		free(image);
		return fail(b, "the kernel is compressed with gzip, which smirror "
		               "does not support yet");
	}
	address = kernel_address(b, image, size);
	if (address < RASPI3B_STUB_SIZE || address > b->arm_memory ||
	    size > b->arm_memory - address) {
		free(image);
		return fail(b,
		            "the kernel, 0x%zx bytes at 0x%" PRIx64 ", does not "
		            "fit between the stub and the end of the Arm's memory",
		            size, address);
	}
	ok = write_bus(b, address, image, size, "the kernel");
	free(image);
	*entry = address;
	b->kernel_start = address;
	b->kernel_end = address + size;
	return ok;
}

/*
 * The initramfs: "initramfs FILES ADDRESS|followkernel", or ramfsfile
 * and ramfsaddr, or auto_initramfs. FILES may be several, with commas,
 * loaded one after the other.
 */
static bool load_initramfs(Boot *b, const char *kernel)
{
	const char *line = get(b, "initramfs", false);
	char files[256], where[64] = "followkernel";
	const char *name;
	uint64_t address, auto_initramfs = 0;

	if (line != NULL) {
		if (sscanf(line, "%255s %63s", files, where) < 1)
			return true;
	} else if (get(b, "ramfsfile", false) != NULL) {
		uint64_t at;

		snprintf(files, sizeof(files), "%s", get(b, "ramfsfile", false));
		if (get_int(b, "ramfsaddr", false, &at))
			snprintf(where, sizeof(where), "0x%" PRIx64, at);
	} else if (get_int(b, "auto_initramfs", false, &auto_initramfs) &&
	           auto_initramfs && strncmp(kernel, "kernel", 6) == 0) {
		/* kernel8.img goes with initramfs8. */
		size_t len = strcspn(kernel + 6, ".");
		bool found;
		char path[256];

		snprintf(files, sizeof(files), "initramfs%.*s", (int)len, kernel + 6);
		os_file(b, files, path, sizeof(path));
		if (!exists(b, path, &found))
			return false;
		if (!found)
			return true;
	} else {
		return true;
	}
	if (strcmp(where, "followkernel") == 0 || strcmp(where, "0") == 0) {
		address =
			(b->kernel_end + INITRD_ALIGN - 1) & ~(uint64_t)(INITRD_ALIGN - 1);
	} else {
		char *end;

		address = strtoull(where, &end, 0);
		if (*end != '\0')
			return fail(b, "initramfs: %s is not an address", where);
	}
	b->initrd_start = address;
	for (name = files; *name != '\0'; name += strspn(name, ",")) {
		size_t name_len = strcspn(name, ",");
		char path[256];
		uint8_t *data;
		size_t size;
		int r;

		if (name_len == 0)
			continue;
		snprintf(path, sizeof(path), "%s%.*s", b->prefix, (int)name_len, name);
		name += name_len;
		r = read_file(b, path, b->arm_memory, &data, &size);
		if (r < 0)
			return false;
		if (r == 0)
			return fail(b, "the initramfs %s is not on the boot partition",
			            path);
		if (address > b->arm_memory || size > b->arm_memory - address) {
			free(data);
			return fail(b,
			            "the initramfs %s does not fit in the Arm's "
			            "memory at 0x%" PRIx64,
			            path, address);
		}
		if (overlap(address, address + size, 0, RASPI3B_STUB_SIZE) ||
		    overlap(address, address + size, b->kernel_start, b->kernel_end)) {
			free(data);
			return fail(b,
			            "the initramfs %s at 0x%" PRIx64
			            " overlaps the stub or the kernel",
			            path, address);
		}
		if (!write_bus(b, address, data, size, path)) {
			free(data);
			return false;
		}
		free(data);
		address += size;
	}
	b->initrd_end = address;
	return true;
}

/* --- The device tree ------------------------------------------------------ */

/* Cells for a number: one, or two for #address-cells or #size-cells 2. */
static size_t cells(uint32_t *out, uint32_t count, uint64_t value)
{
	if (count == 2) {
		out[0] = (uint32_t)(value >> 32);
		out[1] = (uint32_t)value;
		return 2;
	}
	out[0] = (uint32_t)value;
	return 1;
}

/*
 * The values the firmware writes into the device tree: the memory, the
 * command line, the initramfs and the MAC address. Overlays and
 * parameters are not applied.
 */
static bool fixups(Boot *b, Fdt *fdt)
{
	FdtNode *root = fdt_find(fdt, "/"), *memory, *chosen, *ethernet = NULL;
	FdtNode *aliases = fdt_find(fdt, "/aliases");
	uint32_t address_cells = 2, size_cells = 1, reg[4];
	size_t n = 0, len;
	const char *bootargs;
	uint8_t mac[6];

	fdt_get_u32(root, "#address-cells", &address_cells);
	fdt_get_u32(root, "#size-cells", &size_cells);
	if (address_cells < 1 || address_cells > 2 || size_cells < 1 ||
	    size_cells > 2)
		return fail(b,
		            "the device tree has #address-cells %u and "
		            "#size-cells %u, which the firmware cannot use",
		            (unsigned)address_cells, (unsigned)size_cells);
	memory = fdt_find(fdt, "/memory");
	if (memory == NULL) {
		memory = fdt_add_node(root, "memory@0");
		if (memory == NULL || !fdt_set_string(memory, "device_type", "memory"))
			return fail(b, "out of memory");
	}
	n += cells(reg, address_cells, 0);
	n += cells(reg + n, size_cells, b->arm_memory);
	chosen = fdt_add_node(root, "chosen");
	if (chosen == NULL || !fdt_set_cells(memory, "reg", reg, n))
		return fail(b, "out of memory");

	/* The command line comes after what the device tree has. */
	bootargs = fdt_get(chosen, "bootargs", &len);
	if (b->cmdline != NULL) {
		char *joined = malloc((bootargs ? len : 0) + strlen(b->cmdline) + 2);

		if (joined == NULL)
			return fail(b, "out of memory");
		if (bootargs != NULL && len > 1 && bootargs[len - 1] == '\0')
			snprintf(joined, len + strlen(b->cmdline) + 2, "%s %s", bootargs,
			         b->cmdline);
		else
			strcpy(joined, b->cmdline);
		if (!fdt_set_string(chosen, "bootargs", joined)) {
			free(joined);
			return fail(b, "out of memory");
		}
		free(joined);
	}
	if (b->initrd_end != 0 &&
	    (!fdt_set_cells(chosen, "linux,initrd-start",
	                    (uint32_t[]){(uint32_t)b->initrd_start}, 1) ||
	     !fdt_set_cells(chosen, "linux,initrd-end",
	                    (uint32_t[]){(uint32_t)b->initrd_end}, 1)))
		return fail(b, "out of memory");

	/* The Ethernet controller, which the alias ethernet0 names. */
	if (aliases != NULL) {
		const char *path = fdt_get(aliases, "ethernet0", &len);

		if (path != NULL && len > 0 && path[len - 1] == '\0')
			ethernet = fdt_find(fdt, path);
	}
	raspi3b_property_mac(mac);
	if (ethernet != NULL && !fdt_set(ethernet, "local-mac-address", mac, 6))
		return fail(b, "out of memory");
	return true;
}

/*
 * Load the device tree, fix it up, and place it: device_tree_address, or
 * just below DTB_END. Returns its address in *address, 0 for none.
 */
static bool load_dtb(Boot *b, uint32_t *address)
{
	const char *names[] = {"bcm2710-rpi-3-b.dtb", "bcm2837-rpi-3-b.dtb"};
	const char *name = get(b, "device_tree", false);
	uint8_t *blob = NULL, *out;
	size_t size = 0, out_size;
	uint64_t at, end = b->arm_memory < DTB_END ? b->arm_memory : DTB_END;
	Fdt *fdt;
	char path[256], why[160];
	int r = 0;

	*address = 0;
	if (b->fat == NULL || (name != NULL && name[0] == '\0'))
		return true; /* no device tree: X0 is 0 */
	for (size_t i = 0; i < ARRAY_SIZE(names) && r == 0; i++) {
		os_file(b, name != NULL ? name : names[i], path, sizeof(path));
		r = read_file(b, path, 2 * MIB, &blob, &size);
		if (name != NULL)
			break;
	}
	if (r < 0)
		return false;
	if (r == 0) {
		warnf(b, "no device tree (%s): the kernel gets none",
		      name != NULL ? path : "bcm2710-rpi-3-b.dtb");
		return true;
	}
	fdt = fdt_parse(blob, size, why, sizeof(why));
	free(blob);
	if (fdt == NULL)
		return fail(b, "%s: %s", path, why);
	if (!fixups(b, fdt)) {
		fdt_free(fdt);
		return false;
	}
	out_size = fdt_size(fdt);
	out = malloc(out_size);
	if (out == NULL || !fdt_write(fdt, out, out_size)) {
		free(out);
		fdt_free(fdt);
		return fail(b, "out of memory");
	}
	fdt_free(fdt);
	if (get_int(b, "device_tree_end", false, &at) && at < end)
		end = at;
	if (!get_int(b, "device_tree_address", false, &at))
		at = end > out_size ? (end - out_size) & ~(uint64_t)(DTB_ALIGN - 1) : 0;
	/* 8-byte aligned (Devicetree Specification, 5.6), and in the clear. */
	if (at % 8 != 0 || at > end || out_size > end - at ||
	    overlap(at, at + out_size, 0, RASPI3B_STUB_SIZE) ||
	    overlap(at, at + out_size, b->kernel_start, b->kernel_end) ||
	    overlap(at, at + out_size, b->initrd_start, b->initrd_end)) {
		free(out);
		return fail(b,
		            "the device tree, 0x%zx bytes at 0x%" PRIx64 ", does "
		            "not fit below 0x%" PRIx64
		            " clear of the stub, the kernel and the initramfs",
		            out_size, at, end);
	}
	if (!write_bus(b, at, out, out_size, "the device tree")) {
		free(out);
		return false;
	}
	free(out);
	*address = (uint32_t)at;
	return true;
}

/* --- The stub ------------------------------------------------------------- */

/*
 * The boot stub at 0, which starts core 0 at the kernel with the device
 * tree's address in X0. The magic number and the version are also the
 * spin table entry of core 3, which must read 0 until the operating
 * system releases it.
 */
static bool load_stub(Boot *b, uint64_t kernel, uint32_t dtb)
{
	uint8_t words[8];

	if (!write_bus(b, RASPI3B_STUB_ADDRESS, stub, sizeof(stub), "the stub"))
		return false;
	memset(words, 0, sizeof(words));
	if (!write_bus(b, RASPI3B_STUB_MAGIC, words, 8, "the stub"))
		return false;
	store_le32(words, dtb);
	store_le32(words + 4, (uint32_t)kernel);
	return write_bus(b, RASPI3B_STUB_DTB, words, 8, "the stub");
}

bool raspi3b_firmware_boot(const Raspi3bFirmwareConfig *config, char *error,
                           size_t error_size)
{
	Boot b = {
		.cfg = config,
		.m = config->machine,
		.error = error,
		.error_size = error_size,
		.prefix = "",
	};
	Raspi3bVcSettings settings = {0};
	uint64_t enable_uart = 1, kernel = 0;
	uint32_t dtb = 0;
	const char *kernel_file = NULL;
	unsigned partition;
	bool ok;

	/*
	 * bootcode.bin reads the partition in the watchdog's RSTS first: 63
	 * asks it not to boot, which is how the board powers off (Linux's
	 * bcm2835_wdt.c).
	 */
	partition = config->pm != NULL ? bcm2835_pm_partition(config->pm) : 0;
	if (partition == BCM2835_PM_HALT_PARTITION) {
		machine_stop(config->machine, MACHINE_STOP_SHUTDOWN,
		             "the board halted: RSTS gives the firmware partition "
		             "63, which it does not boot");
		return true;
	}
	if (partition != 0)
		return fail(&b,
		            "booting from partition %u, which RSTS asks for, "
		            "is not emulated",
		            partition);

	/* With -kernel, the firmware boots as with enable_uart=1 alone. */
	ok = config->kernel != NULL || open_card(&b);
	if (ok && b.fat != NULL) {
		enable_uart = 0; /* the Pi 3's console is the mini UART */
		get_int(&b, "enable_uart", false, &enable_uart);
		ok = kernel_name(&b, &kernel_file);
		if (ok) {
			const char *dt = get(&b, "device_tree", false);

			ok = choose_prefix(&b, kernel_file,
			                   dt == NULL ? "bcm2710-rpi-3-b.dtb"
			                   : dt[0]    ? dt
			                              : NULL);
		}
	}
	ok = ok && memory_split(&b, &settings) && read_cmdline(&b);
	if (ok) {
		settings.core_freq_fixed = enable_uart != 0;
		settings.cmdline = b.cmdline != NULL ? b.cmdline : "";
		if (!raspi3b_property_configure(config->property, &settings))
			ok = fail(&b, "out of memory");
	}
	ok = ok && set_pins(&b) && (!enable_uart || console(&b)) &&
	     load_kernel(&b, kernel_file, &kernel) &&
	     load_initramfs(&b, kernel_file != NULL ? kernel_file : "") &&
	     load_dtb(&b, &dtb) && load_stub(&b, kernel, dtb);
	if (ok && b.fat != NULL)
		rpi_config_report_unused(&b.config, warn, &b);
	rpi_config_free(&b.config);
	fat_close(b.fat);
	free(b.cmdline);
	return ok;
}
