/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Raspberry Pi 3 Model B: a BCM2837 with four Cortex-A53 cores, 1GB of
 * RAM, and the peripherals of the BCM2835 at a new address. The memory
 * map and the boot are described in docs/boards/raspi3b.md.
 *
 * Devices so far: UART0, a PL011. Its interrupt is not connected, as the
 * interrupt controllers are not modelled yet.
 */

#include "machine/raspi3b.h"

#include <assert.h>
#include <stdio.h>

#include "boot/direct.h"
#include "cpu/arm64/cpu.h"
#include "cpu/arm64/sysreg.h"
#include "hw/char/pl011.h"
#include "util/macros.h"

/*
 * All cores reset at address 0, and the Cortex-A53 implements EL3. Core
 * n has MPIDR_EL1 0x80000000 + n: bit 31 is RES1, and the cores form one
 * cluster with affinity 0 (the reg properties of Linux's bcm2837.dtsi).
 */
static Arm64Config cortex_a53(unsigned core)
{
	return (Arm64Config){
		.reset_pc = 0,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
		.mpidr = 0x80000000u | core,
	};
}

/*
 * The PL011 of the BCM2835 has 16-entry FIFOs (BCM2835 ARM Peripherals,
 * section 13). The datasheet does not document its identification
 * registers, so they read as 0; Linux takes the part number from the
 * device tree instead.
 */
static const Pl011Config bcm2835_pl011 = {.fifo_depth = 16, .periph_id = 0};

/*
 * Where the secondary cores wait until the OS writes an address to their
 * spin table entry, which X5 points to, and sends an event. They then
 * jump to it with X0 to X3 clear, as Linux's spin-table method expects.
 */
static const uint32_t spin_loop[] = {
	0xf94000a4, /* spin: LDR  X4, [X5] */
	0xb5000064, /*       CBNZ X4, go */
	0xd503205f, /*       WFE */
	0x17fffffd, /*       B    spin */
	0xd2800000, /* go:   MOV  X0, #0 */
	0xd61f0080, /*       BR   X4 */
};

static void destroy_serial(void *ctx)
{
	char_stream_destroy(ctx);
}

static void reset_uart(void *ctx)
{
	pl011_reset(ctx);
}

static void destroy_uart(void *ctx)
{
	pl011_destroy(ctx);
}

/*
 * Leave a core as the firmware's stub leaves it after switching from EL3
 * to EL2: at EL2, using SP_EL2, with D, A, I and F masked, and with the
 * System registers that the stub sets up. The values are the ones that
 * armstub8.S writes for the Raspberry Pi 3; the Generic Timer ones come
 * with the timer.
 */
static void enter_el2(Arm64Cpu *c, uint64_t pc)
{
	static const struct {
		uint16_t encoding;
		uint64_t value;
	} stub_state[] = {
		{ARM64_CPTR_EL3, 0},
		/* RW, HCE, SMD, RES1 bits 5 and 4, NS: lower ELs Non-secure */
		{ARM64_SCR_EL3, 0x5b1},
		/* EL2 may write the A53's CPUACTLR, CPUECTLR and L2 registers */
		{ARM64_ACTLR_EL3, 0x73},
		{ARM64_SYSREG(3, 1, 15, 2, 1), 0x40}, /* CPUECTLR_EL1.SMPEN */
		{ARM64_SCTLR_EL2, 0x30c50830},
		{ARM64_SPSR_EL3, 0x3c9}, /* the ERET into EL2, D, A, I, F masked */
	};

	for (unsigned i = 0; i < ARRAY_SIZE(stub_state); i++) {
		bool ok = arm64_cpu_write_sysreg(c, stub_state[i].encoding,
		                                 stub_state[i].value);

		assert(ok);
		(void)ok;
	}
	c->el = 2;
	c->sp_sel = true;
	c->daif = ARM64_DAIF_D | ARM64_DAIF_A | ARM64_DAIF_I | ARM64_DAIF_F;
	c->pc = pc;
}

/*
 * Do what the firmware does for kernel8.img: load it, start core 0 in it
 * and park the other cores in the spin loop.
 */
static bool boot(Machine *m, const char *kernel, char *error, size_t error_size)
{
	if (kernel == NULL) {
		snprintf(error, error_size,
		         "-kernel is required: booting from an SD card image is not "
		         "implemented yet");
		return false;
	}
	if (!boot_direct_load(m->bus, kernel, RASPI3B_KERNEL_ADDRESS,
	                      RASPI3B_RAM_SIZE, error, error_size))
		return false;

	for (unsigned i = 0; i < ARRAY_SIZE(spin_loop); i++) {
		BusStatus status =
			bus_write(m->bus, RASPI3B_SPIN_LOOP + 4 * i, 4, spin_loop[i]);

		if (status != BUS_OK) {
			snprintf(error, error_size, "cannot write the spin loop: %s",
			         bus_status_str(status));
			return false;
		}
	}

	for (unsigned i = 0; i < m->num_cpus; i++) {
		Arm64Cpu *c = arm64_cpu_of(m->cpus[i]);

		if (i == 0) {
			enter_el2(c, RASPI3B_KERNEL_ADDRESS);
			c->x[0] = 0; /* no device tree yet */
		} else {
			enter_el2(c, RASPI3B_SPIN_LOOP);
			c->x[5] = RASPI3B_SPIN_TABLE + 8 * i;
		}
	}
	return true;
}

static Machine *raspi3b_create(const BoardOptions *options, char *error,
                               size_t error_size)
{
	Machine *m = machine_create();
	BusStatus status;
	Pl011 *uart;

	if (m == NULL) {
		char_stream_destroy(options->serial);
		goto out_of_memory;
	}
	if (!machine_add_device(m, options->serial, NULL, destroy_serial)) {
		char_stream_destroy(options->serial);
		goto out_of_memory;
	}

	status = bus_map_ram(m->bus, "sdram", 0, RASPI3B_RAM_SIZE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot allocate the RAM: %s",
		         bus_status_str(status));
		goto fail;
	}

	for (unsigned i = 0; i < RASPI3B_NUM_CPUS; i++) {
		Arm64Config config = cortex_a53(i);
		Cpu *cpu = arm64_cpu_create(&config);

		if (cpu == NULL)
			goto out_of_memory;
		if (!machine_add_cpu(m, cpu)) {
			cpu->ops->destroy(cpu);
			goto out_of_memory;
		}
	}

	uart = pl011_create(&bcm2835_pl011, options->serial, NULL);
	if (uart == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, uart, reset_uart, destroy_uart)) {
		pl011_destroy(uart);
		goto out_of_memory;
	}
	status = pl011_map(uart, m->bus, "uart0", RASPI3B_UART0_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map UART0: %s",
		         bus_status_str(status));
		goto fail;
	}

	if (!boot(m, options->kernel, error, error_size))
		goto fail;
	return m;

out_of_memory:
	snprintf(error, error_size, "out of memory");
fail:
	machine_destroy(m);
	return NULL;
}

const Board raspi3b_board = {
	.name = "raspi3b",
	.description = "Raspberry Pi 3 Model B (BCM2837, 4 Cortex-A53, 1GB RAM)",
	.create = raspi3b_create,
};
