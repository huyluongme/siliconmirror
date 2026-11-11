// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Check the state that the raspi3b boot stub leaves the cores in, as a
// kernel8.img sees it. Core 0 starts here at EL2: it checks its state,
// prints "boot state ok" or the number of the first check that failed,
// then releases cores 1 to 3 through the spin table. Each of them prints
// its number if it too starts as Linux's boot protocol says, or "?" if
// not. Core 0 ends the line when all three have printed.
//
// X9 holds UART0; X19 the number of the check that runs.

	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTIBRD, 0x24		// integer baud rate divisor
	.equ	UARTFBRD, 0x28		// fractional baud rate divisor
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	FR_TXFF, 5		// transmit FIFO full

	.equ	SPIN_TABLE, 0xd8	// core n waits on SPIN_TABLE + 8 * n
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'
	.equ	QUESTION, 0x3f		// '?'

	// Fail check \n unless \reg holds \value.
	.macro	expect n, reg, value
	mov	x19, #\n
	movz	x10, #((\value) & 0xffff)
	movk	x10, #(((\value) >> 16) & 0xffff), lsl #16
	cmp	\reg, x10
	b.ne	fail
	.endm

	.globl	_start
_start:
	orr	x20, x0, x1		// before anything changes X0 to X3
	orr	x20, x20, x2
	orr	x20, x20, x3

	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #26		// 115200 baud from the 48 MHz UARTCLK:
	str	w0, [x9, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w0, #3
	str	w0, [x9, #UARTFBRD]
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]

	adr	x0, vectors		// so that a trap prints "trap"
	msr	vbar_el2, x0

	expect	1, x20, 0		// X0 to X3 are 0: no device tree yet
	mrs	x0, currentel
	expect	2, x0, (2 << 2)		// EL2
	mrs	x0, spsel
	expect	3, x0, 1		// using SP_EL2
	mrs	x0, daif
	expect	4, x0, 0x3c0		// D, A, I and F masked
	mrs	x0, sctlr_el2
	expect	5, x0, 0x30c50830	// MMU, caches, alignment checks off
	mrs	x0, s3_1_c15_c2_1	// CPUECTLR_EL1
	expect	6, x0, 0x40		// SMPEN
	// ACTLR_EL3 lets EL2 write CPUECTLR_EL1: no trap.
	msr	s3_1_c15_c2_1, x0

	adr	x0, ok
	bl	puts

	// Release cores 1 to 3, then wake them.
	adr	x0, secondary
	mov	x1, #(SPIN_TABLE + 8)
	str	x0, [x1]
	str	x0, [x1, #8]
	str	x0, [x1, #16]
	sev

	// Wait for the three cores to print, then end the line.
	adr	x1, done
wait:	ldrb	w2, [x1, #1]
	ldrb	w3, [x1, #2]
	and	w2, w2, w3
	ldrb	w3, [x1, #3]
	ands	w2, w2, w3
	b.eq	wait
	mov	w0, #NEWLINE
	bl	putc
	b	park

fail:	adr	x0, failed
	bl	puts
	add	w0, w19, #DIGIT_0
	bl	putc
	mov	w0, #NEWLINE
	bl	putc
park:	wfi
	b	park

// Cores 1 to 3 start here at EL2, X0 to X3 clear.
secondary:
	orr	x20, x0, x1
	orr	x20, x20, x2
	orr	x20, x20, x3
	mrs	x21, currentel
	mrs	x6, mpidr_el1
	and	x6, x6, #3		// the core number
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)

	add	w0, w6, #DIGIT_0
	cbnz	x20, 1f
	cmp	x21, #(2 << 2)
	b.eq	2f
1:	mov	w0, #QUESTION
2:	bl	putc
	adr	x1, done		// tell core 0
	mov	w2, #1
	strb	w2, [x1, x6]
	b	park

// Print the string at X0. Uses X0, X1, X2.
puts:	mov	x2, x30
	mov	x1, x0
1:	ldrb	w0, [x1], #1
	cbz	w0, 2f
	bl	putc
	b	1b
2:	ret	x2

// Print the character in W0. Uses X3.
putc:	ldr	w3, [x9, #UARTFR]
	tbnz	w3, #FR_TXFF, putc
	str	w0, [x9, #UARTDR]
	ret

ok:	.asciz	"boot state ok\n"
failed:	.asciz	"failed check "
done:	.byte	0, 0, 0, 0		// one per core

// Any exception at EL2 prints "trap" and stops the core.
	.balign	2048
vectors:
	.skip	0x200			// current EL with SP_ELx, synchronous:
	adr	x0, trap
	bl	puts
	b	park
trap:	.asciz	"trap\n"
