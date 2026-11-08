// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Check the Generic Timer as a kernel8.img sees it on core 0 at EL2,
// after the raspi3b boot stub has set it up: the frequency, the counts
// and how fast they go, the EL1 physical timer, and an event stream.
// Prints "timer ok", or the number of the first check that failed, then
// turns the timer and the event stream off so that the machine can go
// idle. smirror runs one instruction per nanosecond of virtual time.
//
// X9 holds UART0; X19 the number of the check that runs.

	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	FR_TXFF, 5		// transmit FIFO full
	.equ	LOCAL_TIMER, 0x4000001c	// core timer, low half then high
	.equ	CRYSTAL_HZ, 19200000
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	// Fail check \n unless \reg holds \value.
	.macro	expect n, reg, value
	mov	x19, #\n
	movz	x10, #((\value) & 0xffff)
	movk	x10, #(((\value) >> 16) & 0xffff), lsl #16
	cmp	\reg, x10
	b.ne	fail
	.endm

	// Fail check \n unless \low <= \reg < \low + \range.
	.macro	within n, reg, low, range
	mov	x19, #\n
	movz	x10, #((\low) & 0xffff)
	movk	x10, #(((\low) >> 16) & 0xffff), lsl #16
	sub	x11, \reg, x10
	cmp	x11, #\range
	b.hs	fail
	.endm

	.globl	_start
_start:
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]
	adr	x0, vectors		// so that a trap prints "trap"
	msr	vbar_el2, x0

	// The stub has told software the counter's frequency, and left
	// the virtual count equal to the physical one.
	mrs	x0, cntfrq_el0
	expect	1, x0, CRYSTAL_HZ
	mrs	x0, cntvoff_el2
	expect	2, x0, 0

	// 2,000,000 instructions take 2 ms: 38,400 ticks of 19.2 MHz,
	// or one more, depending on where the ticks fall.
	mrs	x20, cntpct_el0
	movz	x0, #(1000000 & 0xffff)
	movk	x0, #(1000000 >> 16), lsl #16
1:	subs	x0, x0, #1
	b.ne	1b
	mrs	x21, cntpct_el0
	sub	x0, x21, x20
	within	3, x0, 38400, 2

	// The count is the core timer of the ARM local peripherals: reading
	// its low half latches its high half.
	movz	x1, #(LOCAL_TIMER >> 16), lsl #16
	movk	x1, #(LOCAL_TIMER & 0xffff)
	mrs	x20, cntpct_el0
	ldr	w0, [x1]
	ldr	w2, [x1, #4]
	orr	x0, x0, x2, lsl #32
	sub	x0, x0, x20
	within	4, x0, 0, 2

	// The virtual count is the physical count less CNTVOFF_EL2.
	mov	x0, #1000
	msr	cntvoff_el2, x0
	mrs	x20, cntpct_el0
	mrs	x21, cntvct_el0
	sub	x0, x20, x21
	within	5, x0, 999, 2
	msr	cntvoff_el2, xzr

	// The EL1 physical timer, 100 us (1920 ticks) from now: ISTATUS
	// is clear until the count gets to CompareValue.
	mov	x0, #1920
	msr	cntp_tval_el0, x0
	mov	x0, #1			// ENABLE
	msr	cntp_ctl_el0, x0
	mrs	x0, cntp_ctl_el0
	expect	6, x0, 1
2:	mrs	x0, cntp_ctl_el0
	tbz	x0, #2, 2b		// ISTATUS
	mrs	x20, cntpct_el0
	mrs	x21, cntp_cval_el0
	sub	x0, x20, x21
	within	7, x0, 0, 2
	mrs	x0, cntp_tval_el0	// 0 or just below, in 32 bits
	neg	w0, w0
	within	8, x0, 0, 2
	msr	cntp_ctl_el0, xzr

	// The event stream of CNTHCTL_EL2 from bit 10 of the count, 0 to
	// 1: an event every 2048 ticks, when the count is 1024 modulo
	// 2048. The first WFE clears the Event Register, which the stub's
	// ERET set; the second waits for an event.
	mov	x0, #(10 << 4 | 1 << 2 | 3)	// EVNTI, EVNTEN, EL1PC(T)EN
	msr	cnthctl_el2, x0
	wfe
	wfe
	mrs	x0, cntpct_el0
	and	x0, x0, #2047
	within	9, x0, 1024, 1

	adr	x0, ok
	bl	puts
	b	park

fail:	adr	x0, failed
	bl	puts
	add	w0, w19, #DIGIT_0
	bl	putc
	mov	w0, #NEWLINE
	bl	putc

	// Turn off the timer and the event stream, then wait for nothing.
park:	msr	cntp_ctl_el0, xzr
	mov	x0, #3			// EL1PCEN, EL1PCTEN
	msr	cnthctl_el2, x0
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

ok:	.asciz	"timer ok\n"
failed:	.asciz	"failed check "

// Any exception at EL2 prints "trap" and stops the core.
	.balign	2048
vectors:
	.skip	0x200			// current EL with SP_ELx, synchronous:
	adr	x0, trap
	bl	puts
	b	park
trap:	.asciz	"trap\n"
