/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Check the sound of the Raspberry Pi OS session (session.txt): what
 * smirror's -audio wav: kept of the two files that aplay played on the
 * headphones.
 *
 * Usage: audio_check CARD FIRST.wav SECOND.wav
 *
 * Without CARD, the session did not run: exit status 77. FIRST must be
 * 48000 Hz, two channels of 16 bits, at 0 dB: the 12000 frames of the
 * session's first file as they are; SECOND, made after it, 8000 Hz, one
 * channel of 8 bits, at -20 dB: the 2000 frames of the second, each a
 * tenth as far from 128, rounded to the nearest. After them, less than
 * a second of silence: aplay fills its last period with it.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FIRST_FRAMES 12000u
#define SECOND_FRAMES 2000u

static uint8_t *data;
static uint32_t data_size;

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return le16(p) | le16(p + 2) << 16;
}

static bool fail(const char *path, const char *why)
{
	fprintf(stderr, "audio_check: %s: %s\n", path, why);
	return false;
}

/* Read a WAV file of PCM in format; its samples into data. */
static bool read_wav(const char *path, uint32_t channels, uint32_t rate,
                     uint32_t bits)
{
	uint8_t h[44];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return fail(path, "missing");
	n = fread(h, 1, sizeof(h), f);
	if (n != sizeof(h) || memcmp(h, "RIFF", 4) != 0 ||
	    memcmp(h + 8, "WAVEfmt ", 8) != 0 || le32(h + 16) != 16 ||
	    le16(h + 20) != 1 || memcmp(h + 36, "data", 4) != 0) {
		fclose(f);
		return fail(path, "not a WAV file of PCM");
	}
	if (le16(h + 22) != channels || le32(h + 24) != rate ||
	    le16(h + 34) != bits) {
		fclose(f);
		fprintf(stderr, "audio_check: %s: %u channels, %u Hz, %u bits\n", path,
		        le16(h + 22), le32(h + 24), le16(h + 34));
		return false;
	}
	data_size = le32(h + 40);
	free(data);
	data = malloc(data_size + 1);
	n = data != NULL ? fread(data, 1, data_size, f) : 0;
	fclose(f);
	if (n != data_size)
		return fail(path, "shorter than its data chunk");
	return true;
}

/* x a tenth as far from 0, rounded to the nearest, halves away. */
static int tenth(int x)
{
	double v = x * 0.1;

	return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

static bool check_first(const char *path)
{
	uint32_t frames;

	if (!read_wav(path, 2, 48000, 16))
		return false;
	frames = data_size / 4;
	if (frames < FIRST_FRAMES || frames >= FIRST_FRAMES + 48000)
		return fail(path, "not 12000 frames and less than a second more");
	for (uint32_t i = 0; i < frames; i++) {
		uint32_t left = i < FIRST_FRAMES ? (i * 37) & 0xffff : 0;
		uint32_t right = i < FIRST_FRAMES ? (i * 101 + 5) & 0xffff : 0;

		if (le16(data + 4 * i) != left || le16(data + 4 * i + 2) != right) {
			fprintf(stderr, "audio_check: %s: frame %u differs\n", path, i);
			return false;
		}
	}
	return true;
}

static bool check_second(const char *path)
{
	if (!read_wav(path, 1, 8000, 8))
		return false;
	if (data_size < SECOND_FRAMES || data_size >= SECOND_FRAMES + 8000)
		return fail(path, "not 2000 frames and less than a second more");
	for (uint32_t i = 0; i < data_size; i++) {
		int x = i < SECOND_FRAMES ? (int)((i * 13) & 0xff) : 128;

		if (data[i] != tenth(x - 128) + 128) {
			fprintf(stderr, "audio_check: %s: frame %u differs\n", path, i);
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	struct stat card, first, second;
	bool ok;

	if (argc != 4) {
		fputs("usage: audio_check CARD FIRST.wav SECOND.wav\n", stderr);
		return 2;
	}
	if (stat(argv[1], &card) != 0) {
		printf("audio_check: no card %s: skipped\n", argv[1]);
		return 77;
	}
	/* The second, from this session: made after the first. */
	if (stat(argv[2], &first) != 0 || stat(argv[3], &second) != 0) {
		fputs("audio_check: a WAV file is missing\n", stderr);
		return 1;
	}
	if (second.st_mtime < first.st_mtime) {
		fprintf(stderr, "audio_check: %s is older than %s\n", argv[3], argv[2]);
		return 1;
	}
	ok = check_first(argv[2]) && check_second(argv[3]);
	free(data);
	if (ok)
		puts("audio_check: both files as aplay played them");
	return ok ? 0 : 1;
}
