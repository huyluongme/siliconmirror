/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The host's sound card through ALSA's library; see alsa.h. The calls
 * are those of alsa-lib's PCM interface: snd_pcm_open, snd_pcm_set_params
 * (the simple setup: format, interleaved access, channels, rate,
 * resampling and latency), snd_pcm_writei, snd_pcm_recover after an
 * underrun, snd_pcm_drain and snd_pcm_close. A device is opened without
 * waiting (SND_PCM_NONBLOCK), so that one another program holds fails
 * rather than holds the machine; writes then wait (snd_pcm_nonblock 0).
 */

/* alsa/asoundlib.h needs POSIX's struct timespec. */
#define _POSIX_C_SOURCE 200809L

#include "backend/audio/alsa.h"

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LATENCY_US 100000u

typedef struct AlsaOut {
	AudioOut out; /* first, so that an AudioOut * is an AlsaOut * */
	char *device;
	snd_pcm_t *pcm; /* while a stream plays */
	size_t frame;   /* its frame size */
} AlsaOut;

static bool fail(AlsaOut *o, const char *what, int err)
{
	snprintf(o->out.error, sizeof(o->out.error), "ALSA %s %s: %s", o->device,
	         what, snd_strerror(err));
	return false;
}

/* Close the stream's device: drained, once what it holds is heard. */
static bool close_pcm(AlsaOut *o, bool drain)
{
	int err = 0;

	if (o->pcm == NULL)
		return true;
	if (drain)
		err = snd_pcm_drain(o->pcm);
	snd_pcm_close(o->pcm);
	o->pcm = NULL;
	return err >= 0 || fail(o, "drain", err);
}

static bool alsa_start(AudioOut *a, const AudioFormat *f)
{
	AlsaOut *o = (AlsaOut *)a;
	snd_pcm_format_t format =
		f->bits == 8 ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_S16_LE;
	int err;

	if (!close_pcm(o, true))
		return false;
	err = snd_pcm_open(&o->pcm, o->device, SND_PCM_STREAM_PLAYBACK,
	                   SND_PCM_NONBLOCK);
	if (err < 0) {
		o->pcm = NULL;
		return fail(o, "open", err);
	}
	snd_pcm_nonblock(o->pcm, 0); /* fails only for a PCM not open */
	err = snd_pcm_set_params(o->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
	                         f->channels, f->rate, 1, LATENCY_US);
	if (err < 0) {
		close_pcm(o, false);
		return fail(o, "setup", err);
	}
	o->frame = audio_frame_size(f);
	return true;
}

static bool alsa_play(AudioOut *a, const uint8_t *buf, size_t len)
{
	AlsaOut *o = (AlsaOut *)a;
	snd_pcm_uframes_t frames = len / o->frame;

	while (frames > 0) {
		snd_pcm_sframes_t n = snd_pcm_writei(o->pcm, buf, frames);

		if (n < 0) {
			/* An underrun: the machine ran behind the card. */
			int err = snd_pcm_recover(o->pcm, (int)n, 1);

			if (err < 0)
				return fail(o, "write", err);
			continue;
		}
		buf += (size_t)n * o->frame;
		frames -= (snd_pcm_uframes_t)n;
	}
	return true;
}

static bool alsa_stop(AudioOut *a)
{
	return close_pcm((AlsaOut *)a, true);
}

static void alsa_destroy(AudioOut *a)
{
	AlsaOut *o = (AlsaOut *)a;

	close_pcm(o, true);
	free(o->device);
	free(o);
}

static const AudioOutOps alsa_ops = {
	.start = alsa_start,
	.play = alsa_play,
	.stop = alsa_stop,
	.destroy = alsa_destroy,
};

AudioOut *audio_alsa_open(const char *device, char *error, size_t error_size)
{
	AlsaOut *o = calloc(1, sizeof(*o));
	snd_pcm_t *pcm;
	int err;

	if (o == NULL || (o->device = malloc(strlen(device) + 1)) == NULL) {
		snprintf(error, error_size, "out of memory");
		free(o);
		return NULL;
	}
	strcpy(o->device, device);
	o->out.ops = &alsa_ops;
	/* That the device is there; each stream opens it again. */
	err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		snprintf(error, error_size, "cannot open ALSA device %s: %s", device,
		         snd_strerror(err));
		alsa_destroy(&o->out);
		return NULL;
	}
	snd_pcm_close(pcm);
	return &o->out;
}
