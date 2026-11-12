/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Character streams: the interface between a device that sends and
 * receives bytes, such as a UART, and the host side (src/backend/char/).
 *
 * The device only sees this interface, so the same UART model can talk to
 * a terminal, a file or, later, a socket. A NULL stream means "not
 * connected": data written to it is dropped, as on an unconnected wire,
 * and nothing ever comes from it. A stream without input, such as a file
 * that only takes output, behaves the same for reading.
 */

#ifndef CORE_CHAR_STREAM_H
#define CORE_CHAR_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CharStream CharStream;

typedef struct CharStreamOps {
	/* Pass len bytes from the guest to the host. */
	void (*write)(CharStream *s, const uint8_t *buf, size_t len);

	/*
	 * Take the next byte from the host into *byte, without waiting:
	 * false when none has come, or none ever will. NULL for a stream
	 * without input.
	 */
	bool (*read)(CharStream *s, uint8_t *byte);

	/*
	 * Wait until there may be a byte to read: true then, false when
	 * none ever will, at the end of the input. NULL for a stream
	 * without input.
	 */
	bool (*wait)(CharStream *s);

	/*
	 * Whether the line to the device is held low, as when nothing drives
	 * it: a receiver then sees a start bit that never ends, and no byte
	 * comes from read. NULL for a stream whose line is never held low.
	 */
	bool (*line_low)(CharStream *s);
	/* Free the stream. */
	void (*destroy)(CharStream *s);
} CharStreamOps;

/* Each back end embeds a CharStream in its own state struct. */
struct CharStream {
	const CharStreamOps *ops;
};

static inline void char_stream_write(CharStream *s, const uint8_t *buf,
                                     size_t len)
{
	if (s != NULL)
		s->ops->write(s, buf, len);
}

static inline bool char_stream_read(CharStream *s, uint8_t *byte)
{
	return s != NULL && s->ops->read != NULL && s->ops->read(s, byte);
}

static inline bool char_stream_wait(CharStream *s)
{
	return s != NULL && s->ops->wait != NULL && s->ops->wait(s);
}

static inline bool char_stream_line_low(CharStream *s)
{
	return s != NULL && s->ops->line_low != NULL && s->ops->line_low(s);
}

/* Whether input can come from the stream at all. */
static inline bool char_stream_has_input(const CharStream *s)
{
	return s != NULL && s->ops->read != NULL;
}

static inline void char_stream_destroy(CharStream *s)
{
	if (s != NULL)
		s->ops->destroy(s);
}

#endif /* CORE_CHAR_STREAM_H */
