/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#ifndef SMIRROR_CPU_JIT_X64_H
#define SMIRROR_CPU_JIT_X64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * An emitter of x86-64 machine code, for the JIT (docs/architecture.md,
 * "The JIT"). It knows nothing of the guest: it writes the bytes of host
 * instructions into a buffer, encoded as the Intel SDM volume 2 gives
 * them (chapter 2: REX, ModR/M, SIB, displacement, immediate; chapter 3
 * onwards: the opcodes). Where an instruction has several encodings, it
 * picks the one GNU as picks, so the unit tests can check every byte
 * against the assembler.
 *
 * The buffer may be written through one address and run at another (the
 * two views of ExecMem): `run_base` is where buf[0] runs, which relative
 * calls need.
 *
 * Code that would pass the end of the buffer is dropped and `full` set;
 * the caller then throws the whole translation away. Positions keep
 * counting only up to the end, so they stay inside the buffer.
 */

/* The general registers, by their number in the encodings. */
typedef enum X64Reg {
	X64_RAX,
	X64_RCX,
	X64_RDX,
	X64_RBX,
	X64_RSP,
	X64_RBP,
	X64_RSI,
	X64_RDI,
	X64_R8,
	X64_R9,
	X64_R10,
	X64_R11,
	X64_R12,
	X64_R13,
	X64_R14,
	X64_R15,
} X64Reg;

/* The operand size: 32 bits (the upper half of the register cleared) or
 * 64 bits (REX.W). */
typedef enum X64Width {
	X64_32,
	X64_64,
} X64Width;

/* The eight arithmetic and logic operations of the opcodes 00-3F and of
 * 80/81/83, by their number in the ModR/M reg field. */
typedef enum X64Alu {
	X64_ADD,
	X64_OR,
	X64_ADC,
	X64_SBB,
	X64_AND,
	X64_SUB,
	X64_XOR,
	X64_CMP,
} X64Alu;

/* The conditions of Jcc (SDM vol. 1, appendix B), by their number. */
typedef enum X64Cond {
	X64_O, /* overflow */
	X64_NO,
	X64_B,  /* below: carry */
	X64_AE, /* above or equal: no carry */
	X64_E,  /* equal: zero */
	X64_NE,
	X64_BE, /* below or equal: carry or zero */
	X64_A,
	X64_S, /* sign */
	X64_NS,
	X64_P, /* parity */
	X64_NP,
	X64_L, /* less: sign != overflow */
	X64_GE,
	X64_LE, /* less or equal: zero, or sign != overflow */
	X64_G,
} X64Cond;

typedef struct X64Emitter {
	uint8_t *buf;       /* where the bytes are written */
	uintptr_t run_base; /* the address buf[0] runs at */
	size_t size;        /* the bytes of buf */
	size_t pos;         /* the next byte */
	bool full;          /* code past the end was dropped */
} X64Emitter;

/* The position of a jump's 32-bit displacement, to bind later. */
typedef size_t X64Label;

void x64_init(X64Emitter *e, uint8_t *buf, uintptr_t run_base, size_t size);

/* The address the next instruction will run at. */
uintptr_t x64_here(const X64Emitter *e);

/* PUSH r64, POP r64, RET. */
void x64_push(X64Emitter *e, X64Reg reg);
void x64_pop(X64Emitter *e, X64Reg reg);
void x64_ret(X64Emitter *e);

/* MOV dst, src. */
void x64_mov_rr(X64Emitter *e, X64Width w, X64Reg dst, X64Reg src);

/* dst = imm, all 64 bits, in the shortest form: MOV r32, imm32 (zero
 * extended), MOV r/m64, imm32 (sign extended) or MOV r64, imm64. Flags
 * are kept. */
void x64_mov_ri(X64Emitter *e, X64Reg dst, uint64_t imm);

/* MOV dst, [base + disp]; MOV [base + disp], src; MOV [base + disp],
 * imm32 (sign extended at 64 bits). */
void x64_load(X64Emitter *e, X64Width w, X64Reg dst, X64Reg base, int32_t disp);
void x64_store(X64Emitter *e, X64Width w, X64Reg base, int32_t disp,
               X64Reg src);
void x64_store_imm(X64Emitter *e, X64Width w, X64Reg base, int32_t disp,
                   int32_t imm);

/* LEA dst, [base + disp] (64 bits). */
void x64_lea(X64Emitter *e, X64Reg dst, X64Reg base, int32_t disp);

/* op dst, src; op dst, imm (sign extended); op dst, [base + disp];
 * op [base + disp], imm. */
void x64_alu_rr(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, X64Reg src);
void x64_alu_ri(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, int32_t imm);
void x64_alu_rm(X64Emitter *e, X64Alu op, X64Width w, X64Reg dst, X64Reg base,
                int32_t disp);
void x64_alu_mi(X64Emitter *e, X64Alu op, X64Width w, X64Reg base, int32_t disp,
                int32_t imm);

/* TEST a, b. */
void x64_test_rr(X64Emitter *e, X64Width w, X64Reg a, X64Reg b);

/* CALL target: relative when it is within reach, else through RAX,
 * which it then overwrites (RAX is not kept across a call anyway). */
void x64_call(X64Emitter *e, uintptr_t target);

/* CALL reg. */
void x64_call_r(X64Emitter *e, X64Reg reg);

/* A forward JMP or Jcc with a 32-bit displacement, bound later to the
 * position x64_bind() is called at. */
X64Label x64_jmp(X64Emitter *e);
X64Label x64_jcc(X64Emitter *e, X64Cond cond);
void x64_bind(X64Emitter *e, X64Label label);

#endif
