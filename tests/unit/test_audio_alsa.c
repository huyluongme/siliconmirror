/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the ALSA audio output (src/backend/audio/alsa.c). No
 * sound card is needed: ALSA's own "file" PCM (alsa.conf) writes what it
 * is given to a file, raw, and plays it to its "null" PCM.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "backend/audio/alsa.h"
#include "test.h"

#define RAW "test_audio_alsa.raw"
#define DEVICE "file:FILE=" RAW ",FORMAT=raw"

static uint8_t file[256];

static long read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(file, 1, sizeof(file), f);
	fclose(f);
	return (long)n;
}

static AudioOut *open_device(void)
{
	char error[200];
	AudioOut *a = audio_alsa_open(DEVICE, error, sizeof(error));

	CHECK(a != NULL);
	return a;
}

/* Each stream's samples reach the device, as they are. */
static void test_streams(void)
{
	static const uint8_t s16[16] = {1, 2,  3,  4,  5,  6,  7,  8,
	                                9, 10, 11, 12, 13, 14, 15, 16};
	AudioOut *a;

	unlink(RAW);
	a = open_device();
	CHECK_EQ(read_file(RAW), -1); /* opened, to see it is there, only */
	CHECK(a->ops->start(a, &(AudioFormat){2, 48000, 16}));
	CHECK(a->ops->play(a, s16, 8));
	CHECK(a->ops->play(a, s16 + 8, 8));
	CHECK(a->ops->stop(a));
	CHECK_EQ(read_file(RAW), 16);
	CHECK(memcmp(file, s16, 16) == 0);

	/* 8 bits, mono; a stream that starts closes the one before. */
	CHECK(a->ops->start(a, &(AudioFormat){1, 8000, 8}));
	CHECK(a->ops->play(a, (const uint8_t *)"\x80\x81\x82", 3));
	CHECK(a->ops->start(a, &(AudioFormat){1, 8000, 8}));
	CHECK_EQ(read_file(RAW), 0);
	CHECK(a->ops->play(a, (const uint8_t *)"\x83\x84", 2));
	CHECK(a->ops->play(a, (const uint8_t *)"\x85", 1)); /* one frame */
	audio_destroy(a);                                   /* drains */
	CHECK_EQ(read_file(RAW), 3);
	CHECK(memcmp(file, "\x83\x84\x85", 3) == 0);
	CHECK(a != NULL);
	unlink(RAW);
}

/* What the host refuses. */
static void test_host_failures(void)
{
	char error[200];
	AudioOut *a;

	CHECK(audio_alsa_open("smirror_no_such_pcm", error, sizeof(error)) == NULL);
	CHECK(strncmp(error, "cannot open ALSA device smirror_no_such_pcm: ", 45) ==
	      0);

	a = open_device();
	CHECK(!a->ops->start(a, &(AudioFormat){0, 48000, 16}));
	CHECK(strncmp(a->error, "ALSA " DEVICE " setup: ", 30) == 0);
	CHECK(a->ops->stop(a)); /* nothing open */
	audio_destroy(a);
	unlink(RAW);
}

int main(void)
{
	RUN_TEST(test_streams);
	RUN_TEST(test_host_failures);
	return test_summary();
}
