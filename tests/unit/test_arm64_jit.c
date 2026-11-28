/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The JIT of the AArch64 model (src/cpu/arm64/jit.c) against the
 * interpreter, its specification. Each test runs one program on two
 * machines, the second with a JIT cache, in turns of many sizes or in
 * one long turn, and after each turn compares what the guest and the
 * machine can see: registers, PSTATE, the exception registers of EL3,
 * the instructions executed, the virtual time, why the machine stopped.
 * Then it checks the blocks that the JIT made.
 *
 * A probe device at PROBE reads as the virtual time (offset 0), drives
 * the IRQ input of core 0 (4), turns core 0 off as a power controller
 * would (12) and stops the machine (16). The vectors at VECTORS count
 * synchronous exceptions in X8 and return after the instruction (or,
 * for a misaligned PC, to the instruction after the aligned one) with
 * PSTATE.IL clear; an
 * IRQ copies X5 into X6, lowers the IRQ input and counts in X11.
 *
 * The programs were assembled with GNU as (aarch64-linux-gnu-as).
 */

#include "core/machine.h"
#include "cpu/arm64/cpu.h"
#include "cpu/jit/cache.h"
#include "test.h"
#include "util/macros.h"

#include <string.h>
#include <unistd.h>

#if defined(__linux__) && defined(__x86_64__)

#define CODE 0x80000
#define VECTORS 0x90000
#define TABLES 0xa0000
#define PROBE 0x10000000
#define MAX_CORES 2

/* Instructions in all the turns of a run: a mutant may loop. */
#define RUN_LIMIT 20000

#define EC(esr) ((uint32_t)((esr) >> 26))
#define EC_ILLEGAL_STATE 0x0e
#define EC_PC_ALIGNMENT 0x22

typedef struct Side {
	Machine *m;
	Arm64Cpu *c[MAX_CORES];
	JitCache *jit; /* NULL on the interpreter's side */
} Side;

typedef struct Pair {
	Side interp, jit;
	unsigned cores;
} Pair;

static uint64_t probe_read(void *ctx, uint64_t offset, unsigned size)
{
	Side *s = ctx;

	(void)size;
	return offset == 0 ? machine_now(s->m) : 0;
}

static void probe_write(void *ctx, uint64_t offset, uint64_t value,
                        unsigned size)
{
	Side *s = ctx;

	(void)size;
	if (offset == 4)
		irq_set(arm64_cpu_input(s->c[0], ARM64_INPUT_IRQ), value & 1);
	else if (offset == 12)
		s->c[0]->cpu.state = CPU_OFF;
	else if (offset == 16)
		machine_stop(s->m, MACHINE_STOP_SHUTDOWN, "the probe stops it");
}

static const BusMmioOps probe_ops = {.read = probe_read, .write = probe_write};

/* At VECTORS + 0x200: a synchronous exception from EL3. */
static const uint32_t sync_handler[] = {
	0x91000508, /* add x8, x8, #1 */
	0xd53e4027, /* mrs x7, elr_el3 */
	0x927ef4e7, /* and x7, x7, #~3 */
	0x910010e7, /* add x7, x7, #4 */
	0xd51e4027, /* msr elr_el3, x7 */
	0xd53e4007, /* mrs x7, spsr_el3 */
	0x926bf8e7, /* and x7, x7, #~(1 << 20): PSTATE.IL clear */
	0xd51e4007, /* msr spsr_el3, x7 */
	0xd69f03e0, /* eret */
};

/* At VECTORS + 0x280: an IRQ at EL3. */
static const uint32_t irq_handler[] = {
	0xaa0503e6, /* mov x6, x5 */
	0xb900053f, /* str wzr, [x9, #4] */
	0x9100056b, /* add x11, x11, #1 */
	0xd69f03e0, /* eret */
};

#define ADD_X5 0x910004a5   /* add x5, x5, #1 */
#define ADD_X0 0x91000400   /* add x0, x0, #1 */
#define WFI 0xd503207f      /* wfi */
#define X9_PROBE 0xd2a20009 /* movz x9, #0x1000, lsl #16 */

static void put(Side *s, uint64_t pa, const uint32_t *words, size_t n)
{
	for (size_t i = 0; i < n; i++)
		CHECK_EQ(bus_write(s->m->bus, pa + 4 * i, 4, words[i]), BUS_OK);
}

/*
 * A machine with cores; with a JIT cache of code_size bytes of code and
 * max_blocks blocks when max_blocks is not 0.
 */
static void side_init(Side *s, unsigned cores, size_t code_size,
                      size_t max_blocks)
{
	Arm64Config config = {.reset_pc = CODE, .highest_el = 3};

	s->m = machine_create();
	CHECK_EQ(bus_map_ram(s->m->bus, "ram", 0, 0x100000), BUS_OK);
	CHECK_EQ(bus_map_mmio(s->m->bus, "probe", PROBE, 0x1000, &probe_ops, s),
	         BUS_OK);
	s->jit = NULL;
	if (max_blocks != 0) {
		s->jit = jit_cache_create(s->m->bus, code_size, max_blocks);
		CHECK(s->jit != NULL);
	}
	config.jit = s->jit;
	for (unsigned i = 0; i < cores; i++) {
		Cpu *cpu;

		config.mpidr = i;
		cpu = arm64_cpu_create(&config);
		CHECK(machine_add_cpu(s->m, cpu));
		s->c[i] = arm64_cpu_of(cpu);
		s->c[i]->sys.vbar[3] = VECTORS;
		s->c[i]->sys.scr_el3 |= ARM64_SCR_IRQ | ARM64_SCR_RW;
	}
	put(s, VECTORS + 0x200, sync_handler, ARRAY_SIZE(sync_handler));
	put(s, VECTORS + 0x280, irq_handler, ARRAY_SIZE(irq_handler));
}

/* Two machines with the program code at CODE; the JIT's cache has
 * code_size bytes of code and max_blocks blocks. */
static void pair_init(Pair *p, unsigned cores, const uint32_t *code, size_t n,
                      size_t code_size, size_t max_blocks)
{
	p->cores = cores;
	side_init(&p->interp, cores, 0, 0);
	side_init(&p->jit, cores, code_size, max_blocks);
	put(&p->interp, CODE, code, n);
	put(&p->jit, CODE, code, n);
}

#define PAIR_INIT(p, cores, code)                                              \
	pair_init(p, cores, code, ARRAY_SIZE(code), 1 << 20, 1024)

static void pair_free(Pair *p)
{
	jit_cache_destroy(p->jit.jit); /* before its bus */
	machine_destroy(p->interp.m);
	machine_destroy(p->jit.m);
}

/* Do the same to both sides. */
#define BOTH(p, s, stmt)                                                       \
	do {                                                                       \
		Side *s = &(p)->interp;                                                \
		stmt;                                                                  \
		s = &(p)->jit;                                                         \
		stmt;                                                                  \
	} while (0)

static bool same_core(const Arm64Cpu *a, const Arm64Cpu *b)
{
	return a->pc == b->pc && a->el == b->el && a->nzcv == b->nzcv &&
	       a->daif == b->daif && a->sp_sel == b->sp_sel && a->il == b->il &&
	       a->cpu.state == b->cpu.state && a->cpu.executed == b->cpu.executed &&
	       a->asserted == b->asserted &&
	       memcmp(a->x, b->x, sizeof(a->x)) == 0 &&
	       memcmp(a->sp_el, b->sp_el, sizeof(a->sp_el)) == 0 &&
	       a->sys.elr[3] == b->sys.elr[3] && a->sys.esr[3] == b->sys.esr[3] &&
	       a->sys.spsr[3] == b->sys.spsr[3] && a->sys.far[3] == b->sys.far[3];
}

/* Whether both sides look the same; prints what differs once. */
static bool same(const Pair *p, int line)
{
	const Machine *a = p->interp.m, *b = p->jit.m;
	bool cores = true;

	for (unsigned i = 0; i < p->cores; i++)
		cores = cores && same_core(p->interp.c[i], p->jit.c[i]);
	if (cores && a->insns == b->insns && machine_now(a) == machine_now(b) &&
	    a->stop == b->stop && strcmp(a->stop_message, b->stop_message) == 0)
		return true;
	fprintf(stderr,
	        "line %d: the JIT differs: pc 0x%" PRIx64 " / 0x%" PRIx64
	        ", instructions %" PRIu64 " / %" PRIu64 ", x5 %" PRIu64
	        " / %" PRIu64 ", stop \"%s\" / \"%s\"\n",
	        line, p->interp.c[0]->pc, p->jit.c[0]->pc, a->insns, b->insns,
	        p->interp.c[0]->x[5], p->jit.c[0]->x[5], a->stop_message,
	        b->stop_message);
	test_failures++;
	return false;
}

/* Turns of many sizes, around the length of the blocks. */
static const uint64_t small_turns[] = {1, 2, 3, 4, 5, 7, 11, 64, 65};
static const uint64_t long_turn[] = {RUN_LIMIT};

/*
 * Run both sides in the turns given, over and over, until they stop
 * other than at the limit of a turn; compare them after each turn.
 * Returns why they stopped, MACHINE_STOP_LIMIT when they differ or
 * do not stop.
 */
static MachineStop run_turns(Pair *p, const uint64_t *turns, size_t n, int line)
{
	uint64_t total = 0;

	for (size_t i = 0; total < RUN_LIMIT; i++) {
		MachineStop a = machine_run(p->interp.m, turns[i % n]);
		MachineStop b = machine_run(p->jit.m, turns[i % n]);

		total += turns[i % n];
		if (a != b || !same(p, line))
			return MACHINE_STOP_LIMIT;
		if (a != MACHINE_STOP_LIMIT)
			return a;
	}
	fprintf(stderr, "line %d: no stop after %d instructions\n", line,
	        RUN_LIMIT);
	test_failures++;
	return MACHINE_STOP_LIMIT;
}

#define RUN_SMALL(p)                                                           \
	run_turns(p, small_turns, ARRAY_SIZE(small_turns), __LINE__)
#define RUN_LONG(p) run_turns(p, long_turn, 1, __LINE__)

/* The block at va (= pa) that the JIT made at EL3, NULL if none. */
static const JitBlock *block(const Pair *p, uint64_t va)
{
	return jit_cache_find(p->jit.jit, va, va, 3);
}

/*
 * A loop: blocks end at branches and at system instructions, and the
 * first instruction of a page is the interpreter's, whose fetch fills
 * the page cache.
 */
static const uint32_t loop[] = {
	0xd2800000, /* mov x0, #0 */
	0xd2800c81, /* mov x1, #100 */
	0x91000c00, /* 1: add x0, x0, #3 */
	0xd1000421, /* sub x1, x1, #1 */
	0xb5ffffc1, /* cbnz x1, 1b */
	WFI,        /* wfi */
};

static void test_loop(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 1, loop);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[0], 300);
		CHECK_EQ(p.jit.m->insns, 303);
		if (run == 1) {
			CHECK_EQ(p.jit.jit->translations, 3);
			CHECK(block(&p, CODE) == NULL);
			CHECK(block(&p, CODE + 4) && block(&p, CODE + 4)->count == 4);
			CHECK(block(&p, CODE + 8) && block(&p, CODE + 8)->count == 3);
			CHECK(block(&p, CODE + 20) && block(&p, CODE + 20)->count == 1);
		}
		pair_free(&p);
	}
}

/*
 * Virtual time inside a block counts each instruction as it ends. (A
 * turn that ends at the limit takes the time of a whole slice: only the
 * long run gives the distances.)
 */
static const uint32_t timing[] = {
	X9_PROBE,   /* movz x9, #0x1000, lsl #16 */
	0xf9400120, /* ldr x0, [x9] */
	ADD_X5,     /* add x5, x5, #1 */
	0xf9400121, /* ldr x1, [x9] */
	0xf9400122, /* ldr x2, [x9] */
	ADD_X5,     /* add x5, x5, #1 */
	0xf9400123, /* ldr x3, [x9] */
	WFI,        /* wfi */
};

static void test_time(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 1, timing);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		if (run == 1) {
			CHECK_EQ(p.jit.c[0]->x[1] - p.jit.c[0]->x[0], 2);
			CHECK_EQ(p.jit.c[0]->x[2] - p.jit.c[0]->x[1], 1);
			CHECK_EQ(p.jit.c[0]->x[3] - p.jit.c[0]->x[2], 2);
		}
		pair_free(&p);
	}
}

/*
 * An IRQ that an MMIO write raises in the middle of a block is taken
 * after that instruction; one raised while masked, after the MSR that
 * unmasks it.
 */
static const uint32_t irq_unmasked[] = {
	X9_PROBE,   /* movz x9, #0x1000, lsl #16 */
	0x52800021, /* mov w1, #1 */
	0xd50342ff, /* msr daifclr, #2 */
	ADD_X5,     /* add x5, x5, #1 */
	0xb9000521, /* str w1, [x9, #4] */
	ADD_X5,     /* add x5, x5, #1 */
	ADD_X5,     /* add x5, x5, #1 */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static const uint32_t irq_masked[] = {
	X9_PROBE,   /* movz x9, #0x1000, lsl #16 */
	0x52800021, /* mov w1, #1 */
	0xb9000521, /* str w1, [x9, #4] */
	ADD_X5,     /* add x5, x5, #1 */
	ADD_X5,     /* add x5, x5, #1 */
	0xd50342ff, /* msr daifclr, #2 */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static void test_irq(void)
{
	Pair p;

	for (int run = 0; run < 4; run++) {
		if (run < 2)
			PAIR_INIT(&p, 1, irq_unmasked);
		else
			PAIR_INIT(&p, 1, irq_masked);
		CHECK_EQ(run & 1 ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[6], run < 2 ? 1 : 2);
		CHECK_EQ(p.jit.c[0]->x[11], 1);
		CHECK_EQ(p.jit.c[0]->x[5], run < 2 ? 4 : 3);
		pair_free(&p);
	}
}

/* An exception in the middle of a block ends it. */
static const uint32_t undefined[] = {
	ADD_X5,     /* add x5, x5, #1 */
	0x00000000, /* udf #0 */
	ADD_X5,     /* add x5, x5, #1 */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static void test_exception(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 1, undefined);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[8], 1);
		CHECK_EQ(p.jit.c[0]->x[5], 3);
		pair_free(&p);
	}
}

/*
 * What stops a block in the middle: an access to nothing, which stops
 * the machine; a device that turns the core off; one that stops the
 * machine.
 */
static const uint32_t missing[] = {
	0xd2a4000a, /* movz x10, #0x2000, lsl #16 */
	ADD_X5,     /* add x5, x5, #1 */
	0xf9400140, /* ldr x0, [x10] */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static const uint32_t core_off[] = {
	X9_PROBE,   /* movz x9, #0x1000, lsl #16 */
	ADD_X5,     /* add x5, x5, #1 */
	0xb9000d3f, /* str wzr, [x9, #12] */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static const uint32_t probe_stop[] = {
	X9_PROBE,   /* movz x9, #0x1000, lsl #16 */
	ADD_X5,     /* add x5, x5, #1 */
	0xb900113f, /* str wzr, [x9, #16] */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static void test_stop(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 1, missing);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_ERROR);
		CHECK(strstr(p.jit.m->stop_message, "0x0000000020000000") != NULL);
		CHECK_EQ(p.jit.c[0]->x[5], 1);
		pair_free(&p);

		PAIR_INIT(&p, 1, core_off);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->cpu.state, CPU_OFF);
		CHECK_EQ(p.jit.c[0]->x[5], 1);
		pair_free(&p);

		PAIR_INIT(&p, 1, probe_stop);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_SHUTDOWN);
		CHECK_EQ(p.jit.c[0]->x[5], 1);
		pair_free(&p);
	}
}

/*
 * The faults of a misaligned PC and of PSTATE.IL come before a block:
 * a branch to 0x80012, then (MOVK_X3_11) to 0x80011.
 */
#define MOVK_X3_11 0xf2800223 /* movk x3, #0x11 */

static uint32_t misaligned[] = {
	0xd2a00103, /* movz x3, #0x8, lsl #16 */
	0xf2800243, /* movk x3, #0x12 */
	0xd61f0060, /* br x3 */
	0x910190a5, /* add x5, x5, #100 */
	0x910190a5, /* add x5, x5, #100 */
	ADD_X5,     /* add x5, x5, #1 */
	WFI,        /* wfi */
};

static const uint32_t two_adds[] = {ADD_X0, ADD_X0, WFI};

static void test_pc_faults(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		for (int target = 0; target < 2; target++) {
			uint32_t code[ARRAY_SIZE(misaligned)];

			memcpy(code, misaligned, sizeof(code));
			if (target == 1)
				code[1] = MOVK_X3_11;
			PAIR_INIT(&p, 1, code);
			CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
			CHECK_EQ(p.jit.c[0]->x[8], 1);
			CHECK_EQ(p.jit.c[0]->x[5], 1);
			CHECK_EQ(p.jit.c[0]->sys.far[3], target ? 0x80011 : 0x80012);
			CHECK_EQ(EC(p.jit.c[0]->sys.esr[3]), EC_PC_ALIGNMENT);
			pair_free(&p);
		}

		PAIR_INIT(&p, 1, two_adds);
		BOTH(&p, s, s->c[0]->il = true);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[8], 1);
		CHECK_EQ(p.jit.c[0]->x[0], 1); /* the first ADD did not run */
		CHECK_EQ(EC(p.jit.c[0]->sys.esr[3]), EC_ILLEGAL_STATE);
		pair_free(&p);
	}
}

/*
 * Blocks of at most 64 instructions, which run only when the turn has
 * room for all of them, and which end at the end of a page.
 */
static void test_length(void)
{
	uint32_t adds[101];
	Pair p;

	for (unsigned i = 0; i < 100; i++)
		adds[i] = ADD_X0;
	adds[100] = WFI;
	for (int run = 0; run < 3; run++) {
		static const uint64_t fifty[] = {50};

		PAIR_INIT(&p, 1, adds);
		if (run == 2)
			CHECK_EQ(run_turns(&p, fifty, 1, __LINE__), MACHINE_STOP_IDLE);
		else
			CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[0], 100);
		if (run == 1) {
			CHECK_EQ(p.jit.jit->translations, 2);
			CHECK(block(&p, CODE + 4) && block(&p, CODE + 4)->count == 64);
			CHECK(block(&p, CODE + 260) && block(&p, CODE + 260)->count == 36);
		}
		pair_free(&p);
	}

	/*
	 * From 0x80fdc, 11 ADDs and a WFI: the interpreter's first, a block
	 * of eight to the end of the page, the interpreter's first of the
	 * next page, a block of two.
	 */
	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 1, adds);
		BOTH(&p, s, put(s, 0x80fdc, &adds[89], 12));
		BOTH(&p, s, s->c[0]->pc = 0x80fdc);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[0], 11);
		if (run == 1) {
			CHECK_EQ(p.jit.jit->translations, 2);
			CHECK(block(&p, 0x80fe0) && block(&p, 0x80fe0)->count == 8);
			CHECK(block(&p, 0x81004) && block(&p, 0x81004)->count == 2);
		}
		pair_free(&p);
	}
}

/* Run both sides again from va at el, after a WFI. */
static void again(Pair *p, uint64_t va, unsigned el)
{
	BOTH(p, s, s->c[0]->pc = va);
	BOTH(p, s, s->c[0]->el = el);
	BOTH(p, s, s->c[0]->cpu.state = CPU_RUNNING);
}

/* A block is kept for its Exception level and its virtual address. */
static void test_key(void)
{
	Pair p;

	PAIR_INIT(&p, 1, two_adds);
	CHECK_EQ(RUN_LONG(&p), MACHINE_STOP_IDLE);
	again(&p, CODE, 1);
	CHECK_EQ(RUN_LONG(&p), MACHINE_STOP_IDLE);
	CHECK_EQ(p.jit.c[0]->x[0], 4);
	CHECK_EQ(p.jit.jit->translations, 2);
	CHECK(jit_cache_find(p.jit.jit, CODE + 4, CODE + 4, 3) != NULL);
	CHECK(jit_cache_find(p.jit.jit, CODE + 4, CODE + 4, 1) != NULL);
	pair_free(&p);

	/*
	 * Stage 1 translation at EL3 maps the first two GB to the first: the
	 * code at VA 0x80000 and at 0x40080000. TCR_EL3 0x20019: 39-bit VAs
	 * from level 1, 4KB granule; a level 1 block of Normal memory
	 * (MAIR_EL3 attribute 0 is 0xff) with AF.
	 */
	PAIR_INIT(&p, 1, two_adds);
	BOTH(&p, s, {
		CHECK_EQ(bus_write(s->m->bus, TABLES, 8, 0x401), BUS_OK);
		CHECK_EQ(bus_write(s->m->bus, TABLES + 8, 8, 0x401), BUS_OK);
		s->c[0]->sys.tcr[3] = 0x20019;
		s->c[0]->sys.ttbr0[3] = TABLES;
		s->c[0]->sys.mair[3] = 0xff;
		s->c[0]->sys.sctlr[3] |= ARM64_SCTLR_M;
	});
	CHECK_EQ(RUN_LONG(&p), MACHINE_STOP_IDLE);
	again(&p, 0x40000000 + CODE, 3);
	CHECK_EQ(RUN_LONG(&p), MACHINE_STOP_IDLE);
	CHECK_EQ(p.jit.c[0]->x[0], 4);
	CHECK_EQ(p.jit.jit->translations, 2);
	CHECK(jit_cache_find(p.jit.jit, CODE + 4, CODE + 4, 3) != NULL);
	CHECK(jit_cache_find(p.jit.jit, CODE + 4, 0x40000000 + CODE + 4, 3) !=
	      NULL);
	pair_free(&p);
}

/* The cores of a machine share the cache. */
static void test_cores(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		PAIR_INIT(&p, 2, loop);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[0], 300);
		CHECK_EQ(p.jit.c[1]->x[0], 300);
		if (run == 1)
			CHECK_EQ(p.jit.jit->translations, 3);
		pair_free(&p);
	}
}

/* A full cache is emptied, and translation starts again. */
static void test_full(void)
{
	Pair p;

	for (int run = 0; run < 2; run++) {
		pair_init(&p, 1, loop, ARRAY_SIZE(loop), 4096, 2);
		CHECK_EQ(run ? RUN_LONG(&p) : RUN_SMALL(&p), MACHINE_STOP_IDLE);
		CHECK_EQ(p.jit.c[0]->x[0], 300);
		if (run == 1) {
			CHECK_EQ(p.jit.jit->flushes, 1);
			CHECK_EQ(p.jit.jit->translations, 3);
			CHECK_EQ(p.jit.jit->num_blocks, 1);
			CHECK(block(&p, CODE + 20) != NULL);
		}
		pair_free(&p);
	}
}

int main(void)
{
	alarm(60); /* a mutant may loop in translated code */
	RUN_TEST(test_loop);
	RUN_TEST(test_time);
	RUN_TEST(test_irq);
	RUN_TEST(test_exception);
	RUN_TEST(test_stop);
	RUN_TEST(test_pc_faults);
	RUN_TEST(test_length);
	RUN_TEST(test_key);
	RUN_TEST(test_cores);
	RUN_TEST(test_full);
	return test_summary();
}

#else

int main(void)
{
	puts("SKIP: the JIT needs a Linux x86-64 host");
	return 0;
}

#endif
