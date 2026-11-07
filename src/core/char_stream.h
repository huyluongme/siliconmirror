/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Character streams: the interface between a device that sends bytes,
 * such as a UART, and the host side that receives them
 * (src/backend/char/).
 *
 * The device only sees this interface, so the same UART model can write to
 * a terminal, a file or, later, a socket. A NULL stream means "not
 * connected": data written to it is dropped, as on an unconnected wire.
 */

#ifndef CORE_CHAR_STREAM_H
#define CORE_CHAR_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct CharStream CharStream;

typedef struct CharStreamOps {
	/* Pass len bytes from the guest to the host. */
	void (*write)(CharStream *s, const uint8_t *buf, size_t len);

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

static inline void char_stream_destroy(CharStream *s)
{
	if (s != NULL)
		s->ops->destroy(s);
}

#endif /* CORE_CHAR_STREAM_H */
