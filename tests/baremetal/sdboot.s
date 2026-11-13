// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// The kernel of an SD card image (see sdcard.cmake): what the firmware
// gives a kernel that it boots from the card. Checks that X0 holds the
// address of a device tree, that the kernel runs where the firmware
// loads it, and asks the firmware for the command line, which it prints:
// "cmdline: " and the line, then "sd boot ok", or the number of the
// first check that failed. Prints on the mini UART, as enable_uart=1 on
// the card sets it up.
//
// X9 holds the mini UART, X10 the mailboxes, X19 the number of the check
// that runs, X20 the device tree's address.

	.equ	AUX, 0x3f215000
	.equ	AUX_MU_IO, 0x40		// data
	.equ	AUX_MU_LSR, 0x54	// line status
	.equ	LSR_TX_EMPTY, 5		// room in the transmit FIFO
	.equ	MBOX, 0x3f00b880
	.equ	MAIL0_READ, 0x00
	.equ	MAIL0_STATUS, 0x18
	.equ	MAIL1_WRITE, 0x20
	.equ	STATUS_EMPTY, 30
	.equ	PROPERTY, 8		// the property channel
	.equ	FDT_MAGIC_LE, 0xedfe0dd0 // 0xd00dfeed, big-endian in memory
	.equ	KERNEL, 0x80000		// where the firmware loads this image
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	.globl	_start
_start:
	mov	x20, x0
	movz	x9, #(AUX >> 16), lsl #16
	movk	x9, #(AUX & 0xffff)
	movz	x10, #(MBOX >> 16), lsl #16
	movk	x10, #(MBOX & 0xffff)

	// 1: X0 holds the address of a device tree.
	mov	x19, #1
	cbz	x20, fail
	ldr	w0, [x20]
	movz	w1, #(FDT_MAGIC_LE & 0xffff)
	movk	w1, #(FDT_MAGIC_LE >> 16), lsl #16
	cmp	w0, w1
	b.ne	fail

	// 2: the kernel runs where the firmware loaded it.
	mov	x19, #2
	adr	x0, _start
	cmp	x0, #(KERNEL >> 12), lsl #12
	b.ne	fail

	// 3: the firmware answers with the command line.
	mov	x19, #3
	adr	x2, buffer
	orr	w0, w2, #PROPERTY
	str	w0, [x10, #MAIL1_WRITE]
1:	ldr	w1, [x10, #MAIL0_STATUS]
	tbnz	w1, #STATUS_EMPTY, 1b
	ldr	w1, [x10, #MAIL0_READ]
	cmp	w1, w0
	b.ne	fail
	ldr	w0, [x2, #16]		// the tag's code: bit 31 and the length
	tbz	w0, #31, fail
	and	w4, w0, #0xff		// the length; the buffer holds 200

	adr	x0, cmdline
	bl	puts
	adr	x5, line
2:	cbz	w4, 3f
	ldrb	w0, [x5], #1
	bl	putc
	sub	w4, w4, #1
	b	2b
3:	mov	w0, #NEWLINE
	bl	putc

	adr	x0, ok
	bl	puts
	b	park

fail:	adr	x0, failed
	bl	puts
	add	w0, w19, #DIGIT_0
	bl	putc
	mov	w0, #NEWLINE
	bl	putc
park:	wfe
	b	park

// Print the string at X0. Uses X0, X1, X2, X3.
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

cmdline: .asciz	"cmdline: "
ok:	.asciz	"sd boot ok\n"
failed:	.asciz	"failed check "

// A property buffer: Get command line, with room for 200 bytes.
	.balign	16
buffer:	.word	buffer_end - buffer	// size
	.word	0			// a request
	.word	0x00050001, 200, 0	// Get command line
line:	.space	200
	.word	0			// the end tag
buffer_end:
