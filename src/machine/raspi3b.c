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

#include <stdio.h>
#include <stdlib.h>

#include "cpu/arm64/cpu.h"
#include "hw/char/bcm2835_aux.h"
#include "hw/char/pl011.h"
#include "hw/gpio/bcm2835_gpio.h"
#include "hw/intc/bcm2835_ic.h"
#include "hw/intc/bcm2836_local.h"
#include "hw/misc/bcm2835_mbox.h"
#include "hw/misc/bcm2835_pm.h"
#include "hw/net/lan95xx.h"
#include "hw/sd/bcm2835_emmc.h"
#include "hw/sd/bcm2835_sdhost.h"
#include "hw/sd/sd_card.h"
#include "hw/usb/dwc2.h"
#include "hw/usb/usb_hub.h"
#include "machine/raspi3b_firmware.h"
#include "machine/raspi3b_property.h"
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

#define CORE_CLOCK_HZ RASPI3B_CORE_CLOCK_HZ

/*
 * How long the firmware takes over a message in the mailbox. Not
 * documented: our choice, short enough for every timeout of its users.
 */
#define VC_LATENCY_NS 1000

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

static void reset_property(void *ctx)
{
	raspi3b_property_reset(ctx);
}

static void destroy_property(void *ctx)
{
	raspi3b_property_destroy(ctx);
}

static void reset_mbox(void *ctx)
{
	bcm2835_mbox_reset(ctx);
}

static void destroy_mbox(void *ctx)
{
	bcm2835_mbox_destroy(ctx);
}

static void reset_pm(void *ctx)
{
	bcm2835_pm_reset(ctx);
}

static void destroy_pm(void *ctx)
{
	bcm2835_pm_destroy(ctx);
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

/* The firmware's boot, which a reset of the board runs again. */
typedef struct Firmware {
	Raspi3bFirmwareConfig config;
} Firmware;

static void reset_firmware(void *ctx)
{
	Firmware *fw = ctx;
	char error[256];

	fw->config.warn = NULL; /* warned at the first boot */
	if (!raspi3b_firmware_boot(&fw->config, error, sizeof(error)))
		machine_stop(fw->config.machine, MACHINE_STOP_ERROR, "firmware: %s",
		             error);
}

static void destroy_firmware(void *ctx)
{
	free(ctx);
}

static void destroy_card(void *ctx)
{
	block_destroy(ctx);
}

/*
 * The SD card. A reset of the board does not cut its power, but the boot
 * ROM's CMD0 puts it back in the idle state.
 */
static void reset_sd_card(void *ctx)
{
	sd_card_power(ctx, true);
}

static void destroy_sd_card(void *ctx)
{
	sd_card_destroy(ctx);
}

static void reset_sdhost(void *ctx)
{
	bcm2835_sdhost_reset(ctx);
}

static void destroy_sdhost(void *ctx)
{
	bcm2835_sdhost_destroy(ctx);
}

static void reset_usb(void *ctx)
{
	dwc2_reset(ctx);
}

static void destroy_usb(void *ctx)
{
	dwc2_destroy(ctx);
}

/*
 * The USB controller's identity, as TinyUSB measured it on a BCM2711
 * (dwc2_info.md, "BCM2711 (Pi4)"): the same core, 2.80a, as the
 * BCM2837's, whose drivers Linux and U-Boot share with it.
 */
static const Dwc2Config usb_config = {
	.guid = 0x2708a000,
	.snpsid = 0x4f54280a,
	.hwcfg1 = 0x00000000,
	.hwcfg2 = 0x228ddd50,
	.hwcfg3 = 0x0ff000e8,
	.hwcfg4 = 0x1ff00020,
	.dma_mask = 0x3fffffff, /* bus addresses: the alias in bits 31:30 goes */
};

/*
 * The LAN9514's hub without an EEPROM, as on the board: its hardware
 * defaults (LAN9514 datasheet, Table 3-3 and 3-4): 5 ports, port 1 the
 * Ethernet controller, not removable; self-powered; individual power
 * switching and over-current sensing; a compound device; a TT for each
 * port; 100 ms from power on to power good; 2 mA. Its device release
 * depends on the chip's revision: 2.00 is smirror's.
 */
static const UsbHubConfig lan9514_hub = {
	.vendor = 0x0424,
	.product = 0x9514,
	.release = 0x0200,
	.ports = 5,
	.removable = 0x02,
	.characteristics = 0x000d,
	.power_on_time = 0x32,
	.controller_current = 2,
	.max_power = 1,
	.self_powered = true,
	.multi_tt = true,
};

static void reset_hub(void *ctx)
{
	usb_device_reset(usb_hub_device(ctx));
}

static void destroy_hub(void *ctx)
{
	usb_hub_destroy(ctx);
}

/*
 * The LAN9514's Ethernet controller without an EEPROM. Its USB identity
 * and descriptors: the LAN9514 datasheet's defaults (Table 3-4: 0424:EC00,
 * self-powered with remote wakeup, 2 mA, a 4 ms polling interval); the
 * release and the device protocol, which the datasheets leave to the
 * chip, as a LAN9512 board's lsusb shows them (the same core). Chip ID
 * EC00h is Linux's ID_REV_CHIP_ID_9512_; the revision is unknown, 0 here.
 * The PHY's identity is the LAN9500's (Note 7-25), whose core it shares.
 */
static const Lan95xxConfig lan9514_eth = {
	.vendor = 0x0424,
	.product = 0xec00,
	.release = 0x0200,
	.device_protocol = 1,
	.chip_id = 0xec00,
	.chip_rev = 0,
	.phy_id1 = 0x0007,
	.phy_id2 = 0xc0c3,
	.max_power = 1,
	.interval = 4,
	.self_powered = true,
	.remote_wakeup = true,
};

static void reset_eth(void *ctx)
{
	lan95xx_reset(ctx);
}

static void destroy_eth(void *ctx)
{
	lan95xx_destroy(ctx);
}

static void reset_emmc(void *ctx)
{
	bcm2835_emmc_reset(ctx);
}

static void destroy_emmc(void *ctx)
{
	bcm2835_emmc_destroy(ctx);
}

/*
 * The card's slot is wired to GPIO 48 to 53 (CLK, CMD, DAT0 to DAT3:
 * bcm2710-rpi-3-b.dts): SDHOST reaches the card when they all have ALT0.
 */
static bool slot_connected(void *ctx)
{
	for (unsigned pin = 48; pin <= 53; pin++)
		if (bcm2835_gpio_function(ctx, pin) != BCM2835_GPIO_ALT0)
			return false;
	return true;
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
	Raspi3bProperty *property;
	Bcm2835Mbox *mbox;
	Bcm2835Pm *pm;
	SdCard *sd_card = NULL;
	Bcm2835Sdhost *sdhost;
	Bcm2835Emmc *emmc;
	Dwc2 *usb;
	UsbHub *hub;
	Lan95xx *eth;
	Firmware *fw;

	if (m == NULL) {
		char_stream_destroy(options->serial);
		block_destroy(options->sd);
		goto out_of_memory;
	}
	if (!machine_add_device(m, options->serial, NULL, destroy_serial)) {
		char_stream_destroy(options->serial);
		block_destroy(options->sd);
		goto out_of_memory;
	}
	if (options->sd != NULL &&
	    !machine_add_device(m, options->sd, NULL, destroy_card)) {
		block_destroy(options->sd);
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

	/* The firmware answers the Arm through the mailboxes. */
	property = raspi3b_property_create(m, RASPI3B_RAM_SIZE);
	if (property == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, property, reset_property, destroy_property)) {
		raspi3b_property_destroy(property);
		goto out_of_memory;
	}
	mbox = bcm2835_mbox_create(
		m, &(Bcm2835MboxVc){raspi3b_property_receive, property}, VC_LATENCY_NS,
		bcm2835_ic_arm_input(ic, BCM2835_IC_ARM_MAILBOX));
	if (mbox == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, mbox, reset_mbox, destroy_mbox)) {
		bcm2835_mbox_destroy(mbox);
		goto out_of_memory;
	}
	status = bcm2835_mbox_map(mbox, m->bus, "mbox", RASPI3B_MBOX_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the mailboxes: %s",
		         bus_status_str(status));
		goto fail;
	}
	pm = bcm2835_pm_create(m);
	if (pm == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, pm, reset_pm, destroy_pm)) {
		bcm2835_pm_destroy(pm);
		goto out_of_memory;
	}
	status = bcm2835_pm_map(pm, m->bus, "pm", RASPI3B_PM_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map the watchdog: %s",
		         bus_status_str(status));
		goto fail;
	}

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

	/* The SD card in its slot, and its controller. */
	if (options->sd != NULL) {
		sd_card = sd_card_create(m, options->sd, error, error_size);
		if (sd_card == NULL)
			goto fail;
		if (!machine_add_device(m, sd_card, reset_sd_card, destroy_sd_card)) {
			sd_card_destroy(sd_card);
			goto out_of_memory;
		}
		sd_card_power(sd_card, true);
	}
	sdhost = bcm2835_sdhost_create(
		m, CORE_CLOCK_HZ,
		&(Bcm2835SdhostSlot){
			.card = sd_card, .connected = slot_connected, .ctx = gpio},
		bcm2835_ic_gpu_input(ic, BCM2835_IC_SDHOST));
	if (sdhost == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, sdhost, reset_sdhost, destroy_sdhost)) {
		bcm2835_sdhost_destroy(sdhost);
		goto out_of_memory;
	}
	status = bcm2835_sdhost_map(sdhost, m->bus, "sdhost", RASPI3B_SDHOST_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map SDHOST: %s",
		         bus_status_str(status));
		goto fail;
	}
	emmc = bcm2835_emmc_create(m, RASPI3B_EMMC_CLOCK_HZ,
	                           bcm2835_ic_gpu_input(ic, BCM2835_IC_EMMC));
	if (emmc == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, emmc, reset_emmc, destroy_emmc)) {
		bcm2835_emmc_destroy(emmc);
		goto out_of_memory;
	}
	status = bcm2835_emmc_map(emmc, m->bus, "emmc", RASPI3B_EMMC_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map EMMC: %s",
		         bus_status_str(status));
		goto fail;
	}
	usb = dwc2_create(m, &usb_config, bcm2835_ic_gpu_input(ic, BCM2835_IC_USB));
	if (usb == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, usb, reset_usb, destroy_usb)) {
		dwc2_destroy(usb);
		goto out_of_memory;
	}
	status = dwc2_map(usb, m->bus, "usb", RASPI3B_USB_BASE);
	if (status != BUS_OK) {
		snprintf(error, error_size, "cannot map USB: %s",
		         bus_status_str(status));
		goto fail;
	}
	hub = usb_hub_create(m, &lan9514_hub);
	if (hub == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, hub, reset_hub, destroy_hub)) {
		usb_hub_destroy(hub);
		goto out_of_memory;
	}
	dwc2_attach(usb, usb_hub_device(hub));
	eth = lan95xx_create(m, &lan9514_eth);
	if (eth == NULL)
		goto out_of_memory;
	if (!machine_add_device(m, eth, reset_eth, destroy_eth)) {
		lan95xx_destroy(eth);
		goto out_of_memory;
	}
	usb_hub_attach(hub, 1, lan95xx_device(eth)); /* port 1, on the chip */

	/* After the devices it sets up, so that it resets after them. */
	fw = calloc(1, sizeof(*fw));
	if (fw == NULL)
		goto out_of_memory;
	fw->config = (Raspi3bFirmwareConfig){
		.machine = m,
		.gpio = gpio,
		.property = property,
		.pm = pm,
		.kernel = options->kernel,
		.card = options->sd,
		.warn = options->warn,
		.warn_ctx = options->warn_ctx,
	};
	if (!machine_add_device(m, fw, reset_firmware, destroy_firmware)) {
		free(fw);
		goto out_of_memory;
	}
	if (!raspi3b_firmware_boot(&fw->config, error, error_size))
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
