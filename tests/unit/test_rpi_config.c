/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for reading config.txt (src/boot/rpi_config.c), as the
 * Raspberry Pi documentation describes the file and its filters.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/rpi_config.h"
#include "fat_image.h"
#include "test.h"
#include "util/macros.h"

/* The board of the tests: a Raspberry Pi 3 Model B. */
static const char *const pi3_models[] = {"pi3", NULL};

static bool gpio_level(void *ctx, unsigned pin)
{
	(void)ctx;
	return pin == 4 || pin == 10 || pin == 53; /* the others are low */
}

static const RpiBoard board = {
	.models = pi3_models,
	.board_type = 0x8,
	.serial = 0xc0ffee03,
	.gpio_level = gpio_level,
};

/* The warnings, one a line. */
static char warnings[4096];

static void collect(void *ctx, const char *message)
{
	(void)ctx;
	strncat(warnings, message, sizeof(warnings) - strlen(warnings) - 2);
	strcat(warnings, "\n");
}

static void parse(const char *text, RpiConfig *c)
{
	warnings[0] = '\0';
	CHECK(rpi_config_parse(text, strlen(text), &board, c, collect, NULL));
}

/* The value of key, or "-" when it is not set. */
static const char *get(RpiConfig *c, const char *key)
{
	const char *v = rpi_config_get(c, key, false);

	return v != NULL ? v : "-";
}

static void test_format(void)
{
	RpiConfig c;
	char text[256];

	parse("# a comment\n"
	      "\n"
	      "kernel=u-boot.bin\n"
	      "  arm_64bit = 1  \r\n"
	      "enable_uart=1 # inline comment\n"
	      "cmdline=\n"
	      "initramfs  initrd.img   followkernel\n"
	      "no equals sign\n"
	      "=value\n"
	      "\tdtoverlay=disable-bt\n"
	      "kernel=kernel8.img",
	      &c);
	CHECK_EQ(c.count, 7);
	CHECK(strcmp(get(&c, "kernel"), "kernel8.img") == 0); /* the last */
	CHECK(strcmp(get(&c, "arm_64bit"), "1") == 0);
	CHECK(strcmp(get(&c, "enable_uart"), "1") == 0);
	CHECK(strcmp(get(&c, "cmdline"), "") == 0);
	CHECK(strcmp(get(&c, "initramfs"), "initrd.img   followkernel") == 0);
	CHECK(strcmp(get(&c, "missing"), "-") == 0);
	CHECK(strcmp(warnings, "config.txt:8: \"no equals sign\" is not a "
	                       "setting\nconfig.txt:9: \"=value\" is not a "
	                       "setting\n") == 0);
	CHECK_EQ(c.settings[1].line, 4);
	CHECK(strcmp(c.settings[1].file, "config.txt") == 0);
	CHECK(!c.settings[1].included);

	/* What the firmware did not look up. */
	warnings[0] = '\0';
	rpi_config_report_unused(&c, collect, NULL);
	CHECK(strcmp(warnings,
	             "config.txt:10: dtoverlay=disable-bt is not applied\n") == 0);
	rpi_config_report_unused(&c, NULL, NULL);
	rpi_config_free(&c);

	/* 98 characters a line: the rest is ignored. */
	memset(text, 'x', sizeof(text));
	memcpy(text, "cmdline=", 8);
	memcpy(text + 98, "=more\nb=1", 10);
	parse(text, &c);
	CHECK_EQ(strlen(get(&c, "cmdline")), 90);
	CHECK(strcmp(get(&c, "b"), "1") == 0);
	rpi_config_free(&c);
	rpi_config_free(&c); /* twice: nothing left */

	parse("", &c);
	CHECK_EQ(c.count, 0);
	rpi_config_free(&c);

	/* A shorter line after a longer one; blanks before =. */
	parse("abcdefgh=12345678\nx=2\ngpu_mem =16\nfoo_bar baz=1\n"
	      "includx extra.txt\ninitramfz initrd.img\n",
	      &c);
	CHECK(strcmp(warnings,
	             "config.txt:5: \"includx extra.txt\" is not a setting\n"
	             "config.txt:6: \"initramfz initrd.img\" is not a setting\n") ==
	      0);
	/* Every setting not looked up, the first one too. */
	warnings[0] = '\0';
	rpi_config_get(&c, "x", false);
	rpi_config_report_unused(&c, collect, NULL);
	CHECK(strcmp(warnings,
	             "config.txt:1: abcdefgh=12345678 is not applied\n"
	             "config.txt:3: gpu_mem=16 is not applied\n"
	             "config.txt:4: foo_bar baz=1 is not applied\n") == 0);
	CHECK(strcmp(get(&c, "x"), "2") == 0);
	CHECK(strcmp(get(&c, "gpu_mem"), "16") == 0);
	CHECK(strcmp(get(&c, "foo_bar baz"), "1") == 0);
	rpi_config_free(&c);

	/* What the caller's RpiConfig held before does not count. */
	c = (RpiConfig){.settings = NULL, .count = 7};
	parse("a=1\n", &c);
	CHECK_EQ(c.count, 1);
	rpi_config_free(&c);
}

/* Settings in a section apply when every filter in force matches. */
static void test_filters(void)
{
	static const struct {
		const char *filter;
		bool applies;
		const char *warning; /* or NULL */
	} filters[] = {
		{"[pi3]", true, NULL},
		{"[pi3+]", false, NULL},
		{"[pi4]", false, NULL},
		{"[pi0]", false, NULL},
		{"[cm3]", false, NULL},
		{"[pi2]", false, NULL},
		{"[none]", false, NULL},
		{"[board-type=0x8]", true, NULL},
		{"[board-type=8]", true, NULL},
		{"[board-type=0x11]", false, NULL},
		{"[0xc0ffee03]", true, NULL},
		{"[0XC0FFEE03]", true, NULL},
		{"[0x12345678]", false, NULL},
		{"[gpio4=1]", true, NULL},
		{"[gpio4=0]", false, NULL},
		{"[gpio5=0]", true, NULL},
		{"[gpio5=1]", false, NULL},
		{"[gpio54=0]", false, NULL},
		{"[gpio53=1]", true, NULL},
		{"[gpio10=1]", true, NULL},
		{"[gpio11=1]", false, NULL},
		{"[gpio4=1x]", false, "[gpio4=1x] is not a known filter"},
		{"[gpix4=1]", false, "[gpix4=1] is not a known filter"},
		{"[abcd1=0]", false, "[abcd1=0] is not a known filter"},
		{"[pi1]", false, NULL},
		{"[board-type:8]", false, "[board-type:8] is not a known filter"},
		{"[EDIDX]", false, "[EDIDX] is not a known filter"},
		{"[HDMIX]", false, "[HDMIX] is not a known filter"},
		{"[boot_arg1=1]", false, "boot_arg1 is not set on a Raspberry Pi 3"},
		{"[partition<0]", false, NULL},
		{"[cust_otq0=0]", false, "[cust_otq0=0] is not a known filter"},
		{"[cust_otp/=0]", false, "[cust_otp/=0] is not a known filter"},
		{"[cust_otp00=0]", false, "[cust_otp00=0] is not a known filter"},
		/* Masks of 23 characters at most. */
		{"[partition&0x000000000000000000001=0]", true, NULL},
		{"[partition&0x0000000000000000000001=0]", false, "not a known filter"},
		{"[partition&0xzz=0]", false, "[partition&0xzz=0] is not a known"},
		/* Names of 31 characters at most. */
		{"[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=1]", false, "not a known filter"},
		{"[boot_count]5", false, "[boot_count] is not a known filter"},
		{"[EDID=DEL-DELL_U2422H]", false, NULL},
		{"[tryboot]", false, NULL},
		{"[cust_otp0=0]", true, NULL},
		{"[cust_otp7=0x0]", true, NULL},
		{"[cust_otp0&0x1]", false, NULL},
		{"[cust_otp3&0x1=0]", true, NULL},
		{"[cust_otp3&0x1=1]", false, NULL},
		{"[partition=0]", true, NULL},
		{"[partition<1]", true, NULL},
		{"[partition>0]", false, NULL},
		{"[HDMI:0]", false, "[HDMI:0] is for the Raspberry Pi 4"},
		{"[boot_count=0]", false,
	     "[boot_count=0]: boot_count is not set on a Raspberry Pi 3"},
		{"[bootvar0=42]", false, "bootvar0 is not set on a Raspberry Pi 3"},
		{"[cust_otp8=0]", false, "[cust_otp8=0] is not a known filter"},
		{"[gpio4=2]", false, "[gpio4=2] is not a known filter"},
		{"[board-type=x]", false, "[board-type=x] is not a known filter"},
		{"[0x]", false, "[0x] is not a known filter"},
		{"[partition=x]", false, "[partition=x] is not a known filter"},
		{"[pi6]", false, "[pi6] is not a known filter"},
		{"[]", false, "[] is not a known filter"},
	};
	RpiConfig c;
	char text[128];

	for (unsigned i = 0; i < ARRAY_SIZE(filters); i++) {
		snprintf(text, sizeof(text), "a=1\n%s\nb=2\n[all]\nc=3\n",
		         filters[i].filter);
		parse(text, &c);
		CHECK(strcmp(get(&c, "a"), "1") == 0);
		CHECK(strcmp(get(&c, "c"), "3") == 0);
		if (strcmp(get(&c, "b"), filters[i].applies ? "2" : "-") != 0)
			fprintf(stderr, "%s\n", filters[i].filter);
		CHECK(strcmp(get(&c, "b"), filters[i].applies ? "2" : "-") == 0);
		if (filters[i].warning == NULL)
			CHECK(strcmp(warnings, "") == 0);
		else
			CHECK(strstr(warnings, filters[i].warning) != NULL);
		rpi_config_free(&c);
	}

	/* The HDMI filter says why, once. */
	parse("[HDMI:0]\na=1\n", &c);
	CHECK(strcmp(warnings, "config.txt:1: [HDMI:0] is for the Raspberry Pi 4: "
	                       "the settings after it are not applied\n") == 0);
	rpi_config_free(&c);
	/* A filter after a longer one of its kind: only its own name counts. */
	parse("[boot_partition=1]\n[partition=0]\nb=2\n"
	      "[partition&0x1zz=0]\n[all]\n[partition&0x1=0]\nc=3\n",
	      &c);
	CHECK(strcmp(get(&c, "b"), "2") == 0);
	CHECK(strcmp(get(&c, "c"), "3") == 0);
	CHECK(strcmp(warnings,
	             "config.txt:1: [boot_partition=1]: boot_partition is not set "
	             "on a Raspberry Pi 3: the settings after it are not applied\n"
	             "config.txt:4: [partition&0x1zz=0] is not a known filter: the "
	             "settings after it are not applied\n") == 0);
	rpi_config_free(&c);

	/* Filters of a kind replace each other; kinds combine. */
	parse("[pi4]\na=1\n[pi3]\nb=2\n[0x12345678]\nc=3\n[0xc0ffee03]\nd=4\n"
	      "[none]\ne=5\n[pi3]\nf=6\n[pi3\ng=7\n[all]\n",
	      &c);
	CHECK(strcmp(get(&c, "a"), "-") == 0);
	CHECK(strcmp(get(&c, "b"), "2") == 0);
	CHECK(strcmp(get(&c, "c"), "-") == 0);
	CHECK(strcmp(get(&c, "d"), "4") == 0);
	CHECK(strcmp(get(&c, "e"), "-") == 0);
	CHECK(strcmp(get(&c, "f"), "6") == 0);
	CHECK(strcmp(get(&c, "g"), "7") == 0);
	CHECK(strcmp(warnings, "config.txt:13: a filter without its ]\n") == 0);
	rpi_config_free(&c);

	/* Without GPIO, a GPIO filter never matches. */
	{
		RpiBoard no_gpio = board;

		no_gpio.gpio_level = NULL;
		CHECK(
			rpi_config_parse("[gpio5=0]\na=1\n", 15, &no_gpio, &c, NULL, NULL));
		CHECK(strcmp(get(&c, "a"), "-") == 0);
		rpi_config_free(&c);
	}
}

static void test_int(void)
{
	RpiConfig c;
	uint64_t v = 99;

	parse("a=76\nb=0x3b400000\nc=0X1f\nd=12x\ne=0x\nf=\ng=-1\n"
	      "h=18446744073709551615\ni=18446744073709551616\n",
	      &c);
	CHECK(rpi_config_int(&c, "a", false, &v, collect, NULL));
	CHECK_EQ(v, 76);
	CHECK(rpi_config_int(&c, "b", false, &v, collect, NULL));
	CHECK_EQ(v, 0x3b400000);
	CHECK(rpi_config_int(&c, "c", false, &v, collect, NULL));
	CHECK_EQ(v, 0x1f);
	CHECK(rpi_config_int(&c, "h", false, &v, collect, NULL));
	CHECK_EQ(v, UINT64_MAX);
	v = 99;
	CHECK(!rpi_config_int(&c, "missing", false, &v, collect, NULL));
	CHECK(strcmp(warnings, "") == 0);
	for (const char *k = "defgi"; *k != '\0'; k++) {
		char key[2] = {*k, '\0'};

		warnings[0] = '\0';
		CHECK(!rpi_config_int(&c, key, false, &v, collect, NULL));
		CHECK(strstr(warnings, "not a number, so not applied") != NULL);
	}
	CHECK(!rpi_config_int(&c, "d", false, &v, NULL, NULL));
	CHECK_EQ(v, 99);
	rpi_config_free(&c);

	/* Hexadecimal digits only after 0x. */
	parse("a=1a\n", &c);
	CHECK(!rpi_config_int(&c, "a", false, &v, collect, NULL));
	CHECK(strstr(warnings, "a=1a: not a number") != NULL);
	rpi_config_free(&c);
}

/* Reading from the boot partition: config.txt and the files it includes. */
static void test_read(void)
{
	static const char config[] = "gpu_mem=16\n"
								 "include extra.txt\n"
								 "include missing.txt\n"
								 "include overlays\n"
								 "[pi4]\n"
								 "include pi4.txt\n"
								 "[all]\n"
								 "include loop.txt\n";
	static const char extra[] = "gpu_mem=128\nkernel=u-boot.bin\n";
	static const char loop[] = "a=1\ninclude loop.txt\n";
	Fs fs;
	Dir root;
	FatVolume *v;
	RpiConfig c;
	char error[128];

	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	add_file(&fs, &root, "CONFIG  TXT", NULL, config, sizeof(config) - 1);
	add_file(&fs, &root, "EXTRA   TXT", NULL, extra, sizeof(extra) - 1);
	add_file(&fs, &root, "PI4     TXT", NULL, "pi4=1\n", 6);
	add_file(&fs, &root, "LOOP    TXT", NULL, loop, sizeof(loop) - 1);
	add_dir(&fs, &root, "OVERLAYS   ", NULL);
	v = fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(v != NULL);
	warnings[0] = '\0';
	CHECK(rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strstr(warnings, "config.txt:3: include missing.txt: there is no "
	                       "such file\n") != NULL);
	CHECK(strstr(warnings, "config.txt:4: include overlays: there is no "
	                       "such file\n") != NULL);
	CHECK(strstr(warnings, "loop.txt:2: include loop.txt: files include "
	                       "each other more than 8 deep\n") != NULL);
	CHECK(strcmp(get(&c, "pi4"), "-") == 0);
	CHECK(strcmp(get(&c, "kernel"), "u-boot.bin") == 0);
	/* gpu_mem only counts in config.txt itself. */
	CHECK(strcmp(rpi_config_get(&c, "gpu_mem", true), "16") == 0);
	CHECK(strcmp(rpi_config_get(&c, "gpu_mem", false), "128") == 0);
	CHECK_EQ(c.count, 2 + 1 + 8);
	CHECK(c.settings[1].included);
	CHECK(strcmp(c.settings[1].file, "extra.txt") == 0);
	CHECK_EQ(c.settings[1].line, 1);
	rpi_config_free(&c);

	/* A bootloader setting in an include, never looked up there. */
	CHECK(rpi_config_read(v, &board, &c, NULL, NULL, error, sizeof(error)));
	CHECK(strcmp(rpi_config_get(&c, "gpu_mem", true), "16") == 0);
	warnings[0] = '\0';
	rpi_config_report_unused(&c, collect, NULL);
	CHECK(strstr(warnings, "extra.txt:1: gpu_mem=128 is not applied\n") !=
	      NULL);
	rpi_config_free(&c);
	fat_close(v);
	block_mem_free(&fs.m);

	/* Without config.txt: nothing is set. */
	format(&fs, 12, 2048, 1);
	v = fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK_EQ(c.count, 0);
	root = root_dir(&fs);
	add_dir(&fs, &root, "CONFIG  TXT", NULL); /* a directory: no file */
	CHECK(rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK_EQ(c.count, 0);
	fat_close(v);
	block_mem_free(&fs.m);

	/* Text alone cannot include. */
	parse("include extra.txt\n", &c);
	CHECK(strcmp(warnings, "config.txt:1: include extra.txt: there is no file "
	                       "system\n") == 0);
	rpi_config_free(&c);
}

/* A damaged file system, and files too large. */
static void test_read_errors(void)
{
	static const char include[] = "include big.txt\ninclude bad.txt\n";
	Fs fs;
	Dir root;
	FatVolume *v;
	RpiConfig c;
	char error[128], expected[64];
	uint8_t *big = calloc(1, 1024 * 1024 + 1);
	uint32_t bad;

	format(&fs, 16, 16384, 4);
	root = root_dir(&fs);
	add_file(&fs, &root, "CONFIG  TXT", NULL, include, sizeof(include) - 1);
	add_file(&fs, &root, "BIG     TXT", NULL, big, 1024 * 1024 + 1);
	bad = add_file(&fs, &root, "BAD     TXT", NULL, "x", 1);
	v = fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(v != NULL);
	warnings[0] = '\0';
	set_fat(&fs, bad, 0xfff7);
	short_entry(at(&fs, fs.root + 32 * 2), "BAD     TXT", 0x20, bad, 4000, 16);
	CHECK(!rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	snprintf(expected, sizeof(expected), "cluster %u is marked bad", bad);
	CHECK(strcmp(error, expected) == 0);
	CHECK(strstr(warnings, "include big.txt: the file is too large") != NULL);
	CHECK_EQ(c.count, 0);
	CHECK(c.settings == NULL);

	/* config.txt itself too large, or damaged. */
	short_entry(at(&fs, fs.root), "CONFIG  TXT", 0x20, 2, 1024 * 1024 + 1, 16);
	CHECK(!rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strcmp(error, "config.txt is larger than 1048576 bytes") == 0);
	short_entry(at(&fs, fs.root), "CONFIG  TXT", 0x20, bad, 3000, 16);
	CHECK(!rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strcmp(error, expected) == 0);
	short_entry(at(&fs, fs.root), "CONFIG  TXT", 0x20, 0xfff0, 3, 16);
	CHECK(!rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strstr(error, "does not exist") != NULL);
	fat_close(v);
	block_mem_free(&fs.m);
	free(big);
}

/*
 * Includes: two blanks before the name, initramfs in each file, files
 * of 1 MiB exactly, and a lookup that fails in a directory.
 */
static void test_include_details(void)
{
	static const char config[] = "include  extra.txt\n"
								 "initramfs a.img\n"
								 "include big.txt\n"
								 "include sub/x.txt\n";
	Fs fs;
	Dir root, sub;
	FatVolume *v;
	RpiConfig c;
	char error[128];
	uint8_t *big = calloc(1, 1024 * 1024);

	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	add_file(&fs, &root, "CONFIG  TXT", NULL, config, sizeof(config) - 1);
	add_file(&fs, &root, "EXTRA   TXT", NULL, "initramfs b.img\n", 16);
	add_file(&fs, &root, "BIG     TXT", NULL, big, 1024 * 1024);
	sub = add_dir(&fs, &root, "SUB        ", NULL);
	add_file(&fs, &sub, "X       TXT", NULL, "x=1\n", 4);
	v = fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(v != NULL);
	warnings[0] = '\0';
	c = (RpiConfig){.settings = NULL, .count = 7};
	CHECK(rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strcmp(warnings, "") == 0);
	CHECK_EQ(c.count, 3);
	CHECK(strcmp(c.settings[0].value, "b.img") == 0);
	CHECK(c.settings[0].included);
	CHECK(strcmp(c.settings[1].value, "a.img") == 0);
	CHECK(!c.settings[1].included);
	CHECK(strcmp(get(&c, "x"), "1") == 0);
	rpi_config_free(&c);

	/* The directory of an include cannot be read: that error. */
	fs.m.fail_from = fs.data + (uint64_t)(sub.cluster - 2) * SECTOR;
	fs.m.fail_to = fs.m.fail_from + SECTOR;
	CHECK(!rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strstr(error, "cannot read cluster") != NULL);
	CHECK_EQ(c.count, 0);
	CHECK(c.settings == NULL);
	fs.m.fail_from = fs.m.fail_to = UINT64_MAX;
	fat_close(v);
	block_mem_free(&fs.m);

	/* config.txt of 1 MiB exactly. */
	format(&fs, 16, 16384, 1);
	root = root_dir(&fs);
	memcpy(big, "a=1\n", 4);
	add_file(&fs, &root, "CONFIG  TXT", NULL, big, 1024 * 1024);
	v = fat_open(&fs.m.dev, BASE, (uint64_t)fs.total * SECTOR, error,
	             sizeof(error));
	CHECK(rpi_config_read(v, &board, &c, collect, NULL, error, sizeof(error)));
	CHECK(strcmp(get(&c, "a"), "1") == 0);
	rpi_config_free(&c);
	fat_close(v);
	block_mem_free(&fs.m);
	free(big);
}

int main(void)
{
	RUN_TEST(test_format);
	RUN_TEST(test_filters);
	RUN_TEST(test_int);
	RUN_TEST(test_read);
	RUN_TEST(test_read_errors);
	RUN_TEST(test_include_details);
	return test_summary();
}
