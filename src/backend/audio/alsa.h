/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * An audio output to the host's sound card, through ALSA's library
 * (-audio alsa, or alsa:DEVICE for another than "default"). Built only
 * when the library is found (SMIRROR_HAVE_ALSA).
 *
 * Each stream opens DEVICE in the stream's format, with ALSA's
 * resampling allowed and 100 ms of latency, and closes it at its end,
 * once what was played has been heard. Writes wait while the card's
 * buffer is full: a machine that runs ahead of the host's time is held
 * back, without its virtual time changing. A machine that runs behind
 * leaves the card without samples; the card starts again, with a gap.
 */

#ifndef BACKEND_AUDIO_ALSA_H
#define BACKEND_AUDIO_ALSA_H

#include <stddef.h>

#include "core/audio.h"

/*
 * The output, or NULL with a message in error when DEVICE cannot be
 * opened for playback.
 */
AudioOut *audio_alsa_open(const char *device, char *error, size_t error_size);

#endif /* BACKEND_AUDIO_ALSA_H */
