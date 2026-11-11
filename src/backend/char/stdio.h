/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The character stream of standard input and output: what the guest
 * sends goes to standard output, and what comes on standard input goes to
 * the guest, until its end.
 *
 * When standard input is a terminal, the stream puts it in raw mode, so
 * that each key goes to the guest as it is typed, without the terminal
 * echoing it or acting on Ctrl-C; the stream puts the terminal back as it
 * was when it is destroyed, when smirror exits, or when a signal ends it.
 * Ctrl-A is then the escape key: Ctrl-A x quits, Ctrl-A Ctrl-A sends one
 * Ctrl-A, and Ctrl-A followed by any other key sends nothing. These keys
 * follow QEMU's terminal multiplexer.
 */

#ifndef BACKEND_CHAR_STDIO_H
#define BACKEND_CHAR_STDIO_H

#include <stdio.h>

#include "core/char_stream.h"

/*
 * The stream of standard input and output. Returns NULL when out of
 * memory.
 */
CharStream *char_stdio_create(void);

/*
 * The same with input from the file descriptor in and output to out, for
 * tests. Neither is closed with the stream.
 */
CharStream *char_stdio_create_fd(int in, FILE *out);

/* Call quit(ctx) when the user presses Ctrl-A x on the terminal. */
void char_stdio_on_quit(CharStream *s, void (*quit)(void *ctx), void *ctx);

#endif /* BACKEND_CHAR_STDIO_H */
