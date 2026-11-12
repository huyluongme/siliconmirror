/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Raspberry Pi 3 Model B: a BCM2837 with four Cortex-A53 cores, 1GB of
 * RAM, and the peripherals of the BCM2835 at a new address. The memory
 * map and the boot are described in docs/boards/raspi3b.md.
 *
 * Devices so far: the ARM local peripherals, which give the cores their
 * system counter and route their interrupts; the GPU's interrupt
 * controller, whose IRQ and FIQ go to the ARM local peripherals; and
 * UART0, a PL011, whose interrupt goes to the GPU's interrupt controller.
 */

#include "machine/raspi3b.h"

#include <assert.h>
#include <stdio.h>

#include "boot/direct.h"
#include "cpu/arm64/cpu.h"
#include "hw/char/bcm2835_aux.h"
#include "hw/char/pl011.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "hw/intc/bcm2835_ic.h"
#include "hw/intc/bcm2836_local.h"
#include "machine/raspi3b_serial.h"
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
 * UARTCLK, which the firmware sets to init_uart_clock of config.txt,
 * 48 MHz unless config.txt says otherwise.
 */
#define UART_CLOCK_HZ 48000000

/*
 * The core (VPU) clock, the system clock of the mini UART: with
 * enable_uart=1, the firmware keeps it at 250 MHz (Raspberry Pi
 * documentation, "Configure UARTs").
 */
#define CORE_CLOCK_HZ 250000000

/* What the firmware writes to set up the console; see firmware_uart(). */
#define GPFSEL1 (RASPI3B_GPIO_BASE + 0x04)
#define AUX_ENABLES (RASPI3B_AUX_BASE + 0x04)
#define AUX_MU_LCR_REG (RASPI3B_AUX_BASE + 0x4c)
#define AUX_MU_BAUD_REG (RASPI3B_AUX_BASE + 0x68)

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

/*
 * Wire a core to the ARM local peripherals: the interrupt outputs of its
 * Generic Timer to the block's inputs for the core, and the block's IRQ
 * and FIQ for the core to its nIRQ and nFIQ. The block's document gives
 * no way to drive nVIRQ and nVFIQ, which stay unconnected, and the model
 * has no performance monitors to connect.
 */
static void connect_core(Bcm2836Local *local, Arm64Cpu *core, unsigned i)
{
	static const struct {
		Arm64TimerId timer;
		Bcm2836LocalInput input;
	} timers[] = {
		{ARM64_TIMER_SECURE, BCM2836_LOCAL_CNTPS},
		{ARM64_TIMER_PHYS, BCM2836_LOCAL_CNTPNS},
		{ARM64_TIMER_HYP, BCM2836_LOCAL_CNTHP},
		{ARM64_TIMER_VIRT, BCM2836_LOCAL_CNTV},
	};

	for (unsigned t = 0; t < ARRAY_SIZE(timers); t++)
		arm64_cpu_connect_timer(core, timers[t].timer,
		                        bcm2836_local_input(local, i, timers[t].input));
	bcm2836_local_connect(local, i, arm64_cpu_input(core, ARM64_INPUT_IRQ),
	                      arm64_cpu_input(core, ARM64_INPUT_FIQ));
}

static void reset_local(void *ctx)
{
	bcm2836_local_reset(ctx);
}

static void destroy_local(void *ctx)
{
	bcm2836_local_destroy(ctx);
}

static void reset_ic(void *ctx)
{
	bcm2835_ic_reset(ctx);
}

static void destroy_ic(void *ctx)
{
	bcm2835_ic_destroy(ctx);
}

static void reset_uart(void *ctx)
{
	pl011_reset(ctx);
}

static void destroy_uart(void *ctx)
{
	pl011_destroy(ctx);
}

static void reset_gpio(void *ctx)
{
	bcm2835_gpio_reset(ctx);
}

static void destroy_gpio(void *ctx)
{
	bcm2835_gpio_destroy(ctx);
}

static void destroy_cable(void *ctx)
{
	raspi3b_serial_destroy(ctx);
}

static void reset_aux(void *ctx)
{
	bcm2835_aux_reset(ctx);
}

static void destroy_aux(void *ctx)
{
	bcm2835_aux_destroy(ctx);
}

/*
 * Set up the console as the firmware does with enable_uart=1, which a
 * serial console on the Raspberry Pi 3 needs (Raspberry Pi documentation,
 * "Configure UARTs"): the mini UART on GPIO 14 and 15, their ALT5, which
 * U-Boot checks and keeps (drivers/serial/serial_bcm283x_mu.c), enabled,
 * with 8-bit characters at 115200 baud: 250 MHz / (8 * (270 + 1)) is
 * 115313 baud. The firmware's own register writes are not documented.
 * Returns the status of the first write that fails.
 */
static BusStatus firmware_uart(Machine *m)
{
	static const struct {
		uint64_t address;
		uint32_t value;
	} writes[] = {
		{GPFSEL1, BCM2835_GPIO_ALT5 << 12 | BCM2835_GPIO_ALT5 << 15},
		{AUX_ENABLES, 1},
		{AUX_MU_LCR_REG, 3},
		{AUX_MU_BAUD_REG, 270},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(writes); i++) {
		BusStatus status =
			bus_write(m->bus, writes[i].address, 4, writes[i].value);

		if (status != BUS_OK)
			return status;
	}
	return BUS_OK;
}

/* A reset of the board runs the firmware again. */
static void reset_firmware(void *ctx)
{
	firmware_uart(ctx);
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
	Bcm2835Ic *ic;
	IrqLine *gpio_irqs[BCM2835_GPIO_NUM_IRQS];
	Bcm2835Gpio *gpio;
	Raspi3bSerial *cable;
	Pl011 *uart;
	Bcm2835Aux *aux;

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
		connect_core(local, arm64_cpu_of(cpu), i);
	}

	/* The GPU's interrupt controller goes to the ARM local peripherals. */
	ic = bcm2835_ic_create(m);
	if (ic == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, ic, reset_ic, destroy_ic)) {
		bcm2835_ic_destroy(ic);
		goto out_of_memory;
	}
	status = bcm2835_ic_map(ic, m->bus, "intc", RASPI3B_IC_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the interrupt controller: %s",
		         bus_status_str(status));
		goto fail;
	}
	bcm2835_ic_connect(ic, bcm2836_local_gpu_input(local, false),
	                   bcm2836_local_gpu_input(local, true));

	for (unsigned i = 0; i < BCM2835_GPIO_NUM_IRQS; i++)
		gpio_irqs[i] = bcm2835_ic_gpu_input(ic, BCM2835_IC_GPIO0 + i);
	gpio = bcm2835_gpio_create(m, gpio_irqs);
	if (gpio == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, gpio, reset_gpio, destroy_gpio)) {
		bcm2835_gpio_destroy(gpio);
		goto out_of_memory;
	}
	status = bcm2835_gpio_map(gpio, m->bus, "gpio", RASPI3B_GPIO_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the GPIO: %s",
		         bus_status_str(status));
		goto fail;
	}

	/* -serial is at the end of the cable on GPIO 14 and 15. */
	cable = raspi3b_serial_create(m, gpio, options->serial);
	if (cable == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, cable, NULL, destroy_cable)) {
		raspi3b_serial_destroy(cable);
		goto out_of_memory;
	}

	uart = pl011_create(m, &bcm2835_pl011, UART_CLOCK_HZ,
	                    raspi3b_serial_uart(cable, RASPI3B_SERIAL_UART0),
	                    bcm2835_ic_gpu_input(ic, BCM2835_IC_UART));
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

	aux = bcm2835_aux_create(m, CORE_CLOCK_HZ,
	                         raspi3b_serial_uart(cable, RASPI3B_SERIAL_UART1),
	                         bcm2835_ic_gpu_input(ic, BCM2835_IC_AUX));
	if (aux == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, aux, reset_aux, destroy_aux)) {
		bcm2835_aux_destroy(aux);
		goto out_of_memory;
	}
	status = bcm2835_aux_map(aux, m->bus, "aux", RASPI3B_AUX_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the mini UART: %s",
		         bus_status_str(status));
		goto fail;
	}

	/* After the devices it sets up, so that it resets after them. */
	status = firmware_uart(m);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot set up the mini UART: %s",
		         bus_status_str(status));
		goto fail;
	}
	if (!machine_add_device(m, m, reset_firmware, NULL))
		goto out_of_memory;

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
