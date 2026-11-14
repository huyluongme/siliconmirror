/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Raspberry Pi 3 Model B board (src/machine/): its
 * memory map, the state it boots the cores in, and small programs that
 * print on the mini UART. The programs were built with the GNU assembler.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "backend/char/file.h"
#include "block_mem.h"
#include "core/event.h"
#include "cpu/arm64/cpu.h"
#include "cpu/arm64/sysreg.h"
#include "machine/board.h"
#include "machine/raspi3b.h"
#include "test.h"
#include "util/macros.h"

/* Test files go to the current directory: the build tree under CTest. */
#define KERNEL "test_raspi3b_kernel.bin"

/*
 * Print "Hello, world!\n" on the mini UART, which the firmware has set up,
 * then wait in WFI.
 */
static const uint32_t hello[] = {
	0xd2a7e421, /*       MOVZ X1, #0x3f21, LSL #16 */
	0xf28a0801, /*       MOVK X1, #0x5040: AUX_MU_IO_REG */
	0x10000123, /*       ADR  X3, message */
	0x38401462, /* next: LDRB W2, [X3], #1 */
	0x340000a2, /*       CBZ  W2, done */
	0xb9401424, /* busy: LDR  W4, [X1, #0x14]: AUX_MU_LSR_REG */
	0x362fffe4, /*       TBZ  W4, #5, busy: transmitter empty */
	0xb9000022, /*       STR  W2, [X1]: AUX_MU_IO_REG */
	0x17fffffb, /*       B    next */
	0xd503207f, /* done: WFI */
	0x17ffffff, /*       B    done */
	/* message: */
	0x6c6c6548, /* "Hell" */
	0x77202c6f, /* "o, w" */
	0x646c726f, /* "orld" */
	0x00000a21, /* "!\n" */
};

/* Wait in WFI. */
static const uint32_t park[] = {
	0xd503207f, /* park: WFI */
	0x17ffffff, /*       B park */
};

/*
 * Core 0 waits long enough for the other cores to sleep in WFE, then
 * releases them through the spin table and sends an event. Each of them
 * prints its number.
 */
static uint32_t smp[] = {
	0xd280fa07, /*        MOV  X7, #2000 */
	0xf10004e7, /* delay: SUBS X7, X7, #1 */
	0x54ffffe1, /*        B.NE delay */
	0x10000102, /*        ADR  X2, secondary */
	0xd2801c03, /*        MOV  X3, #0xe0: spin table entry of core 1 */
	0xf9000062, /*        STR  X2, [X3] */
	0xf9000462, /*        STR  X2, [X3, #8] */
	0xf9000862, /*        STR  X2, [X3, #16] */
	0xd503209f, /*        SEV */
	0xd503207f, /* done:  WFI */
	0x17ffffff, /*        B    done */
	/* secondary: */
	0xd2a7e421, /*        MOVZ X1, #0x3f21, LSL #16 */
	0xf28a0801, /*        MOVK X1, #0x5040: AUX_MU_IO_REG */
	0xd53800a6, /*        MRS  X6, MPIDR_EL1 */
	0x924004c6, /*        AND  X6, X6, #3: the core number */
	0x1100c0c6, /*        ADD  W6, W6, #'0' */
	0xb9000026, /*        STR  W6, [X1] */
	0xd503207f, /* park:  WFI */
	0x17ffffff, /*        B    park */
};

#define SMP_SEV 8 /* index of the SEV in smp[] */

static void write_kernel(const uint32_t *words, size_t count)
{
	FILE *f = fopen(KERNEL, "wb");

	CHECK(f != NULL);
	if (f == NULL)
		return;
	for (size_t i = 0; i < count; i++) {
		uint8_t bytes[4] = {
			(uint8_t)words[i],
			(uint8_t)(words[i] >> 8),
			(uint8_t)(words[i] >> 16),
			(uint8_t)(words[i] >> 24),
		};

		CHECK_EQ(fwrite(bytes, 1, 4, f), 4);
	}
	fclose(f);
}

/* Boot a kernel; the serial output goes to *out, which the test closes. */
static Machine *boot(const uint32_t *words, size_t count, FILE **out)
{
	char error[256] = "";

	write_kernel(words, count);
	*out = tmpfile();
	CHECK(*out != NULL);

	BoardOptions options = {
		.kernel = KERNEL,
		.serial = char_file_create(*out, false),
	};
	Machine *m = raspi3b_board.create(&options, error, sizeof(error));

	if (m == NULL)
		fprintf(stderr, "raspi3b: %s\n", error);
	CHECK(m != NULL);
	return m;
}

/* Everything written to out so far. */
static void read_output(FILE *out, char *buf, size_t size)
{
	size_t n;

	rewind(out);
	n = fread(buf, 1, size - 1, out);
	buf[n] = '\0';
}

static void test_board_list(void)
{
	CHECK(board_find("raspi3b") == &raspi3b_board);
	CHECK(board_find("raspi3") == NULL);
	CHECK(board_list[0] == &raspi3b_board);
	CHECK(board_list[1] == NULL);
}

static void test_nothing_to_boot(void)
{
	FILE *out = tmpfile();
	BoardOptions options = {.serial = char_file_create(out, false)};
	char error[256];

	/* The board frees the serial back end when it fails. */
	CHECK(raspi3b_board.create(&options, error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "no SD card and no -kernel: the board has nothing "
	                    "to boot from") == 0);
	fclose(out);

	options = (BoardOptions){.kernel = "no/such/kernel"};
	CHECK(raspi3b_board.create(&options, error, sizeof(error)) == NULL);
	CHECK(strncmp(error, "no/such/kernel: ", 16) == 0);
}

/*
 * The board leaves the cores as they reset, at EL3 at the boot stub's
 * first instruction, and loads the kernel and the stub with its data.
 */
static void test_state_before_the_stub(void)
{
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	uint64_t value;

	CHECK_EQ(m->num_cpus, RASPI3B_NUM_CPUS);
	for (unsigned i = 0; i < m->num_cpus; i++) {
		Arm64Cpu *c = arm64_cpu_of(m->cpus[i]);

		CHECK_EQ(c->el, 3);
		CHECK_EQ(c->pc, RASPI3B_STUB_ADDRESS);
		CHECK_EQ(c->cpu.state, CPU_RUNNING);
		/* MPIDR_EL1.Aff0 numbers the cores of the one cluster. */
		CHECK(arm64_cpu_read_sysreg(c, ARM64_MPIDR_EL1, &value));
		CHECK_EQ(value, 0x80000000u + i);
		/* The Generic Timer is not set up, and its counter stopped. */
		CHECK(arm64_cpu_read_sysreg(c, ARM64_CNTFRQ_EL0, &value));
		CHECK_EQ(value, 0);
	}

	/* The ARM local peripherals reset with the core timer stopped. */
	CHECK_EQ(bus_read(m->bus, RASPI3B_LOCAL_BASE + 0x08, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	event_queue_run_until(&m->events, 1000000);
	CHECK(arm64_cpu_read_sysreg(arm64_cpu_of(m->cpus[0]), ARM64_CNTPCT_EL0,
	                            &value));
	CHECK_EQ(value, 0);

	CHECK_EQ(bus_read(m->bus, RASPI3B_KERNEL_ADDRESS, 4, &value), BUS_OK);
	CHECK_EQ(value, park[0]);
	/* The stub's code, then its data. */
	CHECK_EQ(bus_read(m->bus, RASPI3B_STUB_ADDRESS, 4, &value), BUS_OK);
	CHECK(value != 0);
	/* An empty spin table: the board has cleared the magic number. */
	for (unsigned i = 0; i < 4; i++) {
		CHECK_EQ(bus_read(m->bus, RASPI3B_SPIN_TABLE + 8 * i, 8, &value),
		         BUS_OK);
		CHECK_EQ(value, 0);
	}
	CHECK_EQ(bus_read(m->bus, RASPI3B_STUB_DTB, 4, &value), BUS_OK);
	CHECK_EQ(value, 0); /* no device tree yet */
	CHECK_EQ(bus_read(m->bus, RASPI3B_STUB_KERNEL, 4, &value), BUS_OK);
	CHECK_EQ(value, RASPI3B_KERNEL_ADDRESS);

	/* RAM ends where the peripherals start; UART0 is mapped. */
	CHECK_EQ(bus_read(m->bus, RASPI3B_RAM_SIZE - 8, 8, &value), BUS_OK);
	CHECK_EQ(bus_read(m->bus, RASPI3B_PERIPHERAL_BASE, 4, &value),
	         BUS_ERR_UNMAPPED);
	CHECK_EQ(bus_read(m->bus, RASPI3B_UART0_BASE + 0x30, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x300); /* UARTCR after reset */
	/* The BCM2835 datasheet does not document the identification. */
	for (unsigned offset = 0xfe0; offset < 0x1000; offset += 4) {
		CHECK_EQ(bus_read(m->bus, RASPI3B_UART0_BASE + offset, 4, &value),
		         BUS_OK);
		CHECK_EQ(value, 0);
	}

	/*
	 * The firmware has set up the mini UART on GPIO 14 and 15, which its
	 * TX and the cable keep high: 8 bits, 115200 baud, no interrupts.
	 */
	static const struct {
		uint64_t address;
		uint64_t value;
	} firmware[] = {
		{RASPI3B_GPIO_BASE + 0x04, 0x12000}, /* GPFSEL1: ALT5 */
		{RASPI3B_GPIO_BASE + 0x34, 0xc000},  /* GPLEV0: TX, RX */
		{RASPI3B_AUX_BASE + 0x04, 1},        /* AUX_ENABLES */
		{RASPI3B_AUX_BASE + 0x44, 0},        /* AUX_MU_IER_REG */
		{RASPI3B_AUX_BASE + 0x4c, 1},        /* AUX_MU_LCR_REG */
		{RASPI3B_AUX_BASE + 0x60, 3},        /* AUX_MU_CNTL_REG */
		{RASPI3B_AUX_BASE + 0x68, 270},      /* AUX_MU_BAUD_REG */
	};
	for (unsigned i = 0; i < ARRAY_SIZE(firmware); i++) {
		CHECK_EQ(bus_read(m->bus, firmware[i].address, 4, &value), BUS_OK);
		CHECK_EQ(value & (i == 1 ? UINT64_C(0xc000) : UINT64_MAX),
		         firmware[i].value);
	}

	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * After the stub: every core at EL2, with the registers that only EL3
 * can write set up as armstub8 sets them; core 0 in the kernel, the
 * others asleep in the spin table.
 */
static void test_state_after_the_stub(void)
{
	static const struct {
		uint16_t encoding;
		uint64_t value;
	} stub_state[] = {
		{ARM64_MIDR_EL1, 0x410fd034}, /* a Cortex-A53 r0p4 */
		{ARM64_CPTR_EL3, 0},
		{ARM64_SCR_EL3, 0x5b1},
		{ARM64_ACTLR_EL3, 0x73},
		{ARM64_SYSREG(3, 1, 15, 2, 1), 0x40}, /* CPUECTLR_EL1 */
		/* L2CTLR_EL1: the stub's write changes nothing on an A53 */
		{ARM64_SYSREG(3, 1, 11, 0, 2), 0x03000000},
		{ARM64_SCTLR_EL2, 0x30c50830},
		{ARM64_SPSR_EL3, 0x3c9},
		{ARM64_CNTFRQ_EL0, 19200000},
		{ARM64_CNTVOFF_EL2, 0},
	};
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	uint64_t value;

	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
	for (unsigned i = 0; i < m->num_cpus; i++) {
		Arm64Cpu *c = arm64_cpu_of(m->cpus[i]);

		/* EL2h, interrupts masked, waiting. */
		CHECK_EQ(c->el, 2);
		CHECK(c->sp_sel);
		CHECK_EQ(c->daif, 0x3c0);
		CHECK_EQ(c->cpu.state, CPU_HALTED);
		for (unsigned j = 0; j < ARRAY_SIZE(stub_state); j++) {
			CHECK(arm64_cpu_read_sysreg(c, stub_state[j].encoding, &value));
			CHECK_EQ(value, stub_state[j].value);
		}
		if (i == 0) {
			/* In the kernel, entered through X4, with X0 to X3 clear. */
			CHECK(c->pc >= RASPI3B_KERNEL_ADDRESS &&
			      c->pc < RASPI3B_KERNEL_ADDRESS + sizeof(park));
			CHECK_EQ(c->x[4], RASPI3B_KERNEL_ADDRESS);
			for (unsigned r = 0; r < 4; r++)
				CHECK_EQ(c->x[r], 0);
		} else {
			/* In the stub, waiting on its own spin table entry. */
			CHECK(c->pc < RASPI3B_SPIN_TABLE);
			CHECK_EQ(c->x[5], RASPI3B_SPIN_TABLE);
			CHECK_EQ(c->x[6], i);
		}
	}

	/*
	 * The core timer counts the 19.2 MHz crystal. The stub started it
	 * before its first tick, at 52 ns, so the count is the number of
	 * ticks since time 0.
	 */
	CHECK_EQ(bus_read(m->bus, RASPI3B_LOCAL_BASE, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(m->bus, RASPI3B_LOCAL_BASE + 0x08, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x80000000);
	CHECK(arm64_cpu_read_sysreg(arm64_cpu_of(m->cpus[1]), ARM64_CNTPCT_EL0,
	                            &value));
	CHECK(value > 0);
	CHECK_EQ(value, machine_now(m) * 192 / 10000);

	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

static void test_hello(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	char text[64];

	CHECK_EQ(machine_run(m, 2000000), MACHINE_STOP_IDLE);
	read_output(out, text, sizeof(text));
	CHECK(strcmp(text, "Hello, world!\n") == 0);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

static void test_secondary_cores(void)
{
	FILE *out;
	Machine *m = boot(smp, ARRAY_SIZE(smp), &out);
	char text[64];

	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
	read_output(out, text, sizeof(text));
	CHECK(strcmp(text, "123") == 0);
	machine_destroy(m);
	fclose(out);

	/*
	 * Without the SEV, the cores sleep on in WFE: writing to the spin
	 * table is not enough to wake them.
	 */
	smp[SMP_SEV] = 0xd503201f; /* NOP */
	m = boot(smp, ARRAY_SIZE(smp), &out);
	smp[SMP_SEV] = 0xd503209f; /* SEV */
	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
	read_output(out, text, sizeof(text));
	CHECK(strcmp(text, "") == 0);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

static void test_shared_l2_registers(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	Arm64Cpu *core0 = arm64_cpu_of(m->cpus[0]);
	Arm64Cpu *core3 = arm64_cpu_of(m->cpus[3]);
	uint16_t l2ectlr = ARM64_SYSREG(3, 1, 11, 0, 3);
	uint64_t value;

	/* The four cores share one L2 memory system and its registers. */
	CHECK(arm64_cpu_write_sysreg(core0, l2ectlr, 7));
	CHECK(arm64_cpu_read_sysreg(core3, l2ectlr, &value));
	CHECK_EQ(value, 7);

	/* Resetting the machine resets the cluster too. */
	machine_reset(m);
	CHECK(arm64_cpu_read_sysreg(core3, l2ectlr, &value));
	CHECK_EQ(value, 0);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * The four timers of each core reach the ARM local peripherals, in the
 * bits of its source registers, and its IRQ and FIQ for the core reach
 * the core's nIRQ and nFIQ.
 */
static void test_interrupt_wiring(void)
{
	static const struct {
		uint16_t ctl, cval;
	} timers[] = {
		/* in the order of the source register bits */
		{ARM64_CNTPS_CTL_EL1, ARM64_CNTPS_CVAL_EL1},
		{ARM64_CNTP_CTL_EL0, ARM64_CNTP_CVAL_EL0},
		{ARM64_CNTHP_CTL_EL2, ARM64_CNTHP_CVAL_EL2},
		{ARM64_CNTV_CTL_EL0, ARM64_CNTV_CVAL_EL0},
	};
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	uint64_t value;

	for (unsigned i = 0; i < RASPI3B_NUM_CPUS; i++) {
		Arm64Cpu *core = arm64_cpu_of(m->cpus[i]);
		uint64_t routing = RASPI3B_LOCAL_BASE + 0x40 + 4 * i;
		uint64_t irq_source = RASPI3B_LOCAL_BASE + 0x60 + 4 * i;
		uint64_t fiq_source = RASPI3B_LOCAL_BASE + 0x70 + 4 * i;

		for (unsigned t = 0; t < ARRAY_SIZE(timers); t++) {
			uint64_t isr;

			/* CompareValue 0 is met at once. */
			CHECK(arm64_cpu_write_sysreg(core, timers[t].cval, 0));
			CHECK(arm64_cpu_write_sysreg(core, timers[t].ctl, 1));
			CHECK_EQ(bus_write(m->bus, routing, 4, 1u << t), BUS_OK);
			CHECK_EQ(bus_read(m->bus, irq_source, 4, &value), BUS_OK);
			CHECK_EQ(value, 1u << t);
			CHECK(arm64_cpu_read_sysreg(core, ARM64_ISR_EL1, &isr));
			CHECK_EQ(isr, 0x80);
			CHECK_EQ(bus_write(m->bus, routing, 4, 0x10u << t), BUS_OK);
			CHECK_EQ(bus_read(m->bus, fiq_source, 4, &value), BUS_OK);
			CHECK_EQ(value, 1u << t);
			CHECK(arm64_cpu_read_sysreg(core, ARM64_ISR_EL1, &isr));
			CHECK_EQ(isr, 0x40);
			CHECK(arm64_cpu_write_sysreg(core, timers[t].ctl, 0));
			CHECK(arm64_cpu_read_sysreg(core, ARM64_ISR_EL1, &isr));
			CHECK_EQ(isr, 0);
		}
	}
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * The GPU interrupts of the mini UART (29) and of the GPIO banks (49 to
 * 51, and 52 for all the pins), as the interrupt controller's pending
 * registers show them once enabled.
 */
static void test_gpu_interrupt_wiring(void)
{
	static const struct {
		uint64_t offset; /* in the GPIO block */
		uint32_t value;
		uint32_t pending2; /* GPU interrupts 32-63 */
	} gpio[] = {
		{0x64, 1u << 0, 1u << 17 | 1u << 20},  /* GPHEN0: pin 0 is high */
		{0x70, 1u << 30, 1u << 18 | 1u << 20}, /* GPLEN0: pin 30 is low */
		{0x68, 1u << 18, 1u << 19 | 1u << 20}, /* GPHEN1: pin 50 is high */
	};
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	uint64_t value;

	CHECK_EQ(bus_write(m->bus, RASPI3B_IC_BASE + 0x10, 4, 1u << 29), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_IC_BASE + 0x14, 4, 0xfu << 17), BUS_OK);
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x04, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	/* AUX_MU_IER_REG: the transmit FIFO is empty. */
	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x44, 4, 2), BUS_OK);
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x04, 4, &value), BUS_OK);
	CHECK_EQ(value, 1u << 29);
	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x44, 4, 0), BUS_OK);

	for (unsigned i = 0; i < ARRAY_SIZE(gpio); i++) {
		uint64_t base = RASPI3B_GPIO_BASE;

		CHECK_EQ(bus_write(m->bus, base + gpio[i].offset, 4, gpio[i].value),
		         BUS_OK);
		CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x08, 4, &value), BUS_OK);
		CHECK_EQ(value, gpio[i].pending2);
		CHECK_EQ(bus_write(m->bus, base + gpio[i].offset, 4, 0), BUS_OK);
		CHECK_EQ(bus_write(m->bus, base + 0x40, 4, UINT32_MAX), BUS_OK);
		CHECK_EQ(bus_write(m->bus, base + 0x44, 4, UINT32_MAX), BUS_OK);
	}
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x08, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/* Send the property buffer at 0x1000 through the mailboxes. */
static void property_call(Machine *m, const uint32_t *words, size_t count)
{
	uint64_t now = m->events.now, value;

	for (size_t i = 0; i < count; i++)
		CHECK_EQ(bus_write(m->bus, 0x1000 + 4 * i, 4, words[i]), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_MBOX_BASE + 0x20, 4, 0xc0001008),
	         BUS_OK);
	event_queue_run_until(&m->events, now + 999);
	CHECK_EQ(bus_read(m->bus, RASPI3B_MBOX_BASE + 0x18, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x40000000); /* mailbox 0 is still empty */
	event_queue_run_until(&m->events, now + 1000);
	CHECK_EQ(bus_read(m->bus, RASPI3B_MBOX_BASE + 0x18, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
}

/*
 * The firmware answers through the mailboxes, after 1 us; their
 * interrupt is ARM interrupt 1 of the interrupt controller. A reset of
 * the board starts the firmware again.
 */
static void test_mailbox(void)
{
	uint32_t buf[] = {
		sizeof(uint32_t[17]),
		0,
		0x00010005,
		8,
		0,
		0,
		0, /* ARM memory */
		0x00030002,
		8,
		0,
		4,
		0, /* the core clock */
		0x00038002,
		12,
		0,
		5,
		300000000,
		0,
		0, /* V3D to 300 MHz */
	};
	uint32_t get_v3d[] = {sizeof(uint32_t[8]), 0, 0x00030002, 8, 0, 5, 0, 0};
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	uint64_t value;

	buf[0] = sizeof(buf);
	CHECK_EQ(bus_write(m->bus, RASPI3B_IC_BASE + 0x18, 4, 1u << 1), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_MBOX_BASE + 0x1c, 4, 1), BUS_OK);
	property_call(m, buf, ARRAY_SIZE(buf));
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x00, 4, &value), BUS_OK);
	CHECK_EQ(value & 0xff, 1u << 1);
	CHECK_EQ(bus_read(m->bus, RASPI3B_LOCAL_BASE + 0x60, 4, &value), BUS_OK);
	CHECK_EQ(value, 1u << 8); /* the GPU interrupt of core 0 */
	CHECK_EQ(bus_read(m->bus, RASPI3B_MBOX_BASE + 0x00, 4, &value), BUS_OK);
	CHECK_EQ(value, 0xc0001008);
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x00, 4, &value), BUS_OK);
	CHECK_EQ(value & 0xff, 0);

	CHECK_EQ(bus_read(m->bus, 0x1004, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x80000000);
	CHECK_EQ(bus_read(m->bus, 0x1018, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x3b400000);
	/* As with enable_uart=1, which sets up the console. */
	CHECK_EQ(bus_read(m->bus, 0x102c, 4, &value), BUS_OK);
	CHECK_EQ(value, 250000000);
	CHECK_EQ(bus_read(m->bus, 0x1040, 4, &value), BUS_OK);
	CHECK_EQ(value, 300000000);

	/* A reset empties the mailboxes and sets the clocks back. */
	CHECK_EQ(bus_write(m->bus, RASPI3B_MBOX_BASE + 0x20, 4, 0xc0001008),
	         BUS_OK);
	machine_reset(m);
	CHECK_EQ(bus_read(m->bus, RASPI3B_MBOX_BASE + 0x1c, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	property_call(m, get_v3d, ARRAY_SIZE(get_v3d));
	CHECK_EQ(bus_read(m->bus, 0x1018, 4, &value), BUS_OK);
	CHECK_EQ(value, 400000000);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/* A command to SDHOST, waited for; returns SDCMD. */
static uint32_t sdhost_command(Machine *m, uint32_t cmd, uint32_t arg)
{
	uint64_t value = 0;

	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x04, 4, arg), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE, 4, cmd | 0x8000), BUS_OK);
	for (int i = 0; i < 10000; i++) {
		CHECK_EQ(bus_read(m->bus, RASPI3B_SDHOST_BASE, 4, &value), BUS_OK);
		if (!(value & 0x8000))
			break;
		event_queue_run_until(&m->events, m->events.now + 10000);
	}
	return (uint32_t)value;
}

/*
 * The SD card: SDHOST at 0x3f202000 reaches it through GPIO 48 to 53,
 * which the firmware gives ALT0; its interrupt is GPU interrupt 56.
 */
static void test_sd_card(void)
{
	BlockMem card;
	char error[256];
	uint64_t value;

	write_kernel(park, ARRAY_SIZE(park));
	block_mem_init(&card, 1024 * 1024);
	BoardOptions options = {.kernel = KERNEL, .sd = &card.dev};
	Machine *m = raspi3b_board.create(&options, error, sizeof(error));

	CHECK(m != NULL);
	CHECK_EQ(bus_read(m->bus, RASPI3B_SDHOST_BASE + 0x0c, 4, &value), BUS_OK);
	CHECK_EQ(value, 507);
	/* SDHOST runs from the core clock, 250 MHz with enable_uart=1. */
	CHECK_EQ(RASPI3B_CORE_CLOCK_HZ, 250000000);
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x30, 4, 1), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x38, 4, 0x408), BUS_OK);
	CHECK_EQ(sdhost_command(m, 0x400 | 0, 0), 0x400);
	CHECK_EQ(sdhost_command(m, 8, 0x1aa), 8);
	/* GPIO 50, DAT0, taken away: the card is out of reach. */
	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 0x14, 4, 0x920), BUS_OK);
	CHECK_EQ(sdhost_command(m, 8, 0x1aa), 0x4000 | 8);
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x20, 4, 0x7f8), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 0x14, 4, 0x924), BUS_OK);
	CHECK_EQ(sdhost_command(m, 8, 0x1aa), 8);
	/* Each of GPIO 48 to 53 taken away, then given back. */
	for (unsigned pin = 48; pin <= 53; pin++) {
		uint64_t reg = RASPI3B_GPIO_BASE + 4 * (pin / 10), fsel;
		unsigned shift = 3 * (pin % 10);

		CHECK_EQ(bus_read(m->bus, reg, 4, &fsel), BUS_OK);
		CHECK_EQ((fsel >> shift) & 7, 4); /* ALT0 */
		CHECK_EQ(bus_write(m->bus, reg, 4, fsel & ~(7ull << shift)), BUS_OK);
		CHECK_EQ(sdhost_command(m, 8, 0x1aa), 0x4000 | 8);
		CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x20, 4, 0x7f8),
		         BUS_OK);
		CHECK_EQ(bus_write(m->bus, reg, 4, fsel), BUS_OK);
		CHECK_EQ(sdhost_command(m, 8, 0x1aa), 8);
	}
	/* Identify the card; CMD7's busy raises GPU interrupt 56. */
	for (int i = 0; i < 10; i++) {
		sdhost_command(m, 55, 0);
		sdhost_command(m, 41, 0x40ff8000);
		CHECK_EQ(bus_read(m->bus, RASPI3B_SDHOST_BASE + 0x10, 4, &value),
		         BUS_OK);
		if (value & (1u << 31))
			break;
		event_queue_run_until(&m->events, m->events.now + 500000);
	}
	CHECK(value & (1u << 31));
	sdhost_command(m, 0x200 | 2, 0);
	sdhost_command(m, 3, 0);
	CHECK_EQ(bus_write(m->bus, RASPI3B_IC_BASE + 0x14, 4, 1u << 24), BUS_OK);
	CHECK_EQ(sdhost_command(m, 0x800 | 7, 0xaaaa0000), 0x800 | 7);
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x08, 4, &value), BUS_OK);
	CHECK_EQ(value, 1u << 24);
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x20, 4, 0x400), BUS_OK);
	CHECK_EQ(bus_read(m->bus, RASPI3B_IC_BASE + 0x08, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(sdhost_command(m, 13, 0xaaaa0000), 13);

	/* A reset puts the card in the idle state: it no longer answers. */
	machine_reset(m);
	CHECK_EQ(bus_read(m->bus, RASPI3B_SDHOST_BASE + 0x38, 4, &value), BUS_OK);
	CHECK_EQ(value, 0); /* SDHOST is reset too */
	CHECK_EQ(bus_write(m->bus, RASPI3B_SDHOST_BASE + 0x30, 4, 1), BUS_OK);
	CHECK_EQ(sdhost_command(m, 13, 0xaaaa0000), 0x4000 | 13);
	CHECK_EQ(sdhost_command(m, 8, 0x1aa), 8);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	machine_destroy(m);

	/* An image that is not a card's size. */
	card.dev.size = 1024 * 1024 + 512;
	options = (BoardOptions){.kernel = KERNEL, .sd = &card.dev};
	CHECK(raspi3b_board.create(&options, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "an SD card holds a multiple of 512 KB") != NULL);
	block_mem_free(&card);
	remove(KERNEL);
}

/*
 * UART0 runs from a 48 MHz UARTCLK: at 115200 baud, a byte takes 86823 ns.
 * GPIO 14 and 15 must carry its signals, ALT0, for the cable to get it.
 */
static void test_uart_clock(void)
{
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	char buf[16];

	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 4, 4, 0x24000), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x24, 4, 26), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x28, 4, 3), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x2c, 4, 0x60), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x30, 4, 0x301), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE, 4, 'x'), BUS_OK);
	event_queue_run_until(&m->events, 86822);
	read_output(out, buf, sizeof(buf));
	CHECK(strcmp(buf, "") == 0);
	event_queue_run_until(&m->events, 86823);
	read_output(out, buf, sizeof(buf));
	CHECK(strcmp(buf, "x") == 0);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * The mini UART runs from the 250 MHz core clock, which the firmware set
 * to 115313 baud: a byte of 10 bits takes 10 * 8 * 271 / 250 MHz.
 */
static void test_mini_uart_clock(void)
{
	FILE *out;
	Machine *m = boot(park, ARRAY_SIZE(park), &out);
	char buf[16];

	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x40, 4, 'y'), BUS_OK);
	event_queue_run_until(&m->events, 86719);
	read_output(out, buf, sizeof(buf));
	CHECK(strcmp(buf, "") == 0);
	event_queue_run_until(&m->events, 86720);
	read_output(out, buf, sizeof(buf));
	CHECK(strcmp(buf, "y") == 0);

	/* With GPIO 14 back to an input, nothing reaches the cable. */
	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 4, 4, 0), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x40, 4, 'z'), BUS_OK);
	event_queue_run_until(&m->events, 2 * 86720);
	read_output(out, buf, sizeof(buf));
	CHECK(strcmp(buf, "y") == 0);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

static void test_reset_resets_uart(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	uint64_t value;

	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x30, 4, 0x301), BUS_OK);
	machine_reset(m);
	CHECK_EQ(bus_read(m->bus, RASPI3B_UART0_BASE + 0x30, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x300);

	/*
	 * The GPIO and the mini UART reset, then the firmware sets up the
	 * mini UART again.
	 */
	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x68, 4, 1), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_AUX_BASE + 0x5c, 4, 0x5a), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 4, 4, 0), BUS_OK);
	CHECK_EQ(bus_write(m->bus, RASPI3B_GPIO_BASE + 8, 4, 0x40), BUS_OK);
	machine_reset(m);
	CHECK_EQ(bus_read(m->bus, RASPI3B_AUX_BASE + 0x5c, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(m->bus, RASPI3B_GPIO_BASE + 8, 4, &value), BUS_OK);
	CHECK_EQ(value, 0);
	CHECK_EQ(bus_read(m->bus, RASPI3B_AUX_BASE + 0x68, 4, &value), BUS_OK);
	CHECK_EQ(value, 270);
	CHECK_EQ(bus_read(m->bus, RASPI3B_GPIO_BASE + 4, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x12000);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * Resetting the machine starts the cores in the stub again, which starts
 * the kernel again: it is still in RAM.
 */
static void test_reset_runs_the_stub_again(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	char text[64];

	CHECK_EQ(machine_run(m, 2000000), MACHINE_STOP_IDLE);
	machine_reset(m);
	for (unsigned i = 0; i < m->num_cpus; i++) {
		Arm64Cpu *c = arm64_cpu_of(m->cpus[i]);

		CHECK_EQ(c->el, 3);
		CHECK_EQ(c->pc, RASPI3B_STUB_ADDRESS);
	}
	CHECK_EQ(machine_run(m, 2000000), MACHINE_STOP_IDLE);
	read_output(out, text, sizeof(text));
	CHECK(strcmp(text, "Hello, world!\nHello, world!\n") == 0);

	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

/*
 * USB on the board: DMA transfers on channel 0, as U-Boot makes them.
 * The addresses are the datasheet's, written out, not the header's.
 */
#define USB_BUF 0x200001 /* odd: all of HCDMA's address bits count */
#define USB_AT 0x3f980000
#define EMMC_AT 0x3f300000
#define PM_AT 0x3f100000
#define IC_AT 0x3f00b200

static uint32_t usb_rd(Machine *m, uint64_t offset)
{
	uint64_t v = 0;

	CHECK_EQ(bus_read(m->bus, USB_AT + offset, 4, &v), BUS_OK);
	return (uint32_t)v;
}

static void usb_wr(Machine *m, uint64_t offset, uint32_t v)
{
	CHECK_EQ(bus_write(m->bus, USB_AT + offset, 4, v), BUS_OK);
}

/* A transfer of size bytes at USB_BUF, through the uncached alias. */
static void usb_xfer(Machine *m, unsigned addr, bool in, uint32_t size,
                     uint32_t pid)
{
	uint32_t packets = size == 0 ? 1 : (size + 63) / 64;

	usb_wr(m, 0x510, pid << 29 | packets << 19 | size);
	usb_wr(m, 0x514, 0xc0000000u | USB_BUF);
	usb_wr(m, 0x508, 0x3fff);
	usb_wr(m, 0x500, 1u << 31 | addr << 22 | (in ? 1u << 15 : 0) | 64);
	for (int i = 0; i < 100000 && (usb_rd(m, 0x500) & 1u << 31); i++)
		event_queue_run_until(&m->events, m->events.now + 1);
	CHECK_EQ(usb_rd(m, 0x508) & 1, 1); /* XFERCOMPL */
}

/* A control transfer: SETUP, len bytes of data in or out, the status. */
static void usb_control(Machine *m, unsigned addr, const uint8_t setup[8],
                        uint8_t *data, uint32_t len)
{
	bool in = setup[0] & 0x80;

	CHECK_EQ(bus_write_block(m->bus, USB_BUF, setup, 8), BUS_OK);
	usb_xfer(m, addr, false, 8, 3);
	if (len != 0 && !in)
		CHECK_EQ(bus_write_block(m->bus, USB_BUF, data, len), BUS_OK);
	if (len != 0)
		usb_xfer(m, addr, in, len, 2);
	if (len != 0 && in)
		CHECK_EQ(bus_read_block(m->bus, USB_BUF, data, len), BUS_OK);
	usb_xfer(m, addr, len == 0 || !in, 0, 2);
}

/*
 * The DWC2's identity; the LAN9514's hub on the root port and its
 * Ethernet controller on hub port 1, with their descriptors; and what a
 * reset of the board resets.
 */
static void test_usb(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	uint8_t d[64];
	uint64_t v;

	CHECK_EQ(usb_rd(m, 0x040), 0x4f54280a); /* GSNPSID */
	CHECK_EQ(usb_rd(m, 0x044), 0);          /* GHWCFG1 */
	CHECK_EQ(usb_rd(m, 0x048), 0x228ddd50);
	CHECK_EQ(usb_rd(m, 0x04c), 0x0ff000e8);
	CHECK_EQ(usb_rd(m, 0x050), 0x1ff00020);
	CHECK_EQ(usb_rd(m, 0x03c), 0x2708a000); /* GUID */
	usb_wr(m, 0x440, 1u << 12);
	usb_wr(m, 0x440, 1u << 12 | 1u << 8);
	event_queue_run_until(&m->events, m->events.now + 50000000);
	usb_wr(m, 0x440, 1u << 12);
	CHECK(usb_rd(m, 0x440) & 4);

	/* The hub: 5 ports, several TTs, self powered. */
	usb_control(m, 0, (const uint8_t[8]){0x80, 6, 0, 1, 0, 0, 18, 0}, d, 18);
	CHECK_EQ(d[6], 2);
	CHECK(memcmp(d + 8, "\x24\x04\x14\x95\x00\x02", 6) == 0);
	usb_control(m, 0, (const uint8_t[8]){0x80, 6, 0, 2, 0, 0, 41, 0}, d, 41);
	CHECK(d[2] == 41 && d[7] == 0xc0 && d[8] == 1);
	usb_control(m, 0, (const uint8_t[8]){0, 5, 1, 0, 0, 0, 0, 0}, NULL, 0);
	usb_control(m, 1, (const uint8_t[8]){0, 9, 1, 0, 0, 0, 0, 0}, NULL, 0);
	usb_control(m, 1, (const uint8_t[8]){0xa0, 6, 0, 0x29, 0, 0, 9, 0}, d, 9);
	CHECK(memcmp(d, "\x09\x29\x05\x0d\x00\x32\x02\x02\xff", 9) == 0);
	usb_control(m, 1, (const uint8_t[8]){0x23, 3, 8, 0, 1, 0, 0, 0}, NULL, 0);
	usb_control(m, 1, (const uint8_t[8]){0x23, 3, 4, 0, 1, 0, 0, 0}, NULL, 0);
	event_queue_run_until(&m->events, m->events.now + 10000000);
	usb_control(m, 1, (const uint8_t[8]){0xa3, 0, 0, 0, 1, 0, 4, 0}, d, 4);
	CHECK(d[0] == 0x03 && d[1] == 0x05); /* connected, enabled; power, HS */

	/* The Ethernet controller on port 1. */
	usb_control(m, 0, (const uint8_t[8]){0x80, 6, 0, 1, 0, 0, 18, 0}, d, 18);
	CHECK_EQ(d[6], 1);
	CHECK(memcmp(d + 8, "\x24\x04\x00\xec\x00\x02", 6) == 0);
	usb_control(m, 0, (const uint8_t[8]){0x80, 6, 0, 2, 0, 0, 39, 0}, d, 39);
	CHECK(d[7] == 0xe0 && d[8] == 1 && d[38] == 4);
	usb_control(m, 0, (const uint8_t[8]){0, 5, 2, 0, 0, 0, 0, 0}, NULL, 0);
	usb_control(m, 2, (const uint8_t[8]){0, 9, 1, 0, 0, 0, 0, 0}, NULL, 0);
	usb_control(m, 2, (const uint8_t[8]){0xc0, 0xa1, 0, 0, 0, 0, 4, 0}, d, 4);
	CHECK(memcmp(d, "\x00\x00\x00\xec", 4) == 0); /* ID_REV */
	for (unsigned reg = 2; reg <= 3; reg++) {
		uint8_t access[4] = {(uint8_t)(reg << 6 | 1), 1 << 3, 0, 0};

		usb_control(m, 2, (const uint8_t[8]){0x40, 0xa0, 0, 0, 0x14, 1, 4, 0},
		            access, 4);
		usb_control(m, 2, (const uint8_t[8]){0xc0, 0xa1, 0, 0, 0x18, 1, 4, 0},
		            d, 4);
		CHECK_EQ(d[0] | d[1] << 8, reg == 2 ? 0x0007 : 0xc0c3);
	}

	/* The lines: the DWC2's is GPU interrupt 9, the EMMC's 62. */
	CHECK_EQ(bus_write(m->bus, IC_AT + 0x10, 4, 1u << 9), BUS_OK);
	CHECK_EQ(bus_write(m->bus, IC_AT + 0x14, 4, 1u << 30), BUS_OK);
	usb_wr(m, 0x018, 1u << 24); /* GINTMSK: the port */
	usb_wr(m, 0x008, 1);        /* GAHBCFG: the line */
	CHECK_EQ(bus_read(m->bus, IC_AT + 0x04, 4, &v), BUS_OK);
	CHECK_EQ(v, 1u << 9);
	CHECK_EQ(bus_write(m->bus, EMMC_AT + 0x38, 4, 1), BUS_OK); /* IRPT_EN */
	CHECK_EQ(bus_write(m->bus, EMMC_AT + 0x50, 4, 1), BUS_OK); /* FORCE */
	CHECK_EQ(bus_read(m->bus, IC_AT + 0x08, 4, &v), BUS_OK);
	CHECK_EQ(v, 1u << 30);

	/* A reset: the root port, the EMMC and the watchdog. */
	CHECK_EQ(bus_write(m->bus, EMMC_AT + 0x28, 4, 0x0f02), BUS_OK);
	CHECK_EQ(bus_write(m->bus, PM_AT + 0x24, 4, 0x5a0fffff), BUS_OK);
	CHECK_EQ(bus_write(m->bus, PM_AT + 0x1c, 4, 0x5a000020), BUS_OK);
	machine_reset(m);
	CHECK_EQ(usb_rd(m, 0x440), 0);
	CHECK_EQ(bus_read(m->bus, EMMC_AT + 0x28, 4, &v), BUS_OK);
	CHECK_EQ(v, 0);
	CHECK_EQ(bus_read(m->bus, PM_AT + 0x1c, 4, &v), BUS_OK);
	CHECK_EQ(v, 0);
	CHECK_EQ(m->stop, MACHINE_STOP_NONE);
	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

int main(void)
{
	RUN_TEST(test_board_list);
	RUN_TEST(test_nothing_to_boot);
	RUN_TEST(test_state_before_the_stub);
	RUN_TEST(test_state_after_the_stub);
	RUN_TEST(test_hello);
	RUN_TEST(test_interrupt_wiring);
	RUN_TEST(test_gpu_interrupt_wiring);
	RUN_TEST(test_mailbox);
	RUN_TEST(test_sd_card);
	RUN_TEST(test_uart_clock);
	RUN_TEST(test_mini_uart_clock);
	RUN_TEST(test_secondary_cores);
	RUN_TEST(test_shared_l2_registers);
	RUN_TEST(test_reset_resets_uart);
	RUN_TEST(test_reset_runs_the_stub_again);
	RUN_TEST(test_usb);
	return test_summary();
}
