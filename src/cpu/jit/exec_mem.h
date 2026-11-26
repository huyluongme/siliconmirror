/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#ifndef SMIRROR_CPU_JIT_EXEC_MEM_H
#define SMIRROR_CPU_JIT_EXEC_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Memory for the JIT's code, never writable and executable at the same
 * address (W^X): the same pages are mapped twice, once readable and
 * writable (`write`), once readable and executable (`run`). Code is
 * written through `write` and run at the same offset of `run`.
 *
 * x86 processors see a store into code through any address, since they
 * snoop by physical address; a jump to the new code (a call through a
 * function pointer is one) is all that SDM vol. 3A 11.1.3 asks for
 * before running it ("option 1").
 *
 * Linux only (memfd_create()); elsewhere, and where the kernel refuses
 * (vm.memfd_noexec = 2, an SELinux policy), exec_mem_init() fails and
 * the caller keeps the interpreter.
 */
typedef struct ExecMem {
	uint8_t *write; /* the pages, readable and writable */
	uint8_t *run;   /* the same pages, readable and executable */
	size_t size;    /* bytes, a whole number of host pages */
} ExecMem;

/*
 * clang's -fsanitize=function checks an indirect call by reading a type
 * signature placed just before the called function. Generated code has
 * none, and before the first byte of an ExecMem nothing is mapped, so a
 * function that calls into an ExecMem is marked with this.
 */
#if defined(__clang__)
#define EXEC_MEM_CALLER __attribute__((no_sanitize("function")))
#else
#define EXEC_MEM_CALLER
#endif

/* Maps at least `size` bytes, rounded up to whole host pages, zeroed.
 * On failure, returns false with errno set and leaves *mem empty. */
bool exec_mem_init(ExecMem *mem, size_t size);

/* Unmaps; an empty or failed ExecMem is fine too. */
void exec_mem_free(ExecMem *mem);

#endif
