/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#include "cpu/jit/x64.h"

/*
 * The encodings follow the Intel SDM volume 2: 2.1 and 2.2.1 for the
 * instruction format and the REX prefix, table 2-2 for ModR/M with a
 * 32-bit address, table 2-3 for SIB, table 2-5 for the special cases of
 * RSP, R12, RBP and R13 as a base; chapters 3-4 for each opcode.
 */

void x64_init(X64Emitter *e, uint8_t *buf, uintptr_t run_base, size_t size)
{
	e->buf = buf;
	e->run_base = run_base;
	e->size = size;
	e->pos = 0;
	e->full = false;
}

uintptr_t x64_here(const X64Emitter *e)
{
	return e->run_base + e->pos;
}

static void emit8(X64Emitter *e, uint8_t byte)
{
	if (e->pos == e->size) {
		e->full = true;
		return;
	}
	e->buf[e->pos++] = byte;
}

static void emit32(X64Emitter *e, uint32_t value)
{
	for (int i = 0; i < 4; i++)
		emit8(e, (uint8_t)(value >> (8 * i)));
}

static void emit64(X64Emitter *e, uint64_t value)
{
	emit32(e, (uint32_t)value);
	emit32(e, (uint32_t)(value >> 32));
}

static bool fits_s8(int64_t value)
{
	return value >= INT8_MIN && value <= INT8_MAX;
}

static bool fits_s32(int64_t value)
{
	return value >= INT32_MIN && value <= INT32_MAX;
}

/*
 * The REX prefix (2.2.1.2): 0100WRXB, W for a 64-bit operand, R
 * extending ModR/M reg, B extending ModR/M r/m or the opcode's register
 * (X, the SIB index, is never used here). Left out when all four are 0.
 */
static void rex(X64Emitter *e, X64Width w, unsigned reg, unsigned rm)
{
	uint8_t prefix =
		(uint8_t)(0x40 | (w == X64_64) << 3 | (reg >> 3) << 2 | rm >> 3);

	if (prefix != 0x40)
		emit8(e, prefix);
}

/* ModR/M with mod 11: both operands are registers. */
static void modrm_reg(X64Emitter *e, unsigned reg, unsigned rm)
{
	emit8(e, (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

/*
 * ModR/M, SIB and displacement for [base + disp], as GNU as encodes it:
 * no displacement when it is 0, 8 bits when it fits, else 32. RBP and
 * R13 as a base with mod 00 mean something else (RIP-relative), so they
 * take an 8-bit 0; RSP and R12 as a base in r/m mean "a SIB follows", so
 * they take the SIB 0x24 (scale 1, no index, base RSP or R12).
 */
static void modrm_mem(X64Emitter *e, unsigned reg, unsigned base, int32_t disp)
{
	unsigned mod;

	if (disp == 0 && (base & 7) != X64_RBP)
		mod = 0;
	else if (fits_s8(disp))
		mod = 1;
	else
		mod = 2;

	emit8(e, (uint8_t)(mod << 6 | (reg & 7) << 3 | (base & 7)));
	if ((base & 7) == X64_RSP)
		emit8(e, 0x24);
	if (mod == 1)
		emit8(e, (uint8_t)disp);
	else if (mod == 2)
		emit32(e, (uint32_t)disp);
}

/* opcode with ModR/M: reg and the register rm. */
static void op_rr(X64Emitter *e, X64Width w, uint8_t opcode, unsigned reg,
                  unsigned rm)
{
	rex(e, w, reg, rm);
	emit8(e, opcode);
	modrm_reg(e, reg, rm);
}

/* opcode with ModR/M: reg and the memory operand [base + disp]. */
static void op_rm(X64Emitter *e, X64Width w, uint8_t opcode, unsigned reg,
                  unsigned base, int32_t disp)
{
	rex(e, w, reg, base);
	emit8(e, opcode);
	modrm_mem(e, reg, base, disp);
}

void x64_push(X64Emitter *e, X64Reg reg)
{
	/* 50+rd: PUSH r64, REX.B for R8-R15 */
	rex(e, X64_32, 0, reg);
	emit8(e, (uint8_t)(0x50 | (reg & 7)));
}

void x64_pop(X64Emitter *e, X64Reg reg)
{
	/* 58+rd: POP r64 */
	rex(e, X64_32, 0, reg);
	emit8(e, (uint8_t)(0x58 | (reg & 7)));
}

void x64_ret(X64Emitter *e)
{
	emit8(e, 0xC3);
}

void x64_mov_rr(X64Emitter *e, X64Width w, X64Reg dst, X64Reg src)
{
	/* 89 /r: MOV r/m, r */
	op_rr(e, w, 0x89, src, dst);
}

void x64_mov_ri(X64Emitter *e, X64Reg dst, uint64_t imm)
{
	if (imm <= UINT32_MAX) {
		/* B8+rd id: MOV r32, imm32, the upper half cleared */
		rex(e, X64_32, 0, dst);
		emit8(e, (uint8_t)(0xB8 | (dst & 7)));
		emit32(e, (uint32_t)imm);
	} else if (fits_s32((int64_t)imm)) {
		/* REX.W C7 /0 id: MOV r/m64, imm32 sign extended */
		op_rr(e, X64_64, 0xC7, 0, dst);
		emit32(e, (uint32_t)imm);
	} else {
		/* REX.W B8+rd io: MOV r64, imm64 */
		rex(e, X64_64, 0, dst);
		emit8(e, (uint8_t)(0xB8 | (dst & 7)));
		emit64(e, imm);
	}
}

void x64_load(X64Emitter *e, X64Width w, X64Reg dst, X64Reg base, int32_t disp)
{
	/* 8B /r: MOV r, r/m */
	op_rm(e, w, 0x8B, dst, base, disp);
}

void x64_store(X64Emitter *e, X64Width w, X64Reg base, int32_t disp, X64Reg src)
{
	/* 89 /r: MOV r/m, r */
	op_rm(e, w, 0x89, src, base, disp);
}

void x64_store_imm(X64Emitter *e, X64Width w, X64Reg base, int32_t disp,
                   int32_t imm)
{
	/* C7 /0 id: MOV r/m, imm32 */
	op_rm(e, w, 0xC7, 0, base, disp);
	emit32(e, (uint32_t)imm);
}

void x64_lea(X64Emitter *e, X64Reg dst, X64Reg base, int32_t disp)
{
	/* REX.W 8D /r: LEA r64, m */
	op_rm(e, X64_64, 0x8D, dst, base, disp);
}

void x64_alu_rr(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, X64Reg src)
{
	/* op*8+1 /r: op r/m, r (01 ADD, 09 OR, ... 39 CMP) */
	op_rr(e, w, (uint8_t)(op << 3 | 1), src, dst);
}

void x64_alu_ri(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, int32_t imm)
{
	if (fits_s8(imm)) {
		/* 83 /op ib: op r/m, imm8 sign extended */
		op_rr(e, w, 0x83, op, dst);
		emit8(e, (uint8_t)imm);
	} else if (dst == X64_RAX) {
		/* op*8+5 id: op EAX/RAX, imm32, no ModR/M */
		rex(e, w, 0, 0);
		emit8(e, (uint8_t)(op << 3 | 5));
		emit32(e, (uint32_t)imm);
	} else {
		/* 81 /op id: op r/m, imm32 */
		op_rr(e, w, 0x81, op, dst);
		emit32(e, (uint32_t)imm);
	}
}

void x64_alu_rm(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, X64Reg base,
                int32_t disp)
{
	/* op*8+3 /r: op r, r/m (03 ADD, 0B OR, ... 3B CMP) */
	op_rm(e, w, (uint8_t)(op << 3 | 3), dst, base, disp);
}

void x64_alu_mi(X64Emitter *e, X64Alu op, X64Width w, X64Reg base, int32_t disp,
                int32_t imm)
{
	if (fits_s8(imm)) {
		op_rm(e, w, 0x83, op, base, disp);
		emit8(e, (uint8_t)imm);
	} else {
		op_rm(e, w, 0x81, op, base, disp);
		emit32(e, (uint32_t)imm);
	}
}

void x64_test_rr(X64Emitter *e, X64Width w, X64Reg a, X64Reg b)
{
	/* 85 /r: TEST r/m, r */
	op_rr(e, w, 0x85, b, a);
}

void x64_call(X64Emitter *e, uintptr_t target)
{
	/* E8 cd: CALL rel32, relative to the next instruction */
	int64_t rel = (int64_t)(target - (x64_here(e) + 5));

	if (fits_s32(rel)) {
		emit8(e, 0xE8);
		emit32(e, (uint32_t)rel);
	} else {
		x64_mov_ri(e, X64_RAX, target);
		x64_call_r(e, X64_RAX);
	}
}

void x64_call_r(X64Emitter *e, X64Reg reg)
{
	/* FF /2: CALL r/m64 */
	op_rr(e, X64_32, 0xFF, 2, reg);
}

X64Label x64_jmp(X64Emitter *e)
{
	/* E9 cd: JMP rel32 */
	emit8(e, 0xE9);
	X64Label label = e->pos;
	emit32(e, 0);
	return label;
}

X64Label x64_jcc(X64Emitter *e, X64Cond cond)
{
	/* 0F 80+cc cd: Jcc rel32 */
	emit8(e, 0x0F);
	emit8(e, (uint8_t)(0x80 | cond));
	X64Label label = e->pos;
	emit32(e, 0);
	return label;
}

void x64_bind(X64Emitter *e, X64Label label)
{
	/* The displacement is from the end of the jump, label + 4. A jump
	 * cut by the end of the buffer has no room for it. */
	if (e->full)
		return;

	uint32_t rel = (uint32_t)(e->pos - (label + 4));

	for (int i = 0; i < 4; i++)
		e->buf[label + i] = (uint8_t)(rel >> (8 * i));
}
