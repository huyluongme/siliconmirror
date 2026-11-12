// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Check the MMU as a kernel8.img sees it on the raspi3b, at EL2 after
// the boot stub. Like U-Boot, turn on stage 1 translation at EL2 with an
// identity map: RAM as Normal memory, the peripherals as Device memory,
// in 2MB blocks. Then read RAM through an alias, translate an address
// with AT, zero a block with DC ZVA and take a Translation fault. Then go
// to EL1, where TTBR1_EL1 maps the upper half of the address space, as a
// Linux kernel has it, read RAM there and take a fault there too.
// Prints "mmu ok", or the number of the first check that failed.
//
// The exception handlers keep ESR_ELx in X24 and FAR_ELx in X25, and
// skip the instruction that faulted. X9 holds UART0, X19 the number of
// the check that runs.

	.equ	UART0, 0x3f201000	// PL011
	.equ	UARTDR, 0x00		// data
	.equ	UARTFR, 0x18		// flags
	.equ	UARTIBRD, 0x24		// integer baud rate divisor
	.equ	UARTFBRD, 0x28		// fractional baud rate divisor
	.equ	UARTLCR_H, 0x2c		// line control
	.equ	UARTCR, 0x30		// control
	.equ	FR_TXFF, 5		// transmit FIFO full
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30		// '0'

	// Translation tables, 4KB each, with the 4KB granule and 39-bit
	// virtual addresses, which start at level 1 (D8.2).
	.equ	L1, 0x100000		// level 1 for TTBR0_ELx
	.equ	L2, 0x101000		// level 2 for the first 1GB
	.equ	L1_HIGH, 0x102000	// level 1 for TTBR1_EL1
	.equ	BUFFER, 0x200000
	.equ	ALIAS, 0x80000000	// L1[2]: the first 1GB again
	.equ	HOLE, 0xc0000000	// L1[3]: nothing
	.equ	HIGH, 0xffffff8000000000 // TTBR1_EL1 L1[0]: the first 1GB
	.equ	HIGH_HOLE, 0xffffff8040000000 // TTBR1_EL1 L1[1]: nothing

	// Descriptors: a block with AF set, Inner Shareable, and AttrIndx 0
	// (Normal) or 1 (Device, also execute-never), D8.3.
	.equ	NORMAL, 0x701
	.equ	DEVICE, 0x405 | (1 << 54)
	.equ	TABLE, 0x3
	.equ	DEVICE_BLOCKS, 504	// from 0x3f000000, the peripherals

	// MAIR: attribute 0 Normal Write-Back, attribute 1 Device-nGnRnE.
	.equ	MAIR, 0x00ff
	// TCR_EL2: T0SZ 25, walks Inner Shareable Write-Back, PS 40 bits,
	// and the RES1 bits 31 and 23.
	.equ	TCR_EL2, 0x80823519
	// TCR_EL1: T0SZ and T1SZ 25, TG1 4KB, IPS 40 bits.
	.equ	TCR_EL1, 0x280193519
	.equ	SCTLR_MCI, 0x1005	// M, C and I
	.equ	SPSR_EL1H, 0x3c5	// EL1 with SP_EL1, DAIF masked

	.globl	_start
_start:
	movz	x9, #(UART0 >> 16), lsl #16
	movk	x9, #(UART0 & 0xffff)
	str	wzr, [x9, #UARTCR]	// disable the UART while setting it up
	mov	w0, #26			// 115200 baud from the 48 MHz UARTCLK:
	str	w0, [x9, #UARTIBRD]	// a divisor of 26 + 3/64
	mov	w0, #3
	str	w0, [x9, #UARTFBRD]
	mov	w0, #0x70		// 8-bit characters, FIFOs on
	str	w0, [x9, #UARTLCR_H]
	mov	w0, #0x101		// enable the UART and its transmitter
	str	w0, [x9, #UARTCR]
	adr	x0, vectors2
	msr	vbar_el2, x0

	// The tables, cleared first: with the MMU off, memory is Device
	// memory, where DC ZVA faults.
	ldr	x1, =L1
	mov	x2, #(3 * 4096 / 16)
1:	stp	xzr, xzr, [x1], #16
	subs	x2, x2, #1
	b.ne	1b
	ldr	x1, =L1
	ldr	x0, =(L2 | TABLE)
	str	x0, [x1]		// L1[0]: the level 2 table
	ldr	x0, =(0x40000000 | DEVICE)
	str	x0, [x1, #8]		// L1[1]: the ARM local peripherals
	ldr	x0, =NORMAL
	str	x0, [x1, #16]		// L1[2]: ALIAS
	ldr	x1, =L1_HIGH
	str	x0, [x1]		// TTBR1_EL1 L1[0]: HIGH
	ldr	x1, =L2
	mov	x2, #0			// the physical address
	mov	x3, #0			// the index
	ldr	x4, =NORMAL
	ldr	x5, =DEVICE
	mov	x6, #DEVICE_BLOCKS
2:	cmp	x3, x6
	csel	x7, x4, x5, lo
	orr	x7, x7, x2
	str	x7, [x1, x3, lsl #3]
	add	x2, x2, #0x200000
	add	x3, x3, #1
	cmp	x3, #512
	b.lo	2b

	// Turn on the MMU at EL2.
	ldr	x0, =MAIR
	msr	mair_el2, x0
	ldr	x0, =TCR_EL2
	msr	tcr_el2, x0
	ldr	x0, =L1
	msr	ttbr0_el2, x0
	isb
	tlbi	alle2
	dsb	sy
	mrs	x0, sctlr_el2
	ldr	x1, =SCTLR_MCI
	orr	x0, x0, x1
	msr	sctlr_el2, x0
	isb

	// 1: RAM through the alias.
	mov	x19, #1
	ldr	x1, =(BUFFER + 0x100)
	ldr	x0, =0x1234
	str	x0, [x1]
	dc	cvac, x1
	ldr	x2, =(ALIAS + BUFFER + 0x100)
	ldr	x3, [x2]
	cmp	x3, x0
	b.ne	fail

	// 2: AT S1E2R gives the physical address and attributes.
	mov	x19, #2
	at	s1e2r, x2
	isb
	mrs	x0, par_el1
	ldr	x1, =0xff00000000200b80	// Normal, Inner Shareable, NS
	cmp	x0, x1
	b.ne	fail

	// 3: DC ZVA zeroes 64 bytes.
	mov	x19, #3
	ldr	x1, =BUFFER
	mov	x0, #-1
	stp	x0, x0, [x1]
	stp	x0, x0, [x1, #48]
	str	x0, [x1, #64]
	add	x2, x1, #8
	dc	zva, x2
	ldp	x2, x3, [x1]
	orr	x2, x2, x3
	ldr	x3, [x1, #56]
	orr	x2, x2, x3
	cbnz	x2, fail
	ldr	x3, [x1, #64]
	cmp	x3, x0
	b.ne	fail

	// 4: a Translation fault, level 1, at EL2.
	mov	x19, #4
	ldr	x1, =HOLE
	mov	x24, #0
	ldr	x0, [x1]
	ldr	x0, =0x96000005		// Data Abort, same level
	cmp	x24, x0
	b.ne	fail
	cmp	x25, x1
	b.ne	fail

	// Go to EL1, with TTBR1_EL1 for the upper addresses.
	ldr	x0, =MAIR
	msr	mair_el1, x0
	ldr	x0, =TCR_EL1
	msr	tcr_el1, x0
	ldr	x0, =L1
	msr	ttbr0_el1, x0
	ldr	x0, =L1_HIGH
	msr	ttbr1_el1, x0
	mrs	x0, sctlr_el1
	ldr	x1, =SCTLR_MCI
	orr	x0, x0, x1
	msr	sctlr_el1, x0
	adr	x0, vectors1
	msr	vbar_el1, x0
	mov	x0, #(1 << 31)		// HCR_EL2.RW: EL1 uses AArch64
	msr	hcr_el2, x0
	mov	x0, #SPSR_EL1H
	msr	spsr_el2, x0
	adr	x0, el1
	msr	elr_el2, x0
	tlbi	vmalle1
	dsb	sy
	eret

el1:	// 5: RAM through TTBR1_EL1.
	mov	x19, #5
	ldr	x1, =(HIGH + BUFFER + 0x100)
	ldr	x0, [x1]
	ldr	x2, =0x1234
	cmp	x0, x2
	b.ne	fail

	// 6: a Translation fault, level 1, at EL1.
	mov	x19, #6
	ldr	x1, =HIGH_HOLE
	mov	x24, #0
	str	x0, [x1]
	ldr	x0, =0x96000045		// Data Abort, same level, a write
	cmp	x24, x0
	b.ne	fail
	cmp	x25, x1
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
putc:	ldr	w3, [x9, #UARTFR]
	tbnz	w3, #FR_TXFF, putc
	str	w0, [x9, #UARTDR]
	ret

ok:	.asciz	"mmu ok\n"
failed:	.asciz	"failed check "

	.ltorg

// The vectors at EL2 and EL1, for the current level with SP_ELx: keep
// ESR and FAR, and skip the instruction.
	.balign	2048
vectors2:
	.skip	0x200
	mrs	x24, esr_el2
	mrs	x25, far_el2
	mrs	x26, elr_el2
	add	x26, x26, #4
	msr	elr_el2, x26
	eret

	.balign	2048
vectors1:
	.skip	0x200
	mrs	x24, esr_el1
	mrs	x25, far_el1
	mrs	x26, elr_el1
	add	x26, x26, #4
	msr	elr_el1, x26
	eret
