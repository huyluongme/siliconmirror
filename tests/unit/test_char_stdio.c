/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the stream of standard input and output
 * (src/backend/char/stdio.c), with its input from a pipe, from /dev/null
 * or from a pseudo-terminal.
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "backend/char/file.h"
#include "backend/char/stdio.h"
#include "test.h"

#define CTRL_A "\x01"

static void put(int fd, const char *text)
{
	CHECK_EQ(write(fd, text, strlen(text)), (ssize_t)strlen(text));
}

/* Take what the stream has now into buf, as a string. */
static void take(CharStream *s, char *buf, size_t size)
{
	size_t n = 0;
	uint8_t byte;

	while (n < size - 1 && char_stream_read(s, &byte))
		buf[n++] = (char)byte;
	buf[n] = '\0';
}

/* What the guest sends goes to the output. */
static void test_output(void)
{
	FILE *out = tmpfile();
	CharStream *s = char_stdio_create_fd(-1, out);
	char buf[16] = "";

	char_stream_write(s, (const uint8_t *)"hello", 5);
	rewind(out);
	CHECK(fgets(buf, sizeof(buf), out) != NULL);
	CHECK(strcmp(buf, "hello") == 0);
	char_stream_destroy(s);
	fclose(out);
}

/* Input from a pipe comes as it is, Ctrl-A included, until its end. */
static void test_pipe_input(void)
{
	int fds[2];
	CharStream *s;
	char buf[16];
	uint8_t byte;

	CHECK_EQ(pipe(fds), 0);
	s = char_stdio_create_fd(fds[0], stdout);
	CHECK(char_stream_has_input(s));
	CHECK(!char_stream_read(s, &byte)); /* nothing yet */

	put(fds[1], "ab" CTRL_A "x");
	CHECK(char_stream_wait(s));
	take(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "ab" CTRL_A "x") == 0);
	CHECK(!char_stream_read(s, &byte));

	/* Waiting with input there returns at once; then the end. */
	put(fds[1], "c");
	CHECK(char_stream_wait(s));
	CHECK(char_stream_wait(s));
	close(fds[1]);
	take(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "c") == 0);
	CHECK(!char_stream_wait(s));
	CHECK(!char_stream_read(s, &byte));
	CHECK(!char_stream_wait(s));
	char_stream_destroy(s);
	close(fds[0]);
}

/* /dev/null ends at once. */
static void test_no_input(void)
{
	int fd = open("/dev/null", O_RDONLY);
	CharStream *s = char_stdio_create_fd(fd, stdout);
	uint8_t byte;

	CHECK(fd >= 0);
	CHECK(!char_stream_read(s, &byte));
	CHECK(!char_stream_wait(s));
	char_stream_destroy(s);
	close(fd);
}

/* The end of the input is final, even if the file grows. */
static void test_end_is_final(void)
{
	FILE *f = tmpfile();
	CharStream *s;
	uint8_t byte = 0;

	CHECK(f != NULL);
	CHECK_EQ(pwrite(fileno(f), "a", 1, 0), 1);
	s = char_stdio_create_fd(fileno(f), stdout);
	CHECK(char_stream_read(s, &byte));
	CHECK_EQ(byte, 'a');
	errno = EAGAIN; /* what errno says before has no part in it */
	CHECK(!char_stream_read(s, &byte));
	CHECK_EQ(pwrite(fileno(f), "b", 1, 1), 1);
	CHECK(!char_stream_read(s, &byte));
	CHECK(!char_stream_wait(s));
	char_stream_destroy(s);
	fclose(f);
}

static int quits;

static void count_quit(void *ctx)
{
	(void)ctx;
	quits++;
}

/*
 * A terminal goes to raw mode, and back when the stream goes. Ctrl-A x
 * quits, Ctrl-A Ctrl-A is one Ctrl-A, Ctrl-A and another key nothing.
 */
static void test_terminal(void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	int slave;
	struct termios t;
	CharStream *s;
	char buf[16];

	CHECK(master >= 0);
	CHECK_EQ(grantpt(master), 0);
	CHECK_EQ(unlockpt(master), 0);
	slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	CHECK(slave >= 0);
	CHECK_EQ(tcgetattr(slave, &t), 0);
	CHECK((t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));

	s = char_stdio_create_fd(slave, stdout);
	CHECK_EQ(tcgetattr(slave, &t), 0);
	CHECK_EQ(t.c_lflag & (ICANON | ECHO | ISIG | IEXTEN), 0);
	CHECK_EQ(t.c_iflag & (ICRNL | IXON), 0);

	/* A second stream on a terminal leaves it as the first one set it. */
	CharStream *other = char_stdio_create_fd(slave, stdout);

	char_stream_destroy(other);
	CHECK_EQ(tcgetattr(slave, &t), 0);
	CHECK_EQ(t.c_lflag & ICANON, 0);

	char_stdio_on_quit(s, count_quit, NULL);
	quits = 0;
	put(master, "a\r" CTRL_A CTRL_A "b" CTRL_A "zc" CTRL_A);
	CHECK(char_stream_wait(s));
	take(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "a\r" CTRL_A "bc") == 0);
	CHECK_EQ(quits, 0);
	put(master, "xd");
	CHECK(char_stream_wait(s));
	take(s, buf, sizeof(buf));
	CHECK_EQ(quits, 1);
	CHECK(strcmp(buf, "") == 0);
	take(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "d") == 0);

	/* Without a quit function, Ctrl-A x sends nothing. */
	char_stdio_on_quit(s, NULL, NULL);
	put(master, CTRL_A "xe");
	CHECK(char_stream_wait(s));
	take(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "e") == 0);
	CHECK_EQ(quits, 1);

	char_stream_destroy(s);
	CHECK_EQ(tcgetattr(slave, &t), 0);
	CHECK((t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
	close(slave);
	close(master);
}

/* A signal that ends smirror puts the terminal back first. */
static void test_terminal_on_signal(void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	int slave, status;
	struct termios t;
	pid_t child;

	CHECK(master >= 0);
	CHECK_EQ(grantpt(master), 0);
	CHECK_EQ(unlockpt(master), 0);
	slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	CHECK(slave >= 0);
	child = fork();
	if (child == 0) {
		char_stdio_create_fd(slave, stdout);
		tcgetattr(slave, &t);
		if (t.c_lflag & ICANON)
			_exit(1); /* not in raw mode */
		raise(SIGTERM);
		_exit(2);
	}
	CHECK(child > 0);
	CHECK_EQ(waitpid(child, &status, 0), child);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
	CHECK_EQ(tcgetattr(slave, &t), 0);
	CHECK((t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
	close(slave);
	close(master);
}

/* Streams without input. */
static void test_without_input(void)
{
	CharStream *s = char_file_create(stdout, false);
	uint8_t byte;

	CHECK(!char_stream_has_input(s));
	CHECK(!char_stream_read(s, &byte));
	CHECK(!char_stream_wait(s));
	char_stream_destroy(s);
	CHECK(!char_stream_has_input(NULL));
	CHECK(!char_stream_read(NULL, &byte));
	CHECK(!char_stream_wait(NULL));
}

int main(void)
{
	RUN_TEST(test_output);
	RUN_TEST(test_pipe_input);
	RUN_TEST(test_no_input);
	RUN_TEST(test_end_is_final);
	RUN_TEST(test_terminal);
	RUN_TEST(test_terminal_on_signal);
	RUN_TEST(test_without_input);
	return test_summary();
}
