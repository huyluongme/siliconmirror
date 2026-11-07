/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * smirror command-line front end.
 *
 * Options follow QEMU conventions: they start with a single dash. A double
 * dash is accepted as well, so -version and --version mean the same thing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SMIRROR_VERSION
#error "SMIRROR_VERSION must be defined by the build system"
#endif

static void print_usage(FILE *out)
{
	fputs("Usage: smirror [options]\n"
	      "\n"
	      "Options:\n"
	      "  -h, -help    Show this help and exit\n"
	      "  -version     Show version information and exit\n"
	      "\n"
	      "Options may also start with a double dash, for example "
	      "--version.\n",
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

int main(int argc, char **argv)
{
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
		fprintf(stderr, "smirror: unknown option '%s'\n", argv[i]);
		fputs("Try 'smirror -help' for more information.\n", stderr);
		return EXIT_FAILURE;
	}

	/* Nothing can run yet: no machine has been implemented. */
	print_usage(stderr);
	return EXIT_FAILURE;
}
