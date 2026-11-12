// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Print a message on UART0 of the Raspberry Pi 3, then wait forever.
// Booted like kernel8.img: loaded at 0x80000, core 0 starts here.

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
	.equ	FR_TXFF, 5		// transmit FIFO full

	.globl	_start
_start:
	movz	x1, #(UART0 >> 16), lsl #16
	movk	x1, #(UART0 & 0xffff)
	// GPIO 14 and 15 carry TXD0 and RXD0 (ALT0): the PL011 on the
	// serial cable, instead of the mini UART that the firmware set up.
	movz	x10, #(GPIO >> 16), lsl #16
	movz	w11, #(GPFSEL1_ALT0 & 0xffff)
	movk	w11, #(GPFSEL1_ALT0 >> 16), lsl #16
	str	w11, [x10, #GPFSEL1]
	str	wzr, [x1, #UARTCR]	// disable the UART while setting it up
	mov	w2, #26		// 115200 baud from the 48 MHz UARTCLK:
	str	w2, [x1, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w2, #3
	str	w2, [x1, #UARTFBRD]
	mov	w2, #0x70		// 8-bit characters, FIFOs on: the
	str	w2, [x1, #UARTLCR_H]	// PL011 resets to 5-bit characters
	mov	w2, #0x301		// enable the UART, transmit and receive
	str	w2, [x1, #UARTCR]

	adr	x3, message
next:	ldrb	w2, [x3], #1
	cbz	w2, done
busy:	ldr	w4, [x1, #UARTFR]	// wait for room in the FIFO
	tbnz	w4, #FR_TXFF, busy
	str	w2, [x1, #UARTDR]
	b	next

done:	wfi
	b	done

message:
	.asciz	"Hello, world!\n"
