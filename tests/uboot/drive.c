/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Drive smirror through a session, as a user at its terminal would: on
 * a pseudo-terminal, wait for what the guest prints, then type.
 *
 * Usage: drive CARD COPY SCRIPT SMIRROR ARGUMENT...
 *
 * CARD is copied to COPY first, so that the session does not change it;
 * the arguments name COPY. Without CARD the test is skipped: exit
 * status 77. SCRIPT holds a command a line, # starts a comment:
 *
 *   timeout SECONDS   how long each wait may take from then on
 *   expect TEXT       wait for TEXT, after what the last expect found
 *   type TEXT         type TEXT; \r, \n, \\ and \xHH are escapes
 *   quit              press Ctrl-A x; smirror must end with status 0
 *
 * When a wait fails, everything smirror printed is shown.
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
#include <time.h>
#include <unistd.h>

#define SKIP 77
#define QUIT_MESSAGE "smirror: shutdown: the user pressed Ctrl-A x"

static char *output; /* everything smirror printed */
static size_t output_len, output_size;
static size_t found; /* where the last expect's text ended */
static int master = -1;
static pid_t child;

static void fail(const char *fmt, const char *arg)
{
	fprintf(stderr, "drive: ");
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n--- smirror printed:\n%.*s\n---\n", (int)output_len,
	        output != NULL ? output : "");
	if (child > 0)
		kill(child, SIGKILL);
	exit(1);
}

static double seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Read what smirror prints until text shows up after found. */
static bool read_until(const char *text, double timeout)
{
	double end = seconds() + timeout;
	char *at;

	for (;;) {
		struct pollfd p = {.fd = master, .events = POLLIN};
		double left = end - seconds();
		ssize_t n;

		if (output != NULL) {
			at = strstr(output + found, text);
			if (at != NULL) {
				found = (size_t)(at - output) + strlen(text);
				return true;
			}
		}
		if (left <= 0 || poll(&p, 1, (int)(left * 1000) + 1) <= 0)
			return false;
		if (output_size - output_len < 4097) {
			output_size = output_size * 2 + 8192;
			output = realloc(output, output_size);
			if (output == NULL)
				fail("%s", "out of memory");
		}
		n = read(master, output + output_len, 4096);
		if (n <= 0)
			return false; /* EIO once smirror has closed the terminal */
		output_len += (size_t)n;
		output[output_len] = '\0';
	}
}

/* The bytes of TEXT, its escapes decoded; returns the length. */
static size_t unescape(const char *text, char *out)
{
	size_t n = 0;

	while (*text != '\0') {
		if (*text != '\\') {
			out[n++] = *text++;
			continue;
		}
		text++;
		switch (*text) {
		case 'r':
			out[n++] = '\r';
			text++;
			break;
		case 'n':
			out[n++] = '\n';
			text++;
			break;
		case 'x':
			out[n++] =
				(char)strtol((char[3]){text[1], text[2], '\0'}, NULL, 16);
			text += 3;
			break;
		default:
			out[n++] = *text++;
			break;
		}
	}
	return n;
}

static void type(const char *text)
{
	char bytes[512];
	size_t n = unescape(text, bytes);

	if (write(master, bytes, n) != (ssize_t)n)
		fail("cannot type \"%s\"", text);
}

static void start(char **argv)
{
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("drive: pseudo-terminal");
		exit(1);
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
		execv(argv[0], argv);
		_exit(127);
	}
}

static bool copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[65536];
	size_t n;
	bool ok = true;

	if (in == NULL)
		return false;
	out = fopen(to, "wb");
	if (out == NULL) {
		fclose(in);
		return false;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok &= fwrite(buf, 1, n, out) == n;
	ok &= !ferror(in);
	fclose(in);
	ok &= fclose(out) == 0;
	return ok;
}

int main(int argc, char **argv)
{
	char line[512], *arg;
	double timeout = 60;
	unsigned number = 0;
	int status;
	FILE *script;

	if (argc < 5) {
		fputs("usage: drive CARD COPY SCRIPT SMIRROR ARGUMENT...\n", stderr);
		return 2;
	}
	if (access(argv[1], R_OK) != 0) {
		printf("drive: skipped, no %s: make it with scripts/uboot-card.sh\n",
		       argv[1]);
		return SKIP;
	}
	if (!copy(argv[1], argv[2])) {
		perror("drive: copying the card");
		return 1;
	}
	script = fopen(argv[3], "r");
	if (script == NULL) {
		perror("drive: the script");
		return 1;
	}
	start(argv + 4);

	while (fgets(line, sizeof(line), script) != NULL) {
		number++;
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		arg = strchr(line, ' ');
		arg = arg != NULL ? arg + 1 : line + strlen(line);
		if (strncmp(line, "timeout ", 8) == 0) {
			timeout = atof(arg);
		} else if (strncmp(line, "expect ", 7) == 0) {
			if (!read_until(arg, timeout))
				fail("no \"%s\" in time", arg);
		} else if (strncmp(line, "type ", 5) == 0) {
			type(arg);
		} else if (strcmp(line, "quit") == 0) {
			type("\\x01x");
			if (!read_until(QUIT_MESSAGE, timeout))
				fail("%s", "Ctrl-A x did not quit");
			while (waitpid(child, &status, 0) < 0 && errno == EINTR)
				continue;
			child = 0;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				fail("%s", "smirror did not end with status 0");
		} else {
			fprintf(stderr, "drive: %s:%u: what is \"%s\"?\n", argv[3], number,
			        line);
			kill(child, SIGKILL);
			return 1;
		}
	}
	fclose(script);
	if (child > 0)
		fail("%s", "the script ends without quit");
	printf("drive: %u lines, ok\n", number);
	return 0;
}
