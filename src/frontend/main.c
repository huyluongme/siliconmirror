/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * smirror command-line front end.
 *
 * Options follow QEMU conventions: they start with a single dash, and an
 * option that takes a value reads it from the next argument. A double
 * dash is accepted as well, so -version and --version mean the same
 * thing.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/char/file.h"
#include "backend/char/stdio.h"
#include "core/machine.h"
#include "machine/board.h"

#ifndef SMIRROR_VERSION
#error "SMIRROR_VERSION must be defined by the build system"
#endif

typedef struct Options {
	const char *machine; /* -M */
	const char *kernel;  /* -kernel */
	const char *serial;  /* -serial */
} Options;

static void print_usage(FILE *out)
{
	fputs("Usage: smirror [options]\n"
	      "\n"
	      "Options:\n"
	      "  -M, -machine NAME  Board to emulate; -M help lists them\n"
	      "  -kernel FILE       Image to boot, loaded as the board's "
	      "firmware would\n"
	      "  -serial DEV        Where the serial console goes: stdio (the "
	      "default),\n"
	      "                     null or file:PATH\n"
	      "  -h, -help          Show this help and exit\n"
	      "  -version           Show version information and exit\n"
	      "\n"
	      "Options may also start with a double dash, for example "
	      "--version.\n"
	      "\n"
	      "With -serial stdio on a terminal, press Ctrl-A x to quit, and "
	      "Ctrl-A Ctrl-A\n"
	      "to send a Ctrl-A.\n",
	      out);
}

static void print_version(void)
{
	printf("smirror %s\n"
	       "Copyright (C) 2026 Huy Luong Duc\n"
	       "License GPL-2.0-or-later: GNU GPL version 2 or later.\n"
	       "This is free software: you are free to change and "
	       "redistribute it.\n"
	       "There is NO WARRANTY, to the extent permitted by law.\n",
	       SMIRROR_VERSION);
}

static void print_machines(void)
{
	puts("Supported machines are:");
	for (const Board *const *b = board_list; *b != NULL; b++)
		printf("%-20s %s\n", (*b)->name, (*b)->description);
}

/*
 * Return the option name without its leading dashes, or NULL if the
 * argument is not an option.
 */
static const char *option_name(const char *arg)
{
	if (arg[0] != '-' || arg[1] == '\0')
		return NULL;
	arg++;
	if (arg[0] == '-')
		arg++;
	return arg;
}

/* Where the value of an option goes, or NULL if it takes no value. */
static const char **option_value(Options *options, const char *name)
{
	if (strcmp(name, "M") == 0 || strcmp(name, "machine") == 0)
		return &options->machine;
	if (strcmp(name, "kernel") == 0)
		return &options->kernel;
	if (strcmp(name, "serial") == 0)
		return &options->serial;
	return NULL;
}

/*
 * Open the back end that -serial names. "null" leaves the serial
 * console unconnected, which is a NULL stream. Returns false after printing a
 * message.
 */
static bool open_serial(const char *spec, CharStream **stream)
{
	*stream = NULL;
	if (strcmp(spec, "null") == 0)
		return true;

	if (strcmp(spec, "stdio") == 0) {
		*stream = char_stdio_create();
	} else if (strncmp(spec, "file:", 5) == 0) {
		FILE *file = fopen(spec + 5, "wb");

		if (file == NULL) {
			fprintf(stderr, "smirror: %s: %s\n", spec + 5, strerror(errno));
			return false;
		}
		*stream = char_file_create(file, true);
		if (*stream == NULL)
			fclose(file);
	} else {
		fprintf(stderr, "smirror: unknown -serial device '%s'\n", spec);
		return false;
	}

	if (*stream == NULL) {
		fputs("smirror: out of memory\n", stderr);
		return false;
	}
	return true;
}

/* Ctrl-A x on the terminal. */
static void quit(void *ctx)
{
	machine_stop(ctx, MACHINE_STOP_SHUTDOWN, "the user pressed Ctrl-A x");
}

/* Run the machine and return the exit status that its stop calls for. */
static int run(Machine *m)
{
	MachineStop stop = machine_run(m, 0);

	fprintf(stderr, "smirror: %s: %s\n", machine_stop_str(stop),
	        m->stop_message);
	return stop == MACHINE_STOP_ERROR ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	Options options = {.serial = "stdio"};
	bool serial_given = false;

	for (int i = 1; i < argc; i++) {
		const char *name = option_name(argv[i]);

		if (name == NULL) {
			fprintf(stderr, "smirror: unexpected argument '%s'\n", argv[i]);
			return EXIT_FAILURE;
		}
		if (strcmp(name, "h") == 0 || strcmp(name, "help") == 0) {
			print_usage(stdout);
			return EXIT_SUCCESS;
		}
		if (strcmp(name, "version") == 0) {
			print_version();
			return EXIT_SUCCESS;
		}

		const char **value = option_value(&options, name);

		if (value == NULL) {
			fprintf(stderr, "smirror: unknown option '%s'\n", argv[i]);
			fputs("Try 'smirror -help' for more information.\n", stderr);
			return EXIT_FAILURE;
		}
		if (i + 1 == argc) {
			fprintf(stderr, "smirror: option '%s' needs a value\n", argv[i]);
			return EXIT_FAILURE;
		}
		if (value == &options.serial) {
			if (serial_given) {
				fputs("smirror: only one -serial is supported\n", stderr);
				return EXIT_FAILURE;
			}
			serial_given = true;
		}
		*value = argv[++i];
	}

	if (options.machine == NULL) {
		fputs("smirror: no machine given: use -M NAME "
		      "(-M help lists them)\n",
		      stderr);
		return EXIT_FAILURE;
	}
	if (strcmp(options.machine, "help") == 0) {
		print_machines();
		return EXIT_SUCCESS;
	}

	const Board *board = board_find(options.machine);

	if (board == NULL) {
		fprintf(stderr, "smirror: unknown machine '%s' (-M help lists them)\n",
		        options.machine);
		return EXIT_FAILURE;
	}

	BoardOptions board_options = {.kernel = options.kernel};
	char error[512];

	if (!open_serial(options.serial, &board_options.serial))
		return EXIT_FAILURE;

	CharStream *terminal =
		strcmp(options.serial, "stdio") == 0 ? board_options.serial : NULL;

	Machine *m = board->create(&board_options, error, sizeof(error));

	if (m == NULL) {
		fprintf(stderr, "smirror: %s: %s\n", board->name, error);
		return EXIT_FAILURE;
	}

	if (terminal != NULL)
		char_stdio_on_quit(terminal, quit, m);

	int status = run(m);

	machine_destroy(m);
	return status;
}
