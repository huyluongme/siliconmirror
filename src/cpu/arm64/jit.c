/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The JIT of the AArch64 model, phase 3a (docs/architecture.md, "The
 * JIT"): blocks of guest code and run() of a core that has a JIT cache.
 * The interpreter stays the specification: a core executes the same
 * instructions in the same virtual time with the JIT as without it.
 *
 * A block starts at the PC and holds the instructions after it in the
 * same 4KB page, at most BLOCK_MAX of them. It ends after an instruction
 * of the group "Branches, Exception Generating and System instructions"
 * of C4.1: they may change the PC, PSTATE, the translation regime or the
 * interrupt masks. Phase 3a translates each instruction into a call of
 * block_step(), the interpreter's step without its fetch, with the word
 * that the block read from RAM; later phases translate instructions into
 * code of their own.
 *
 * A block is found through the fetch page cache (ARM64_PAGE_FETCH), which
 * holds what the interpreter's fetch translated and checked: the page is
 * RAM, and the instruction fetch is permitted at the Exception level.
 * When the PC's page is not there, the interpreter steps the instruction,
 * and its fetch fills the page cache. A block is kept for the physical
 * address, the virtual address and the Exception level of its first
 * instruction.
 *
 * Code written to RAM is seen as the interpreter sees it, at the next
 * instruction. The cache watches each page that it holds blocks of, and
 * a write that the bus makes to the page drops them: a device's DMA, a
 * loader, a core's slow path. The cores' write page caches skip watched
 * pages, so their writes to them take the slow path: a page is dropped
 * from every core's write page cache when a block of it is translated,
 * and is not filled in again while watched. A block stops after an
 * instruction that dropped blocks, which may have been its own later
 * instructions: the next block is found, or translated, from what RAM
 * holds then.
 *
 * Virtual time stays exact:
 *  - a block runs only when the turn has room for all its instructions,
 *    else the interpreter steps them one by one;
 *  - each instruction counts in Cpu.executed as it ends, as in the
 *    interpreter's loop, so machine_now() is the same inside a block;
 *  - the interpreter looks for an interrupt to take before each
 *    instruction, the JIT before each block. Whether an interrupt is
 *    taken depends on the core's interrupt inputs, HCR_EL2, SCR_EL3,
 *    PSTATE.DAIF and the Exception level (interrupt.c); within a block
 *    only the inputs can change, through a device that an MMIO access
 *    reaches, so a block stops after an instruction that changed them;
 *  - a block also stops after an instruction that raised an exception,
 *    that left the core not running or that stopped the machine, or that
 *    dropped blocks (above).
 *
 * Translating blocks of guest code into host code, kept in a cache, is
 * the common technique of dynamic binary translators, and so is
 * dropping translations when the pages they came from are written
 * (general knowledge); this design is written from the Arm ARM and the
 * Intel SDM, without reading any other project's source.
 */

#include "core/machine.h"
#include "cpu/arm64/internal.h"
#include "cpu/jit/cache.h"
#include "cpu/jit/x64.h"
#include "util/byteorder.h"

/*
 * The most instructions in a block. A block runs only when the turn has
 * room for all of it, and a turn is at most MACHINE_SLICE_NS (1000
 * instructions), less when an event is near; longer blocks would leave
 * more of each turn to the interpreter. Guest code branches every few
 * instructions (Phase 3.0 of the performance notes: 20 % branches), so
 * few blocks reach it. A choice of this model.
 */
#define BLOCK_MAX 64

/* Whether a block ends after insn. */
static bool ends_block(uint32_t insn)
{
	return arm64_group(insn) == ARM64_GROUP_BRANCH_SYS;
}

/*
 * Execute insn, the instruction at the PC, for the code of a block.
 * Returns whether the block goes on to its next instruction.
 */
static int block_step(Arm64Cpu *c, uint32_t insn)
{
	uint32_t asserted = c->asserted;
	uint64_t dropped = c->config.jit->dropped;
	Arm64StepEnd end = arm64_step_insn(c, insn);

	if (end == ARM64_STEP_STOP)
		return 0;
	c->cpu.executed++;
	return end == ARM64_STEP_DONE && c->asserted == asserted &&
	       c->config.jit->dropped == dropped && c->cpu.state == CPU_RUNNING &&
	       c->cpu.machine->stop == MACHINE_STOP_NONE;
}

/*
 * The code of a block of count instructions, a function of the System V
 * AMD64 ABI that takes the core: for each instruction, block_step(core,
 * insn), and a return when it gives 0. RBX holds the core and R12
 * block_step(), callee-saved (ABI 3.2.1); three pushes after the return
 * address leave RSP at a multiple of 16 for the calls (3.2.2).
 */
static void emit_block(X64Emitter *e, const uint32_t *insns, unsigned count)
{
	X64Label exits[BLOCK_MAX];

	x64_push(e, X64_RBX);
	x64_push(e, X64_R12);
	x64_push(e, X64_R13);
	x64_mov_rr(e, X64_64, X64_RBX, X64_RDI);
	x64_mov_ri(e, X64_R12, (uintptr_t)block_step);
	for (unsigned i = 0; i < count; i++) {
		x64_mov_rr(e, X64_64, X64_RDI, X64_RBX);
		x64_mov_ri(e, X64_RSI, insns[i]);
		x64_call_r(e, X64_R12);
		if (i + 1 < count) {
			x64_test_rr(e, X64_32, X64_RAX, X64_RAX);
			exits[i] = x64_jcc(e, X64_E);
		}
	}
	for (unsigned i = 0; i + 1 < count; i++)
		x64_bind(e, exits[i]);
	x64_pop(e, X64_R13);
	x64_pop(e, X64_R12);
	x64_pop(e, X64_RBX);
	x64_ret(e);
}

/* Add the block of count instructions at the PC, at pa; NULL if full. */
static const JitBlock *add_block(Arm64Cpu *c, JitCache *cache,
                                 const uint32_t *insns, unsigned count,
                                 uint64_t pa)
{
	X64Emitter e;

	jit_cache_begin(cache, &e);
	emit_block(&e, insns, count);
	return jit_cache_add(cache, &e, pa, c->pc, c->el, count);
}

/*
 * Translate the block at the PC, whose physical address is pa and whose
 * page is at page in host memory. When the cache is full, it is emptied
 * and the block translated again. Returns NULL when the block does not
 * fit even in an empty cache. The cache watches the page of a block that
 * it keeps, so no core may write to it through its write page cache.
 */
static const JitBlock *translate(Arm64Cpu *c, JitCache *cache,
                                 const uint8_t *page, uint64_t pa)
{
	unsigned offset = (unsigned)(c->pc & 0xfff);
	uint32_t insns[BLOCK_MAX];
	unsigned count = 0;
	const JitBlock *b;

	do {
		insns[count] = load_le32(page + offset + 4 * count);
		count++;
	} while (count < BLOCK_MAX && offset + 4 * count < 0x1000 &&
	         !ends_block(insns[count - 1]));

	b = add_block(c, cache, insns, count, pa);
	if (b == NULL) {
		jit_cache_flush(cache);
		b = add_block(c, cache, insns, count, pa);
	}
	if (b != NULL)
		arm64_drop_write_page_all_pes(c, pa & ~(uint64_t)0xfff);
	return b;
}

/*
 * The block at the PC, translated now if it was not, or NULL when the
 * fetch page cache does not hold the PC's page.
 */
static const JitBlock *find_block(Arm64Cpu *c, JitCache *cache)
{
	const Arm64Page *page = arm64_page_find(c, ARM64_PAGE_FETCH, c->pc, c->el);
	uint64_t pa;
	const JitBlock *b;

	if (page == NULL)
		return NULL;
	pa = page->pa | (c->pc & 0xfff);
	b = jit_cache_find(cache, pa, c->pc, c->el);
	return b != NULL ? b : translate(c, cache, page->host, pa);
}

EXEC_MEM_CALLER uint64_t arm64_jit_run(Arm64Cpu *c, uint64_t max_insns)
{
	Cpu *base = &c->cpu;
	JitCache *cache = c->config.jit;

	while (base->executed < max_insns && base->state == CPU_RUNNING &&
	       base->machine->stop == MACHINE_STOP_NONE) {
		const JitBlock *b = NULL;

		/* What the interpreter's step() does first. */
		if (arm64_interrupt_requested(c) && arm64_take_pending_interrupt(c)) {
			base->executed++;
			continue;
		}
		/* step() raises the faults of a misaligned PC and of PSTATE.IL. */
		if ((c->pc & 3) == 0 && !c->il)
			b = find_block(c, cache);
		if (b != NULL && b->count <= max_insns - base->executed) {
			b->code(c);
			continue;
		}
		if (!arm64_step(c))
			break;
		base->executed++;
	}
	return base->executed;
}
