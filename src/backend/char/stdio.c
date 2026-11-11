/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Standard input and output as a character stream. Input is read with
 * poll() and read(), so that taking a byte never waits; the terminal's
 * raw mode follows the POSIX termios interface.
 */

#define _POSIX_C_SOURCE 200809L

#include "backend/char/stdio.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "util/macros.h"

#define CTRL_A 0x01

typedef struct CharStdio {
	CharStream stream;
	int in;
	FILE *out;
	bool ended;  /* the input has ended */
	bool raw;    /* in is a terminal that the stream put in raw mode */
	bool escape; /* the last key was Ctrl-A */
	void (*quit)(void *ctx);
	void *quit_ctx;
	uint8_t buf[256]; /* input read and not taken yet: buf[head..len) */
	size_t head, len;
} CharStdio;

/*
 * The terminal in raw mode, and its settings from before, for atexit()
 * and the signal handlers. One terminal at a time is in raw mode.
 */
static int raw_fd = -1;
static struct termios saved;

static void restore_terminal(void)
{
	if (raw_fd >= 0) {
		tcsetattr(raw_fd, TCSANOW, &saved);
		raw_fd = -1;
	}
}

/* A signal that ends smirror: put the terminal back first. */
static void restore_and_die(int sig)
{
	restore_terminal();
	raise(sig); /* SA_RESETHAND made the default action current */
}

static void make_raw(CharStdio *c)
{
	static const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
	static bool hooked;
	struct termios t;

	if (raw_fd >= 0 || !isatty(c->in) || tcgetattr(c->in, &saved) != 0)
		return;

	/*
	 * Bytes as they come: no line editing, echo, signal keys, flow
	 * control or translation of carriage returns. Output is left alone.
	 */
	t = saved;
	t.c_iflag &=
		~(tcflag_t)(BRKINT | ICRNL | IGNCR | INLCR | INPCK | ISTRIP | IXON);
	t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cflag = (t.c_cflag & ~(tcflag_t)(CSIZE | PARENB)) | CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(c->in, TCSANOW, &t) != 0)
		return;
	raw_fd = c->in;
	c->raw = true;

	if (!hooked) {
		struct sigaction sa = {.sa_handler = restore_and_die,
		                       .sa_flags = SA_RESETHAND};

		sigemptyset(&sa.sa_mask);
		for (unsigned i = 0; i < ARRAY_SIZE(signals); i++)
			sigaction(signals[i], &sa, NULL);
		atexit(restore_terminal);
		hooked = true;
	}
}

static CharStdio *char_stdio_of(CharStream *s)
{
	return container_of(s, CharStdio, stream);
}

static void stdio_write(CharStream *s, const uint8_t *buf, size_t len)
{
	CharStdio *c = char_stdio_of(s);

	/* The guest cannot be told about host errors, so they are ignored. */
	fwrite(buf, 1, len, c->out);
	fflush(c->out);
}

/*
 * Read what has come on the input into buf, waiting up to timeout ms, or
 * for ever when timeout is -1. Returns whether something came; the end of
 * the input, or an error, ends it.
 */
static bool fill(CharStdio *c, int timeout)
{
	struct pollfd p = {.fd = c->in, .events = POLLIN};
	ssize_t n;

	if (c->ended || poll(&p, 1, timeout) <= 0)
		return false;
	n = read(c->in, c->buf, sizeof(c->buf));
	if (n > 0) {
		c->head = 0;
		c->len = (size_t)n;
		return true;
	}
	if (n == 0 || (errno != EINTR && errno != EAGAIN))
		c->ended = true;
	return false;
}

static bool stdio_read(CharStream *s, uint8_t *byte)
{
	CharStdio *c = char_stdio_of(s);

	for (;;) {
		uint8_t b;

		if (c->head == c->len && !fill(c, 0))
			return false;
		b = c->buf[c->head++];
		if (!c->raw) {
			*byte = b;
			return true;
		}
		if (c->escape) {
			c->escape = false;
			if (b == CTRL_A) {
				*byte = b;
				return true;
			}
			if (b == 'x' && c->quit != NULL) {
				c->quit(c->quit_ctx);
				return false;
			}
			continue; /* Ctrl-A and another key: nothing */
		}
		if (b == CTRL_A) {
			c->escape = true;
			continue;
		}
		*byte = b;
		return true;
	}
}

static bool stdio_wait(CharStream *s)
{
	CharStdio *c = char_stdio_of(s);

	while (c->head == c->len) {
		if (c->ended)
			return false;
		fill(c, -1);
	}
	return true;
}

static void stdio_destroy(CharStream *s)
{
	CharStdio *c = char_stdio_of(s);

	if (c->raw)
		restore_terminal();
	free(c);
}

static const CharStreamOps char_stdio_ops = {
	.write = stdio_write,
	.read = stdio_read,
	.wait = stdio_wait,
	.destroy = stdio_destroy,
};

CharStream *char_stdio_create_fd(int in, FILE *out)
{
	CharStdio *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	c->stream.ops = &char_stdio_ops;
	c->in = in;
	c->out = out;
	make_raw(c);
	return &c->stream;
}

CharStream *char_stdio_create(void)
{
	return char_stdio_create_fd(STDIN_FILENO, stdout);
}

void char_stdio_on_quit(CharStream *s, void (*quit)(void *ctx), void *ctx)
{
	CharStdio *c = char_stdio_of(s);

	c->quit = quit;
	c->quit_ctx = ctx;
}
