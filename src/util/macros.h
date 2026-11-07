/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/* Small general-purpose macros. */

#ifndef UTIL_MACROS_H
#define UTIL_MACROS_H

#include <stddef.h>

/* Number of elements in an array. Does not work on a pointer. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Pointer to the struct of the given type that contains member, from a
 * pointer to that member. The name is the one Linux uses.
 */
#define container_of(ptr, type, member)                                        \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* Let the compiler check the arguments of a printf-style function. */
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FORMAT(fmt_index, first_arg_index)                              \
	__attribute__((format(printf, fmt_index, first_arg_index)))
#else
#define PRINTF_FORMAT(fmt_index, first_arg_index)
#endif

#endif /* UTIL_MACROS_H */
