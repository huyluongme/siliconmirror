// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Echo what comes on UART0, then print "echo ok", on core 0 at EL2 after
// the raspi3b boot stub. The UART interrupts when bytes wait in its
// receive FIFO: through the receive interrupt when the FIFO reaches half
// of its 16 entries, or the receive timeout interrupt when no byte came
// for 32 bit periods. The interrupt goes through GPU interrupt 57 of the
// BCM2835 interrupt controller and the ARM local peripherals to the IRQ
// of core 0, which waits in WFI meanwhile. Its handler sends back each
// byte, and the program ends after a newline.
//
// X8 holds the BCM2835 interrupt controller, X9 UART0, X20 1 once the
// newline came.

	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTIBRD, 0x24		// integer baud rate divisor
	.equ	UARTFBRD, 0x28		// fractional baud rate divisor
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	UARTIMSC, 0x38		// interrupt mask
	.equ	FR_RXFE, 4		// receive FIFO empty
	.equ	FR_TXFF, 5		// transmit FIFO full
	.equ	RX_INTERRUPTS, 0x50	// receive and receive timeout
	.equ	IC, 0x3f00b200		// BCM2835 interrupt controller
	.equ	ENABLE2, 0x14
	.equ	UART_IRQ, 57		// GPU interrupt 57, bit 25 of enable 2
	.equ	NEWLINE, 0x0a

	.globl	_start
_start:
	movz	x8, #(IC >> 16), lsl #16
	movk	x8, #(IC & 0xffff)
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #26			// 115200 baud from the 48 MHz UARTCLK:
	str	w0, [x9, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w0, #3
	str	w0, [x9, #UARTFBRD]
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]
	adr	x0, vectors
	msr	vbar_el2, x0

	mov	w0, #RX_INTERRUPTS
	str	w0, [x9, #UARTIMSC]
	mov	w0, #(1 << (UART_IRQ - 32))
	str	w0, [x8, #ENABLE2]
	mov	x20, #0
	msr	daifclr, #2		// take IRQs
1:	wfi
	cbz	x20, 1b

	msr	daifset, #2
	str	wzr, [x9, #UARTIMSC]
	mov	w0, #0x101		// stop receiving, go on sending
	str	w0, [x9, #UARTCR]
	adr	x0, ok
	bl	puts
2:	wfi
	b	2b

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

ok:	.asciz	"echo ok\n"
trap:	.asciz	"trap\n"

// Core 0's vectors at EL2, current level with SP_EL2.
	.balign	2048
vectors:
	.skip	0x200
	adr	x0, trap		// synchronous: print "trap", stop
	bl	puts
2:	wfi
	b	2b
	.balign	0x80			// IRQ: send back every byte waiting
irq:	ldr	w4, [x9, #UARTFR]
	tbnz	w4, #FR_RXFE, 2f
	ldr	w0, [x9, #UARTDR]
	and	w0, w0, #0xff		// the data, without the error bits
	cmp	w0, #NEWLINE
	cset	x5, eq
	orr	x20, x20, x5
1:	ldr	w4, [x9, #UARTFR]
	tbnz	w4, #FR_TXFF, 1b
	str	w0, [x9, #UARTDR]
	b	irq
2:	eret
