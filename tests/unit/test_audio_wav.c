/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the WAV audio output (src/backend/audio/wav.c), against
 * the WAVE layout of the McGill "Audio File Format Specifications" page.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend/audio/wav.h"
#include "test.h"
#include "util/byteorder.h"

#define WAV "test_audio_wav.wav"
#define WAV1 "test_audio_wav-1.wav"
#define WAV2 "test_audio_wav-2.wav"

static const AudioFormat stereo16 = {2, 48000, 16};
static const AudioFormat mono8 = {1, 8000, 8};
static const AudioFormat eight16 = {8, 44100, 16};

static uint8_t file[1024];

/* The file's bytes into file[]; its size, or -1 when it is not there. */
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

static void clean(void)
{
	unlink(WAV);
	unlink(WAV1);
	unlink(WAV2);
}

static AudioOut *open_wav(const char *path)
{
	char error[200];
	AudioOut *a = audio_wav_open(path, error, sizeof(error));

	CHECK(a != NULL);
	return a;
}

/* RIFF, WAVE, then fmt of PCM for format, the data of size bytes. */
static void check_pcm_header(const AudioFormat *f, uint32_t data)
{
	uint32_t frame = f->channels * f->bits / 8;

	CHECK(memcmp(file, "RIFF", 4) == 0);
	CHECK_EQ(load_le32(file + 4), 36 + data + (data & 1));
	CHECK(memcmp(file + 8, "WAVEfmt ", 8) == 0);
	CHECK_EQ(load_le32(file + 16), 16);
	CHECK_EQ(load_le16(file + 20), 1); /* WAVE_FORMAT_PCM */
	CHECK_EQ(load_le16(file + 22), f->channels);
	CHECK_EQ(load_le32(file + 24), f->rate);
	CHECK_EQ(load_le32(file + 28), f->rate * frame);
	CHECK_EQ(load_le16(file + 32), frame);
	CHECK_EQ(load_le16(file + 34), f->bits);
	CHECK(memcmp(file + 36, "data", 4) == 0);
	CHECK_EQ(load_le32(file + 40), data);
}

/* PATH is created, empty, with the output, and an old one emptied. */
static void test_created_empty(void)
{
	FILE *f;
	AudioOut *a;

	clean();
	f = fopen(WAV, "wb");
	CHECK(f != NULL);
	fputs("old", f);
	fclose(f);
	a = open_wav(WAV);
	CHECK_EQ(read_file(WAV), 0);
	audio_destroy(a);
	CHECK_EQ(read_file(WAV), 0);
	CHECK_EQ(read_file(WAV1), -1);
}

/* The header is whole after every write. */
static void test_stereo(void)
{
	static const uint8_t samples[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	AudioOut *a;

	clean();
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &stereo16));
	CHECK_EQ(read_file(WAV), 44);
	check_pcm_header(&stereo16, 0);
	CHECK(a->ops->play(a, samples, 8));
	CHECK_EQ(read_file(WAV), 52);
	check_pcm_header(&stereo16, 8);
	CHECK(memcmp(file + 44, samples, 8) == 0);
	CHECK(a->ops->play(a, samples + 8, 4));
	CHECK_EQ(read_file(WAV), 56);
	check_pcm_header(&stereo16, 12);
	CHECK(memcmp(file + 44, samples, 12) == 0);
	CHECK(a->ops->stop(a));
	audio_destroy(a);
	CHECK_EQ(read_file(WAV), 56);
	clean();
}

/* Data of odd size: a pad byte, which the next samples overwrite. */
static void test_pad_byte(void)
{
	AudioOut *a;

	clean();
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &mono8));
	CHECK(a->ops->play(a, (const uint8_t *)"\x80\x81\x82", 3));
	CHECK_EQ(read_file(WAV), 48);
	check_pcm_header(&mono8, 3);
	CHECK(memcmp(file + 44, "\x80\x81\x82\0", 4) == 0);
	CHECK(a->ops->play(a, (const uint8_t *)"\x83", 1));
	CHECK_EQ(read_file(WAV), 48);
	check_pcm_header(&mono8, 4);
	CHECK(memcmp(file + 44, "\x80\x81\x82\x83", 4) == 0);
	CHECK(a->ops->play(a, (const uint8_t *)"\x84", 1));
	CHECK_EQ(read_file(WAV), 50);
	check_pcm_header(&mono8, 5);
	CHECK(memcmp(file + 44, "\x80\x81\x82\x83\x84\0", 6) == 0);
	audio_destroy(a);
	clean();
}

/* More than two channels: WAVE_FORMAT_EXTENSIBLE. */
static void check_extensible(const AudioFormat *f, uint32_t data)
{
	static const uint8_t subformat[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	                                      0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
	                                      0x00, 0x38, 0x9b, 0x71};
	uint32_t frame = f->channels * f->bits / 8;

	CHECK(memcmp(file, "RIFF", 4) == 0);
	CHECK_EQ(load_le32(file + 4), 60 + data + (data & 1));
	CHECK(memcmp(file + 8, "WAVEfmt ", 8) == 0);
	CHECK_EQ(load_le32(file + 16), 40);
	CHECK_EQ(load_le16(file + 20), 0xfffe);
	CHECK_EQ(load_le16(file + 22), f->channels);
	CHECK_EQ(load_le32(file + 24), f->rate);
	CHECK_EQ(load_le32(file + 28), f->rate * frame);
	CHECK_EQ(load_le16(file + 32), frame);
	CHECK_EQ(load_le16(file + 34), f->bits);
	CHECK_EQ(load_le16(file + 36), 22); /* cbSize */
	CHECK_EQ(load_le16(file + 38), f->bits);
	CHECK_EQ(load_le32(file + 40), 0); /* channels in order */
	CHECK(memcmp(file + 44, subformat, 16) == 0);
	CHECK(memcmp(file + 60, "data", 4) == 0);
	CHECK_EQ(load_le32(file + 64), data);
}

static void test_extensible(void)
{
	static const AudioFormat three8 = {3, 22050, 8};
	uint8_t frame[16];
	AudioOut *a;

	for (unsigned i = 0; i < sizeof(frame); i++)
		frame[i] = (uint8_t)(0x40 + i);
	clean();
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &eight16));
	CHECK(a->ops->play(a, frame, 16));
	CHECK_EQ(read_file(WAV), 84);
	check_extensible(&eight16, 16);
	CHECK(memcmp(file + 68, frame, 16) == 0);
	audio_destroy(a);

	/* Three channels too; an odd size has its pad byte. */
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &three8));
	CHECK(a->ops->play(a, frame, 3));
	CHECK_EQ(read_file(WAV), 72);
	check_extensible(&three8, 3);
	CHECK(memcmp(file + 68, frame, 3) == 0);
	CHECK_EQ(file[71], 0);
	audio_destroy(a);
	clean();
}

/*
 * Streams in the same format follow on; another format starts a new
 * file, and so does the first again.
 */
static void test_streams(void)
{
	AudioOut *a;

	clean();
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &stereo16));
	CHECK(a->ops->play(a, (const uint8_t *)"abcd", 4));
	CHECK(a->ops->stop(a));
	CHECK(a->ops->start(a, &stereo16));
	CHECK(a->ops->play(a, (const uint8_t *)"efgh", 4));
	CHECK(a->ops->stop(a));
	CHECK_EQ(read_file(WAV1), -1);
	CHECK(a->ops->start(a, &mono8));
	CHECK(a->ops->play(a, (const uint8_t *)"ij", 2));
	CHECK(a->ops->stop(a));
	CHECK(a->ops->start(a, &(AudioFormat){1, 8000, 16}));
	CHECK(a->ops->play(a, (const uint8_t *)"kl", 2));
	CHECK(a->ops->start(a, &(AudioFormat){1, 16000, 16}));
	CHECK(a->ops->play(a, (const uint8_t *)"mn", 2));
	CHECK(a->ops->start(a, &(AudioFormat){2, 16000, 16}));
	CHECK(a->ops->play(a, (const uint8_t *)"opqr", 4));
	audio_destroy(a);

	CHECK_EQ(read_file(WAV), 52);
	check_pcm_header(&stereo16, 8);
	CHECK(memcmp(file + 44, "abcdefgh", 8) == 0);
	CHECK_EQ(read_file(WAV1), 46);
	check_pcm_header(&mono8, 2);
	CHECK(memcmp(file + 44, "ij", 2) == 0);
	CHECK_EQ(read_file(WAV2), 46);
	check_pcm_header(&(AudioFormat){1, 8000, 16}, 2);
	CHECK(memcmp(file + 44, "kl", 2) == 0);
	CHECK_EQ(read_file("test_audio_wav-3.wav"), 46);
	check_pcm_header(&(AudioFormat){1, 16000, 16}, 2);
	CHECK(memcmp(file + 44, "mn", 2) == 0);
	CHECK_EQ(read_file("test_audio_wav-4.wav"), 48);
	check_pcm_header(&(AudioFormat){2, 16000, 16}, 4);
	unlink("test_audio_wav-3.wav");
	unlink("test_audio_wav-4.wav");
	clean();
}

/* The next files' names, with and without .wav. */
static void test_names(void)
{
	static const char *const names[][2] = {
		{"test_audio_out", "test_audio_out-1"},
		{"test_audio_wav.wav.x", "test_audio_wav.wav.x-1"},
		{"test_audio_.wav", "test_audio_-1.wav"},
		{".wav", "-1.wav"},
		{"wav", "wav-1"},
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		AudioOut *a = open_wav(names[i][0]);

		CHECK(a->ops->start(a, &stereo16));
		CHECK(a->ops->start(a, &mono8));
		audio_destroy(a);
		CHECK_EQ(read_file(names[i][0]), 44);
		CHECK_EQ(read_file(names[i][1]), 44);
		check_pcm_header(&mono8, 0);
		unlink(names[i][0]);
		unlink(names[i][1]);
	}
}

/* A file that would pass its size goes on in the next, whole frames. */
static void test_full(void)
{
	char error[200];
	AudioOut *a;

	clean();
	a = audio_wav_open_sized(WAV, 10, error, sizeof(error));
	CHECK(a != NULL);
	CHECK(a->ops->start(a, &stereo16));
	CHECK(a->ops->play(a, (const uint8_t *)"0123456789ab", 12));
	CHECK_EQ(read_file(WAV), 52);
	check_pcm_header(&stereo16, 8);
	CHECK(memcmp(file + 44, "01234567", 8) == 0);
	CHECK_EQ(read_file(WAV1), 48);
	check_pcm_header(&stereo16, 4);
	CHECK(memcmp(file + 44, "89ab", 4) == 0);
	/* Exactly full: the next write starts the next file. */
	CHECK(a->ops->play(a, (const uint8_t *)"cdef", 4));
	CHECK_EQ(read_file(WAV1), 52);
	CHECK_EQ(read_file(WAV2), -1);
	CHECK(a->ops->play(a, (const uint8_t *)"gh", 2));
	CHECK_EQ(read_file(WAV2), 46);
	check_pcm_header(&stereo16, 2);
	audio_destroy(a);
	clean();
}

/* What the host refuses: a file it cannot create or write. */
static void test_host_failures(void)
{
	char error[200];
	AudioOut *a;

	CHECK(audio_wav_open("no/such/dir.wav", error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "cannot create no/such/dir.wav: No such file or "
	                    "directory") == 0);

	a = open_wav("/dev/full");
	CHECK(!a->ops->start(a, &stereo16));
	CHECK(strcmp(a->error, "cannot write the WAV file: No space left on "
	                       "device") == 0);
	audio_destroy(a);

	/* The next file, which cannot be created. */
	a = open_wav("/dev/full");
	a->ops->start(a, &stereo16);
	strcpy(a->error, "");
	CHECK(!a->ops->start(a, &mono8));
	CHECK(strncmp(a->error, "cannot create /dev/full-1: ", 27) == 0);
	audio_destroy(a);
}

/*
 * A host that stops taking the file: its size limit (RLIMIT_FSIZE,
 * SIGXFSZ ignored) in the middle of the samples, then at the pad byte.
 */
static void test_file_limit(void)
{
	struct rlimit old, lim;
	AudioOut *a;

	clean();
	signal(SIGXFSZ, SIG_IGN);
	CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
	lim = old;
	a = open_wav(WAV);
	lim.rlim_cur = 60;
	CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
	CHECK(a->ops->start(a, &stereo16));
	CHECK(a->ops->play(a, (const uint8_t *)"01234567", 8));
	CHECK(!a->ops->play(a, (const uint8_t *)"0123456789abcdef", 16));
	CHECK(strcmp(a->error, "cannot write the WAV file: File too large") == 0);
	CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
	audio_destroy(a);

	a = open_wav(WAV);
	lim.rlim_cur = 47;
	CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
	CHECK(a->ops->start(a, &mono8));
	CHECK(!a->ops->play(a, (const uint8_t *)"abc", 3));
	CHECK(strcmp(a->error, "cannot write the WAV file: File too large") == 0);
	CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
	audio_destroy(a);
	signal(SIGXFSZ, SIG_DFL);
	clean();
}

/* Sizes past 16 MB: all four bytes of each. */
static void test_large(void)
{
	static uint8_t chunk[1 << 20];
	AudioOut *a;

	clean();
	a = open_wav(WAV);
	CHECK(a->ops->start(a, &stereo16));
	for (int i = 0; i < 16; i++)
		CHECK(a->ops->play(a, chunk, sizeof(chunk)));
	CHECK(a->ops->play(a, chunk, 4));
	audio_destroy(a);
	CHECK_EQ(read_file(WAV), sizeof(file));
	check_pcm_header(&stereo16, 0x1000004);
	clean();
}

/* The next file cannot be made when the first is full. */
static void test_next_fails(void)
{
	char error[200];
	AudioOut *a;

	clean();
	CHECK(mkdir(WAV1, 0777) == 0);
	a = audio_wav_open_sized(WAV, 8, error, sizeof(error));
	CHECK(a != NULL);
	CHECK(a->ops->start(a, &stereo16));
	CHECK(!a->ops->play(a, (const uint8_t *)"0123456789ab", 12));
	CHECK(strcmp(a->error, "cannot create " WAV1 ": Is a directory") == 0);
	audio_destroy(a);
	CHECK_EQ(read_file(WAV), 52);
	check_pcm_header(&stereo16, 8);
	CHECK(rmdir(WAV1) == 0);
	clean();
}

int main(void)
{
	/*
	 * A back end that loops making files would fill the disk: end the
	 * test first (it takes well under a second).
	 */
	alarm(10);
	RUN_TEST(test_created_empty);
	RUN_TEST(test_stereo);
	RUN_TEST(test_pad_byte);
	RUN_TEST(test_extensible);
	RUN_TEST(test_streams);
	RUN_TEST(test_names);
	RUN_TEST(test_full);
	RUN_TEST(test_host_failures);
	RUN_TEST(test_file_limit);
	RUN_TEST(test_large);
	RUN_TEST(test_next_fails);
	return test_summary();
}
