// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Take exceptions at EL2, EL1 and EL0 of the Raspberry Pi 3, and return
// from them with ERET. The handler prints the Exception level it runs at
// and the exception class (EC) from its ESR, as "<EL>:<EC in hex>", then
// returns past the instruction: SVC and HVC return after it by
// themselves, the others return to it. Booted like kernel8.img: core 0
// starts here at EL2.
//
// The main code uses X0 to X9, the handler X10 to X18. X9 holds UART0.

	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	FR_TXFF, 5		// transmit FIFO full

	.equ	HCR_RW, 1 << 31		// HCR_EL2: EL1 uses AArch64
	.equ	HCR_TACR, 1 << 21	// HCR_EL2: trap ACTLR_EL1 to EL2
	.equ	SPSR_EL1H, 0x3c5	// EL1 with SP_EL1; D, A, I, F masked
	.equ	SPSR_EL0T, 0x3c0	// EL0; D, A, I, F masked

	.globl	_start
_start:
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]

	adr	x0, vectors		// one table for EL2 and EL1
	msr	vbar_el2, x0
	msr	vbar_el1, x0

	svc	#0			// 2:15  SVC at EL2
	brk	#0			// 2:3c  BRK
	.inst	0x00000000		// 2:00  UDF #0, an unknown instruction
	smc	#0			// 2:00  SMC: UNDEFINED, SCR_EL3.SMD is set

	// Go down to EL1, using AArch64, with ACTLR_EL1 trapped to EL2.
	movz	x0, #((HCR_RW | HCR_TACR) >> 16), lsl #16
	msr	hcr_el2, x0
	mov	x0, #SPSR_EL1H
	msr	spsr_el2, x0
	adr	x0, el1
	msr	elr_el2, x0
	eret

el1:	svc	#1			// 1:15  SVC at EL1
	hvc	#2			// 2:16  HVC, from EL1 to EL2
	movz	x1, #0x8, lsl #16
	movk	x1, #0x0001		// 0x80001: not 8-byte aligned
	ldr	x0, [x1]		// 1:25  alignment fault: the MMU is off
	mrs	x0, actlr_el1		// 2:18  trapped by HCR_EL2.TACR

	// Go down to EL0.
	mov	x0, #SPSR_EL0T
	msr	spsr_el1, x0
	adr	x0, el0
	msr	elr_el1, x0
	eret

el0:	svc	#3			// 1:15  SVC from EL0 to EL1
	mrs	x0, daif		// 1:18  trapped: SCTLR_EL1.UMA is 0
done:	wfi				// allowed at EL0: SCTLR_EL1.nTWI is 1
	b	done

// The vector table: 16 entries of 0x80 bytes, D1.3.1. Only synchronous
// exceptions from the current level with SP_ELx and from a lower level
// using AArch64 are expected.
	.macro	vector target
	.balign	0x80
	b	\target
	.endm

	.balign	0x800
vectors:
	vector	unexpected		// current level with SP_EL0
	vector	unexpected
	vector	unexpected
	vector	unexpected
	vector	handler			// current level with SP_ELx: synchronous
	vector	unexpected
	vector	unexpected
	vector	unexpected
	vector	handler			// lower level, AArch64: synchronous
	vector	unexpected
	vector	unexpected
	vector	unexpected
	vector	unexpected		// lower level, AArch32
	vector	unexpected
	vector	unexpected
	vector	unexpected

handler:
	mov	x18, x30		// keep the LR of the interrupted code
	mrs	x10, CurrentEL
	lsr	x10, x10, #2		// the level this handler runs at
	cmp	x10, #2
	b.eq	1f
	mrs	x11, esr_el1
	b	2f
1:	mrs	x11, esr_el2
2:	ubfx	x14, x11, #26, #6	// EC

	add	w12, w10, #'0'
	bl	putc
	mov	w12, #':'
	bl	putc
	lsr	w12, w14, #4
	bl	puthex
	and	w12, w14, #0xf
	bl	puthex
	mov	w12, #'\n'
	bl	putc
	mov	x30, x18

	// SVC, HVC and SMC (EC 0x15 to 0x17) return after the instruction.
	cmp	w14, #0x15
	b.lo	3f
	cmp	w14, #0x17
	b.ls	5f
3:	cmp	x10, #2
	b.eq	4f
	mrs	x15, elr_el1
	add	x15, x15, #4
	msr	elr_el1, x15
	b	5f
4:	mrs	x15, elr_el2
	add	x15, x15, #4
	msr	elr_el2, x15
5:	eret

unexpected:
	mov	w12, #'?'
	bl	putc
1:	wfi
	b	1b

// Print the hexadecimal digit in W12.
puthex:	add	w12, w12, #'0'
	cmp	w12, #'9'
	b.ls	putc
	add	w12, w12, #('a' - '0' - 10)
	// fall through

// Print the character in W12, using X13.
putc:	ldr	w13, [x9, #UARTFR]	// wait for room in the FIFO
	tbnz	w13, #FR_TXFF, putc
	str	w12, [x9, #UARTDR]
	ret
