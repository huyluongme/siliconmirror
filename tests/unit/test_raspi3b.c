/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the Raspberry Pi 3 Model B board (src/machine/): its
 * memory map, the state it boots the cores in, and small programs that
 * print on UART0. The programs were built with the GNU assembler.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "backend/char/file.h"
#include "cpu/arm64/cpu.h"
#include "cpu/arm64/sysreg.h"
#include "machine/board.h"
#include "machine/raspi3b.h"
#include "test.h"
#include "util/macros.h"

/* Test files go to the current directory: the build tree under CTest. */
#define KERNEL "test_raspi3b_kernel.bin"

/* Print "Hello, world!\n" on UART0, then wait in WFI. */
static const uint32_t hello[] = {
	0xd2a7e401, /*       MOVZ X1, #0x3f20, LSL #16 */
	0xf2820001, /*       MOVK X1, #0x1000: UART0 */
	0xb900303f, /*       STR  WZR, [X1, #0x30]: UARTCR, disable */
	0x52800e02, /*       MOV  W2, #0x70 */
	0xb9002c22, /*       STR  W2, [X1, #0x2c]: UARTLCR_H, 8 bits */
	0x52806022, /*       MOV  W2, #0x301 */
	0xb9003022, /*       STR  W2, [X1, #0x30]: UARTCR, enable */
	0x10000123, /*       ADR  X3, message */
	0x38401462, /* next: LDRB W2, [X3], #1 */
	0x340000a2, /*       CBZ  W2, done */
	0xb9401824, /* busy: LDR  W4, [X1, #0x18]: UARTFR */
	0x372fffe4, /*       TBNZ W4, #5, busy: TXFF */
	0xb9000022, /*       STR  W2, [X1]: UARTDR */
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
	0xd2a7e401, /*        MOVZ X1, #0x3f20, LSL #16 */
	0xf2820001, /*        MOVK X1, #0x1000 */
	0xb900303f, /*        STR  WZR, [X1, #0x30] */
	0x52800e02, /*        MOV  W2, #0x70 */
	0xb9002c22, /*        STR  W2, [X1, #0x2c] */
	0x52806022, /*        MOV  W2, #0x301 */
	0xb9003022, /*        STR  W2, [X1, #0x30] */
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
	0xd2a7e401, /*        MOVZ X1, #0x3f20, LSL #16 */
	0xf2820001, /*        MOVK X1, #0x1000 */
	0xd53800a6, /*        MRS  X6, MPIDR_EL1 */
	0x924004c6, /*        AND  X6, X6, #3: the core number */
	0x1100c0c6, /*        ADD  W6, W6, #'0' */
	0xb9000026, /*        STR  W6, [X1] */
	0xd503207f, /* park:  WFI */
	0x17ffffff, /*        B    park */
};

#define SMP_SEV 15 /* index of the SEV in smp[] */

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

static void test_kernel_is_required(void)
{
	FILE *out = tmpfile();
	BoardOptions options = {.serial = char_file_create(out, false)};
	char error[256];

	/* The board frees the serial back end when it fails. */
	CHECK(raspi3b_board.create(&options, error, sizeof(error)) == NULL);
	CHECK(strcmp(error, "-kernel is required: booting from an SD card "
	                    "image is not implemented yet") == 0);
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

	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
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

static void test_reset_resets_uart(void)
{
	FILE *out;
	Machine *m = boot(hello, ARRAY_SIZE(hello), &out);
	uint64_t value;

	CHECK_EQ(bus_write(m->bus, RASPI3B_UART0_BASE + 0x30, 4, 0x301), BUS_OK);
	machine_reset(m);
	CHECK_EQ(bus_read(m->bus, RASPI3B_UART0_BASE + 0x30, 4, &value), BUS_OK);
	CHECK_EQ(value, 0x300);
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

	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
	machine_reset(m);
	for (unsigned i = 0; i < m->num_cpus; i++) {
		Arm64Cpu *c = arm64_cpu_of(m->cpus[i]);

		CHECK_EQ(c->el, 3);
		CHECK_EQ(c->pc, RASPI3B_STUB_ADDRESS);
	}
	CHECK_EQ(machine_run(m, 100000), MACHINE_STOP_IDLE);
	read_output(out, text, sizeof(text));
	CHECK(strcmp(text, "Hello, world!\nHello, world!\n") == 0);

	machine_destroy(m);
	fclose(out);
	remove(KERNEL);
}

int main(void)
{
	RUN_TEST(test_board_list);
	RUN_TEST(test_kernel_is_required);
	RUN_TEST(test_state_before_the_stub);
	RUN_TEST(test_state_after_the_stub);
	RUN_TEST(test_hello);
	RUN_TEST(test_secondary_cores);
	RUN_TEST(test_shared_l2_registers);
	RUN_TEST(test_reset_resets_uart);
	RUN_TEST(test_reset_runs_the_stub_again);
	return test_summary();
}
