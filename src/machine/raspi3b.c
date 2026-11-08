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
#include "hw/char/pl011.h"
#include "hw/intc/bcm2836_local.h"
#include "util/macros.h"

/*
 * All cores reset at address 0, and the Cortex-A53 implements EL3. Core
 * n has MPIDR_EL1 0x80000000 + n: bit 31 is RES1, and the cores form one
 * cluster with affinity 0 (the reg properties of Linux's bcm2837.dtsi),
 * which shares one L2 memory system. Their Generic Timers count from
 * the core timer of the ARM local peripherals.
 */
static Arm64Config cortex_a53(unsigned core, Arm64Cluster *cluster,
                              SystemCounter *counter)
{
	return (Arm64Config){
		.reset_pc = RASPI3B_STUB_ADDRESS,
		.highest_el = 3,
		.model = &arm64_cortex_a53_bcm2837,
		.mpidr = 0x80000000u | core,
		.cluster = cluster,
		.counter = counter,
	};
}

/*
 * The clocks of the ARM local peripherals: the 19.2 MHz crystal, and the
 * APB clock at half the ARM clock, which is 1.2 GHz on the Raspberry
 * Pi 3 Model B (arm_freq in the config.txt documentation).
 */
static const Bcm2836LocalConfig bcm2837_local = {
	.crystal_hz = 19200000,
	.apb_hz = 600000000,
};

/*
 * The PL011 of the BCM2835 has 16-entry FIFOs (BCM2835 ARM Peripherals,
 * section 13). The datasheet does not document its identification
 * registers, so they read as 0; Linux takes the part number from the
 * device tree instead.
 */
static const Pl011Config bcm2835_pl011 = {.fifo_depth = 16, .periph_id = 0};

/*
 * The boot stub, assembled from raspi3b_stub.s: run `make stub` after
 * changing that file.
 */
static const uint32_t stub[] = {
#include "machine/raspi3b_stub.inc"
};

static_assert(sizeof(stub) == RASPI3B_STUB_SIZE, "the stub has the wrong size");

static void destroy_serial(void *ctx)
{
	char_stream_destroy(ctx);
}

static void reset_cluster(void *ctx)
{
	arm64_cluster_reset(ctx);
}

static void destroy_cluster(void *ctx)
{
	arm64_cluster_destroy(ctx);
}

static void reset_local(void *ctx)
{
	bcm2836_local_reset(ctx);
}

static void destroy_local(void *ctx)
{
	bcm2836_local_destroy(ctx);
}

static void reset_uart(void *ctx)
{
	pl011_reset(ctx);
}

static void destroy_uart(void *ctx)
{
	pl011_destroy(ctx);
}

static bool write_word(Machine *m, uint64_t address, unsigned size,
                       uint64_t value, char *error, size_t error_size)
{
	BusStatus status = bus_write(m->bus, address, size, value);

	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot write the boot stub: %s",
		         bus_status_str(status));
		return false;
	}
	return true;
}

/*
 * Do what the firmware does for kernel8.img: load it at 0x80000 and the
 * boot stub at 0, and tell the stub where the kernel and the device tree
 * are. The cores are left as they reset, at EL3 at the stub's first
 * instruction: running the stub moves them to EL2, and starts core 0 in
 * the kernel.
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

	for (unsigned i = 0; i < ARRAY_SIZE(stub); i++) {
		if (!write_word(m, RASPI3B_STUB_ADDRESS + 4 * i, 4, stub[i], error,
		                error_size))
			return false;
	}
	/*
	 * The magic number and the version are also the spin table entry of
	 * core 3, which must read 0 until the operating system releases it.
	 * There is no device tree yet: its address is 0.
	 */
	return write_word(m, RASPI3B_STUB_MAGIC, 8, 0, error, error_size) &&
	       write_word(m, RASPI3B_STUB_DTB, 4, 0, error, error_size) &&
	       write_word(m, RASPI3B_STUB_KERNEL, 4, RASPI3B_KERNEL_ADDRESS, error,
	                  error_size);
}

static Machine *raspi3b_create(const BoardOptions *options, char *error,
                               size_t error_size)
{
	Machine *m = machine_create();
	BusStatus status;
	Arm64Cluster *cluster;
	Bcm2836Local *local;
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

	/* The machine resets the cluster before the cores, and frees it. */
	cluster = arm64_cluster_create(&arm64_cortex_a53_bcm2837);
	if (cluster == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, cluster, reset_cluster, destroy_cluster)) {
		arm64_cluster_destroy(cluster);
		goto out_of_memory;
	}
	local = bcm2836_local_create(m, &bcm2837_local);
	if (local == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, local, reset_local, destroy_local)) {
		bcm2836_local_destroy(local);
		goto out_of_memory;
	}
	status = bcm2836_local_map(local, m->bus, "local", RASPI3B_LOCAL_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the ARM local peripherals: %s",
		         bus_status_str(status));
		goto fail;
	}
	for (unsigned i = 0; i < RASPI3B_NUM_CPUS; i++) {
		Arm64Config config =
			cortex_a53(i, cluster, bcm2836_local_counter(local));
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
