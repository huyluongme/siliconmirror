/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

#define _POSIX_C_SOURCE 200809L /* alarm() */

#include "cpu/jit/exec_mem.h"
#include "cpu/jit/x64.h"
#include "test.h"

#include <string.h>
#include <unistd.h>

/*
 * The x86-64 emitter. Each CASE emits with a fresh emitter and checks
 * every byte; the comment after it is the same instruction in Intel
 * syntax, from which .claude/tools/x64_bytes_check.py assembles the
 * expected bytes with GNU as and compares. Cases without a comment
 * (relative calls, buffer ends) were worked out by hand from the SDM.
 * Then small functions are emitted into executable memory and run.
 */

#define RUN_BASE 0x10000000u

static uint8_t buf[64];

static void expect(const X64Emitter *e, const uint8_t *want, size_t n,
                   const char *what, int line)
{
	if (e->full || e->pos != n || memcmp(e->buf, want, n) != 0) {
		fprintf(stderr, "%s:%d: %s gave", __FILE__, line, what);
		for (size_t i = 0; i < e->pos; i++)
			fprintf(stderr, " %02x", e->buf[i]);
		fprintf(stderr, "%s, expected", e->full ? " (full)" : "");
		for (size_t i = 0; i < n; i++)
			fprintf(stderr, " %02x", want[i]);
		fprintf(stderr, "\n");
		test_failures++;
	}
}

#define CASE_AT(base, code, ...)                                               \
	do {                                                                       \
		X64Emitter e;                                                          \
		static const uint8_t want_[] = {__VA_ARGS__};                          \
		x64_init(&e, buf, (base), sizeof buf);                                 \
		code;                                                                  \
		expect(&e, want_, sizeof want_, #code, __LINE__);                      \
	} while (0)

#define CASE(code, ...) CASE_AT(RUN_BASE, code, __VA_ARGS__)

static void test_push_pop_ret(void)
{
	CASE(x64_push(&e, X64_RBX), 0x53);       /* push rbx */
	CASE(x64_push(&e, X64_R12), 0x41, 0x54); /* push r12 */
	CASE(x64_pop(&e, X64_RBP), 0x5D);        /* pop rbp */
	CASE(x64_pop(&e, X64_R15), 0x41, 0x5F);  /* pop r15 */
	CASE(x64_ret(&e), 0xC3);                 /* ret */
}

static void test_mov_rr(void)
{
	CASE(x64_mov_rr(&e, X64_32, X64_RAX, X64_RCX), 0x89, 0xC8);
	/* mov eax, ecx */
	CASE(x64_mov_rr(&e, X64_64, X64_RDI, X64_RSI), 0x48, 0x89, 0xF7);
	/* mov rdi, rsi */
	CASE(x64_mov_rr(&e, X64_64, X64_R8, X64_RAX), 0x49, 0x89, 0xC0);
	/* mov r8, rax */
	CASE(x64_mov_rr(&e, X64_64, X64_RAX, X64_R9), 0x4C, 0x89, 0xC8);
	/* mov rax, r9 */
	CASE(x64_mov_rr(&e, X64_32, X64_R10, X64_R11), 0x45, 0x89, 0xDA);
	/* mov r10d, r11d */
}

static void test_mov_ri(void)
{
	CASE(x64_mov_ri(&e, X64_RAX, 5), 0xB8, 0x05, 0x00, 0x00, 0x00);
	/* mov eax, 5 */
	CASE(x64_mov_ri(&e, X64_R9, 0xFFFFFFFF), 0x41, 0xB9, 0xFF, 0xFF, 0xFF,
	     0xFF);
	/* mov r9d, 0xffffffff */
	CASE(x64_mov_ri(&e, X64_RAX, 0x100000000), 0x48, 0xB8, 0x00, 0x00, 0x00,
	     0x00, 0x01, 0x00, 0x00, 0x00);
	/* movabs rax, 0x100000000 */
	CASE(x64_mov_ri(&e, X64_RCX, UINT64_MAX), 0x48, 0xC7, 0xC1, 0xFF, 0xFF,
	     0xFF, 0xFF);
	/* mov rcx, -1 */
	CASE(x64_mov_ri(&e, X64_R12, 0xFFFFFFFF80000000), 0x49, 0xC7, 0xC4, 0x00,
	     0x00, 0x00, 0x80);
	/* mov r12, -0x80000000 */
	CASE(x64_mov_ri(&e, X64_RDX, 0xFFFFFFFF7FFFFFFF), 0x48, 0xBA, 0xFF, 0xFF,
	     0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF);
	/* movabs rdx, 0xffffffff7fffffff */
	CASE(x64_mov_ri(&e, X64_R15, 0x123456789ABCDEF0), 0x49, 0xBF, 0xF0, 0xDE,
	     0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12);
	/* movabs r15, 0x123456789abcdef0 */
}

/* The addressing forms of [base + disp], through MOV r, r/m. */
static void test_load(void)
{
	CASE(x64_load(&e, X64_64, X64_RAX, X64_RBX, 0), 0x48, 0x8B, 0x03);
	/* mov rax, [rbx] */
	CASE(x64_load(&e, X64_32, X64_RAX, X64_RBX, 8), 0x8B, 0x43, 0x08);
	/* mov eax, [rbx+8] */
	CASE(x64_load(&e, X64_64, X64_RCX, X64_RBX, 127), 0x48, 0x8B, 0x4B, 0x7F);
	/* mov rcx, [rbx+127] */
	CASE(x64_load(&e, X64_64, X64_RCX, X64_RBX, 128), 0x48, 0x8B, 0x8B, 0x80,
	     0x00, 0x00, 0x00);
	/* mov rcx, [rbx+128] */
	CASE(x64_load(&e, X64_64, X64_RCX, X64_RBX, -128), 0x48, 0x8B, 0x4B, 0x80);
	/* mov rcx, [rbx-128] */
	CASE(x64_load(&e, X64_64, X64_RCX, X64_RBX, -129), 0x48, 0x8B, 0x8B, 0x7F,
	     0xFF, 0xFF, 0xFF);
	/* mov rcx, [rbx-129] */
	CASE(x64_load(&e, X64_64, X64_RAX, X64_RSP, 0), 0x48, 0x8B, 0x04, 0x24);
	/* mov rax, [rsp] */
	CASE(x64_load(&e, X64_64, X64_RAX, X64_RSP, 8), 0x48, 0x8B, 0x44, 0x24,
	     0x08);
	/* mov rax, [rsp+8] */
	CASE(x64_load(&e, X64_64, X64_RAX, X64_R12, 0), 0x49, 0x8B, 0x04, 0x24);
	/* mov rax, [r12] */
	CASE(x64_load(&e, X64_32, X64_RDX, X64_R12, 0x1000), 0x41, 0x8B, 0x94, 0x24,
	     0x00, 0x10, 0x00, 0x00);
	/* mov edx, [r12+0x1000] */
	CASE(x64_load(&e, X64_64, X64_RAX, X64_RBP, 0), 0x48, 0x8B, 0x45, 0x00);
	/* mov rax, [rbp] */
	CASE(x64_load(&e, X64_64, X64_RAX, X64_R13, 0), 0x49, 0x8B, 0x45, 0x00);
	/* mov rax, [r13] */
	CASE(x64_load(&e, X64_64, X64_RSI, X64_RBP, 16), 0x48, 0x8B, 0x75, 0x10);
	/* mov rsi, [rbp+16] */
	CASE(x64_load(&e, X64_64, X64_R8, X64_R14, 0x10), 0x4D, 0x8B, 0x46, 0x10);
	/* mov r8, [r14+0x10] */
	CASE(x64_load(&e, X64_32, X64_R15, X64_RDI, 0), 0x44, 0x8B, 0x3F);
	/* mov r15d, [rdi] */
}

static void test_store(void)
{
	CASE(x64_store(&e, X64_64, X64_RBX, 0x200, X64_RDX), 0x48, 0x89, 0x93, 0x00,
	     0x02, 0x00, 0x00);
	/* mov [rbx+0x200], rdx */
	CASE(x64_store(&e, X64_32, X64_R13, 4, X64_R9), 0x45, 0x89, 0x4D, 0x04);
	/* mov [r13+4], r9d */
	CASE(x64_store_imm(&e, X64_64, X64_RBX, 8, -1), 0x48, 0xC7, 0x43, 0x08,
	     0xFF, 0xFF, 0xFF, 0xFF);
	/* mov QWORD PTR [rbx+8], -1 */
	CASE(x64_store_imm(&e, X64_32, X64_RSP, 0, 0x12345678), 0xC7, 0x04, 0x24,
	     0x78, 0x56, 0x34, 0x12);
	/* mov DWORD PTR [rsp], 0x12345678 */
}

static void test_lea(void)
{
	CASE(x64_lea(&e, X64_RDI, X64_RBX, 0x40), 0x48, 0x8D, 0x7B, 0x40);
	/* lea rdi, [rbx+0x40] */
	CASE(x64_lea(&e, X64_R11, X64_RSP, -8), 0x4C, 0x8D, 0x5C, 0x24, 0xF8);
	/* lea r11, [rsp-8] */
}

static void test_alu_rr(void)
{
	CASE(x64_alu_rr(&e, X64_ADD, X64_64, X64_RDI, X64_RSI), 0x48, 0x01, 0xF7);
	/* add rdi, rsi */
	CASE(x64_alu_rr(&e, X64_OR, X64_32, X64_RAX, X64_RCX), 0x09, 0xC8);
	/* or eax, ecx */
	CASE(x64_alu_rr(&e, X64_ADC, X64_64, X64_R8, X64_RDX), 0x49, 0x11, 0xD0);
	/* adc r8, rdx */
	CASE(x64_alu_rr(&e, X64_SBB, X64_32, X64_RBX, X64_R10), 0x44, 0x19, 0xD3);
	/* sbb ebx, r10d */
	CASE(x64_alu_rr(&e, X64_AND, X64_64, X64_R15, X64_R14), 0x4D, 0x21, 0xF7);
	/* and r15, r14 */
	CASE(x64_alu_rr(&e, X64_SUB, X64_32, X64_RSP, X64_RBP), 0x29, 0xEC);
	/* sub esp, ebp */
	CASE(x64_alu_rr(&e, X64_XOR, X64_32, X64_RAX, X64_RAX), 0x31, 0xC0);
	/* xor eax, eax */
	CASE(x64_alu_rr(&e, X64_CMP, X64_64, X64_RCX, X64_RDX), 0x48, 0x39, 0xD1);
	/* cmp rcx, rdx */
}

static void test_alu_ri(void)
{
	CASE(x64_alu_ri(&e, X64_ADD, X64_32, X64_RAX, 5), 0x83, 0xC0, 0x05);
	/* add eax, 5 */
	CASE(x64_alu_ri(&e, X64_SUB, X64_64, X64_RCX, -128), 0x48, 0x83, 0xE9,
	     0x80);
	/* sub rcx, -128 */
	CASE(x64_alu_ri(&e, X64_ADD, X64_64, X64_RCX, 127), 0x48, 0x83, 0xC1, 0x7F);
	/* add rcx, 127 */
	CASE(x64_alu_ri(&e, X64_ADD, X64_64, X64_RCX, 128), 0x48, 0x81, 0xC1, 0x80,
	     0x00, 0x00, 0x00);
	/* add rcx, 128 */
	CASE(x64_alu_ri(&e, X64_ADD, X64_32, X64_RCX, -129), 0x81, 0xC1, 0x7F, 0xFF,
	     0xFF, 0xFF);
	/* add ecx, -129 */
	CASE(x64_alu_ri(&e, X64_OR, X64_32, X64_RCX, 1), 0x83, 0xC9, 0x01);
	/* or ecx, 1 */
	CASE(x64_alu_ri(&e, X64_ADC, X64_32, X64_RCX, 1), 0x83, 0xD1, 0x01);
	/* adc ecx, 1 */
	CASE(x64_alu_ri(&e, X64_SBB, X64_32, X64_RCX, 1), 0x83, 0xD9, 0x01);
	/* sbb ecx, 1 */
	CASE(x64_alu_ri(&e, X64_AND, X64_64, X64_R9, -16), 0x49, 0x83, 0xE1, 0xF0);
	/* and r9, -16 */
	CASE(x64_alu_ri(&e, X64_XOR, X64_32, X64_RCX, 1), 0x83, 0xF1, 0x01);
	/* xor ecx, 1 */
	CASE(x64_alu_ri(&e, X64_CMP, X64_32, X64_RCX, 1), 0x83, 0xF9, 0x01);
	/* cmp ecx, 1 */
	CASE(x64_alu_ri(&e, X64_CMP, X64_32, X64_RAX, 1000), 0x3D, 0xE8, 0x03, 0x00,
	     0x00);
	/* cmp eax, 1000 */
	CASE(x64_alu_ri(&e, X64_AND, X64_64, X64_RAX, 0x7FFFFFFF), 0x48, 0x25, 0xFF,
	     0xFF, 0xFF, 0x7F);
	/* and rax, 0x7fffffff */
	CASE(x64_alu_ri(&e, X64_ADD, X64_64, X64_RAX, -129), 0x48, 0x05, 0x7F, 0xFF,
	     0xFF, 0xFF);
	/* add rax, -129 */
	CASE(x64_alu_ri(&e, X64_XOR, X64_64, X64_R8, 0x10000), 0x49, 0x81, 0xF0,
	     0x00, 0x00, 0x01, 0x00);
	/* xor r8, 0x10000 */
}

static void test_alu_mem(void)
{
	CASE(x64_alu_rm(&e, X64_ADD, X64_64, X64_RAX, X64_RBX, 8), 0x48, 0x03, 0x43,
	     0x08);
	/* add rax, [rbx+8] */
	CASE(x64_alu_rm(&e, X64_CMP, X64_32, X64_RCX, X64_RBX, 0), 0x3B, 0x0B);
	/* cmp ecx, [rbx] */
	CASE(x64_alu_rm(&e, X64_SUB, X64_64, X64_R9, X64_RSP, 0x100), 0x4C, 0x2B,
	     0x8C, 0x24, 0x00, 0x01, 0x00, 0x00);
	/* sub r9, [rsp+0x100] */
	CASE(x64_alu_mi(&e, X64_ADD, X64_64, X64_RBX, 0x10, 1), 0x48, 0x83, 0x43,
	     0x10, 0x01);
	/* add QWORD PTR [rbx+0x10], 1 */
	CASE(x64_alu_mi(&e, X64_SUB, X64_32, X64_RBX, 0x200, 0x1000), 0x81, 0xAB,
	     0x00, 0x02, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00);
	/* sub DWORD PTR [rbx+0x200], 0x1000 */
	CASE(x64_alu_mi(&e, X64_CMP, X64_64, X64_R12, 0, -128), 0x49, 0x83, 0x3C,
	     0x24, 0x80);
	/* cmp QWORD PTR [r12], -128 */
	CASE(x64_alu_mi(&e, X64_ADD, X64_64, X64_RAX, 0, 128), 0x48, 0x81, 0x00,
	     0x80, 0x00, 0x00, 0x00);
	/* add QWORD PTR [rax], 128 */
}

static void test_test_rr(void)
{
	CASE(x64_test_rr(&e, X64_32, X64_RAX, X64_RAX), 0x85, 0xC0);
	/* test eax, eax */
	CASE(x64_test_rr(&e, X64_64, X64_RCX, X64_RDX), 0x48, 0x85, 0xD1);
	/* test rcx, rdx */
	CASE(x64_test_rr(&e, X64_32, X64_R8, X64_R15), 0x45, 0x85, 0xF8);
	/* test r8d, r15d */
}

static void test_call(void)
{
	CASE(x64_call_r(&e, X64_RAX), 0xFF, 0xD0);       /* call rax */
	CASE(x64_call_r(&e, X64_R11), 0x41, 0xFF, 0xD3); /* call r11 */

	/* E8 with the target minus the end of the CALL. */
	CASE(x64_call(&e, RUN_BASE + 5 + 0x100), 0xE8, 0x00, 0x01, 0x00, 0x00);
	CASE(x64_call(&e, RUN_BASE), 0xE8, 0xFB, 0xFF, 0xFF, 0xFF);
	CASE(x64_call(&e, RUN_BASE + 5 + INT32_MAX), 0xE8, 0xFF, 0xFF, 0xFF, 0x7F);
	/* One byte further: through RAX, a 32-bit MOV here. */
	CASE(x64_call(&e, RUN_BASE + 5 + 0x80000000u), 0xB8, 0x05, 0x00, 0x00, 0x90,
	     0xFF, 0xD0);
	/* From the current position, not from the start of the buffer. */
	CASE(
		{
			x64_ret(&e);
			x64_call(&e, RUN_BASE + 6);
		},
		0xC3, 0xE8, 0x00, 0x00, 0x00, 0x00);

	/* Backwards, at the edge of reach, and one byte past it. */
	const uintptr_t high = 0x7F0000000000;
	CASE_AT(high, x64_call(&e, high + 5 - 0x80000000u), 0xE8, 0x00, 0x00, 0x00,
	        0x80);
	CASE_AT(high, x64_call(&e, high + 4 - 0x80000000u), 0x48, 0xB8, 0x04, 0x00,
	        0x00, 0x80, 0xFF, 0x7E, 0x00, 0x00, 0xFF, 0xD0);
}

static void test_jumps(void)
{
	CASE(x64_bind(&e, x64_jmp(&e)), 0xE9, 0x00, 0x00, 0x00, 0x00);
	/* {disp32} jmp 1f; 1: */
	CASE(x64_bind(&e, x64_jcc(&e, X64_O)), 0x0F, 0x80, 0x00, 0x00, 0x00, 0x00);
	/* {disp32} jo 1f; 1: */
	CASE(x64_bind(&e, x64_jcc(&e, X64_E)), 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00);
	/* {disp32} je 1f; 1: */
	CASE(x64_bind(&e, x64_jcc(&e, X64_G)), 0x0F, 0x8F, 0x00, 0x00, 0x00, 0x00);
	/* {disp32} jg 1f; 1: */
	CASE(
		{
			X64Label skip = x64_jmp(&e);
			x64_ret(&e);
			x64_bind(&e, skip);
		},
		0xE9, 0x01, 0x00, 0x00, 0x00, 0xC3);
	/* {disp32} jmp 1f; ret; 1: */
	CASE(
		{
			x64_push(&e, X64_RBX);
			X64Label a = x64_jcc(&e, X64_NE);
			X64Label b = x64_jcc(&e, X64_L);
			x64_ret(&e);
			x64_bind(&e, b);
			x64_push(&e, X64_R12);
			x64_bind(&e, a);
		},
		0x53, 0x0F, 0x85, 0x09, 0x00, 0x00, 0x00, 0x0F, 0x8C, 0x01, 0x00, 0x00,
		0x00, 0xC3, 0x41, 0x54);
	/* push rbx; {disp32} jne 2f; {disp32} jl 1f; ret; 1: push r12; 2: */
}

static void test_here(void)
{
	X64Emitter e;

	x64_init(&e, buf, RUN_BASE, sizeof buf);
	CHECK_EQ(x64_here(&e), RUN_BASE);
	x64_push(&e, X64_R8);
	CHECK_EQ(x64_here(&e), RUN_BASE + 2);
	CHECK_EQ(e.pos, 2);

	/* init starts again. */
	e.full = true;
	x64_init(&e, buf + 8, 0x1000, 4);
	CHECK(!e.full);
	CHECK_EQ(e.pos, 0);
	CHECK_EQ(e.size, 4);
	CHECK(e.buf == buf + 8);
	CHECK_EQ(x64_here(&e), 0x1000);
}

/* Code past the end of the buffer is dropped, never written. */
static void test_full(void)
{
	uint8_t mem[8];
	X64Emitter e;

	/* Exactly the size: not full. */
	memset(mem, 0xAA, sizeof mem);
	x64_init(&e, mem, RUN_BASE, 5);
	x64_mov_ri(&e, X64_RAX, 5);
	CHECK(!e.full);
	CHECK_EQ(e.pos, 5);

	/* One more byte. */
	x64_ret(&e);
	CHECK(e.full);
	CHECK_EQ(e.pos, 5);
	CHECK_EQ(mem[5], 0xAA);

	/* Cut in the middle. */
	memset(mem, 0xAA, sizeof mem);
	x64_init(&e, mem, RUN_BASE, 3);
	x64_mov_ri(&e, X64_RAX, 0x55);
	CHECK(e.full);
	CHECK_EQ(e.pos, 3);
	CHECK_EQ(mem[0], 0xB8);
	CHECK_EQ(mem[2], 0x00);
	CHECK_EQ(mem[3], 0xAA);

	/* A jump cut by the end has no room for its displacement; binding
	 * it writes nothing. */
	memset(mem, 0xAA, sizeof mem);
	x64_init(&e, mem, RUN_BASE, 4);
	X64Label label = x64_jcc(&e, X64_E);
	CHECK(e.full);
	x64_bind(&e, label);
	CHECK_EQ(mem[2], 0x00);
	CHECK_EQ(mem[3], 0x00);
	CHECK_EQ(mem[4], 0xAA);
	CHECK_EQ(mem[5], 0xAA);

	memset(mem, 0xAA, sizeof mem);
	x64_init(&e, mem, RUN_BASE, 3);
	x64_bind(&e, x64_jmp(&e));
	CHECK(e.full);
	CHECK_EQ(mem[0], 0xE9);
	CHECK_EQ(mem[1], 0x00);
	CHECK_EQ(mem[2], 0x00);
	CHECK_EQ(mem[3], 0xAA);
}

/* A displacement with all four bytes in use. bind() writes only at the
 * label, so the buffer only needs to be as long as the jump. */
static void test_far_bind(void)
{
	uint8_t mem[8];
	X64Emitter e;

	x64_init(&e, mem, RUN_BASE, SIZE_MAX);
	X64Label label = x64_jmp(&e);
	e.pos = 5 + 0x12345678;
	x64_bind(&e, label);
	CHECK_EQ(mem[1], 0x78);
	CHECK_EQ(mem[2], 0x56);
	CHECK_EQ(mem[3], 0x34);
	CHECK_EQ(mem[4], 0x12);

	x64_init(&e, mem, RUN_BASE, SIZE_MAX);
	label = x64_jcc(&e, X64_NE);
	e.pos = 6 + (size_t)0x7FFFFFFF;
	x64_bind(&e, label);
	CHECK_EQ(mem[2], 0xFF);
	CHECK_EQ(mem[3], 0xFF);
	CHECK_EQ(mem[4], 0xFF);
	CHECK_EQ(mem[5], 0x7F);
}

/* Running emitted code: x86-64 Linux hosts only. */
#if defined(__x86_64__) && defined(__linux__)

static ExecMem code;

typedef uint64_t (*Fn)(uint64_t, uint64_t, uint64_t, uint64_t);

/* Starts a function at the start of the code memory. */
static X64Emitter start(void)
{
	X64Emitter e;

	x64_init(&e, code.write, (uintptr_t)code.run, code.size);
	return e;
}

static Fn finish(const X64Emitter *e)
{
	const uint8_t *run = code.run;
	Fn fn;

	CHECK(!e->full);
	memcpy(&fn, &run, sizeof fn);
	return fn;
}

static EXEC_MEM_CALLER uint64_t call(Fn fn, uint64_t a, uint64_t b, uint64_t c,
                                     uint64_t d)
{
	return fn(a, b, c, d);
}

/* rax = rdi op rsi, for each operation. */
static uint64_t run_alu(X64Alu op, X64Width w, uint64_t a, uint64_t b)
{
	X64Emitter e = start();

	x64_mov_rr(&e, X64_64, X64_RAX, X64_RDI);
	x64_alu_rr(&e, op, w, X64_RAX, X64_RSI);
	x64_ret(&e);
	return call(finish(&e), a, b, 0, 0);
}

static void test_run_alu(void)
{
	CHECK_EQ(run_alu(X64_ADD, X64_64, 0x100000000, 7), 0x100000007);
	CHECK_EQ(run_alu(X64_OR, X64_64, 0xF0, 0x0F), 0xFF);
	CHECK_EQ(run_alu(X64_AND, X64_64, 0xF0F0, 0xFF00), 0xF000);
	CHECK_EQ(run_alu(X64_SUB, X64_64, 10, 12), (uint64_t)-2);
	CHECK_EQ(run_alu(X64_XOR, X64_64, 0xFF, 0x0F), 0xF0);
	CHECK_EQ(run_alu(X64_CMP, X64_64, 10, 12), 10);
	/* A 32-bit operation clears the upper half. */
	CHECK_EQ(run_alu(X64_ADD, X64_32, 0xFFFFFFFF00000001, 1), 2);

	/* rax = rdx + rcx + carry, the carry from rdi < rsi; the MOVs
	 * keep the flags. */
	X64Emitter e = start();
	x64_alu_rr(&e, X64_CMP, X64_64, X64_RDI, X64_RSI);
	x64_mov_rr(&e, X64_64, X64_RAX, X64_RDX);
	x64_mov_ri(&e, X64_R8, 0x123456789);
	x64_alu_rr(&e, X64_ADC, X64_64, X64_RAX, X64_RCX);
	x64_ret(&e);
	Fn adc = finish(&e);
	CHECK_EQ(call(adc, 1, 2, 10, 20), 31);
	CHECK_EQ(call(adc, 2, 1, 10, 20), 30);

	/* rax = rdx - rcx - borrow. */
	e = start();
	x64_alu_rr(&e, X64_CMP, X64_32, X64_RDI, X64_RSI);
	x64_mov_rr(&e, X64_64, X64_RAX, X64_RDX);
	x64_alu_rr(&e, X64_SBB, X64_64, X64_RAX, X64_RCX);
	x64_ret(&e);
	Fn sbb = finish(&e);
	CHECK_EQ(call(sbb, 1, 2, 30, 10), 19);
	CHECK_EQ(call(sbb, 2, 1, 30, 10), 20);

	/* Immediates: rax = ((rdi + 1000) & -16) ^ 0x10000. */
	e = start();
	x64_mov_rr(&e, X64_64, X64_RAX, X64_RDI);
	x64_alu_ri(&e, X64_ADD, X64_64, X64_RAX, 1000);
	x64_alu_ri(&e, X64_AND, X64_64, X64_RAX, -16);
	x64_alu_ri(&e, X64_XOR, X64_64, X64_RAX, 0x10000);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), 0x123456789, 0, 0, 0),
	         ((0x123456789 + 1000) & ~(uint64_t)15) ^ 0x10000);
}

static void test_run_mov(void)
{
	static const uint64_t values[] = {0,
	                                  5,
	                                  0xFFFFFFFF,
	                                  0x100000000,
	                                  UINT64_MAX,
	                                  0xFFFFFFFF80000000,
	                                  0xFFFFFFFF7FFFFFFF,
	                                  0x123456789ABCDEF0};

	for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
		X64Emitter e = start();
		x64_mov_ri(&e, X64_R10, values[i]);
		x64_mov_rr(&e, X64_64, X64_RAX, X64_R10);
		x64_ret(&e);
		CHECK_EQ(call(finish(&e), 0, 0, 0, 0), values[i]);
	}

	/* A 32-bit MOV clears the upper half. */
	X64Emitter e = start();
	x64_mov_ri(&e, X64_RAX, UINT64_MAX);
	x64_mov_rr(&e, X64_32, X64_RAX, X64_RDI);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), 0x8000000012345678, 0, 0, 0), 0x12345678);

	/* lea rax, [rdi - 8] */
	e = start();
	x64_lea(&e, X64_RAX, X64_RDI, -8);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), 100, 0, 0, 0), 92);
}

/* Loads and stores on a block of memory given in rdi; r12 and rbp, with
 * their special encodings, are saved, used as bases and restored. */
static void test_run_memory(void)
{
	uint64_t data[48] = {0};
	X64Emitter e = start();

	data[1] = 0x1111;
	data[2] = 0x2222;
	data[20] = 0x100;
	data[40] = 0xAAAABBBBCCCCDDDD;
	x64_push(&e, X64_R12);
	x64_push(&e, X64_RBP);
	x64_mov_rr(&e, X64_64, X64_R12, X64_RDI);
	x64_lea(&e, X64_RBP, X64_RDI, 8);
	x64_load(&e, X64_64, X64_RAX, X64_R12, 8);            /* data[1] */
	x64_alu_rm(&e, X64_ADD, X64_64, X64_RAX, X64_RBP, 8); /* + data[2] */
	x64_store(&e, X64_64, X64_RDI, 24, X64_RAX);          /* data[3] */
	x64_store_imm(&e, X64_64, X64_RBP, 0, -1);            /* data[1] */
	x64_store_imm(&e, X64_32, X64_RDI, 32, -1);          /* data[4], low half */
	x64_alu_mi(&e, X64_ADD, X64_64, X64_R12, 160, 1000); /* data[20] */
	x64_alu_mi(&e, X64_SUB, X64_64, X64_RBP, 152, 1);    /* data[20] */
	x64_load(&e, X64_32, X64_RCX, X64_RDI, 320);         /* data[40], low */
	x64_store(&e, X64_64, X64_RDI, 0, X64_RCX);          /* data[0] */
	x64_pop(&e, X64_RBP);
	x64_pop(&e, X64_R12);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), (uintptr_t)data, 0, 0, 0), 0x3333);
	CHECK_EQ(data[0], 0xCCCCDDDD);
	CHECK_EQ(data[1], UINT64_MAX);
	CHECK_EQ(data[2], 0x2222);
	CHECK_EQ(data[3], 0x3333);
	CHECK_EQ(data[4], 0xFFFFFFFF);
	CHECK_EQ(data[20], 0x100 + 1000 - 1);
}

static uint64_t triple(uint64_t x)
{
	return 3 * x;
}

/* Calls into C, both ways, with the stack aligned as the ABI asks:
 * rsp is 8 past a multiple of 16 at entry, so one PUSH aligns it. */
static void test_run_call(void)
{
	X64Emitter e = start();

	x64_push(&e, X64_RBX);
	x64_mov_rr(&e, X64_64, X64_RBX, X64_RSI);
	x64_call(&e, (uintptr_t)triple); /* rax = 3 * rdi */
	x64_mov_rr(&e, X64_64, X64_RDI, X64_RAX);
	x64_mov_ri(&e, X64_R11, (uintptr_t)triple);
	x64_call_r(&e, X64_R11); /* rax = 9 * rdi */
	x64_alu_rr(&e, X64_ADD, X64_64, X64_RAX, X64_RBX);
	x64_pop(&e, X64_RBX);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), 5, 2, 0, 0), 47);

	/* A function placed further on, called relatively: the CALL at
	 * the start falls in reach of it. */
	e = start();
	x64_push(&e, X64_RBX);
	x64_call(&e, (uintptr_t)code.run + 32);
	x64_pop(&e, X64_RBX);
	x64_ret(&e);
	CHECK_EQ(e.buf[1], 0xE8);
	e.pos = 32;
	x64_lea(&e, X64_RAX, X64_RDI, 1);
	x64_ret(&e);
	CHECK_EQ(call(finish(&e), 41, 0, 0, 0), 42);
}

/* rax = rdi < rsi (signed) ? 1 : rdi == rsi ? 2 : rdi & rdx ? 3 : 4. */
static void test_run_jumps(void)
{
	X64Emitter e = start();

	x64_alu_rr(&e, X64_CMP, X64_64, X64_RDI, X64_RSI);
	X64Label less = x64_jcc(&e, X64_L);
	X64Label equal = x64_jcc(&e, X64_E);
	x64_test_rr(&e, X64_64, X64_RDI, X64_RDX);
	X64Label zero = x64_jcc(&e, X64_E);
	x64_mov_ri(&e, X64_RAX, 3);
	X64Label out = x64_jmp(&e);
	x64_bind(&e, less);
	x64_mov_ri(&e, X64_RAX, 1);
	x64_ret(&e);
	x64_bind(&e, equal);
	x64_mov_ri(&e, X64_RAX, 2);
	x64_ret(&e);
	x64_bind(&e, zero);
	x64_mov_ri(&e, X64_RAX, 4);
	x64_bind(&e, out);
	x64_ret(&e);

	Fn fn = finish(&e);
	CHECK_EQ(call(fn, (uint64_t)-5, 3, 0, 0), 1);
	CHECK_EQ(call(fn, 7, 7, 0, 0), 2);
	CHECK_EQ(call(fn, 9, 3, 1, 0), 3);
	CHECK_EQ(call(fn, 9, 3, 6, 0), 4);
}

static void test_run(void)
{
	if (!exec_mem_init(&code, 4096)) {
		perror("exec_mem_init");
		test_failures++;
		return;
	}
	RUN_TEST(test_run_alu);
	RUN_TEST(test_run_mov);
	RUN_TEST(test_run_memory);
	RUN_TEST(test_run_call);
	RUN_TEST(test_run_jumps);
	exec_mem_free(&code);
}

#else

static void test_run(void)
{
	printf("SKIP running code: not an x86-64 Linux host\n");
}

#endif

int main(void)
{
	/* Wrong code can loop for ever. */
	alarm(30);

	RUN_TEST(test_push_pop_ret);
	RUN_TEST(test_mov_rr);
	RUN_TEST(test_mov_ri);
	RUN_TEST(test_load);
	RUN_TEST(test_store);
	RUN_TEST(test_lea);
	RUN_TEST(test_alu_rr);
	RUN_TEST(test_alu_ri);
	RUN_TEST(test_alu_mem);
	RUN_TEST(test_test_rr);
	RUN_TEST(test_call);
	RUN_TEST(test_jumps);
	RUN_TEST(test_here);
	RUN_TEST(test_full);
	RUN_TEST(test_far_bind);
	test_run();
	return test_summary();
}
