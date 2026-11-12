// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Check the transmit interrupt of UART0 all the way to core 0, at EL2
// after the raspi3b boot stub: from the PL011 to GPU interrupt 57 of the
// BCM2835 interrupt controller, its IRQ to the ARM local peripherals,
// and their GPU IRQ routing to core 0; then the same through the FIQ.
// Prints "uart irq ok", or the number of the first check that failed.
//
// The UART runs without FIFOs, so its transmit interrupt sets when the
// holding register empties: at once for the first byte, "u", and when
// the first is sent for the second, "a". The rest of the line follows.
//
// Core 0's handler records what it took, clears the UART's interrupt,
// then returns with IRQ and FIQ masked: X25 is 1 for an IRQ and 2 for an
// FIQ, X24 and X27 the IRQ and FIQ source registers of core 0, X23 the
// UART's masked interrupt status.
//
// X8 holds the ARM local peripherals, X9 UART0, X12 the interrupt
// controller, X19 the number of the check that runs.

	.equ	GPIO, 0x3f200000
	.equ	GPFSEL1, 0x04		// function select of GPIO 10-19
	.equ	GPFSEL1_ALT0, 0x24000	// GPIO 14 and 15: ALT0
	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTIBRD, 0x24		// integer baud rate divisor
	.equ	UARTFBRD, 0x28		// fractional baud rate divisor
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	UARTIMSC, 0x38		// interrupt mask
	.equ	UARTMIS, 0x40		// masked interrupt status
	.equ	UARTICR, 0x44		// interrupt clear
	.equ	FR_TXFF, 5		// transmit FIFO full
	.equ	UART_TX, 1 << 5		// the transmit interrupt
	.equ	IC, 0x3f00b200		// BCM2835 interrupt controller
	.equ	BASIC_PENDING, 0x00
	.equ	PENDING2, 0x08
	.equ	FIQ_CONTROL, 0x0c
	.equ	ENABLE2, 0x14
	.equ	DISABLE2, 0x20
	.equ	UART_IRQ, 57		// GPU interrupt 57, bit 25 of pending 2
	.equ	LOCAL, 0x40000000	// ARM local peripherals
	.equ	GPU_ROUTING, 0x0c
	.equ	IRQ_SOURCE0, 0x60
	.equ	FIQ_SOURCE0, 0x70
	.equ	SOURCE_GPU, 1 << 8
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	// Put the 32-bit value in \reg.
	.macro	mov32	reg, value
	movz	\reg, #((\value) & 0xffff)
	movk	\reg, #(((\value) >> 16) & 0xffff), lsl #16
	.endm

	// Fail check \n unless \reg holds \value.
	.macro	expect	n, reg, value
	mov	x19, #\n
	mov32	x10, \value
	cmp	\reg, x10
	b.ne	fail
	.endm

	.globl	_start
_start:
	movz	x8, #(LOCAL >> 16), lsl #16
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	movz	x12, #(IC >> 16), lsl #16
	movk	x12, #(IC & 0xffff)
	// GPIO 14 and 15 carry TXD0 and RXD0 (ALT0): the PL011 on the
	// serial cable, instead of the mini UART that the firmware set up.
	movz	x10, #(GPIO >> 16), lsl #16
	movz	w11, #(GPFSEL1_ALT0 & 0xffff)
	movk	w11, #(GPFSEL1_ALT0 >> 16), lsl #16
	str	w11, [x10, #GPFSEL1]
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #26			// 115200 baud from the 48 MHz UARTCLK:
	str	w0, [x9, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w0, #3
	str	w0, [x9, #UARTFBRD]
	mov	w0, #0x60		// 8-bit characters, no FIFOs
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]
	adr	x0, vectors
	msr	vbar_el2, x0

	// The UART's transmit interrupt, enabled as GPU interrupt 57. The
	// GPU IRQ goes to core 0, as the ARM local peripherals reset.
	mov	w0, #UART_TX
	str	w0, [x9, #UARTIMSC]
	mov32	w0, (1 << (UART_IRQ - 32))
	str	w0, [x12, #ENABLE2]
	ldr	w0, [x12, #BASIC_PENDING]
	expect	1, x0, 0		// not before a byte was written

	// "u" goes from the holding register to the transmitter at once.
	// IRQs are masked: it is pending at each level.
	mov	w0, #'u'
	str	w0, [x9, #UARTDR]
	ldr	w0, [x12, #BASIC_PENDING]
	expect	2, x0, (1 << 19)	// GPU IRQ 57, among the selected ones
	ldr	w0, [x12, #PENDING2]
	expect	3, x0, (1 << (UART_IRQ - 32))
	ldr	w0, [x8, #IRQ_SOURCE0]
	expect	4, x0, SOURCE_GPU
	mov	x25, #0
	msr	daifclr, #3
	expect	5, x25, 1
	expect	6, x24, SOURCE_GPU
	expect	7, x23, UART_TX
	mrs	x0, isr_el1
	expect	8, x0, 0		// the handler cleared it

	// Now through the FIQ: GPU interrupt 57 as the FIQ source, not as
	// an IRQ. The GPU FIQ goes to core 0, as the routing resets.
	mov32	w0, (1 << (UART_IRQ - 32))
	str	w0, [x12, #DISABLE2]
	mov	w0, #(0x80 | UART_IRQ)	// FIQ enable, source 57
	str	w0, [x12, #FIQ_CONTROL]
	ldr	w0, [x12, #ENABLE2]
	expect	9, x0, 0

	// "a" waits in the holding register until "u" is sent.
	mov	x25, #0
	mov	w0, #'a'
	str	w0, [x9, #UARTDR]
	msr	daifclr, #3
	wfi
	expect	10, x25, 2
	expect	11, x27, SOURCE_GPU
	str	wzr, [x12, #FIQ_CONTROL]
	str	wzr, [x9, #UARTIMSC]
	mrs	x0, isr_el1
	expect	12, x0, 0

	adr	x0, ok
	bl	puts
	b	park

fail:	adr	x0, failed
	bl	puts
	mov	w20, #10
	udiv	w21, w19, w20		// tens
	msub	w22, w21, w20, w19	// units
	cbz	w21, 1f
	add	w0, w21, #DIGIT_0
	bl	putc
1:	add	w0, w22, #DIGIT_0
	bl	putc
	mov	w0, #NEWLINE
	bl	putc

	// Turn off whatever could interrupt, then wait for nothing.
park:	msr	daifset, #3
	str	wzr, [x9, #UARTIMSC]
	str	wzr, [x12, #FIQ_CONTROL]
	mov32	w0, 0xffffffff
	str	w0, [x12, #DISABLE2]
1:	wfi
	b	1b

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

ok:	.asciz	"rt irq ok\n"
failed:	.asciz	"\nfailed check "
trap:	.asciz	"\ntrap\n"

// Core 0's vectors at EL2, current level with SP_EL2.
	.balign	2048
vectors:
	.skip	0x200
	adr	x0, trap		// synchronous: print "trap", stop
	bl	puts
	b	park
	.balign	0x80
	mov	x25, #1			// IRQ
	b	handler
	.balign	0x80
	mov	x25, #2			// FIQ
handler:
	ldr	w24, [x8, #IRQ_SOURCE0]
	ldr	w27, [x8, #FIQ_SOURCE0]
	ldr	w23, [x9, #UARTMIS]
	mov	w0, #UART_TX
	str	w0, [x9, #UARTICR]
	mrs	x0, spsr_el2
	orr	x0, x0, #0xc0		// return with IRQ and FIQ masked
	msr	spsr_el2, x0
	eret
