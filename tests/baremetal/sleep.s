// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Sleep for 2 s, as a kernel8.img that waits with its interrupts masked:
// the EL1 physical timer, routed to core 0's IRQ, ends the WFI, which
// PSTATE.I does not stop (D1.7). Then print "awake", turn the timer off
// and wait for nothing, so that the machine goes idle. With -clock host,
// the 2 s take 2 s on the host's clock too.

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
	.equ	LOCAL, 0x40000000	// ARM local peripherals
	.equ	TIMER_ROUTING0, 0x40	// core 0's timer interrupt control
	.equ	TICKS, 38400000		// 2 s of the 19.2 MHz counter

	.globl	_start
_start:
	movz	x1, #(UART0 >> 16), lsl #16
	movk	x1, #(UART0 & 0xffff)
	movz	x10, #(GPIO >> 16), lsl #16
	movz	w11, #(GPFSEL1_ALT0 & 0xffff)
	movk	w11, #(GPFSEL1_ALT0 >> 16), lsl #16
	str	w11, [x10, #GPFSEL1]
	str	wzr, [x1, #UARTCR]	// disable the UART while setting it up
	mov	w2, #26		// 115200 baud from the 48 MHz UARTCLK:
	str	w2, [x1, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w2, #3
	str	w2, [x1, #UARTFBRD]
	mov	w2, #0x70		// 8-bit characters, FIFOs on
	str	w2, [x1, #UARTLCR_H]
	mov	w2, #0x301		// enable the UART, transmit and receive
	str	w2, [x1, #UARTCR]

	movz	x8, #(LOCAL >> 16), lsl #16
	mov	w0, #(1 << 1)		// nCNTPNSIRQ IRQ control
	str	w0, [x8, #TIMER_ROUTING0]
	movz	x0, #(TICKS >> 16), lsl #16
	movk	x0, #(TICKS & 0xffff)
	msr	cntp_tval_el0, x0
	mov	x0, #1			// ENABLE
	msr	cntp_ctl_el0, x0
sleep:	wfi
	mrs	x0, cntp_ctl_el0
	tbz	x0, #2, sleep		// until ISTATUS
	msr	cntp_ctl_el0, xzr

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
	.asciz	"awake\n"
