/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Audio outputs: the interface between a device that plays sound, such
 * as the VideoCore's audio service behind the headphone jack, and the
 * host side that takes it (src/backend/audio/): a WAV file, or the
 * host's sound card.
 *
 * The device plays streams. A stream starts in one format, takes the
 * samples as the device plays them, at the device's pace in virtual
 * time, and stops. A NULL output means nothing is plugged in: the
 * device still plays, at the same pace, and the sound goes nowhere.
 *
 * A call fails only when the host does; the back end then writes why
 * in error, and the device stops the machine.
 */

#ifndef CORE_AUDIO_H
#define CORE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AudioFormat {
	unsigned channels; /* 1 to 8, interleaved */
	unsigned rate;     /* frames a second */
	unsigned bits;     /* 8: unsigned, 16: signed, little-endian */
} AudioFormat;

typedef struct AudioOut AudioOut;

typedef struct AudioOutOps {
	/* A stream starts: what play() passes until stop() is in format. */
	bool (*start)(AudioOut *a, const AudioFormat *format);

	/* The next len bytes of the stream, whole frames, as played. */
	bool (*play)(AudioOut *a, const uint8_t *buf, size_t len);

	/* The stream stops: what was played is heard, or kept, to its end. */
	bool (*stop)(AudioOut *a);

	/* Free the output, as stop() first if a stream plays. */
	void (*destroy)(AudioOut *a);
} AudioOutOps;

/* Each back end embeds an AudioOut in its own state struct. */
struct AudioOut {
	const AudioOutOps *ops;
	char error[160]; /* why the last call failed */
};

/* The size in bytes of a frame, a sample of each channel. */
static inline size_t audio_frame_size(const AudioFormat *f)
{
	return (size_t)f->channels * (f->bits / 8);
}

static inline void audio_destroy(AudioOut *a)
{
	if (a != NULL)
		a->ops->destroy(a);
}

#endif /* CORE_AUDIO_H */
