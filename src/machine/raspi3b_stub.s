// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Huy Luong Duc

// The boot stub of the raspi3b board: the first code that every core
// runs. Before it starts the cores, the Raspberry Pi firmware loads a
// stub like this one, armstub8, at address 0, where the cores start at
// EL3. This one is written for smirror from what armstub8.S does (see
// docs/boards/raspi3b.md). It executes the same instructions in the
// same order and with the same registers, so the cores reach the kernel
// with the same register values as on the real board, and it keeps
// armstub8's memory layout at 0xd8 to 0xff, which Linux and the
// firmware rely on.
//
// Each core sets up the registers that only EL3 can write and drops to
// EL2 with ERET. Core 0 then jumps to the kernel; the others wait in
// the spin table until the operating system releases them.
//
// smirror does not assemble this file when it is built: it loads the
// bytes kept in raspi3b_stub.inc. After changing this file, run
// `make stub` to update raspi3b_stub.inc; a test fails until then.

	// SCR_EL3 (Arm ARM D24.2.168): EL2 and EL1 are Non-secure (NS,
	// bit 0) and use AArch64 (RW, bit 10); HVC is enabled (HCE, bit 8)
	// and SMC is UNDEFINED (SMD, bit 7). Bits 5 and 4 are RES1.
	.equ	SCR_VALUE, 0x5b1

	// ACTLR_EL3 (A53 TRM 4.3.33): EL2 may write CPUACTLR_EL1 (bit 0),
	// CPUECTLR_EL1 (bit 1), L2CTLR_EL1 (bit 4), L2ECTLR_EL1 (bit 5) and
	// L2ACTLR_EL1 (bit 6).
	.equ	ACTLR_VALUE, 0x73

	// CPUECTLR_EL1 (A53 TRM 4.3.79): SMPEN, bit 6. The core takes part
	// in the coherency of the cluster's caches.
	.equ	CPUECTLR_SMPEN, 0x40

	// SCTLR_EL2 (Arm ARM D24.2.175): only the RES1 bits; the MMU, the
	// caches and the alignment checks are off, little-endian.
	.equ	SCTLR_EL2_VALUE, 0x30c50830

	// SPSR_EL3 (Arm ARM C5.2.23) for the ERET: EL2 with SP_EL2 (EL2h),
	// and the D, A, I and F interrupt masks set.
	.equ	SPSR_EL2H_DAIF, 0x3c9

	// The ARM local peripherals (BCM2836 ARM-local peripherals, rev
	// 3.4, sections 4.2 and 4.3): the control register, and the
	// prescaler of the 64-bit core timer, which the Generic Timers of
	// all four cores count from.
	.equ	LOCAL_CONTROL, 0x40000000
	.equ	LOCAL_PRESCALER, 0x40000008

	// The crystal, 19.2 MHz: the frequency of the core timer once the
	// prescaler divides by 1.
	.equ	CRYSTAL_HZ, 19200000

	.text
	.globl	_start
_start:
	// The core timer counts the crystal (control bit 8 clear) by 1
	// (bit 9 clear); a prescaler of 2^31 divides by 1. Until it is
	// written, the prescaler is 0 and the timer stopped.
	ldr	x0, =LOCAL_CONTROL
	str	wzr, [x0]
	mov	w1, #0x80000000
	str	w1, [x0, #(LOCAL_PRESCALER - LOCAL_CONTROL)]

	// L2CTLR_EL1 (A53 TRM 4.3.64): armstub8 sets bits 5 and 1 to ask
	// for slower L2 RAM. On the Cortex-A53 the latency fields are
	// fixed when the chip is made and ignore writes, so this changes
	// nothing; it is done because the real stub does it.
	// The A53's own registers go by their encodings: binutils for
	// generic AArch64 does not know their names.
	mrs	x0, s3_1_c11_c0_2	// L2CTLR_EL1
	mov	x1, #0x22
	orr	x0, x0, x1
	msr	s3_1_c11_c0_2, x0

	// CNTFRQ_EL0 (Arm ARM D24.10.1) tells software the frequency of the
	// system counter; only the highest Exception level can write it.
	// CNTVOFF_EL2 (D24.10.27): the virtual count is the physical one.
	ldr	x0, =CRYSTAL_HZ
	msr	cntfrq_el0, x0
	msr	cntvoff_el2, xzr

	// CPTR_EL3 (Arm ARM D24.2.38): trap nothing to EL3, in particular
	// not the floating-point and SIMD instructions.
	msr	cptr_el3, xzr

	mov	x0, #SCR_VALUE
	msr	scr_el3, x0
	mov	x0, #ACTLR_VALUE
	msr	actlr_el3, x0
	mov	x0, #CPUECTLR_SMPEN
	msr	s3_1_c15_c2_1, x0	// CPUECTLR_EL1
	mov	x0, #(SCTLR_EL2_VALUE & 0xffff)
	movk	x0, #(SCTLR_EL2_VALUE >> 16), lsl #16
	msr	sctlr_el2, x0

	// Return to entry_el2 at EL2.
	mov	x0, #SPSR_EL2H_DAIF
	msr	spsr_el3, x0
	adr	x0, entry_el2
	msr	elr_el3, x0
	eret

entry_el2:
	// MPIDR_EL1.Aff0 (Arm ARM D24.2.137) numbers the four cores of the
	// one cluster.
	mrs	x6, mpidr_el1
	and	x6, x6, #3
	cbz	x6, core0

	// Cores 1 to 3: wait until the 64-bit word at spin_table + 8 * n
	// holds an address. The operating system writes it, then executes
	// SEV to wake the core from WFE. The ERET above has set the Event
	// Register, so the first WFE falls through and the word is read
	// once before the core sleeps.
	adr	x5, spin_table
park:	wfe
	ldr	x4, [x5, x6, lsl #3]
	cbz	x4, park
	mov	x0, #0
	b	jump

core0:
	// The board writes these two words when it loads the kernel.
	ldr	w4, kernel_entry32
	ldr	w0, dtb_ptr32

	// Linux's boot protocol: X0 holds the device tree's address, or 0
	// for a secondary core; X1 to X3 are 0.
jump:	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	br	x4

	// The constants of the LDR instructions above.
	.ltorg

	// The data, at the offsets of armstub8. The assembler stops with
	// an error if the code above grows past 0xd8.
	.org	0xd8
spin_table:			// cpu-release-addr of each core in the
	.quad	0		// device tree: core 0 at 0xd8 (unused),
	.quad	0		// core 1 at 0xe0,
	.quad	0		// core 2 at 0xe8,
				// core 3 at 0xf0, where the firmware first
				// finds the magic number; it clears these 8
				// bytes after reading them.
	.org	0xf0
stub_magic:
	.word	0x5afe570b	// marks a stub with this layout
stub_version:
	.word	0
dtb_ptr32:			// the device tree's address
	.word	0
kernel_entry32:			// the kernel's address
	.word	0
	.org	0x100
