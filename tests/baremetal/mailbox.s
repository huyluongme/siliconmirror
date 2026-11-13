// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Ask the firmware about the board through the property channel of the
// mailboxes, as U-Boot and Linux do: a buffer of tags in memory, its bus
// address on channel 8 of mailbox 1, then the answer in mailbox 0. Wait
// for the answer with WFI, woken by the mailbox interrupt, which reaches
// core 0 through the BCM2835 interrupt controller and the ARM local
// peripherals; the interrupt stays masked in PSTATE. Prints "mailbox ok",
// or the number of the first check that failed, on the mini UART that
// the firmware has set up.
//
// X9 holds the mini UART, X10 the mailboxes, X19 the number of the check
// that runs, X20 the message sent.

	.equ	AUX, 0x3f215000
	.equ	AUX_MU_IO, 0x40		// data
	.equ	AUX_MU_LSR, 0x54	// line status
	.equ	LSR_TX_EMPTY, 5		// room in the transmit FIFO
	.equ	MBOX, 0x3f00b880
	.equ	MAIL0_READ, 0x00
	.equ	MAIL0_STATUS, 0x18
	.equ	MAIL0_CONFIG, 0x1c
	.equ	MAIL1_WRITE, 0x20
	.equ	MAIL1_STATUS, 0x38
	.equ	STATUS_FULL, 31
	.equ	STATUS_EMPTY, 30
	.equ	CONFIG_DATA_IRQ, 1	// interrupt while mailbox 0 has data
	.equ	PROPERTY, 8		// the property channel, Arm to VideoCore
	.equ	UNCACHED, 0xc0000000	// the bus alias of uncached memory
	.equ	IC, 0x3f00b200		// BCM2835 interrupt controller
	.equ	BASIC_PENDING, 0x00
	.equ	ENABLE_BASIC, 0x18
	.equ	BASIC_MAILBOX, 1	// bit of the ARM mailbox interrupt
	.equ	LOCAL, 0x40000000	// ARM local peripherals
	.equ	IRQ_SOURCE0, 0x60
	.equ	SOURCE_GPU, 8		// bit of the GPU interrupt
	.equ	RESPONSE, 0x8000	// bits 31:16 of a response code
	.equ	REVISION_LO, 0x2082	// board revision 0xa02082
	.equ	REVISION_HI, 0xa0
	.equ	ARM_MEMORY_HI, 0x3b40	// 0x3b400000: 1024 - 76 MiB
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	.globl	_start
_start:
	movz	x9, #(AUX >> 16), lsl #16
	movk	x9, #(AUX & 0xffff)
	movz	x10, #(MBOX >> 16), lsl #16
	movk	x10, #(MBOX & 0xffff)

	// 1: mailbox 0 starts empty, mailbox 1 has room.
	mov	x19, #1
	ldr	w0, [x10, #MAIL0_STATUS]
	tbz	w0, #STATUS_EMPTY, fail
	ldr	w0, [x10, #MAIL1_STATUS]
	tbnz	w0, #STATUS_FULL, fail

	// Enable the interrupt: in the mailbox, and in the controller.
	mov	w0, #CONFIG_DATA_IRQ
	str	w0, [x10, #MAIL0_CONFIG]
	movz	x1, #(IC >> 16), lsl #16
	movk	x1, #(IC & 0xffff)
	mov	w0, #(1 << BASIC_MAILBOX)
	str	w0, [x1, #ENABLE_BASIC]

	// 2: send the buffer; the answer takes time to come.
	mov	x19, #2
	adr	x20, buffer
	orr	x20, x20, #UNCACHED
	orr	x20, x20, #PROPERTY
	str	w20, [x10, #MAIL1_WRITE]
	ldr	w0, [x10, #MAIL0_STATUS]
	tbz	w0, #STATUS_EMPTY, fail

	// 3: the interrupt wakes the core: from the mailbox, through the GPU
	// interrupt of core 0.
	mov	x19, #3
1:	wfi
	ldr	w0, [x10, #MAIL0_STATUS]
	tbnz	w0, #STATUS_EMPTY, 1b
	movz	x1, #(LOCAL >> 16), lsl #16
	ldr	w0, [x1, #IRQ_SOURCE0]
	tbz	w0, #SOURCE_GPU, fail
	movz	x1, #(IC >> 16), lsl #16
	movk	x1, #(IC & 0xffff)
	ldr	w0, [x1, #BASIC_PENDING]
	tbz	w0, #BASIC_MAILBOX, fail

	// 4: the answer is the message, and reading it lowers the interrupt.
	mov	x19, #4
	ldr	w0, [x10, #MAIL0_READ]
	cmp	w0, w20
	b.ne	fail
	ldr	w0, [x10, #MAIL0_STATUS]
	tbz	w0, #STATUS_EMPTY, fail
	ldr	w0, [x1, #BASIC_PENDING]
	tbnz	w0, #BASIC_MAILBOX, fail

	// 5: the buffer's code says every tag was answered.
	mov	x19, #5
	adr	x2, buffer
	ldr	w0, [x2, #4]
	movz	w1, #RESPONSE, lsl #16
	cmp	w0, w1
	b.ne	fail

	// 6: the board revision, with its response code.
	mov	x19, #6
	ldr	w0, [x2, #16]
	movz	w1, #RESPONSE, lsl #16
	orr	w1, w1, #4		// 4 bytes long
	cmp	w0, w1
	b.ne	fail
	ldr	w0, [x2, #20]
	movz	w1, #REVISION_LO
	movk	w1, #REVISION_HI, lsl #16
	cmp	w0, w1
	b.ne	fail

	// 7: the Arm's memory: from 0, up to the GPU's 76 MiB.
	mov	x19, #7
	ldr	w0, [x2, #36]
	cbnz	w0, fail
	ldr	w0, [x2, #40]
	movz	w1, #ARM_MEMORY_HI, lsl #16
	cmp	w0, w1
	b.ne	fail

	// 8: the MAC address starts with the Raspberry Pi's B8:27:EB.
	mov	x19, #8
	ldrb	w0, [x2, #56]
	cmp	w0, #0xb8
	b.ne	fail
	ldrb	w0, [x2, #57]
	cmp	w0, #0x27
	b.ne	fail
	ldrb	w0, [x2, #58]
	cmp	w0, #0xeb
	b.ne	fail

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

ok:	.asciz	"mailbox ok\n"
failed:	.asciz	"failed check "

// The property buffer: 16-byte aligned, as only bits 31:4 of its address
// fit in a message.
	.balign	16
buffer:	.word	buffer_end - buffer	// size
	.word	0			// a request
	.word	0x00010002, 4, 0, 0	// board revision, at 8
	.word	0x00010005, 8, 0, 0, 0	// ARM memory, at 24
	.word	0x00010003, 8, 0, 0, 0	// MAC address, at 44
	.word	0			// the end tag
buffer_end:
