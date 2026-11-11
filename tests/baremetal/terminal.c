/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Run smirror on a terminal, a pseudo-terminal that this program drives,
 * as a user would: wait for what the kernel prints, press Ctrl-A x, and
 * check that smirror quits with status 0 and says why.
 *
 * Usage: terminal SMIRROR KERNEL TEXT, where KERNEL prints TEXT.
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define QUIT_MESSAGE "smirror: shutdown: the user pressed Ctrl-A x"

static char output[4096];
static size_t output_len;

/*
 * Read what smirror prints until text shows up, the terminal closes, or
 * 10 seconds pass without anything. Returns whether text showed up.
 */
static bool read_until(int master, const char *text)
{
	while (strstr(output, text) == NULL) {
		struct pollfd p = {.fd = master, .events = POLLIN};
		ssize_t n;

		if (poll(&p, 1, 10000) <= 0)
			return false;
		n = read(master, output + output_len, sizeof(output) - 1 - output_len);
		if (n <= 0)
			return false; /* EIO once smirror has closed the terminal */
		output_len += (size_t)n;
		output[output_len] = '\0';
	}
	return true;
}

int main(int argc, char **argv)
{
	int master, status;
	pid_t child;

	if (argc != 4) {
		fputs("usage: terminal SMIRROR KERNEL TEXT\n", stderr);
		return 2;
	}
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("terminal: pseudo-terminal");
		return 1;
	}
	child = fork();
	if (child == 0) {
		int slave;

		setsid();
		slave = open(ptsname(master), O_RDWR);
		if (slave < 0)
			_exit(127);
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		close(master);
		execl(argv[1], argv[1], "-M", "raspi3b", "-kernel", argv[2],
		      (char *)NULL);
		_exit(127);
	}

	if (!read_until(master, argv[3])) {
		fprintf(stderr, "terminal: no \"%s\" in:\n%s\n", argv[3], output);
		kill(child, SIGKILL);
		return 1;
	}

	/* The kernel waits now, and smirror with it, for a key. */
	if (write(master, "\x01x", 2) != 2) {
		perror("terminal: write");
		return 1;
	}
	if (!read_until(master, QUIT_MESSAGE)) {
		fprintf(stderr, "terminal: Ctrl-A x did not quit:\n%s\n", output);
		kill(child, SIGKILL);
		return 1;
	}
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		continue;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "terminal: smirror ended with status %d\n", status);
		return 1;
	}
	puts("terminal ok");
	return 0;
}
