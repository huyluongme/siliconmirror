/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * WAV files of what the guest plays; see wav.h.
 *
 * The format is RIFF's WAVE (the McGill "Audio File Format
 * Specifications" page, WAVE): a RIFF chunk of form WAVE that holds a
 * "fmt " chunk and a "data" chunk; a chunk of odd size is followed by a
 * pad byte. The format chunk is WAVE_FORMAT_PCM, 16 bytes, for one or
 * two channels, and WAVE_FORMAT_EXTENSIBLE, 40 bytes, for more, as that
 * page says it should be: the same fields, then the valid bits (all of
 * them), a channel mask of 0 (channels in order) and the PCM SubFormat
 * GUID.
 */

/* pwrite() is POSIX. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "backend/audio/wav.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/byteorder.h"

#define FORMAT_PCM 0x0001u
#define FORMAT_EXTENSIBLE 0xfffeu
#define FMT_PCM_SIZE 16u
#define FMT_EXTENSIBLE_SIZE 40u
#define CHUNK_HEADER 8u
#define WAVE_ID 4u /* "WAVE", the RIFF chunk's form */

/* The rest of the SubFormat GUID after its format code. */
static const uint8_t guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                      0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

typedef struct WavOut {
	AudioOut out; /* first, so that an AudioOut * is a WavOut * */
	char *path;
	uint32_t max_data; /* bytes of samples a file holds at most */
	unsigned files;    /* made so far, PATH the first */
	int fd;
	bool has_format; /* the file has its header */
	AudioFormat format;
	uint32_t data_at; /* where the samples start */
	uint32_t data;    /* bytes of samples in the file */
} WavOut;

static bool fail(WavOut *w, const char *what)
{
	snprintf(w->out.error, sizeof(w->out.error), "%s: %s", what,
	         strerror(errno));
	return false;
}

static bool write_at(WavOut *w, uint64_t offset, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	/* A regular file takes it all, or what fits and then an error. */
	while (len > 0) {
		ssize_t n = pwrite(w->fd, p, len, (off_t)offset);

		if (n <= 0)
			return fail(w, "cannot write the WAV file");
		p += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

/*
 * The name of file n: PATH for 0, then PATH with -n before its .wav, or
 * after it without one.
 */
static char *file_name(const char *path, unsigned n)
{
	size_t len = strlen(path), stem = len;
	char *name = malloc(len + 16);

	if (name == NULL)
		return NULL;
	if (n == 0) {
		memcpy(name, path, len + 1);
		return name;
	}
	if (len >= 4 && strcmp(path + len - 4, ".wav") == 0)
		stem = len - 4;
	snprintf(name, len + 16, "%.*s-%u%s", (int)stem, path, n, path + stem);
	return name;
}

/* Create the next file, empty, as the one that takes the samples. */
static bool next_file(WavOut *w)
{
	char *name = file_name(w->path, w->files);
	int fd;

	if (name == NULL) {
		snprintf(w->out.error, sizeof(w->out.error), "out of memory");
		return false;
	}
	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		snprintf(w->out.error, sizeof(w->out.error), "cannot create %s: %s",
		         name, strerror(errno));
		free(name);
		return false;
	}
	free(name);
	if (w->fd >= 0)
		close(w->fd);
	w->fd = fd;
	w->files++;
	w->data = 0;
	return true;
}

/* The RIFF size, the data size and the pad byte, for w->data bytes. */
static bool update_sizes(WavOut *w)
{
	uint32_t pad = w->data & 1;
	uint8_t size[4];

	store_le32(size, w->data_at - CHUNK_HEADER + w->data + pad);
	if (!write_at(w, 4, size, 4))
		return false;
	store_le32(size, w->data);
	if (!write_at(w, w->data_at - 4, size, 4))
		return false;
	return pad == 0 || write_at(w, (uint64_t)w->data_at + w->data, "", 1);
}

static bool write_header(WavOut *w, const AudioFormat *f)
{
	uint8_t h[CHUNK_HEADER + WAVE_ID + CHUNK_HEADER + FMT_EXTENSIBLE_SIZE +
	          CHUNK_HEADER];
	bool extensible = f->channels > 2;
	uint32_t fmt_size = extensible ? FMT_EXTENSIBLE_SIZE : FMT_PCM_SIZE;
	uint32_t frame = (uint32_t)audio_frame_size(f);
	uint8_t *p = h;

	memset(h, 0, sizeof(h)); /* the sizes: update_sizes() */
	memcpy(p, "RIFF", 4);
	memcpy(p + 8, "WAVE", 4);
	memcpy(p + 12, "fmt ", 4);
	store_le32(p + 16, fmt_size);
	p += 20;
	store_le16(p, extensible ? FORMAT_EXTENSIBLE : FORMAT_PCM);
	store_le16(p + 2, (uint16_t)f->channels);
	store_le32(p + 4, f->rate);
	store_le32(p + 8, f->rate * frame);
	store_le16(p + 12, (uint16_t)frame);
	store_le16(p + 14, (uint16_t)f->bits);
	p += FMT_PCM_SIZE;
	if (extensible) {
		store_le16(p, FMT_EXTENSIBLE_SIZE - FMT_PCM_SIZE - 2);
		store_le16(p + 2, (uint16_t)f->bits); /* the mask, 0: memset */
		store_le16(p + 8, FORMAT_PCM);
		memcpy(p + 10, guid_tail, sizeof(guid_tail));
		p += FMT_EXTENSIBLE_SIZE - FMT_PCM_SIZE;
	}
	memcpy(p, "data", 4);
	p += CHUNK_HEADER;
	w->data_at = (uint32_t)(p - h);
	w->format = *f;
	w->has_format = true;
	return write_at(w, 0, h, w->data_at) && update_sizes(w);
}

static bool same_format(const AudioFormat *a, const AudioFormat *b)
{
	return a->channels == b->channels && a->rate == b->rate &&
	       a->bits == b->bits;
}

static bool wav_start(AudioOut *a, const AudioFormat *format)
{
	WavOut *w = (WavOut *)a;

	if (w->has_format && same_format(&w->format, format))
		return true;
	if (w->has_format && !next_file(w))
		return false;
	return write_header(w, format);
}

/* The bytes of whole frames that the file still holds. */
static uint32_t room(const WavOut *w)
{
	uint32_t frame = (uint32_t)audio_frame_size(&w->format);
	uint32_t left = w->max_data - w->data;

	return left - left % frame;
}

static bool wav_play(AudioOut *a, const uint8_t *buf, size_t len)
{
	WavOut *w = (WavOut *)a;

	while (len > 0) {
		AudioFormat f = w->format;
		uint32_t n;

		/* Full: the next file, in the same format. */
		if (room(w) == 0 && (!next_file(w) || !write_header(w, &f)))
			return false;
		n = room(w);
		if (n > len)
			n = (uint32_t)len;
		if (!write_at(w, (uint64_t)w->data_at + w->data, buf, n))
			return false;
		w->data += n;
		if (!update_sizes(w))
			return false;
		buf += n;
		len -= n;
	}
	return true;
}

static bool wav_stop(AudioOut *a)
{
	(void)a; /* the file is whole after every write */
	return true;
}

static void wav_destroy(AudioOut *a)
{
	WavOut *w = (WavOut *)a;

	if (w->fd >= 0)
		close(w->fd);
	free(w->path);
	free(w);
}

static const AudioOutOps wav_ops = {
	.start = wav_start,
	.play = wav_play,
	.stop = wav_stop,
	.destroy = wav_destroy,
};

AudioOut *audio_wav_open_sized(const char *path, uint32_t max_data, char *error,
                               size_t error_size)
{
	WavOut *w = calloc(1, sizeof(*w));

	if (w == NULL || (w->path = malloc(strlen(path) + 1)) == NULL) {
		snprintf(error, error_size, "out of memory");
		free(w);
		return NULL;
	}
	strcpy(w->path, path);
	w->out.ops = &wav_ops;
	w->max_data = max_data;
	w->fd = -1;
	if (!next_file(w)) {
		snprintf(error, error_size, "%s", w->out.error);
		wav_destroy(&w->out);
		return NULL;
	}
	return &w->out;
}

AudioOut *audio_wav_open(const char *path, char *error, size_t error_size)
{
	/*
	 * The RIFF size, 32 bits, counts all but its chunk's header: the
	 * WAVE id, the format chunk, the data chunk's header, the samples
	 * and a pad byte, with the larger format chunk.
	 */
	uint32_t max_data = UINT32_MAX - WAVE_ID - CHUNK_HEADER -
	                    FMT_EXTENSIBLE_SIZE - CHUNK_HEADER - 1;

	return audio_wav_open_sized(path, max_data, error, error_size);
}
