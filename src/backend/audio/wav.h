/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * An audio output that writes WAV files (-audio wav:PATH).
 *
 * The first stream's samples go to PATH, in the stream's own format:
 * PCM, 8 bits unsigned or 16 bits signed, its channels and rate, as the
 * guest sent them. Later streams in the same format follow on in the
 * same file, back to back: the time between streams is not in it. A
 * stream in another format starts a new file, PATH with -1, -2... put
 * before its .wav, or after PATH without one; so does a file that would
 * pass the 4 GB that a WAV file can hold.
 *
 * The header is kept up to date after each write, so a file is whole
 * however smirror ends. PATH is created, empty, when the output is.
 */

#ifndef BACKEND_AUDIO_WAV_H
#define BACKEND_AUDIO_WAV_H

#include <stddef.h>
#include <stdint.h>

#include "core/audio.h"

/*
 * The output, or NULL with a message in error when PATH cannot be
 * created.
 */
AudioOut *audio_wav_open(const char *path, char *error, size_t error_size);

/*
 * The same, with at most max_data bytes of samples a file, for tests;
 * audio_wav_open's is the most that a WAV file can hold.
 */
AudioOut *audio_wav_open_sized(const char *path, uint32_t max_data, char *error,
                               size_t error_size);

#endif /* BACKEND_AUDIO_WAV_H */
