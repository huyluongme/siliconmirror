/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Minimal unit test helpers.
 *
 * Each test program is one source file that includes this header, runs
 * its test functions with RUN_TEST() and returns test_summary() from
 * main(). A failed check prints its file and line and the test goes on.
 */

#ifndef TESTS_UNIT_TEST_H
#define TESTS_UNIT_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static int test_failures;

/* Fail the current test if cond is false. */
#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,   \
			        #cond);                                                    \
			test_failures++;                                                   \
		}                                                                      \
	} while (0)

/* Fail the current test if two integer values differ. */
#define CHECK_EQ(actual, expected)                                             \
	do {                                                                       \
		uint64_t actual_ = (uint64_t)(actual);                                 \
		uint64_t expected_ = (uint64_t)(expected);                             \
		if (actual_ != expected_) {                                            \
			fprintf(stderr,                                                    \
			        "%s:%d: %s is 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",    \
			        __FILE__, __LINE__, #actual, actual_, expected_);          \
			test_failures++;                                                   \
		}                                                                      \
	} while (0)

/* Run one test function and print whether it passed. */
#define RUN_TEST(fn)                                                           \
	do {                                                                       \
		int before_ = test_failures;                                           \
		fn();                                                                  \
		printf("%s %s\n", test_failures == before_ ? "PASS" : "FAIL", #fn);    \
	} while (0)

/* Return the exit status for main(): 0 if every check passed. */
static inline int test_summary(void)
{
	if (test_failures == 0)
		return 0;
	fprintf(stderr, "%d check(s) failed\n", test_failures);
	return 1;
}

#endif /* TESTS_UNIT_TEST_H */
