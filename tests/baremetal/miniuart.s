// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Use the console as U-Boot does on the raspi3b: the mini UART, which the
// firmware has set up, on GPIO 14 and 15. Check that the pins carry it,
// print without setting anything up, then send back what comes until a
// newline, waiting for the receive interrupt in AUX_MU_IIR_REG, which
// bit 0 of AUX_MU_IER_REG enables. Prints "mini uart ok", or the number
// of the first check that failed.
//
// X9 holds the AUX block, X19 the number of the check that runs.

	.equ	AUX, 0x3f215000
	.equ	AUX_IRQ, 0x00		// interrupt status
	.equ	AUX_MU_IO, 0x40		// data
	.equ	AUX_MU_IER, 0x44	// interrupt enable
	.equ	AUX_MU_IIR, 0x48	// interrupt identify
	.equ	AUX_MU_LSR, 0x54	// line status
	.equ	LSR_TX_EMPTY, 5		// room in the transmit FIFO
	.equ	IER_RX, 1		// the receive interrupt
	.equ	IIR_NONE, 0		// bit: no interrupt pending
	.equ	IIR_ID, 0x6		// interrupt ID
	.equ	IIR_RX, 0x4		// the receiver holds a byte
	.equ	GPIO, 0x3f200000
	.equ	GPFSEL1, 0x04		// function select of GPIO 10-19
	.equ	FSEL14_15, 0x3f000	// GPIO 14 and 15
	.equ	FSEL14_15_ALT5, 0x12000
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	.globl	_start
_start:
	movz	x9, #(AUX >> 16), lsl #16
	movk	x9, #(AUX & 0xffff)

	// 1: GPIO 14 and 15 carry TXD1 and RXD1 (ALT5).
	mov	x19, #1
	movz	x0, #(GPIO >> 16), lsl #16
	ldr	w1, [x0, #GPFSEL1]
	and	w1, w1, #FSEL14_15
	cmp	w1, #(FSEL14_15_ALT5 >> 12), lsl #12
	b.ne	fail
	adr	x0, hello
	bl	puts

	// 2: each byte that comes raises the receive interrupt.
	mov	x19, #2
	mov	w0, #IER_RX
	str	w0, [x9, #AUX_MU_IER]
next:	ldr	w0, [x9, #AUX_MU_IIR]
	tbnz	w0, #IIR_NONE, next
	and	w1, w0, #IIR_ID
	cmp	w1, #IIR_RX
	b.ne	fail
	ldr	w1, [x9, #AUX_IRQ]
	cbz	w1, fail
	ldr	w0, [x9, #AUX_MU_IO]
	bl	putc
	cmp	w0, #NEWLINE
	b.ne	next

	adr	x0, ok
	bl	puts
	b	park

fail:	adr	x0, failed
	bl	puts
	add	w0, w19, #DIGIT_0
	bl	putc
	mov	w0, #NEWLINE
	bl	putc
park:	wfi
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
putc:	ldr	w3, [x9, #AUX_MU_LSR]
	tbz	w3, #LSR_TX_EMPTY, putc
	str	w0, [x9, #AUX_MU_IO]
	ret

hello:	.asciz	"mini uart\n"
ok:	.asciz	"mini uart ok\n"
failed:	.asciz	"failed check "
