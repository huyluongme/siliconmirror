/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Raspberry Pi 3 firmware's boot
 * (src/machine/raspi3b_firmware.c), on SD cards built in memory. What is
 * expected comes from the Raspberry Pi documentation (the boot sequence,
 * config.txt), from raspberrypi/firmware (issue #1415 for the load
 * address, dt-blob.dts for the pins) and from Linux's arm64 booting.rst
 * for the image header.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/fat.h"
#include "core/event.h"
#include "fat_image.h"
#include "fdt_blob.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "machine/board.h"
#include "machine/raspi3b.h"
#include "test.h"
#include "util/byteorder.h"
#include "util/fdt.h"
#include "util/macros.h"

#define MB (1024u * 1024u)
#define KERNEL_FILE "test_raspi3b_firmware_kernel.bin"

/*
 * The kernel of the tests: it stores X0, the device tree's address, at
 * 0x1000, and where it runs (the address of its third instruction) at
 * 0x1008, then waits.
 */
static const uint32_t kernel[] = {
	0xd2820001, /* mov x1, #0x1000 */
	0xf9000020, /* str x0, [x1] */
	0x10000002, /* adr x2, . */
	0xf9000422, /* str x2, [x1, #8] */
	0xd503205f, /* wfe */
	0x14000000, /* b . */
};

/* The same kernel after a Linux image header with text_offset. */
static size_t image_kernel(uint8_t *out, uint64_t text_offset)
{
	memset(out, 0, 64);
	store_le32(out, 0x14000010); /* b +64 */
	store_le64(out + 8, text_offset);
	store_le64(out + 16, 64 + sizeof(kernel));
	memcpy(out + 56, "ARM\x64", 4);
	for (size_t i = 0; i < ARRAY_SIZE(kernel); i++)
		store_le32(out + 64 + 4 * i, kernel[i]);
	return 64 + sizeof(kernel);
}

/* A device tree like the board's: the nodes the firmware changes. */
static size_t board_dtb(uint8_t **blob, uint32_t address_cells)
{
	static Builder b;
	uint8_t reg[16] = {0}; /* up to #address-cells 3 */

	memset(&b, 0, sizeof(b));
	begin(&b, "");
	prop(&b, "model", "Raspberry Pi 3 Model B", 23);
	prop_u32(&b, "#address-cells", address_cells);
	prop_u32(&b, "#size-cells", 1);
	begin(&b, "aliases");
	prop(&b, "ethernet0", "/soc/usb@7e980000/ethernet@1", 29);
	end(&b);
	begin(&b, "chosen");
	prop(&b, "bootargs", "coherent_pool=1M", 17);
	end(&b);
	begin(&b, "memory@0");
	prop(&b, "device_type", "memory", 7);
	prop(&b, "reg", reg, 4 * (address_cells + 1));
	end(&b);
	begin(&b, "soc");
	begin(&b, "usb@7e980000");
	begin(&b, "ethernet@1");
	prop_u32(&b, "reg", 1);
	end(&b);
	end(&b);
	end(&b);
	end(&b);
	return finish(&b, blob);
}

/* A device tree with only a root: the firmware adds what it needs. */
static size_t bare_dtb(uint8_t **blob)
{
	static Builder b;

	memset(&b, 0, sizeof(b));
	begin(&b, "");
	end(&b);
	return finish(&b, blob);
}

typedef struct Card {
	Fs fs;
	Dir root;
} Card;

static void add(Card *c, const char name11[11], const char *long_name,
                const void *data, size_t size)
{
	add_file(&c->fs, &c->root, name11, long_name, data, (uint32_t)size);
}

static void add_text(Card *c, const char name11[11], const char *long_name,
                     const char *text)
{
	add(c, name11, long_name, text, strlen(text));
}

/*
 * A card with what every boot needs: bootcode.bin, start.elf, and the
 * kernel as kernel8.img, unless the test leaves them out.
 */
static void card(Card *c, bool firmware, bool kernel8)
{
	format(&c->fs, 16, 16384, 1);
	add_mbr(&c->fs, 0x0c);
	c->root = root_dir(&c->fs);
	if (firmware) {
		add(c, "BOOTCODEBIN", "bootcode.bin", "", 0);
		add(c, "START   ELF", "start.elf", "", 0);
	}
	if (kernel8) {
		uint8_t words[sizeof(kernel)];

		for (size_t i = 0; i < ARRAY_SIZE(kernel); i++)
			store_le32(words + 4 * i, kernel[i]);
		add(c, "KERNEL8 IMG", "kernel8.img", words, sizeof(words));
	}
}

static void add_dtb(Card *c, const char name11[11], const char *long_name)
{
	uint8_t *blob;
	size_t size = board_dtb(&blob, 1);

	add(c, name11, long_name, blob, size);
	free(blob);
}

/* The warnings of the last boot. */
static char warnings[4096];

static void collect(void *ctx, const char *message)
{
	(void)ctx;
	strncat(warnings, message, sizeof(warnings) - strlen(warnings) - 2);
	strcat(warnings, "\n");
}

/* Boot the board from c; NULL with the message in error. */
static Machine *boot(Card *c, const char *kernel_file, char *error,
                     size_t error_size)
{
	BoardOptions options = {
		.kernel = kernel_file,
		.sd = c != NULL ? &c->fs.m.dev : NULL,
		.warn = collect,
	};

	warnings[0] = '\0';
	error[0] = '\0';
	return raspi3b_board.create(&options, error, error_size);
}

static Machine *boot_ok(Card *c)
{
	char error[256];
	Machine *m = boot(c, NULL, error, sizeof(error));

	if (m == NULL)
		fprintf(stderr, "boot: %s\n", error);
	CHECK(m != NULL);
	return m;
}

static void boot_fails(Card *c, const char *message)
{
	char error[256];
	Machine *m = boot(c, NULL, error, sizeof(error));

	CHECK(m == NULL);
	if (strstr(error, message) == NULL)
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message, error);
	CHECK(strstr(error, message) != NULL);
	machine_destroy(m);
}

static uint64_t rd(Machine *m, uint64_t address, unsigned size)
{
	uint64_t value = 0;

	CHECK_EQ(bus_read(m->bus, address, size, &value), BUS_OK);
	return value;
}

/* Run the cores until the kernel has stored X0 and its address. */
static void run_kernel(Machine *m, uint64_t *x0, uint64_t *where)
{
	CHECK_EQ(bus_write(m->bus, 0x1008, 8, 0), BUS_OK);
	machine_run(m, 200000);
	*x0 = rd(m, 0x1000, 8);
	*where = rd(m, 0x1008, 8) - 8;
}

/* The device tree that the stub hands the kernel. */
static Fdt *guest_dtb(Machine *m, uint32_t *address)
{
	uint32_t at = (uint32_t)rd(m, RASPI3B_STUB_DTB, 4);
	uint8_t header[8];
	uint8_t *blob;
	uint32_t size;
	char error[128];
	Fdt *fdt;

	*address = at;
	CHECK(at != 0);
	CHECK_EQ(bus_read_block(m->bus, at, header, 8), BUS_OK);
	size = load_be32(header + 4);
	blob = malloc(size);
	CHECK_EQ(bus_read_block(m->bus, at, blob, size), BUS_OK);
	fdt = fdt_parse(blob, size, error, sizeof(error));
	CHECK(fdt != NULL);
	free(blob);
	return fdt;
}

/* A property call through the mailboxes; returns the first value word. */
static uint32_t property(Machine *m, uint32_t tag, uint32_t arg)
{
	uint32_t buf[] = {32, 0, tag, 8, 0, arg, 0, 0};
	uint64_t now = m->events.now;

	for (size_t i = 0; i < ARRAY_SIZE(buf); i++)
		CHECK_EQ(bus_write(m->bus, 0x2000 + 4 * i, 4, buf[i]), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_MBOX_BASE + 0x20, 4, 0x2008), BUS_OK);
	event_queue_run_until(&m->events, now + 1000);
	CHECK_EQ(rd(m, RASPI3B_MBOX_BASE + 0x00, 4), 0x2008);
	return (uint32_t)rd(m, 0x2000 + 24, 4);
}

static uint32_t gpfsel(Machine *m, unsigned pin)
{
	uint64_t reg = rd(m, RASPI3B_GPIO_BASE + 4 * (pin / 10), 4);

	return (uint32_t)(reg >> (3 * (pin % 10))) & 7;
}

static bool gplev(Machine *m, unsigned pin)
{
	return (rd(m, RASPI3B_GPIO_BASE + 0x34 + 4 * (pin / 32), 4) >> (pin % 32)) &
	       1;
}

/* The way the boot ROM and the firmware find their files. */
static void test_card_errors(void)
{
	Card c;
	uint8_t gzip[] = {0x1f, 0x8b, 8, 0};

	boot_fails(NULL, "no SD card and no -kernel: the board has nothing to "
	                 "boot from");

	card(&c, true, true);
	c.fs.m.data[510] = 0;
	boot_fails(&c, "the SD card: no partition table");
	block_mem_free(&c.fs.m);

	card(&c, true, true);
	memset(c.fs.m.data + 446, 0, 64);
	boot_fails(&c, "the SD card has no partition");
	block_mem_free(&c.fs.m);

	card(&c, true, true);
	memset(c.fs.m.data + BASE, 0, 512);
	boot_fails(&c, "the first partition of the SD card: not a FAT file "
	               "system");
	block_mem_free(&c.fs.m);

	card(&c, false, true);
	boot_fails(&c, "bootcode.bin is not on the boot partition: the board's "
	               "boot ROM would stop there");
	block_mem_free(&c.fs.m);

	card(&c, false, true);
	add(&c, "BOOTCODEBIN", "bootcode.bin", "", 0);
	boot_fails(&c, "start.elf is not on the boot partition: bootcode.bin "
	               "would stop there");
	block_mem_free(&c.fs.m);

	/* The start file that config.txt asks for. */
	static const struct {
		const char *config;
		const char *start;
	} starts[] = {
		{"start_x=1\n", "start_x.elf"},
		{"start_debug=1\nstart_x=1\n", "start_db.elf"},
		{"gpu_mem=16\n", "start_cd.elf"},
		{"start_file=mine.elf\nstart_x=1\n", "mine.elf"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(starts); i++) {
		char message[96];

		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", starts[i].config);
		snprintf(message, sizeof(message), "%s is not on the boot partition",
		         starts[i].start);
		boot_fails(&c, message);
		block_mem_free(&c.fs.m);
	}

	/* 32-bit kernels. */
	card(&c, true, true);
	boot_fails(&c, "config.txt boots kernel7.img as a 32-bit kernel, but "
	               "smirror runs only 64-bit kernels: set arm_64bit=1");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\nkernel=kernel7.img\n");
	boot_fails(&c, "config.txt boots kernel7.img as a 32-bit kernel");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "kernel=u-boot.bin\n");
	boot_fails(&c, "config.txt boots u-boot.bin as a 32-bit kernel");
	block_mem_free(&c.fs.m);

	/* Kernels that cannot be loaded. */
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	boot_fails(&c, "the kernel, kernel8.img, is not on the boot partition");
	block_mem_free(&c.fs.m);
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "kernel=kernel8.img\n");
	add(&c, "KERNEL8 IMG", "kernel8.img", "", 0);
	boot_fails(&c, "the kernel, kernel8.img, is empty");
	block_mem_free(&c.fs.m);
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "kernel=kernel8.img\n");
	add(&c, "KERNEL8 IMG", "kernel8.img", gzip, sizeof(gzip));
	boot_fails(&c, "the kernel is compressed with gzip, which smirror does "
	               "not support yet");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\nkernel_address=0x3b3ffff0\n");
	boot_fails(&c, "the kernel, 0x18 bytes at 0x3b3ffff0, does not fit "
	               "between the stub and the end of the Arm's memory");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\nkernel_address=0xf0\n");
	boot_fails(&c, "the kernel, 0x18 bytes at 0xf0, does not fit");
	block_mem_free(&c.fs.m);

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\ngpu_mem=1009\n");
	boot_fails(&c, "gpu_mem=1009 leaves the Arm less than 16 MB");
	block_mem_free(&c.fs.m);

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	add_text(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", "not a dtb");
	boot_fails(&c, "bcm2710-rpi-3-b.dtb: not a devicetree blob");
	block_mem_free(&c.fs.m);

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs missing.img followkernel\n");
	boot_fails(&c, "the initramfs missing.img is not on the boot partition");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs initrd.img 0x1g\n");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	boot_fails(&c, "initramfs: 0x1g is not an address");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs initrd.img 0x3b3ffffc\n");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	boot_fails(&c, "the initramfs initrd.img does not fit in the Arm's "
	               "memory at 0x3b3ffffc");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs initrd.img 0xfc\n");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	boot_fails(&c, "the initramfs initrd.img at 0xfc overlaps the stub or the "
	               "kernel");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs initrd.img 0x80014\n");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	boot_fails(&c, "at 0x80014 overlaps the stub or the kernel");
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninitramfs initrd.img 0x7fffb\n");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	boot_fails(&c, "at 0x7fffb overlaps");
	block_mem_free(&c.fs.m);

	{
		static char line[5000];

		memset(line, 'x', sizeof(line) - 1);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add_text(&c, "CMDLINE TXT", "cmdline.txt", line);
		boot_fails(&c, "cmdline.txt is longer than 4096 characters");
		block_mem_free(&c.fs.m);
	}
}

/* A boot from the card with the usual files of an operating system. */
static void test_card_boot(void)
{
	static const char config[] = "# U-Boot\n"
								 "arm_64bit=1\n"
								 "enable_uart=1\n"
								 "dtoverlay=vc4-kms-v3d\n";
	Card c;
	Machine *m;
	Fdt *fdt;
	FdtNode *node;
	uint32_t dtb;
	uint64_t x0, where;
	const uint8_t *v;
	size_t len;

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", config);
	add_text(&c, "CMDLINE TXT", "cmdline.txt",
	         "console=serial0,115200 root=/dev/mmcblk0p2 \nsecond line\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	m = boot_ok(&c);
	CHECK(strcmp(warnings,
	             "config.txt:4: dtoverlay=vc4-kms-v3d is not applied\n") == 0);

	/* The stub: the kernel at 0x80000, the device tree in X0. */
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x80000);
	CHECK_EQ(rd(m, RASPI3B_STUB_MAGIC, 8), 0);
	fdt = guest_dtb(m, &dtb);
	CHECK_EQ(dtb % 256, 0);
	CHECK_EQ(dtb + fdt_size(fdt) <= 0x2f000000, 1);
	CHECK_EQ(dtb + fdt_size(fdt) > 0x2f000000 - 256, 1);
	run_kernel(m, &x0, &where);
	CHECK_EQ(x0, dtb);
	CHECK_EQ(where, 0x80000);

	/* The device tree: memory, command line and MAC address. */
	v = fdt_get(fdt_find(fdt, "/memory"), "reg", &len);
	CHECK(v != NULL && len == 8);
	CHECK_EQ(load_be32(v), 0);
	CHECK_EQ(load_be32(v + 4), 0x3b400000);
	v = fdt_get(fdt_find(fdt, "/chosen"), "bootargs", &len);
	CHECK(v != NULL && strcmp((const char *)v, "coherent_pool=1M "
	                                           "console=serial0,115200 "
	                                           "root=/dev/mmcblk0p2") == 0);
	node = fdt_find(fdt, "/soc/usb@7e980000/ethernet@1");
	v = fdt_get(node, "local-mac-address", &len);
	CHECK(v != NULL && len == 6);
	CHECK(memcmp(v, "\xb8\x27\xeb\xff\xee\x03", 6) == 0);
	CHECK(fdt_get(fdt_find(fdt, "/chosen"), "linux,initrd-start", &len) ==
	      NULL);
	fdt_free(fdt);

	/* The console: the mini UART on GPIO 14 and 15, the core at 250 MHz. */
	CHECK_EQ(gpfsel(m, 14), BCM2835_GPIO_ALT5);
	CHECK_EQ(gpfsel(m, 15), BCM2835_GPIO_ALT5);
	CHECK_EQ(rd(m, RASPI3B_AUX_BASE + 0x04, 4), 1);
	CHECK_EQ(rd(m, RASPI3B_AUX_BASE + 0x68, 4), 270);
	CHECK_EQ(property(m, 0x00030004, 4), 250000000);
	CHECK_EQ(property(m, 0x00010005, 0), 0x3b400000); /* from 0 */
	CHECK_EQ(rd(m, 0x2000 + 20, 4), 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
}

/* The pins as dt-blob.dts sets them, and enable_uart=0 by default. */
static void test_pins(void)
{
	Card c;
	Machine *m;

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	m = boot_ok(&c);
	CHECK(strcmp(warnings, "no device tree (bcm2710-rpi-3-b.dtb): the kernel "
	                       "gets none\n") == 0);
	CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 4), 0);
	CHECK_EQ(gpfsel(m, 14), BCM2835_GPIO_ALT0);
	CHECK_EQ(gpfsel(m, 15), BCM2835_GPIO_ALT0);
	CHECK_EQ(rd(m, RASPI3B_AUX_BASE + 0x04, 4), 0);
	for (unsigned pin = 34; pin <= 39; pin++) {
		CHECK_EQ(gpfsel(m, pin), BCM2835_GPIO_INPUT);
		CHECK(gplev(m, pin)); /* pulled up */
	}
	for (unsigned pin = 40; pin <= 43; pin++)
		CHECK_EQ(gpfsel(m, pin), BCM2835_GPIO_ALT0);
	for (unsigned pin = 44; pin <= 47; pin++) {
		CHECK_EQ(gpfsel(m, pin), BCM2835_GPIO_INPUT);
		CHECK(gplev(m, pin));
	}
	for (unsigned pin = 48; pin <= 53; pin++)
		CHECK_EQ(gpfsel(m, pin), BCM2835_GPIO_ALT0);
	/* The pull sequence ends with GPPUD and GPPUDCLKn back at 0. */
	CHECK_EQ(gpfsel(m, 13), BCM2835_GPIO_INPUT);
	CHECK_EQ(rd(m, RASPI3B_GPIO_BASE + 0x94, 4), 0);
	CHECK_EQ(rd(m, RASPI3B_GPIO_BASE + 0x98, 4), 0);
	CHECK_EQ(rd(m, RASPI3B_GPIO_BASE + 0x9c, 4), 0);
	/* Without enable_uart, the core clock may rise to 400 MHz. */
	CHECK_EQ(property(m, 0x00030004, 4), 400000000);
	/*
	 * The pulls, seen as the inputs read them: up for the pins of
	 * dt-blob.dts that it pulls up, and those that reset pulls up. The
	 * cable drives GPIO 15 high, Bluetooth GPIO 33.
	 */
	for (unsigned i = 0; i < 6; i++)
		CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 4 * i, 4, 0), BUS_OK);
	for (unsigned pin = 0; pin <= 53; pin++) {
		bool up =
			pin <= 8 || pin == 15 || (pin >= 33 && pin <= 39) || pin >= 44;

		if (gplev(m, pin) != up)
			fprintf(stderr, "pin %u\n", pin);
		CHECK_EQ(gplev(m, pin), up);
	}
	machine_destroy(m);
	block_mem_free(&c.fs.m);
}

/* Where the kernel goes: by its header, or kernel_address. */
static void test_kernel_address(void)
{
	static const struct {
		uint64_t text_offset; /* or UINT64_MAX for no header */
		const char *config;
		uint64_t expected;
	} cases[] = {
		{0, "", 0x200000},      /* Linux 5.8 and later */
		{0x80000, "", 0x80000}, /* older Linux */
		{0x10, "", 0x200010},   /* overlaps the stub: 2 MB more */
		{0x100, "", 0x100},     /* right after the stub */
		{0, "kernel_address=0x100000\n", 0x100000},
		{UINT64_MAX, "kernel_address=0x400000\n", 0x400000},
	};
	uint8_t image[64 + sizeof(kernel)];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		char config[96];
		size_t size;
		Card c;
		Machine *m;
		uint64_t x0, where;

		card(&c, true, cases[i].text_offset == UINT64_MAX);
		if (cases[i].text_offset != UINT64_MAX) {
			size = image_kernel(image, cases[i].text_offset);
			add(&c, "KERNEL8 IMG", "kernel8.img", image, size);
		}
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", cases[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		m = boot_ok(&c);
		CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), cases[i].expected);
		run_kernel(m, &x0, &where);
		CHECK_EQ(x0, 0);
		CHECK_EQ(where, cases[i].expected +
		                    (cases[i].text_offset == UINT64_MAX ? 0 : 64));
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}

	/* -kernel follows the same rule; the card is not read. */
	{
		FILE *f = fopen(KERNEL_FILE, "wb");
		size_t size = image_kernel(image, 0);
		Card c;
		Machine *m;
		char error[256];

		CHECK(f != NULL);
		CHECK_EQ(fwrite(image, 1, size, f), size);
		fclose(f);
		card(&c, false, false);
		add_text(&c, "CONFIG  TXT", "config.txt", "enable_uart=0\n");
		m = boot(&c, KERNEL_FILE, error, sizeof(error));
		CHECK(m != NULL);
		CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x200000);
		CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 4), 0);
		CHECK_EQ(gpfsel(m, 14), BCM2835_GPIO_ALT5); /* as enable_uart=1 */
		CHECK(strcmp(warnings, "") == 0);
		machine_destroy(m);
		block_mem_free(&c.fs.m);

		m = boot(NULL, KERNEL_FILE, error, sizeof(error));
		CHECK(m != NULL);
		machine_destroy(m);
		CHECK(boot(NULL, "no/such/kernel", error, sizeof(error)) == NULL);
		CHECK(strncmp(error, "no/such/kernel: ", 16) == 0);
	}
}

/* initramfs: after the kernel or at an address, from one file or more. */
static void test_initramfs(void)
{
	static const struct {
		const char *config;
		uint32_t start, end;
		const char *data;
	} cases[] = {
		{"initramfs initrd.img followkernel\n", 0x81000, 0x81006, "initrd"},
		{"initramfs initrd.img\n", 0x81000, 0x81006, "initrd"},
		{"initramfs initrd.img 0\n", 0x81000, 0x81006, "initrd"},
		{"initramfs initrd.img 0x800000\n", 0x800000, 0x800006, "initrd"},
		{"initramfs initrd.img,second.img 0x800000\n", 0x800000, 0x80000c,
	     "initrdsecond"},
		{"initramfs ,initrd.img,,second.img, 0x800000\n", 0x800000, 0x80000c,
	     "initrdsecond"},
		{"ramfsfile=second.img\nramfsaddr=0x900000\n", 0x900000, 0x900006,
	     "second"},
		{"ramfsfile=second.img\n", 0x81000, 0x81006, "second"},
		{"auto_initramfs=1\n", 0x81000, 0x81004, "auto"},
		{"auto_initramfs=1\nkernel=boot.img\n", 0, 0, NULL},
		{"auto_initramfs=0\n", 0, 0, NULL},
		{"initramfs\n", 0, 0, NULL},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		char config[160];
		Card c;
		Machine *m;
		Fdt *fdt;
		uint32_t dtb, value;
		char data[16] = "";

		card(&c, true, true);
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", cases[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
		add_text(&c, "SECOND  IMG", "second.img", "second");
		add_text(&c, "INITRA~1   ", "initramfs8", "auto");
		add(&c, "BOOT    IMG", "boot.img", kernel, sizeof(kernel));
		add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
		m = boot_ok(&c);
		fdt = guest_dtb(m, &dtb);
		if (cases[i].data == NULL) {
			size_t len;

			CHECK(fdt_get(fdt_find(fdt, "/chosen"), "linux,initrd-start",
			              &len) == NULL);
		} else {
			CHECK(fdt_get_u32(fdt_find(fdt, "/chosen"), "linux,initrd-start",
			                  &value));
			CHECK_EQ(value, cases[i].start);
			CHECK(fdt_get_u32(fdt_find(fdt, "/chosen"), "linux,initrd-end",
			                  &value));
			CHECK_EQ(value, cases[i].end);
			CHECK_EQ(bus_read_block(m->bus, cases[i].start, data,
			                        cases[i].end - cases[i].start),
			         BUS_OK);
			CHECK(strcmp(data, cases[i].data) == 0);
		}
		fdt_free(fdt);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}
}

/* Which device tree, where it goes, and what the firmware adds to it. */
static void test_device_tree(void)
{
	Card c;
	Machine *m;
	Fdt *fdt;
	uint32_t dtb;
	const uint8_t *v;
	size_t len;
	uint8_t *blob;

	/* device_tree= with no name: none. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\ndevice_tree=\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 4), 0);
	CHECK(strcmp(warnings, "") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* A name of our own; one that is missing. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ndevice_tree=my.dtb\n");
	add_dtb(&c, "MY      DTB", "my.dtb");
	m = boot_ok(&c);
	fdt = guest_dtb(m, &dtb);
	fdt_free(fdt);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ndevice_tree=missing.dtb\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 4), 0);
	CHECK(strcmp(warnings,
	             "no device tree (missing.dtb): the kernel gets none\n") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* The upstream name, when the Raspberry Pi one is missing. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	add_dtb(&c, "BCM283~1DTB", "bcm2837-rpi-3-b.dtb");
	m = boot_ok(&c);
	fdt = guest_dtb(m, &dtb);
	fdt_free(fdt);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* device_tree_address, device_tree_end, and less memory. */
	static const struct {
		const char *config;
		uint32_t end;     /* the device tree ends just below */
		uint32_t address; /* or exactly here */
		const char *error;
	} places[] = {
		{"device_tree_address=0x100000\n", 0, 0x100000, NULL},
		{"device_tree_end=0x1000000\n", 0x1000000, 0, NULL},
		{"device_tree_end=0x30000000\n", 0x2f000000, 0, NULL},
		{"gpu_mem=600\n", 0x1a800000, 0, NULL},
		{"device_tree_address=0x100004\n", 0, 0, "the device tree, 0x"},
		{"device_tree_address=0x80008\n", 0, 0,
	     "does not fit below 0x2f000000 clear of the stub, the kernel and "
	     "the initramfs"},
		{"device_tree_end=0x100\n", 0, 0, "does not fit below 0x100"},
		{"device_tree_address=0x1000\n", 0, 0x1000, NULL}, /* under it */
		{"device_tree_address=0xf8\n", 0, 0, "clear of the stub"},
		{"device_tree_address=0x7fff8\n", 0, 0, "clear of the stub"},
		{"initramfs initrd.img 0x100000\ndevice_tree_address=0x100000\n", 0, 0,
	     "the initramfs"},
		{"device_tree_address=0x2efffff8\n", 0, 0, "does not fit"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(places); i++) {
		char config[128];

		card(&c, true, true);
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", places[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
		if (places[i].error != NULL) {
			boot_fails(&c, places[i].error);
			block_mem_free(&c.fs.m);
			continue;
		}
		m = boot_ok(&c);
		fdt = guest_dtb(m, &dtb);
		if (places[i].address != 0) {
			CHECK_EQ(dtb, places[i].address);
		} else {
			CHECK_EQ(dtb % 256, 0);
			CHECK(dtb + fdt_size(fdt) <= places[i].end);
			CHECK(dtb + fdt_size(fdt) > places[i].end - 256);
		}
		fdt_free(fdt);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}

	/* A tree without the nodes: the firmware adds /memory and /chosen. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	add_text(&c, "CMDLINE TXT", "cmdline.txt", "quiet\r\n");
	add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", NULL, 0);
	{
		size_t size = bare_dtb(&blob);

		block_mem_free(&c.fs.m);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add_text(&c, "CMDLINE TXT", "cmdline.txt", "quiet\r\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
	}
	m = boot_ok(&c);
	fdt = guest_dtb(m, &dtb);
	v = fdt_get(fdt_find(fdt, "/memory@0"), "device_type", &len);
	CHECK(v != NULL && strcmp((const char *)v, "memory") == 0);
	v = fdt_get(fdt_find(fdt, "/memory@0"), "reg", &len);
	CHECK(v != NULL && len == 12); /* #address-cells 2, #size-cells 1 */
	CHECK_EQ(load_be32(v), 0);
	CHECK_EQ(load_be32(v + 4), 0);
	CHECK_EQ(load_be32(v + 8), 0x3b400000);
	v = fdt_get(fdt_find(fdt, "/chosen"), "bootargs", &len);
	CHECK(v != NULL && strcmp((const char *)v, "quiet") == 0);
	fdt_free(fdt);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* #address-cells 2, and cells the firmware cannot use. */
	{
		size_t size = board_dtb(&blob, 2);

		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
		m = boot_ok(&c);
		fdt = guest_dtb(m, &dtb);
		v = fdt_get(fdt_find(fdt, "/memory"), "reg", &len);
		CHECK(v != NULL && len == 12);
		CHECK_EQ(load_be32(v + 8), 0x3b400000);
		/* No command line: the tree's bootargs stay. */
		v = fdt_get(fdt_find(fdt, "/chosen"), "bootargs", &len);
		CHECK(v != NULL && strcmp((const char *)v, "coherent_pool=1M") == 0);
		fdt_free(fdt);
		machine_destroy(m);
		block_mem_free(&c.fs.m);

		size = board_dtb(&blob, 3);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
		boot_fails(&c, "the device tree has #address-cells 3 and #size-cells "
		               "1, which the firmware cannot use");
		block_mem_free(&c.fs.m);
	}
}

/* os_prefix: used when the kernel and the device tree are there. */
static void test_os_prefix(void)
{
	Card c;
	Dir next;
	Machine *m;
	uint8_t image[64 + sizeof(kernel)];
	size_t size = image_kernel(image, 0);
	uint8_t *blob;
	size_t dtb_size = board_dtb(&blob, 1);

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\nos_prefix=next/\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	next = add_dir(&c.fs, &c.root, "NEXT       ", "next");
	add_file(&c.fs, &next, "KERNEL8 IMG", "kernel8.img", image, (uint32_t)size);
	add_file(&c.fs, &next, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob,
	         (uint32_t)dtb_size);
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x200000); /* next/'s kernel */
	CHECK(strcmp(warnings, "") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* Without the device tree there: not used. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\nos_prefix=next/\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	next = add_dir(&c.fs, &c.root, "NEXT       ", "next");
	add_file(&c.fs, &next, "KERNEL8 IMG", "kernel8.img", image, (uint32_t)size);
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x80000);
	CHECK(strcmp(warnings, "os_prefix=next/: the kernel or the device tree is "
	                       "not there, so it is not used\n") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* With device_tree= (none), only the kernel counts. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\nos_prefix=next/\ndevice_tree=\nos_prefix=\n"
	         "os_prefix=next/\n");
	next = add_dir(&c.fs, &c.root, "NEXT       ", "next");
	add_file(&c.fs, &next, "KERNEL8 IMG", "kernel8.img", image, (uint32_t)size);
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x200000);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* A prefix of one character, which is not a directory. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\nos_prefix=x\n");
	add(&c, "XKERNE~1IMG", "xkernel8.img", image, size);
	add(&c, "XBCM27~1DTB", "xbcm2710-rpi-3-b.dtb", blob, dtb_size);
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x200000);
	CHECK(strcmp(warnings, "") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
	free(blob);
}

/* gpu_mem and total_mem, from config.txt alone. */
static void test_memory(void)
{
	static const struct {
		const char *config;
		uint32_t arm;
		const char *warning;
	} cases[] = {
		{"", 0x3b400000, ""},
		{"gpu_mem=128\n", 0x38000000, ""},
		{"gpu_mem=8\n", 0x3f000000, ""}, /* at least 16 */
		{"gpu_mem=128\ngpu_mem_1024=64\n", 0x3c000000, ""},
		{"gpu_mem_256=200\ngpu_mem_512=200\n", 0x3b400000, ""},
		{"total_mem=512\n", 0x1b400000, ""},
		{"total_mem=64\n", 0x03400000, ""}, /* at least 128 */
		{"total_mem=2048\n", 0x3b400000, ""},
		{"total_mem=127\n", 0x03400000, ""}, /* 128 */
		{"gpu_mem=15\n", 0x3f000000, ""},    /* 16 */
		{"gpu_mem=1008\n", 0x01000000, ""},  /* 16 MB left */
		{"include more.txt\n", 0x3b400000,
	     "more.txt:1: gpu_mem=128 is not applied\n"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		char config[128];
		Card c;
		Machine *m;
		uint32_t dtb;
		Fdt *fdt;
		const uint8_t *v;
		size_t len;

		card(&c, true, true);
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", cases[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		add_text(&c, "MORE    TXT", "more.txt", "gpu_mem=128\n");
		add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
		m = boot_ok(&c);
		if (strcmp(warnings, cases[i].warning) != 0)
			fprintf(stderr, "%s: %s", cases[i].config, warnings);
		CHECK(strcmp(warnings, cases[i].warning) == 0);
		CHECK_EQ(property(m, 0x00010005, 0), cases[i].arm);
		fdt = guest_dtb(m, &dtb);
		v = fdt_get(fdt_find(fdt, "/memory"), "reg", &len);
		CHECK(v != NULL && load_be32(v + 4) == cases[i].arm);
		fdt_free(fdt);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}
}

/* A reset of the board runs the firmware's boot again, from the card. */
static void test_reset(void)
{
	Card c;
	Machine *m;
	FatVolume *v;
	FatFile f;
	char error[128];
	uint64_t x0, where;

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ndtparam=audio=on\n");
	m = boot_ok(&c);
	CHECK(strstr(warnings, "dtparam=audio=on is not applied") != NULL);
	run_kernel(m, &x0, &where);
	CHECK_EQ(where, 0x80000);

	/* The kernel's first word changes on the card, and in RAM. */
	CHECK_EQ(bus_write(m->bus, 0x80000, 4, 0), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_STUB_DTB, 8, UINT64_MAX), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_STUB_MAGIC, 8, UINT64_MAX), BUS_OK);
	warnings[0] = '\0';
	m->stop = MACHINE_STOP_NONE; /* as when the guest resets the board */
	machine_reset(m);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	CHECK_EQ(rd(m, 0x80000, 4), kernel[0]);
	CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 8), (uint64_t)0x80000 << 32);
	CHECK_EQ(rd(m, RASPI3B_STUB_MAGIC, 8), 0);
	CHECK(strcmp(warnings, "") == 0); /* said once, at the first boot */
	run_kernel(m, &x0, &where);
	CHECK_EQ(where, 0x80000);

	/* bootcode.bin is gone: the board stops at the reset. */
	v = fat_open(&c.fs.m.dev, BASE, (uint64_t)c.fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(v != NULL && fat_find(v, "bootcode.bin", &f, error, sizeof(error)));
	fat_close(v);
	for (unsigned i = 0; i < 64; i++) {
		uint8_t *e = at(&c.fs, c.fs.root + 32 * i);

		if (memcmp(e, "BOOTCODEBIN", 11) == 0)
			e[0] = 0xe5;
	}
	m->stop = MACHINE_STOP_NONE;
	machine_reset(m);
	CHECK_EQ(m->stop, MACHINE_STOP_ERROR);
	CHECK(strcmp(m->stop_message, "firmware: bootcode.bin is not on the boot "
	                              "partition: the board's boot ROM would "
	                              "stop there") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
}

/* config.txt alone, with a device tree, on a card with everything. */
static void card_config(Card *c, const char *config)
{
	card(c, true, true);
	add_text(c, "CONFIG  TXT", "config.txt", config);
	add_dtb(c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
}

static const char *bootargs(Machine *m)
{
	static char text[8192];
	uint32_t dtb;
	Fdt *fdt = guest_dtb(m, &dtb);
	size_t len;
	const char *v = fdt_get(fdt_find(fdt, "/chosen"), "bootargs", &len);

	snprintf(text, sizeof(text), "%s", v != NULL ? v : "(none)");
	fdt_free(fdt);
	return text;
}

static uint32_t chosen_u32(Machine *m, const char *name)
{
	uint32_t dtb, value = 0;
	Fdt *fdt = guest_dtb(m, &dtb);

	if (!fdt_get_u32(fdt_find(fdt, "/chosen"), name, &value))
		value = 0;
	fdt_free(fdt);
	return value;
}

/*
 * Settings from a file that config.txt includes: those that the first-
 * stage bootloader reads do not count there; the others do.
 */
static void test_include(void)
{
	Card c;
	Machine *m;
	Dir next;
	uint64_t x0, where;
	uint32_t dtb;
	uint8_t image[64 + sizeof(kernel)];
	size_t size = image_kernel(image, 0);
	uint8_t *blob;
	size_t dtb_size;

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt",
	         "arm_64bit=1\ninclude extra.txt\n");
	add_text(&c, "EXTRA   TXT", "extra.txt",
	         "start_file=mine.elf\nstart_debug=1\nstart_x=1\n"
	         "fixup_file=mine.dat\ntotal_mem=512\ngpu_mem_1024=200\n"
	         "gpu_mem_256=100\ngpu_mem_512=100\n");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	m = boot_ok(&c);
	CHECK(strcmp(warnings,
	             "extra.txt:1: start_file=mine.elf is not applied\n"
	             "extra.txt:2: start_debug=1 is not applied\n"
	             "extra.txt:3: start_x=1 is not applied\n"
	             "extra.txt:4: fixup_file=mine.dat is not applied\n"
	             "extra.txt:5: total_mem=512 is not applied\n"
	             "extra.txt:6: gpu_mem_1024=200 is not applied\n"
	             "extra.txt:7: gpu_mem_256=100 is not applied\n"
	             "extra.txt:8: gpu_mem_512=100 is not applied\n") == 0);
	CHECK_EQ(property(m, 0x00010005, 0), 0x3b400000);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* The kernel, its place, the console, the command line, the tree. */
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "include extra.txt\n");
	add_text(&c, "EXTRA   TXT", "extra.txt",
	         "arm_64bit=1\nkernel=boot.img\nkernel_address=0x100000\n"
	         "enable_uart=1\ncmdline=cmd.txt\ndevice_tree=my.dtb\n"
	         "device_tree_address=0x200000\n"
	         "initramfs initrd.img 0x800000\n");
	add(&c, "BOOT    IMG", "boot.img", kernel, sizeof(kernel));
	add_text(&c, "CMD     TXT", "cmd.txt", "from extra");
	add_dtb(&c, "MY      DTB", "my.dtb");
	add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
	m = boot_ok(&c);
	CHECK(strcmp(warnings, "") == 0);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x100000);
	CHECK_EQ(rd(m, RASPI3B_STUB_DTB, 4), 0x200000);
	CHECK_EQ(gpfsel(m, 14), BCM2835_GPIO_ALT5);
	CHECK(strcmp(bootargs(m), "coherent_pool=1M from extra") == 0);
	CHECK_EQ(chosen_u32(m, "linux,initrd-start"), 0x800000);
	run_kernel(m, &x0, &where);
	CHECK_EQ(where, 0x100000);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* ramfsfile, ramfsaddr and device_tree_end. */
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "include extra.txt\n");
	add_text(&c, "EXTRA   TXT", "extra.txt",
	         "arm_64bit=1\nramfsfile=second.img\nramfsaddr=0x900000\n"
	         "device_tree_end=0x1000000\n");
	add_text(&c, "SECOND  IMG", "second.img", "second");
	add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
	m = boot_ok(&c);
	CHECK(strcmp(warnings, "") == 0);
	CHECK_EQ(chosen_u32(m, "linux,initrd-start"), 0x900000);
	dtb = (uint32_t)rd(m, RASPI3B_STUB_DTB, 4);
	CHECK(dtb < 0x1000000 && dtb > 0x1000000 - 0x1000);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* auto_initramfs, os_prefix and device_tree, from next/. */
	dtb_size = board_dtb(&blob, 1);
	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", "include extra.txt\n");
	add_text(&c, "EXTRA   TXT", "extra.txt",
	         "arm_64bit=1\nauto_initramfs=1\nos_prefix=next/\n"
	         "device_tree=my.dtb\n");
	next = add_dir(&c.fs, &c.root, "NEXT       ", "next");
	add_file(&c.fs, &next, "KERNEL8 IMG", "kernel8.img", image, (uint32_t)size);
	add_file(&c.fs, &next, "MY      DTB", "my.dtb", blob, (uint32_t)dtb_size);
	add_file(&c.fs, &next, "INITRA~1   ", "initramfs8", "auto", 4);
	free(blob);
	m = boot_ok(&c);
	CHECK(strcmp(warnings, "") == 0);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x200000);
	CHECK_EQ(chosen_u32(m, "linux,initrd-start"), 0x201000);
	CHECK_EQ(chosen_u32(m, "linux,initrd-end"), 0x201004);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
}

/* The filters that the board matches, and some it does not. */
static void test_filters(void)
{
	static const struct {
		const char *config;
		bool boots;
	} cases[] = {
		{"[pi3]\narm_64bit=1\n", true},
		{"[board-type=0x8]\narm_64bit=1\n", true},
		{"[0xc0ffee03]\narm_64bit=1\n", true},
		{"[pi4]\narm_64bit=1\n", false},
		{"[board-type=0x9]\narm_64bit=1\n", false},
		{"[board-type=0x4]\narm_64bit=1\n", false},
		{"[board-type=0x10]\narm_64bit=1\n", false},
		{"[0xc0ffee04]\narm_64bit=1\n", false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		Card c;

		card_config(&c, cases[i].config);
		if (cases[i].boots)
			machine_destroy(boot_ok(&c));
		else
			boot_fails(&c, "as a 32-bit kernel");
		block_mem_free(&c.fs.m);
	}
}

/*
 * Where the files are: the first partition that is not empty, files
 * that are directories, and a card whose reads fail.
 */
static void test_card_layout(void)
{
	Card c;
	Machine *m;
	Dir dir;

	/* Partition 1 empty, the file system in partition 2. */
	card_config(&c, "arm_64bit=1\n");
	memcpy(c.fs.m.data + 462, c.fs.m.data + 446, 16);
	memset(c.fs.m.data + 446, 0, 16);
	m = boot_ok(&c);
	CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), 0x80000);
	machine_destroy(m);
	block_mem_free(&c.fs.m);

	/* A directory is not a file. */
	card_config(&c, "arm_64bit=1\n");
	add_dir(&c.fs, &c.root, "CMDLINE TXT", "cmdline.txt");
	m = boot_ok(&c);
	CHECK(strcmp(bootargs(m), "coherent_pool=1M") == 0);
	machine_destroy(m);
	block_mem_free(&c.fs.m);
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
	dir = add_dir(&c.fs, &c.root, "KERNEL8 IMG", "kernel8.img");
	(void)dir;
	boot_fails(&c, "the kernel, kernel8.img, is not on the boot partition");
	block_mem_free(&c.fs.m);

	/* fixup_file is the start file's: config.txt may set it. */
	card_config(&c, "arm_64bit=1\nfixup_file=fixup.dat\n");
	machine_destroy(boot_ok(&c));
	CHECK(strcmp(warnings, "") == 0);
	block_mem_free(&c.fs.m);

	/* A boot that fails reports no settings as not applied. */
	card(&c, true, false);
	add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\ndtparam=x=y\n");
	boot_fails(&c, "the kernel, kernel8.img, is not on the boot partition");
	CHECK(strcmp(warnings, "") == 0);
	block_mem_free(&c.fs.m);
}

/*
 * Each read of the card that a boot makes, when it fails, makes the boot
 * fail with the reason: no error is lost on the way.
 */
static void test_read_errors(void)
{
	static const char config[] = "arm_64bit=1\ninclude extra.txt\n";
	static const char extra[] = "os_prefix=next/\ncmdline=cmd.txt\n"
								"auto_initramfs=1\nenable_uart=1\n";
	uint8_t image[64 + sizeof(kernel)];
	size_t size = image_kernel(image, 0);
	uint8_t *blob;
	size_t dtb_size = board_dtb(&blob, 1);
	unsigned reads;
	Card c;
	Dir next;

	card(&c, true, true);
	add_text(&c, "CONFIG  TXT", "config.txt", config);
	add_text(&c, "EXTRA   TXT", "extra.txt", extra);
	next = add_dir(&c.fs, &c.root, "NEXT       ", "next");
	add_file(&c.fs, &next, "KERNEL8 IMG", "kernel8.img", image, (uint32_t)size);
	add_file(&c.fs, &next, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob,
	         (uint32_t)dtb_size);
	add_file(&c.fs, &next, "CMD     TXT", "cmd.txt", "quiet", 5);
	add_file(&c.fs, &next, "INITRA~1   ", "initramfs8", "auto", 4);
	free(blob);

	c.fs.m.reads = 0;
	machine_destroy(boot_ok(&c));
	CHECK(strcmp(warnings, "") == 0);
	reads = c.fs.m.reads;
	CHECK(reads > 20);
	for (unsigned n = 0; n < reads; n++) {
		char error[256];
		Machine *m;

		c.fs.m.reads = 0;
		c.fs.m.fail_read = n;
		m = boot(&c, NULL, error, sizeof(error));
		if (m != NULL || strstr(error, "cannot read") == NULL)
			fprintf(stderr, "read %u: \"%s\"\n", n, error);
		CHECK(m == NULL);
		CHECK(strstr(error, "cannot read") != NULL);
		machine_destroy(m);
	}
	block_mem_free(&c.fs.m);
}

/* The command line: its first line, without the blanks at its end. */
static void test_cmdline_edges(void)
{
	static char x4096[4097], x4097[4098];
	static char expected[4200];
	static const struct {
		const char *file;
		const char *bootargs; /* NULL: the boot fails */
	} cases[] = {
		{"", "coherent_pool=1M"},
		{"\nquiet\n", "coherent_pool=1M"},
		{"   \n", "coherent_pool=1M"},
		{" ", "coherent_pool=1M"},
		{"quiet \t\t\n", "coherent_pool=1M quiet"},
		{"x", "coherent_pool=1M x"},
		{x4096, expected},
		{x4097, NULL},
	};

	memset(x4096, 'x', 4096);
	memset(x4097, 'x', 4097);
	snprintf(expected, sizeof(expected), "coherent_pool=1M %s", x4096);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		Card c;
		Machine *m;

		card_config(&c, "arm_64bit=1\n");
		add_text(&c, "CMDLINE TXT", "cmdline.txt", cases[i].file);
		if (cases[i].bootargs == NULL) {
			boot_fails(&c, "cmdline.txt is longer than 4096 characters");
		} else {
			m = boot_ok(&c);
			CHECK(strcmp(bootargs(m), cases[i].bootargs) == 0);
			machine_destroy(m);
		}
		block_mem_free(&c.fs.m);
	}
}

/* Kernels by name and by content, at the limits. */
static void test_kernel_edges(void)
{
	static const struct {
		const char *config;
		const char *error; /* NULL: boots */
	} names[] = {
		{"arm_64bit=1\nkernel=kernel.img\n", "boots kernel.img as a 32-bit"},
		{"arm_64bit=1\nkernel=kernel7l.img\n",
	     "boots kernel7l.img as a 32-bit"},
		{"kernel=kernel_2712.img\n", NULL},
	};
	static const struct {
		uint8_t data[64];
		size_t size;
		const char *config;
		uint64_t address; /* 0: the boot fails */
	} images[] = {
		{{0x1f, 0x8b}, 2, "", 0},
		{{0x1f}, 1, "", 0x80000},
		{{0x1f, 0x00, 0x8b}, 3, "", 0x80000},
		{{0}, 24, "kernel_address=0x3b3fffe8\n", 0x3b3fffe8},
	};
	uint8_t image[64 + sizeof(kernel)];

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		Card c;

		card_config(&c, names[i].config);
		add(&c, "KERNEL  IMG", "kernel.img", kernel, sizeof(kernel));
		add(&c, "KERNEL~1IMG", "kernel7l.img", kernel, sizeof(kernel));
		add(&c, "KERNEL~2IMG", "kernel_2712.img", kernel, sizeof(kernel));
		if (names[i].error != NULL)
			boot_fails(&c, names[i].error);
		else
			machine_destroy(boot_ok(&c));
		block_mem_free(&c.fs.m);
	}

	for (size_t i = 0; i < ARRAY_SIZE(images); i++) {
		char config[96];
		Card c;
		Machine *m;

		card(&c, true, false);
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", images[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		add(&c, "KERNEL8 IMG", "kernel8.img", images[i].data, images[i].size);
		if (images[i].address == 0) {
			boot_fails(&c, "the kernel is compressed with gzip");
		} else {
			m = boot_ok(&c);
			CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), images[i].address);
			machine_destroy(m);
		}
		block_mem_free(&c.fs.m);
	}

	/* A whole header and nothing else; one byte short of a header. */
	for (size_t len = 63; len <= 64; len++) {
		Card c;
		Machine *m;

		image_kernel(image, 0x100000);
		card(&c, true, false);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "KERNEL8 IMG", "kernel8.img", image, len);
		m = boot_ok(&c);
		CHECK_EQ(rd(m, RASPI3B_STUB_KERNEL, 4), len == 64 ? 0x100000 : 0x80000);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}
}

/* initramfs at the limits: after kernels of 4096 bytes and more. */
static void test_initramfs_edges(void)
{
	static uint8_t big[4097];
	static const struct {
		size_t kernel_size;
		const char *config;
		uint32_t start, end; /* 0, 0: none */
	} cases[] = {
		{4096, "initramfs initrd.img\n", 0x81000, 0x81006},
		{4097, "initramfs initrd.img\n", 0x82000, 0x82006},
		{24, "initramfs initrd.img 0x3b3ffffa\n", 0x3b3ffffa, 0x3b400000},
		{24, "initramfs empty.img 0x3b400000\n", 0x3b400000, 0x3b400000},
		{24, "auto_initramfs=1\n", 0, 0}, /* no initramfs8 */
		{24, "auto_initramfs=1\nkernel=kernex.img\n", 0, 0},
	};

	for (size_t i = 0; i < sizeof(kernel); i += 4)
		store_le32(big + i, kernel[i / 4]);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		char config[96];
		Card c;
		Machine *m;

		card(&c, true, false);
		snprintf(config, sizeof(config), "arm_64bit=1\n%s", cases[i].config);
		add_text(&c, "CONFIG  TXT", "config.txt", config);
		add(&c, "KERNEL8 IMG", "kernel8.img", big, cases[i].kernel_size);
		add(&c, "KERNEX  IMG", "kernex.img", big, cases[i].kernel_size);
		add_text(&c, "INITRD  IMG", "initrd.img", "initrd");
		add(&c, "EMPTY   IMG", "empty.img", "", 0);
		add_text(&c, "INITRA~1   ", "initramfs", "wrong");
		add_dtb(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb");
		m = boot_ok(&c);
		CHECK_EQ(chosen_u32(m, "linux,initrd-start"), cases[i].start);
		CHECK_EQ(chosen_u32(m, "linux,initrd-end"), cases[i].end);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}
}

/* A device tree with the given cells, bootargs and ethernet0 alias. */
static size_t custom_dtb(uint8_t **blob, uint32_t address_cells,
                         uint32_t size_cells, const char *args, size_t args_len,
                         const char *alias, size_t alias_len)
{
	static Builder b;

	memset(&b, 0, sizeof(b));
	begin(&b, "");
	prop_u32(&b, "#address-cells", address_cells);
	prop_u32(&b, "#size-cells", size_cells);
	begin(&b, "aliases");
	if (alias != NULL)
		prop(&b, "ethernet0", alias, alias_len);
	end(&b);
	begin(&b, "chosen");
	if (args != NULL)
		prop(&b, "bootargs", args, args_len);
	end(&b);
	begin(&b, "eth");
	end(&b);
	end(&b);
	return finish(&b, blob);
}

/* Device trees at the limits of what the firmware takes. */
static void test_device_tree_edges(void)
{
	static const struct {
		uint32_t address_cells, size_cells;
		const char *error;
		size_t reg_len;
	} cells[] = {
		{1, 2, NULL, 12},
		{2, 2, NULL, 16},
		{0, 1, "#address-cells 0 and #size-cells 1", 0},
		{1, 0, "#address-cells 1 and #size-cells 0", 0},
		{1, 3, "#address-cells 1 and #size-cells 3", 0},
	};
	static const struct {
		const char *args;
		size_t len;
		const char *expected;
	} args[] = {
		{"", 0, "quiet"},
		{"", 1, "quiet"},
		{"x", 2, "x quiet"},
		{"abc", 3, "quiet"}, /* not a string */
	};
	static const struct {
		const char *alias;
		size_t len;
		bool mac;
	} aliases[] = {
		{NULL, 0, false},  {"", 0, false},     {"", 1, false},
		{"/no", 4, false}, {"/eth", 4, false}, {"/eth", 5, true},
	};
	Card c;
	Machine *m;
	Fdt *fdt;
	uint32_t dtb;
	const uint8_t *v;
	size_t len, size;
	uint8_t *blob;

	/* Files of 2 MiB at most. */
	for (size_t extra = 0; extra <= 1; extra++) {
		uint8_t *big = calloc(1, 2 * MB + extra);

		size = board_dtb(&blob, 1);
		CHECK(big != NULL);
		memcpy(big, blob, size);
		free(blob);
		store_be32(big + 4, (uint32_t)(2 * MB + extra));
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", big, 2 * MB + extra);
		free(big);
		if (extra == 0) {
			m = boot_ok(&c);
			fdt = guest_dtb(m, &dtb);
			fdt_free(fdt);
			machine_destroy(m);
		} else {
			boot_fails(&c, "bcm2710-rpi-3-b.dtb is larger than 0x200000 bytes");
		}
		block_mem_free(&c.fs.m);
	}

	for (size_t i = 0; i < ARRAY_SIZE(cells); i++) {
		size = custom_dtb(&blob, cells[i].address_cells, cells[i].size_cells,
		                  NULL, 0, NULL, 0);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
		if (cells[i].error != NULL) {
			boot_fails(&c, cells[i].error);
		} else {
			m = boot_ok(&c);
			fdt = guest_dtb(m, &dtb);
			v = fdt_get(fdt_find(fdt, "/memory@0"), "reg", &len);
			CHECK(v != NULL && len == cells[i].reg_len);
			for (size_t j = 0; j + 4 < len; j += 4)
				CHECK_EQ(load_be32(v + j), 0);
			CHECK_EQ(load_be32(v + len - 4), 0x3b400000);
			fdt_free(fdt);
			machine_destroy(m);
		}
		block_mem_free(&c.fs.m);
	}

	for (size_t i = 0; i < ARRAY_SIZE(args); i++) {
		size = custom_dtb(&blob, 1, 1, args[i].args, args[i].len, NULL, 0);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add_text(&c, "CMDLINE TXT", "cmdline.txt", "quiet");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
		m = boot_ok(&c);
		CHECK(strcmp(bootargs(m), args[i].expected) == 0);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}

	for (size_t i = 0; i < ARRAY_SIZE(aliases); i++) {
		size =
			custom_dtb(&blob, 1, 1, NULL, 0, aliases[i].alias, aliases[i].len);
		card(&c, true, true);
		add_text(&c, "CONFIG  TXT", "config.txt", "arm_64bit=1\n");
		add(&c, "BCM271~1DTB", "bcm2710-rpi-3-b.dtb", blob, size);
		free(blob);
		m = boot_ok(&c);
		fdt = guest_dtb(m, &dtb);
		v = fdt_get(fdt_find(fdt, "/eth"), "local-mac-address", &len);
		CHECK_EQ(v != NULL, aliases[i].mac);
		CHECK(fdt_get(fdt_find(fdt, "/"), "local-mac-address", &len) == NULL);
		fdt_free(fdt);
		machine_destroy(m);
		block_mem_free(&c.fs.m);
	}

	/* No room below device_tree_end: the message says where it tried. */
	card_config(&c, "arm_64bit=1\ndevice_tree_end=0x100\n");
	boot_fails(&c, "bytes at 0x0, does not fit below 0x100 clear of the stub");
	block_mem_free(&c.fs.m);
}

int main(void)
{
	RUN_TEST(test_card_errors);
	RUN_TEST(test_card_boot);
	RUN_TEST(test_pins);
	RUN_TEST(test_kernel_address);
	RUN_TEST(test_initramfs);
	RUN_TEST(test_device_tree);
	RUN_TEST(test_os_prefix);
	RUN_TEST(test_memory);
	RUN_TEST(test_reset);
	RUN_TEST(test_include);
	RUN_TEST(test_filters);
	RUN_TEST(test_card_layout);
	RUN_TEST(test_read_errors);
	RUN_TEST(test_cmdline_edges);
	RUN_TEST(test_kernel_edges);
	RUN_TEST(test_initramfs_edges);
	RUN_TEST(test_device_tree_edges);
	remove(KERNEL_FILE);
	return test_summary();
}
