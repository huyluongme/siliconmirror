// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Check interrupts as a kernel8.img sees them on the raspi3b, at EL2
// after the boot stub: a Generic Timer interrupt through the ARM local
// peripherals to the IRQ, then to the FIQ, of core 0; a mailbox
// interrupt while masked, then unmasked; the local timer; the AXI
// outstanding interrupt; and a mailbox interrupt from core 0 to core 1.
// Prints "irq ok", or the number of the first check that failed.
//
// Core 0's handler records what it took, then returns with IRQ and FIQ
// masked: X25 is 1 for an IRQ and 2 for an FIQ, X24 and X27 the IRQ and
// FIQ source registers of core 0, X26 the return address.
//
// X8 holds the ARM local peripherals, X9 UART0, X19 the number of the
// check that runs.

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
	.equ	AXI_COUNTERS, 0x2c
	.equ	AXI_IRQ, 0x30
	.equ	LOCAL_TIMER, 0x34
	.equ	LOCAL_FLAGS, 0x38
	.equ	TIMER_ROUTING0, 0x40	// core 0's; core n's 4 * n further
	.equ	MAILBOX_ROUTING0, 0x50
	.equ	IRQ_SOURCE0, 0x60
	.equ	FIQ_SOURCE0, 0x70
	.equ	MAILBOX_SET0, 0x80	// core 0 mailbox 0; core 1's at 0x90
	.equ	MAILBOX_CLEAR0, 0xc0	// core 0 mailbox 0; core 1's at 0xd0
	.equ	SPIN_TABLE, 0xd8	// core n waits on SPIN_TABLE + 8 * n
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

	// Fail check \n unless \reg holds the address of \label.
	.macro	expect_at n, reg, label
	mov	x19, #\n
	adr	x10, \label
	cmp	\reg, x10
	b.ne	fail
	.endm

	// Fail check \n unless \low <= \reg < \low + \range.
	.macro	within	n, reg, low, range
	mov	x19, #\n
	mov32	x10, \low
	sub	x11, \reg, x10
	cmp	x11, #\range
	b.hs	fail
	.endm

	.globl	_start
_start:
	movz	x8, #(LOCAL >> 16), lsl #16
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	// GPIO 14 and 15 carry TXD0 and RXD0 (ALT0): the PL011 on the
	// serial cable, instead of the mini UART that the firmware set up.
	movz	x10, #(GPIO >> 16), lsl #16
	movz	w11, #(GPFSEL1_ALT0 & 0xffff)
	movk	w11, #(GPFSEL1_ALT0 >> 16), lsl #16
	str	w11, [x10, #GPFSEL1]
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #26		// 115200 baud from the 48 MHz UARTCLK:
	str	w0, [x9, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w0, #3
	str	w0, [x9, #UARTFBRD]
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x301		// enable the UART, transmit and receive
	str	w0, [x9, #UARTCR]
	adr	x0, vectors
	msr	vbar_el2, x0

	// The EL1 physical timer, 100 us from now, to core 0's IRQ. SCR_EL3
	// does not take IRQs to EL3, so they come to EL2, which PSTATE.I
	// masks until now.
	mov	w0, #(1 << 1)		// nCNTPNSIRQ IRQ control
	str	w0, [x8, #TIMER_ROUTING0]
	mov	x0, #1920
	msr	cntp_tval_el0, x0
	mov	x0, #1			// ENABLE
	msr	cntp_ctl_el0, x0
	mov	x25, #0
	msr	daifclr, #3
	wfi
1:	expect	1, x25, 1
	expect	2, x24, (1 << 1)	// CNTPNSIRQ
	expect_at 3, x26, 1b
	mrs	x20, cntpct_el0
	mrs	x21, cntp_cval_el0
	sub	x0, x20, x21
	within	4, x0, 0, 2
	msr	cntp_ctl_el0, xzr
	ldr	w0, [x8, #IRQ_SOURCE0]
	expect	5, x0, 0

	// The same timer to the FIQ.
	mov	w0, #(1 << 5)		// nCNTPNSIRQ FIQ control
	str	w0, [x8, #TIMER_ROUTING0]
	mov	x0, #192
	msr	cntp_tval_el0, x0
	mov	x0, #1
	msr	cntp_ctl_el0, x0
	mov	x25, #0
	msr	daifclr, #3
	wfi
	expect	6, x25, 2
	expect	7, x27, (1 << 1)
	msr	cntp_ctl_el0, xzr
	str	wzr, [x8, #TIMER_ROUTING0]

	// Mailbox 0 of core 0 while IRQs are masked: pending, not taken,
	// and WFI does not wait. Unmasking it takes it at once.
	mov	w0, #1			// mailbox 0 IRQ control
	str	w0, [x8, #MAILBOX_ROUTING0]
	mov	w0, #0x55
	str	w0, [x8, #MAILBOX_SET0]
	mov	x25, #0
	mrs	x0, isr_el1
	expect	8, x0, 0x80		// I
	wfi
	expect	9, x25, 0
	ldr	w0, [x8, #IRQ_SOURCE0]
	expect	10, x0, (1 << 4)	// mailbox 0
	ldr	w20, [x8, #MAILBOX_CLEAR0]
	expect	11, x20, 0x55
	msr	daifclr, #2
2:	expect_at 12, x26, 2b
	str	w20, [x8, #MAILBOX_CLEAR0]
	mrs	x0, isr_el1
	expect	13, x0, 0
	str	wzr, [x8, #MAILBOX_ROUTING0]

	// The local timer, every 384 pulses (10 us), to core 0's IRQ.
	str	wzr, [x8, #0x24]	// local interrupt routing: core 0 IRQ
	mov32	w0, (1 << 29 | 1 << 28 | 384)
	str	w0, [x8, #LOCAL_TIMER]
	mov	x25, #0
	msr	daifclr, #2
	wfi
	expect	14, x24, (1 << 11)
	mov32	w0, (1 << 31)		// clear the flag
	str	w0, [x8, #LOCAL_FLAGS]
	str	wzr, [x8, #LOCAL_TIMER]
	ldr	w0, [x8, #LOCAL_TIMER]
	expect	15, x0, 0

	// The AXI outstanding interrupt, after 15 APB clocks of a quiet bus:
	// once core 0 waits, as cores 1 to 3 do in the stub.
	mov32	w0, (1 << 20)
	str	w0, [x8, #AXI_IRQ]
	mov	x25, #0
	msr	daifclr, #2
	wfi
	expect	16, x24, (1 << 10)
	str	wzr, [x8, #AXI_IRQ]
	ldr	w0, [x8, #AXI_COUNTERS]
	expect	17, x0, 1

	// Release core 1, wait until it waits for its mailbox 0, then
	// write to that mailbox and wait for its answer.
	adr	x0, secondary
	mov	x1, #(SPIN_TABLE + 8)
	str	x0, [x1]
	sev
	adr	x1, ready
	mov32	x2, 1000000
3:	subs	x2, x2, #1
	b.eq	4f
	ldrb	w0, [x1]
	cbz	w0, 3b
	mov32	w0, 0x1234
	str	w0, [x8, #MAILBOX_SET0 + 0x10]
	adr	x1, got
	mov32	x2, 1000000
3:	subs	x2, x2, #1
	b.eq	4f
	ldr	w0, [x1]
	cbz	w0, 3b
4:	ldr	w0, [x1]
	expect	18, x0, 0x1234

	// Nothing is left pending.
	mrs	x0, isr_el1
	expect	19, x0, 0

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
	msr	cntp_ctl_el0, xzr
	str	wzr, [x8, #LOCAL_TIMER]
	mov32	w0, (1 << 31)
	str	w0, [x8, #LOCAL_FLAGS]
	str	wzr, [x8, #AXI_IRQ]
	str	wzr, [x8, #TIMER_ROUTING0]
	str	wzr, [x8, #MAILBOX_ROUTING0]
1:	wfi
	b	1b

// Core 1 starts here at EL2: it takes the IRQ of its mailbox 0.
secondary:
	movz	x8, #(LOCAL >> 16), lsl #16
	adr	x0, vectors1
	msr	vbar_el2, x0
	mov	w0, #1			// mailbox 0 IRQ control
	str	w0, [x8, #MAILBOX_ROUTING0 + 4]
	msr	daifclr, #2
	adr	x1, ready
	mov	w0, #1
	strb	w0, [x1]
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

ok:	.asciz	"irq ok\n"
failed:	.asciz	"failed check "
trap:	.asciz	"trap\n"
	.balign	4
ready:	.word	0
got:	.word	0

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
	mrs	x26, elr_el2
	mrs	x0, spsr_el2
	orr	x0, x0, #0xc0		// return with IRQ and FIQ masked
	msr	spsr_el2, x0
	eret

// Core 1's vectors: its IRQ answers core 0 with the mailbox's value; a
// synchronous exception answers 0xdead. Either way, core 1 stops.
	.balign	2048
vectors1:
	.skip	0x200
	mov	w0, #0xdead
	b	answer
	.balign	0x80
	ldr	w0, [x8, #MAILBOX_CLEAR0 + 0x10]
	str	w0, [x8, #MAILBOX_CLEAR0 + 0x10]
answer:	str	wzr, [x8, #MAILBOX_ROUTING0 + 4]
	adr	x1, got
	str	w0, [x1]
1:	wfi
	b	1b
