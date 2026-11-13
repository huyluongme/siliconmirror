// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// Read and write the SD card it was booted from through SDHOST, as
// U-Boot's bcm2835_sdhost.c does, by polling: power on, identify the card
// (CMD0, CMD8, ACMD41, CMD2, CMD3), select it (CMD7), read its MBR
// (CMD17), check the signature and the boot partition's type, then write
// sector 1, which nothing uses, and read it back. The card is an SDSC
// one: byte addresses. Prints "sd card ok", or the number of the first
// check that failed, on the mini UART.
//
// X9 holds the mini UART, X10 SDHOST, X19 the number of the check that
// runs, X20 the card's RCA.

	.equ	AUX, 0x3f215000
	.equ	AUX_MU_IO, 0x40
	.equ	AUX_MU_LSR, 0x54
	.equ	LSR_TX_EMPTY, 5
	.equ	SDHOST, 0x3f202000
	.equ	SDCMD, 0x00
	.equ	SDARG, 0x04
	.equ	SDTOUT, 0x08
	.equ	SDCDIV, 0x0c
	.equ	SDRSP0, 0x10
	.equ	SDHSTS, 0x20
	.equ	SDVDD, 0x30
	.equ	SDEDM, 0x34
	.equ	SDHCFG, 0x38
	.equ	SDHBCT, 0x3c
	.equ	SDDATA, 0x40
	.equ	SDHBLC, 0x50
	.equ	CMD_NEW, 15		// bits of SDCMD
	.equ	CMD_FAIL, 14
	.equ	BUSYWAIT, 0x800
	.equ	NO_RESPONSE, 0x400
	.equ	LONG, 0x200
	.equ	WRITE, 0x80
	.equ	READ, 0x40
	.equ	BLOCK_IRPT, 9		// bits of SDHSTS
	.equ	BUSY_IRPT, 10
	.equ	SLOW_CARD, 0x8
	.equ	IDENT_CDIV, 623		// 250 MHz / 625 = 400 kHz
	.equ	DATA_CDIV, 3		// 50 MHz
	.equ	OCR_READY, 31
	.equ	NEWLINE, 0x0a
	.equ	DIGIT_0, 0x30

	.globl	_start
_start:
	movz	x9, #(AUX >> 16), lsl #16
	movk	x9, #(AUX & 0xffff)
	movz	x10, #(SDHOST >> 16), lsl #16
	movk	x10, #(SDHOST & 0xffff)

	// Power on, slow for the identification.
	mov	w0, #1
	str	w0, [x10, #SDVDD]
	mov	w0, #SLOW_CARD
	str	w0, [x10, #SDHCFG]
	mov	w0, #IDENT_CDIV
	str	w0, [x10, #SDCDIV]
	movz	w0, #0xf0, lsl #16
	str	w0, [x10, #SDTOUT]

	// 1: CMD0, then CMD8 echoes its argument.
	mov	x19, #1
	mov	w0, #(0 | NO_RESPONSE)
	mov	w1, #0
	bl	command
	mov	w0, #8
	mov	w1, #0x1aa
	bl	command
	ldr	w0, [x10, #SDRSP0]
	cmp	w0, #0x1aa
	b.ne	fail

	// 2: ACMD41 until the card is ready.
	mov	x19, #2
	mov	x21, #10000
1:	mov	w0, #55
	mov	w1, #0
	bl	command
	mov	w0, #41
	movz	w1, #0x40ff, lsl #16
	movk	w1, #0x8000
	bl	command
	ldr	w0, [x10, #SDRSP0]
	tbnz	w0, #OCR_READY, 2f
	subs	x21, x21, #1
	b.ne	1b
	b	fail

	// 3: CMD2 sends the CID, CMD3 publishes an RCA.
2:	mov	x19, #3
	mov	w0, #(2 | LONG)
	mov	w1, #0
	bl	command
	mov	w0, #3
	mov	w1, #0
	bl	command
	ldr	w0, [x10, #SDRSP0]
	lsr	w20, w0, #16
	cbz	w20, fail

	// 4: CMD7 selects the card; its busy end raises BUSY_IRPT.
	mov	x19, #4
	mov	w0, #(7 | BUSYWAIT)
	lsl	w1, w20, #16
	bl	command
	ldr	w0, [x10, #SDHSTS]
	tbz	w0, #BUSY_IRPT, fail
	mov	w0, #(1 << BUSY_IRPT)
	str	w0, [x10, #SDHSTS]
	mov	w0, #DATA_CDIV
	str	w0, [x10, #SDCDIV]
	mov	w0, #512
	str	w0, [x10, #SDHBCT]
	mov	w0, #1
	str	w0, [x10, #SDHBLC]

	// 5: sector 0, the MBR: 0x55 0xaa, and a FAT32 first partition.
	mov	x19, #5
	mov	x1, #0
	adr	x2, sector
	bl	read
	adr	x2, sector
	ldrh	w0, [x2, #510]
	movz	w1, #0xaa55
	cmp	w0, w1
	b.ne	fail
	ldrb	w0, [x2, #450]
	cmp	w0, #0x0c
	b.ne	fail

	// 6: write sector 1, and read it back.
	mov	x19, #6
	adr	x2, sector
	mov	x3, #0
3:	add	w0, w3, w3, lsl #8	// a word of each index
	eor	w0, w0, #0x0f0f0f0f
	str	w0, [x2, x3, lsl #2]
	add	x3, x3, #1
	cmp	x3, #128
	b.ne	3b
	mov	w0, #(24 | WRITE)
	mov	w1, #512
	bl	command
	adr	x2, sector
	mov	x3, #0
4:	ldr	w0, [x10, #SDEDM]
	ubfx	w0, w0, #4, #5		// words in the FIFO
	cmp	w0, #16
	b.eq	4b
	ldr	w0, [x2, x3, lsl #2]
	str	w0, [x10, #SDDATA]
	add	x3, x3, #1
	cmp	x3, #128
	b.ne	4b
5:	ldr	w0, [x10, #SDHSTS]
	tbz	w0, #BLOCK_IRPT, 5b
	mov	w0, #(1 << BLOCK_IRPT)
	str	w0, [x10, #SDHSTS]
	mov	x1, #512
	adr	x2, back
	bl	read
	adr	x2, sector
	adr	x4, back
	mov	x3, #0
6:	ldr	w0, [x2, x3, lsl #2]
	ldr	w1, [x4, x3, lsl #2]
	cmp	w0, w1
	b.ne	fail
	add	x3, x3, #1
	cmp	x3, #128
	b.ne	6b

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

// Send command W0 with argument W1 and wait until it is done; a failed
// command fails the check. Uses W0, W1.
command:
	str	w1, [x10, #SDARG]
	orr	w0, w0, #(1 << CMD_NEW)
	str	w0, [x10, #SDCMD]
1:	ldr	w0, [x10, #SDCMD]
	tbnz	w0, #CMD_NEW, 1b
	tbnz	w0, #CMD_FAIL, fail
	ret

// Read the block at byte address X1 to X2: CMD17, 128 words from the
// FIFO as they come, then the block's end. Uses X0, X1, X2, X3, X5.
read:	mov	x5, x30
	mov	w0, #(17 | READ)
	bl	command
	mov	x3, #0
1:	ldr	w0, [x10, #SDEDM]
	ubfx	w0, w0, #4, #5
	cbz	w0, 1b
	ldr	w0, [x10, #SDDATA]
	str	w0, [x2, x3, lsl #2]
	add	x3, x3, #1
	cmp	x3, #128
	b.ne	1b
2:	ldr	w0, [x10, #SDHSTS]	// the block's end
	tbz	w0, #BLOCK_IRPT, 2b
	mov	w0, #(1 << BLOCK_IRPT)
	str	w0, [x10, #SDHSTS]
	ret	x5

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

ok:	.asciz	"sd card ok\n"
failed:	.asciz	"failed check "

	.balign	4
sector:	.space	512
back:	.space	512
