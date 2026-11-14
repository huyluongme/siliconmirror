/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * Unit tests for the LAN95xx Ethernet controller (src/hw/net/lan95xx.c),
 * from the LAN950x datasheet (DS00001875D), with the LAN9514's identity.
 */

#include <stdint.h>
#include <string.h>

#include "core/event.h"
#include "core/machine.h"
#include "hw/net/lan95xx.h"
#include "test.h"
#include "util/byteorder.h"

#define US 1000ULL
#define MS 1000000ULL

enum {
	ID_REV = 0x000,
	INT_STS = 0x008,
	RX_CFG = 0x00c,
	TX_CFG = 0x010,
	HW_CFG = 0x014,
	RX_FIFO_INF = 0x018,
	TX_FIFO_INF = 0x01c,
	PMT_CTL = 0x020,
	LED_GPIO_CFG = 0x024,
	GPIO_CFG = 0x028,
	AFC_CFG = 0x02c,
	E2P_CMD = 0x030,
	E2P_DATA = 0x034,
	BURST_CAP = 0x038,
	DP_SEL = 0x040,
	DP_ADDR = 0x048,
	DP_DATA0 = 0x04c,
	DP_DATA1 = 0x050,
	GPIO_WAKE = 0x064,
	INT_EP_CTL = 0x068,
	BULK_IN_DLY = 0x06c,
	MAC_CR = 0x100,
	ADDRH = 0x104,
	ADDRL = 0x108,
	HASHH = 0x10c,
	HASHL = 0x110,
	MII_ACCESS = 0x114,
	MII_DATA = 0x118,
	VLAN1 = 0x120,
	VLAN2 = 0x124,
	COE_CR = 0x130,
};

static const Lan95xxConfig lan9514 = {
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

typedef struct Setup {
	Machine *m;
	Lan95xx *e;
	UsbDevice *d;
} Setup;

/* What the partner on the cable got. */
static uint8_t sent[2100];
static size_t sent_len;
static unsigned sent_count;

static void partner(void *ctx, const uint8_t *frame, size_t len)
{
	(void)ctx;
	memcpy(sent, frame, len);
	sent_len = len;
	sent_count++;
}

static UsbResult control(UsbDevice *d, uint8_t type, uint8_t request,
                         uint16_t value, uint16_t index, uint16_t length,
                         uint8_t *data, size_t *got)
{
	uint8_t setup[8] = {type,
	                    request,
	                    (uint8_t)value,
	                    (uint8_t)(value >> 8),
	                    (uint8_t)index,
	                    (uint8_t)(index >> 8),
	                    (uint8_t)length,
	                    (uint8_t)(length >> 8)};
	size_t len, done = 0;
	UsbResult r;

	*got = 0;
	CHECK_EQ(usb_device_setup(d, setup), USB_ACK);
	if (type & 0x80) {
		while (done < length) {
			r = usb_device_in(d, 0, data + done, 64, &len);
			if (r != USB_ACK)
				return r;
			done += len;
			if (len < 64)
				break;
		}
		*got = done;
		return usb_device_out(d, 0, NULL, 0);
	}
	if (length != 0) {
		r = usb_device_out(d, 0, data, length);
		if (r != USB_ACK)
			return r;
	}
	return usb_device_in(d, 0, NULL, 0, &len);
}

static Setup setup(bool configure)
{
	Setup s;
	size_t got;

	s.m = machine_create();
	s.e = lan95xx_create(s.m, &lan9514);
	s.d = lan95xx_device(s.e);
	if (configure)
		CHECK_EQ(control(s.d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	sent_count = 0;
	sent_len = 0;
	return s;
}

static void teardown(Setup *s)
{
	lan95xx_destroy(s->e);
	machine_destroy(s->m);
}

static void run(Setup *s, uint64_t ns)
{
	event_queue_run_until(&s->m->events, s->m->events.now + ns);
}

static UsbResult try_read(Setup *s, unsigned addr, uint32_t *v)
{
	uint8_t b[4] = {0};
	size_t got;
	UsbResult r = control(s->d, 0xc0, 0xa1, 0, (uint16_t)addr, 4, b, &got);

	*v = load_le32(b);
	return r;
}

static UsbResult try_write(Setup *s, unsigned addr, uint32_t v)
{
	uint8_t b[4];
	size_t got;

	store_le32(b, v);
	return control(s->d, 0x40, 0xa0, 0, (uint16_t)addr, 4, b, &got);
}

static uint32_t rd(Setup *s, unsigned addr)
{
	uint32_t v;

	CHECK_EQ(try_read(s, addr, &v), USB_ACK);
	return v;
}

static void wr(Setup *s, unsigned addr, uint32_t v)
{
	CHECK_EQ(try_write(s, addr, v), USB_ACK);
}

static uint16_t mdio_read(Setup *s, unsigned index)
{
	wr(s, MII_ACCESS, 1u << 11 | index << 6 | 1u);
	CHECK_EQ(rd(s, MII_ACCESS) & 1, 0);
	return (uint16_t)rd(s, MII_DATA);
}

static void mdio_write(Setup *s, unsigned index, uint16_t v)
{
	wr(s, MII_DATA, v);
	wr(s, MII_ACCESS, 1u << 11 | index << 6 | 2u | 1u);
}

/* The FCS, written here apart from the model's. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

/* A frame to dst of len bytes, FCS included, its payload counting up. */
static size_t make_frame(uint8_t *f, const uint8_t *dst, uint16_t type,
                         size_t len)
{
	static const uint8_t src[6] = {0x02, 0, 0, 0, 0, 0x99};

	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = (uint8_t)(type >> 8);
	f[13] = (uint8_t)type;
	for (size_t i = 14; i < len - 4; i++)
		f[i] = (uint8_t)i;
	store_le32(f + len - 4, crc32(f, len - 4));
	return len;
}

/* A bulk IN transfer: packets until a short one. */
static UsbResult bulk_read(Setup *s, uint8_t *buf, size_t *got)
{
	size_t len;
	UsbResult r;

	*got = 0;
	do {
		r = usb_device_in(s->d, 1, buf + *got, 512, &len);
		if (r != USB_ACK)
			return r;
		*got += len;
	} while (len == 512);
	return USB_ACK;
}

/* A bulk OUT transfer, in packets of 512 bytes. */
static UsbResult bulk_write(Setup *s, const uint8_t *buf, size_t n)
{
	size_t done = 0;
	UsbResult r;

	do {
		size_t len = n - done < 512 ? n - done : 512;

		r = usb_device_out(s->d, 2, buf + done, len);
		if (r != USB_ACK)
			return r;
		done += len;
		if (len < 512)
			break;
	} while (done <= n);
	return USB_ACK;
}

/*
 * PMT_CTL's PHY reset: the write's status stage NAKs until the reset is
 * over, 2 ms later; the host tries again until it is.
 */
static void phy_reset(Setup *s)
{
	size_t len;

	CHECK_EQ(try_write(s, PMT_CTL, 1u << 4), USB_NAK);
	run(s, 2 * MS - 1);
	CHECK_EQ(usb_device_in(s->d, 0, NULL, 0, &len), USB_NAK);
	run(s, 1);
	CHECK_EQ(usb_device_in(s->d, 0, NULL, 0, &len), USB_ACK);
}

/* A plugged-in cable with the link up, and both paths running. */
static void link_up(Setup *s)
{
	lan95xx_plug(s->e, partner, NULL);
	phy_reset(s); /* the PHY powers up after its reset */
	run(s, 2000 * MS);
	CHECK(mdio_read(s, 1) & 0x0004);
	wr(s, MAC_CR, 1u << 18 | 1u << 3 | 1u << 2); /* PRMS, TXEN, RXEN */
	wr(s, TX_CFG, 1u << 2);
}

static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t own[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t almost[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};

/* Descriptors (5.3.1.6), from the LAN9514's defaults; remote wakeup. */
static void test_descriptors(void)
{
	Setup s = setup(false);
	uint8_t buf[64];
	size_t got;
	static const uint8_t device[18] = {18,   1,    0,    2,    0xff, 0,
	                                   1,    64,   0x24, 0x04, 0x00, 0xec,
	                                   0x00, 0x02, 0,    0,    0,    1};
	static const uint8_t config[39] = {
		9,    2, 39, 0, 1, 1, 0, 0xe0, 1, 9, 4, 0, 0, 3, 0xff, 0, 0xff, 0, 7, 5,
		0x81, 2, 0,  2, 0, 7, 5, 2,    2, 0, 2, 0, 7, 5, 0x83, 3, 16,   0, 4,
	};

	CHECK_EQ(control(s.d, 0x80, 6, 0x0100, 0, 18, buf, &got), USB_ACK);
	CHECK(got == 18 && memcmp(buf, device, 18) == 0);
	CHECK_EQ(control(s.d, 0x80, 6, 0x0200, 0, 64, buf, &got), USB_ACK);
	CHECK(got == 39 && memcmp(buf, config, 39) == 0);
	/* No strings without an EEPROM. */
	CHECK_EQ(control(s.d, 0x80, 6, 0x0300, 0, 64, buf, &got), USB_STALL);
	/* Self-powered; remote wakeup after SET_FEATURE (9.4.5). */
	CHECK_EQ(control(s.d, 0x80, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 1);
	CHECK_EQ(control(s.d, 0x00, 3, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(s.d, 0x80, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 3);
	CHECK_EQ(control(s.d, 0x00, 1, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(control(s.d, 0x80, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 1);
	CHECK_EQ(control(s.d, 0x00, 3, 2, 0, 0, NULL, &got), USB_STALL);
	CHECK_EQ(control(s.d, 0x00, 3, 1, 0, 0, NULL, &got), USB_ACK);
	usb_device_reset(s.d); /* a bus reset clears it */
	CHECK_EQ(control(s.d, 0x80, 0, 0, 0, 2, buf, &got), USB_ACK);
	CHECK_EQ(buf[0], 1);
	teardown(&s);
}

/* The registers' reset values and writable bits (7.3, 7.4). */
static void test_registers(void)
{
	Setup s = setup(false);
	uint32_t v;

	CHECK_EQ(rd(&s, ID_REV), 0xec000000u);
	CHECK_EQ(rd(&s, PMT_CTL), 0x000001c0u); /* READY */
	CHECK_EQ(rd(&s, HW_CFG), 0);
	CHECK_EQ(rd(&s, TX_FIFO_INF), 0x2000);
	CHECK_EQ(rd(&s, RX_FIFO_INF), 0);
	CHECK_EQ(rd(&s, BULK_IN_DLY), 0x800);
	CHECK_EQ(rd(&s, GPIO_CFG), 0xff0000ffu); /* inputs, pulled up */
	CHECK_EQ(rd(&s, LED_GPIO_CFG), 0x7);
	CHECK_EQ(rd(&s, DP_SEL), 0x80000000u);
	/* MAC CSRs stall until the device is configured (5.3.3). */
	CHECK_EQ(try_read(&s, MAC_CR, &v), USB_STALL);
	CHECK_EQ(control(s.d, 0, 9, 1, 0, 0, NULL, &(size_t){0}), USB_ACK);
	CHECK_EQ(rd(&s, MAC_CR), 0x00040000u); /* PRMS */
	CHECK_EQ(rd(&s, ADDRH), 0xffff);
	CHECK_EQ(rd(&s, ADDRL), 0xffffffffu);
	CHECK_EQ(rd(&s, VLAN1), 0xffff);

	wr(&s, AFC_CFG, 0xffffffffu);
	CHECK_EQ(rd(&s, AFC_CFG), 0x00ffffffu);
	wr(&s, MAC_CR, 0xffffffffu & ~(1u << 21));
	CHECK_EQ(rd(&s, MAC_CR), 0x809fbdecu);
	wr(&s, HW_CFG, 0xfffffff6u); /* not LRST, not SRST */
	CHECK_EQ(rd(&s, HW_CFG), 0x1ff2);
	wr(&s, ID_REV, 0);
	CHECK_EQ(rd(&s, ID_REV), 0xec000000u);
	/* GPIO outputs read what was written. */
	wr(&s, GPIO_CFG, 0x0000f00au);
	CHECK_EQ(rd(&s, GPIO_CFG), 0x0000f00fu);
	wr(&s, LED_GPIO_CFG, 0x00000010u);
	CHECK_EQ(rd(&s, LED_GPIO_CFG), 0x00000016u);
	/* PMT_CTL: WUPS is write 1 to clear; READY is read-only. */
	wr(&s, PMT_CTL, 0x00000003u);
	CHECK_EQ(rd(&s, PMT_CTL), 0x00000080u);

	/* The vendor requests' format (Tables 5-33 to 5-36). */
	CHECK_EQ(control(s.d, 0xc0, 0xa1, 0, 0, 2, (uint8_t[4]){0}, &(size_t){0}),
	         USB_STALL);
	CHECK_EQ(control(s.d, 0xc0, 0xa1, 1, 0, 4, (uint8_t[4]){0}, &(size_t){0}),
	         USB_STALL);
	CHECK_EQ(
		control(s.d, 0xc0, 0xa1, 0, 0x1000, 4, (uint8_t[4]){0}, &(size_t){0}),
		USB_STALL);
	CHECK_EQ(control(s.d, 0xc1, 0xa1, 0, 0, 4, (uint8_t[4]){0}, &(size_t){0}),
	         USB_STALL);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* Each register keeps its own bits, and changes no other (7.3). */
static void test_register_bits(void)
{
	static const struct {
		unsigned addr;
		uint32_t bits, fixed;
	} regs[] = {
		{AFC_CFG, 0x00ffffffu, 0},    {E2P_DATA, 0xff, 0},
		{BURST_CAP, 0xff, 0},         {DP_SEL, 0x7, 0x80000000u}, /* DPRDY */
		{DP_ADDR, 0x7fff, 0},         {DP_DATA0, 0xffffffffu, 0},
		{DP_DATA1, 0x1f, 0},          {GPIO_WAKE, 0x07ff07ffu, 0},
		{INT_EP_CTL, 0x800fffffu, 0}, {BULK_IN_DLY, 0xffff, 0},
		{ADDRH, 0xffff, 0},           {ADDRL, 0xffffffffu, 0},
		{HASHH, 0xffffffffu, 0},      {HASHL, 0xffffffffu, 0},
		{MII_DATA, 0xffff, 0},        {VLAN1, 0xffff, 0},
		{VLAN2, 0xffff, 0},
	};
	const size_t n = sizeof(regs) / sizeof(regs[0]);
	Setup s = setup(true);

	for (size_t i = 0; i < n; i++)
		wr(&s, regs[i].addr, 0);
	for (size_t i = 0; i < n; i++) {
		wr(&s, regs[i].addr, 0xffffffffu);
		for (size_t j = 0; j < n; j++)
			CHECK_EQ(rd(&s, regs[j].addr),
			         regs[j].fixed | (i == j ? regs[j].bits : 0));
		wr(&s, regs[i].addr, 0);
	}
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What stops the machine: reserved registers and missing features. */
static void test_unmodelled(void)
{
	static const struct {
		unsigned addr;
		uint32_t value;
		bool write;
		const char *message;
	} stops[] = {
		{0x004, 0, false, "reserved register"},
		{0x0a0, 0, false, "reserved register"},
		{0x200, 0, true, "reserved register"},
		{0x009, 0, false, "reserved register"},
		{0x134, 0, false, "reserved register"},
		{0x134, 0, true, "reserved register"},
		{0x070, 0, false, "debug registers"},
		{0x07c, 0, false, "debug registers"},
		{0x044, 1, true, "data port"},
		{HW_CFG, 1, true, "soft reset"},
		{0x11c, 1, true, "pause frames"},
		{0x128, 0, true, "wake-up frame filter"},
		{0x12c, 4, true, "wake-up detection"},
		{COE_CR, 1, true, "checksum offload"},
		{MII_ACCESS, 2u << 11 | 1, true, "no PHY"},
		{MII_ACCESS, 1u << 11 | 7u << 6 | 1, true, "PHY register"},
	};

	for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		Setup s = setup(true);
		uint32_t v;

		if (stops[i].write)
			try_write(&s, stops[i].addr, stops[i].value);
		else
			try_read(&s, stops[i].addr, &v);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, "lan95xx: ") != NULL);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}
}

/* The EEPROM controller with no EEPROM (5.7.4, 7.3.12). */
static void test_eeprom(void)
{
	Setup s = setup(true);
	uint32_t v;

	CHECK_EQ(rd(&s, E2P_CMD), 0); /* tried after the reset: not loaded */
	wr(&s, E2P_DATA, 0x5a);
	wr(&s, E2P_CMD, 0x80000000u | 0x1ff); /* READ 0x1ff */
	CHECK_EQ(rd(&s, E2P_CMD), 0x800001ffu);
	run(&s, 20 * 1120 - 1);
	CHECK(rd(&s, E2P_CMD) & 0x80000000u);
	run(&s, 1);
	CHECK_EQ(rd(&s, E2P_CMD), 0x000001ffu); /* no time-out */
	CHECK_EQ(rd(&s, E2P_DATA), 0);          /* the data line reads low */

	/* RELOAD finds no 0xA5: Data Loaded stays clear. */
	wr(&s, E2P_CMD, 0xf0000000u);
	run(&s, 20 * 1120 - 1);
	CHECK(rd(&s, E2P_CMD) & 0x80000000u);
	run(&s, 1);
	CHECK_EQ(rd(&s, E2P_CMD), 0x70000000u);

	/* A write times out after 30 ms; the time-out bit clears with a 1. */
	wr(&s, E2P_CMD, 0xb0000010u);
	run(&s, 30 * MS - 1);
	CHECK(rd(&s, E2P_CMD) & 0x80000000u);
	run(&s, 1);
	CHECK_EQ(rd(&s, E2P_CMD), 0x30000410u);
	wr(&s, E2P_CMD, 0x00000400u);
	CHECK_EQ(rd(&s, E2P_CMD), 0);
	/* With ETC, after 1.28 us. */
	wr(&s, HW_CFG, 1u << 4);
	wr(&s, E2P_CMD, 0xe0000000u); /* ERAL */
	run(&s, 1280);
	CHECK_EQ(rd(&s, E2P_CMD), 0x60000400u);
	/* EWEN is quick and does not time out. */
	wr(&s, E2P_CMD, 0x00000400u);
	wr(&s, E2P_CMD, 0xa0000000u);
	run(&s, 12 * 1120 - 1);
	CHECK(rd(&s, E2P_CMD) & 0x80000000u);
	run(&s, 1);
	CHECK_EQ(rd(&s, E2P_CMD), 0x20000000u);

	/* A command while one runs stops the machine. */
	wr(&s, E2P_CMD, 0x80000000u);
	CHECK_EQ(try_write(&s, E2P_CMD, 0x80000000u), USB_STALL);
	CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
	(void)v;
	teardown(&s);
}

/* The lite reset, the PHY's resets and the USB reset (5.13). */
static void test_resets(void)
{
	Setup s = setup(true);
	uint8_t b[4];
	size_t len;

	/* After a USB reset the PHY is powered down (5.13.5). */
	CHECK(mdio_read(&s, 0) & 0x0800);
	CHECK_EQ(mdio_read(&s, 0), 0x3800);

	/*
	 * PMT_CTL's PHY reset: 2 ms, every transfer NAKs meanwhile. A new
	 * SETUP is taken; its data comes when the device answers.
	 */
	CHECK_EQ(try_write(&s, PMT_CTL, 1u << 4), USB_NAK);
	{
		uint8_t setup_read[8] = {0xc0, 0xa1, 0, 0, PMT_CTL, 0, 4, 0};

		CHECK_EQ(usb_device_setup(s.d, setup_read), USB_ACK);
		CHECK_EQ(usb_device_in(s.d, 0, b, 64, &len), USB_NAK);
		run(&s, 2 * MS - 1);
		CHECK_EQ(usb_device_in(s.d, 0, b, 64, &len), USB_NAK);
		run(&s, 1);
		CHECK_EQ(usb_device_in(s.d, 0, b, 64, &len), USB_ACK);
		CHECK_EQ(load_le32(b), 0x00000080u); /* the write cleared 8, 6 */
		CHECK_EQ(usb_device_out(s.d, 0, NULL, 0), USB_ACK);
	}
	CHECK_EQ(mdio_read(&s, 0), 0x3000); /* powered up */

	/* The soft reset of BMCR: bit 15 reads 1 for 256 us. */
	mdio_write(&s, 4, 0x0061);
	mdio_write(&s, 0, 0x8000);
	CHECK_EQ(mdio_read(&s, 0), 0xb000);
	run(&s, 256 * US);
	CHECK_EQ(mdio_read(&s, 0), 0x3000);
	CHECK_EQ(mdio_read(&s, 4), 0x01e1);

	/* The lite reset: registers to their defaults, at once. */
	wr(&s, MAC_CR, 0x0c);
	wr(&s, BURST_CAP, 37);
	wr(&s, HW_CFG, 1u << 3 | 1u << 5);
	CHECK_EQ(rd(&s, HW_CFG), 0);
	CHECK_EQ(rd(&s, MAC_CR), 0x00040000u);
	CHECK_EQ(rd(&s, BURST_CAP), 0);
	CHECK_EQ(mdio_read(&s, 0), 0x3000); /* not powered down */

	/* A USB reset: unconfigured, the PHY down again. */
	lan95xx_reset(s.e);
	CHECK_EQ(s.d->config, 0);
	CHECK_EQ(control(s.d, 0, 9, 1, 0, 0, NULL, &len), USB_ACK);
	CHECK_EQ(mdio_read(&s, 0), 0x3800);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/*
 * The lite reset: the FIFOs empty, the counters clear, the EEPROM and
 * the PHY reset (5.13.2); a USB reset ends the PHY's resets.
 */
static void test_lite_reset(void)
{
	Setup s = setup(true);
	uint8_t frame[100], buf[64], c[40];
	size_t got;

	link_up(&s);
	make_frame(frame, own, 0x0800, 100);
	CHECK(lan95xx_receive(s.e, frame, 100));
	frame[50] ^= 1;
	CHECK(lan95xx_receive(s.e, frame, 100));
	/* A frame sent, and the start of another. */
	store_le32(buf, 42u | 1u << 13 | 1u << 12);
	store_le32(buf + 4, 42);
	memcpy(buf + 8, frame, 42);
	CHECK_EQ(bulk_write(&s, buf, 50), USB_ACK);
	CHECK_EQ(sent_count, 1);
	store_le32(buf, 100u | 1u << 13 | 1u << 12);
	store_le32(buf + 4, 100);
	CHECK_EQ(bulk_write(&s, buf, 16), USB_ACK);
	CHECK(rd(&s, TX_FIFO_INF) < 0x2000);
	CHECK_EQ(rd(&s, RX_FIFO_INF), 208);
	wr(&s, E2P_CMD, 0xb0000010u); /* a write, to time out in 30 ms */

	wr(&s, HW_CFG, 1u << 3);
	CHECK_EQ(rd(&s, RX_FIFO_INF), 0);
	CHECK_EQ(rd(&s, TX_FIFO_INF), 0x2000);
	CHECK_EQ(control(s.d, 0xc0, 0xa2, 0, 0, 32, c, &got), USB_ACK);
	for (int i = 0; i < 32; i++)
		CHECK_EQ(c[i], 0);
	CHECK_EQ(control(s.d, 0xc0, 0xa2, 0, 1, 40, c, &got), USB_ACK);
	for (int i = 0; i < 40; i++)
		CHECK_EQ(c[i], 0);
	CHECK_EQ(rd(&s, E2P_CMD), 0);
	run(&s, 30 * MS);
	CHECK_EQ(rd(&s, E2P_CMD), 0);
	/* The PHY starts again: the link, its latch clear. */
	run(&s, 2000 * MS);
	CHECK(mdio_read(&s, 1) & 0x0004);

	/* A USB reset during PMT_CTL's PHY reset, or BMCR's. */
	CHECK_EQ(try_write(&s, PMT_CTL, 1u << 4), USB_NAK);
	lan95xx_reset(s.e);
	CHECK_EQ(control(s.d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	phy_reset(&s);
	mdio_write(&s, 0, 0x8000);
	lan95xx_reset(s.e);
	CHECK_EQ(control(s.d, 0, 9, 1, 0, 0, NULL, &got), USB_ACK);
	CHECK_EQ(mdio_read(&s, 0), 0x3800);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* The PHY: identity, link with and without a cable, interrupts (7.5). */
static void test_phy(void)
{
	Setup s = setup(true);

	CHECK_EQ(mdio_read(&s, 2), 0x0007);
	CHECK_EQ(mdio_read(&s, 3), 0xc0c3);
	CHECK_EQ(mdio_read(&s, 18), 0x00e1);
	CHECK_EQ(mdio_read(&s, 17), 0x0002); /* ENERGYON after the reset */
	run(&s, 256 * MS);
	CHECK_EQ(mdio_read(&s, 17), 0x0000); /* no cable: no energy */
	phy_reset(&s);
	CHECK_EQ(mdio_read(&s, 1), 0x7809);
	run(&s, 5000 * MS);
	CHECK_EQ(mdio_read(&s, 1), 0x7809); /* never a link */

	/* A cable: ENERGYON, then the link after negotiation. */
	mdio_write(&s, 30, 0x00d0); /* ENERGYON, AN done, link down */
	CHECK_EQ(mdio_read(&s, 29), 0);
	lan95xx_plug(s.e, partner, NULL);
	CHECK_EQ(mdio_read(&s, 17), 0x0002);
	CHECK(rd(&s, INT_STS) & 1u << 15); /* PHY_INT */
	CHECK_EQ(mdio_read(&s, 29), 0x0080);
	CHECK_EQ(rd(&s, INT_STS) & 1u << 15, 0);
	run(&s, 2000 * MS - 1);
	CHECK_EQ(mdio_read(&s, 1), 0x7809);
	run(&s, 1);
	CHECK_EQ(mdio_read(&s, 1), 0x782d);
	CHECK_EQ(mdio_read(&s, 5), 0x41e1);
	CHECK_EQ(mdio_read(&s, 6), 0x0003);
	CHECK_EQ(mdio_read(&s, 6), 0x0001);  /* page received: latched high */
	CHECK_EQ(mdio_read(&s, 31), 0x1058); /* autodone, 100 full */
	CHECK_EQ(mdio_read(&s, 29), 0x004a);

	/* 10 Mb/s half duplex only: what the advertisement leaves. */
	mdio_write(&s, 4, 0x0021);
	mdio_write(&s, 0, 0x1200);
	CHECK_EQ(mdio_read(&s, 1) & 0x0024, 0);
	run(&s, 2000 * MS);
	CHECK_EQ(mdio_read(&s, 31), 0x1044);
	/* Forced 100 Mb/s full duplex: no negotiation. */
	mdio_write(&s, 0, 0x2100);
	run(&s, 2000 * MS);
	CHECK_EQ(mdio_read(&s, 1), 0x7809); /* latched low: the link fell */
	CHECK_EQ(mdio_read(&s, 1), 0x780d);
	CHECK_EQ(mdio_read(&s, 31), 0x0058);
	CHECK_EQ(mdio_read(&s, 5), 0);

	/* Unplugged: link status latches low until read; INT4. */
	mdio_read(&s, 29);
	lan95xx_plug(s.e, NULL, NULL);
	CHECK_EQ(mdio_read(&s, 29), 0x0010);
	CHECK_EQ(mdio_read(&s, 1), 0x7809);
	run(&s, 256 * MS);
	CHECK_EQ(mdio_read(&s, 17), 0);
	/* Powered down, a cable brings no link. */
	mdio_write(&s, 0, 0x3800);
	lan95xx_plug(s.e, partner, NULL);
	run(&s, 3000 * MS);
	CHECK_EQ(mdio_read(&s, 1) & 0x0004, 0);
	mdio_write(&s, 0, 0x3000); /* up again: a reset, then the link */
	run(&s, 256 * US + 2000 * MS);
	CHECK(mdio_read(&s, 1) & 0x0004);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* The PHY's registers: their writable bits, the read-only ones (7.5). */
static void test_phy_registers(void)
{
	static const struct {
		unsigned index;
		uint16_t bits;
	} regs[] = {
		{2, 0xffff},  {3, 0xffff},  {4, 0x2dff},  {17, 0x2001},
		{27, 0xe400}, {30, 0x00ff}, {31, 0x0fe0},
	};
	static const unsigned read_only[] = {1, 5, 6, 29};
	const size_t n = sizeof(regs) / sizeof(regs[0]);
	uint16_t base[32];
	Setup s = setup(true);

	phy_reset(&s);
	run(&s, 256 * MS); /* no cable: ENERGYON has fallen */
	for (size_t i = 0; i < n; i++)
		mdio_write(&s, regs[i].index, 0);
	for (size_t i = 0; i < n; i++)
		base[regs[i].index] = mdio_read(&s, regs[i].index);
	for (size_t i = 0; i < n; i++) {
		mdio_write(&s, regs[i].index, 0xffff);
		for (size_t j = 0; j < n; j++)
			CHECK_EQ(mdio_read(&s, regs[j].index),
			         base[regs[j].index] | (i == j ? regs[j].bits : 0));
		mdio_write(&s, regs[i].index, 0);
	}
	for (size_t i = 0; i < sizeof(read_only) / sizeof(read_only[0]); i++) {
		uint16_t v = mdio_read(&s, read_only[i]);

		mdio_write(&s, read_only[i], 0xffff);
		CHECK_EQ(mdio_read(&s, read_only[i]), v);
	}
	/* BMCR's reserved bits read 0; SMR keeps 8 bits. */
	mdio_write(&s, 0, 0x3101);
	CHECK_EQ(mdio_read(&s, 0), 0x3100);
	mdio_write(&s, 18, 0xff41);
	CHECK_EQ(mdio_read(&s, 18), 0x0041);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* A soft reset: the registers from SMR's MODE (Table 7-7). */
static void test_phy_modes(void)
{
	static const uint16_t bmcr[8] = {0x0000, 0x0100, 0x2000, 0x2100,
	                                 0x3000, 0x3000, 0,      0x3000};
	Setup s = setup(true);

	phy_reset(&s);
	for (unsigned mode = 0; mode < 8; mode++) {
		if (mode == 6)
			continue;
		mdio_write(&s, 18, (uint16_t)(mode << 5 | 1));
		mdio_write(&s, 0, 0x8000);
		run(&s, 256 * US);
		CHECK_EQ(mdio_read(&s, 0), bmcr[mode]);
		CHECK_EQ(mdio_read(&s, 4), mode == 4 || mode == 5 ? 0x0081 : 0x01e1);
		CHECK_EQ(mdio_read(&s, 18), mode << 5 | 1);
	}
	/* What a soft reset clears; ENERGYON stays, with a cable. */
	lan95xx_plug(s.e, partner, NULL);
	mdio_write(&s, 17, 0x2001);
	mdio_write(&s, 27, 0xe400);
	mdio_write(&s, 30, 0x00ff);
	mdio_write(&s, 0, 0x8000);
	run(&s, 256 * US);
	CHECK_EQ(mdio_read(&s, 17), 0x0002);
	CHECK_EQ(mdio_read(&s, 27), 0);
	CHECK_EQ(mdio_read(&s, 29), 0);
	CHECK_EQ(mdio_read(&s, 30), 0);
	lan95xx_plug(s.e, NULL, NULL);
	run(&s, 256 * MS);
	mdio_write(&s, 0, 0x8000);
	run(&s, 256 * US);
	CHECK_EQ(mdio_read(&s, 17), 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* What the PHY does not model stops the machine. */
static void test_phy_unmodelled(void)
{
	static const struct {
		unsigned index;
		uint16_t value;
		const char *message;
	} stops[] = {
		{0, 0x4000, "PHY loopback"},
		{0, 0x0080, "collision test"},
		{7, 0, "PHY register not modelled"},
		{18, 0x00c1, "reserved mode"}, /* then the soft reset */
	};

	for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		Setup s = setup(true);

		phy_reset(&s);
		mdio_write(&s, stops[i].index, stops[i].value);
		if (stops[i].index == 18)
			mdio_write(&s, 0, 0x8000);
		CHECK_EQ(s.m->stop, MACHINE_STOP_ERROR);
		CHECK(strstr(s.m->stop_message, stops[i].message) != NULL);
		teardown(&s);
	}
}

/* Speed and duplex as negotiated or forced; what restarts the link. */
static void test_phy_link(void)
{
	static const struct {
		uint16_t anar, bmcr;
		unsigned speed; /* PSCSR's HCDSPEED */
	} modes[] = {
		{0x01e1, 0x1200, 6}, {0x0081, 0x1200, 2}, {0x0041, 0x1200, 5},
		{0x0021, 0x1200, 1}, {0x01e1, 0x2100, 6}, {0x01e1, 0x0100, 5},
		{0x01e1, 0x0000, 1}, {0x01e1, 0x2000, 2},
	};
	Setup s = setup(true);

	link_up(&s);
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		mdio_write(&s, 4, modes[i].anar);
		mdio_write(&s, 0, modes[i].bmcr);
		CHECK_EQ(mdio_read(&s, 1) & 0x0004, 0);
		run(&s, 2000 * MS);
		CHECK(mdio_read(&s, 1) & 0x0004);
		CHECK_EQ(mdio_read(&s, 31) >> 2 & 7, modes[i].speed);
	}
	/* The same BMCR again, or ANRESTART without AN: no restart. */
	mdio_write(&s, 0, 0x2000);
	CHECK(mdio_read(&s, 1) & 0x0004);
	mdio_write(&s, 0, 0x2200);
	CHECK(mdio_read(&s, 1) & 0x0004);
	/* No ability in common: no link. */
	mdio_write(&s, 4, 0x0001);
	mdio_write(&s, 0, 0x1200);
	CHECK_EQ(mdio_read(&s, 6), 0);
	run(&s, 3000 * MS);
	mdio_read(&s, 1);
	CHECK_EQ(mdio_read(&s, 1) & 0x0004, 0);
	/* Powered down, the link falls. */
	mdio_write(&s, 4, 0x01e1);
	mdio_write(&s, 0, 0x1200);
	run(&s, 2000 * MS);
	CHECK(mdio_read(&s, 1) & 0x0004);
	CHECK(mdio_read(&s, 1) & 0x0004);
	mdio_write(&s, 0, 0x1800);
	mdio_read(&s, 1);
	CHECK_EQ(mdio_read(&s, 1) & 0x0004, 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* TX: commands A and B, padding and FCS, errors (5.4.2). */
static void test_tx(void)
{
	Setup s = setup(true);
	uint8_t buf[1600], frame[1600];
	size_t n;

	link_up(&s);
	/* One buffer: 42 bytes, padded to 60, then the FCS. */
	make_frame(frame, bcast, 0x0806, 46);
	store_le32(buf, 42u | 1u << 13 | 1u << 12);
	store_le32(buf + 4, 42);
	memcpy(buf + 8, frame, 42);
	CHECK_EQ(bulk_write(&s, buf, 50), USB_ACK);
	CHECK_EQ(sent_count, 1);
	CHECK_EQ(sent_len, 64);
	CHECK(memcmp(sent, frame, 42) == 0);
	for (n = 42; n < 60; n++)
		CHECK_EQ(sent[n], 0);
	CHECK_EQ(load_le32(sent + 60), crc32(sent, 60));
	CHECK_EQ(crc32((const uint8_t *)"123456789", 9), 0xcbf43926u);

	/* Two buffers, the second with a data offset; no FCS added. */
	make_frame(frame, own, 0x0800, 1000);
	store_le32(buf, 600u | 1u << 13);
	store_le32(buf + 4, 1000u | 1u << 13);
	memcpy(buf + 8, frame, 600);
	store_le32(buf + 608, 400u | 2u << 16 | 1u << 12);
	store_le32(buf + 612, 1000u | 1u << 13 | 1u << 14);
	memcpy(buf + 618, frame + 600, 400);
	CHECK_EQ(bulk_write(&s, buf, 1018), USB_ACK);
	CHECK_EQ(sent_count, 2);
	CHECK(sent_len == 1000 && memcmp(sent, frame, 1000) == 0);
	CHECK_EQ(rd(&s, TX_FIFO_INF), 0x2000);

	/* Statistics: two good frames, cleared when read (5.3.3.3). */
	{
		uint8_t st[40];
		size_t got;

		CHECK_EQ(control(s.d, 0xc0, 0xa2, 0, 1, 40, st, &got), USB_ACK);
		CHECK(got == 40 && load_le32(st) == 2);
		CHECK_EQ(control(s.d, 0xc0, 0xa2, 0, 1, 40, st, &got), USB_ACK);
		CHECK_EQ(load_le32(st), 0);
	}

	/* A missing first segment: TXE, and bulk OUT stalls. */
	store_le32(buf, 10u | 1u << 12);
	store_le32(buf + 4, 10);
	CHECK_EQ(bulk_write(&s, buf, 18), USB_ACK);
	CHECK(rd(&s, INT_STS) & 1u << 14);
	CHECK_EQ(usb_device_out(s.d, 2, buf, 18), USB_STALL);
	CHECK_EQ(control(s.d, 0x02, 1, 0, 0x02, 0, NULL, &n), USB_ACK);
	wr(&s, INT_STS, 1u << 14);
	CHECK_EQ(rd(&s, INT_STS), 0);
	/* With SBP the pipe goes on; a changed command B, a zero size. */
	wr(&s, HW_CFG, 1u << 8);
	store_le32(buf, 4u | 1u << 13);
	store_le32(buf + 4, 8);
	store_le32(buf + 12, 4u | 1u << 12);
	store_le32(buf + 16, 9);
	CHECK_EQ(bulk_write(&s, buf, 24), USB_ACK);
	CHECK(rd(&s, INT_STS) & 1u << 14);
	wr(&s, INT_STS, 1u << 14);
	store_le32(buf, 0u | 3u << 12);
	CHECK_EQ(bulk_write(&s, buf, 8), USB_ACK);
	CHECK(rd(&s, INT_STS) & 1u << 14);
	CHECK_EQ(sent_count, 2);

	/* The transmitter off: the FIFO fills, then bulk OUT NAKs. */
	wr(&s, TX_CFG, 1u << 1); /* STOP_TX */
	CHECK_EQ(rd(&s, TX_CFG), 0);
	CHECK(rd(&s, INT_STS) & 1u << 17);
	memset(buf, 0, 512);
	store_le32(buf, 504u | 3u << 12); /* a packet, a buffer, a frame */
	store_le32(buf + 4, 504);
	for (n = 0; n < 15; n++)
		CHECK_EQ(usb_device_out(s.d, 2, buf, 512), USB_ACK);
	CHECK_EQ(rd(&s, TX_FIFO_INF), 512);
	CHECK_EQ(usb_device_out(s.d, 2, buf, 512), USB_ACK);
	CHECK_EQ(usb_device_out(s.d, 2, buf, 512), USB_NAK);
	wr(&s, TX_CFG, 1u << 2);
	CHECK_EQ(sent_count, 18);
	CHECK_EQ(rd(&s, TX_FIFO_INF), 0x2000);
	/* Unplugged: frames are lost, as carrier errors. */
	lan95xx_plug(s.e, NULL, NULL);
	CHECK_EQ(usb_device_out(s.d, 2, buf, 512), USB_ACK);
	CHECK_EQ(sent_count, 18);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* RX: the status word, SEF and MEF bursts, the FIFO (5.4.1, 5.3.1.1). */
static void test_rx(void)
{
	Setup s = setup(true);
	uint8_t frame[2100], buf[20480];
	size_t n, got;
	uint32_t st;

	/* Nothing before the link and the receiver are up. */
	make_frame(frame, bcast, 0x0800, 64);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	link_up(&s);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 0); /* a ZLP when empty */
	wr(&s, HW_CFG, 1u << 12);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_NAK); /* BIR */

	CHECK(lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(rd(&s, RX_FIFO_INF), 68);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 68);
	st = load_le32(buf);
	CHECK_EQ(st, 64u << 16 | 1u << 13 | 1u << 5); /* broadcast, type */
	CHECK(memcmp(buf + 4, frame, 64) == 0);

	/* RXDOFF puts bytes between the status word and the frame. */
	wr(&s, HW_CFG, 2u << 9 | 1u << 12);
	CHECK(lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK(got == 70 && memcmp(buf + 6, frame, 64) == 0);

	/* A frame of 508 bytes: 512 with its status word, then a ZLP. */
	make_frame(frame, bcast, 0x0800, 508);
	wr(&s, HW_CFG, 1u << 12);
	CHECK(lan95xx_receive(s.e, frame, 508));
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_ACK);
	CHECK_EQ(n, 512);
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_ACK);
	CHECK_EQ(n, 0);
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_NAK);

	/* SEF: a frame a burst; MEF: all, from DWORD boundaries. */
	make_frame(frame, bcast, 0x0800, 65);
	CHECK(lan95xx_receive(s.e, frame, 65));
	CHECK(lan95xx_receive(s.e, frame, 65));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 69);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 69);
	wr(&s, HW_CFG, 1u << 12 | 1u << 5);
	for (int i = 0; i < 3; i++)
		CHECK(lan95xx_receive(s.e, frame, 65));
	CHECK_EQ(rd(&s, RX_FIFO_INF), 3 * 72);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 72 + 72 + 69);
	CHECK_EQ(load_le32(buf + 72) >> 16, 65);
	CHECK(memcmp(buf + 148, frame, 65) == 0);

	/* The burst cap: whole frames up to BURST_CAP * 512 bytes. */
	make_frame(frame, bcast, 0x0800, 1514);
	for (int i = 0; i < 3; i++)
		CHECK(lan95xx_receive(s.e, frame, 1514));
	wr(&s, BURST_CAP, 5);
	wr(&s, HW_CFG, 1u << 12 | 1u << 5 | 1u << 1);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 1518);
	wr(&s, BURST_CAP, 6);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 1520 + 1518);

	/* Errors: a bad FCS; a runt goes only with PASSBAD; DRP drops. */
	make_frame(frame, bcast, 0x0800, 100);
	frame[50] ^= 1;
	CHECK(lan95xx_receive(s.e, frame, 100));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(load_le32(buf),
	         100u << 16 | 1u << 15 | 1u << 13 | 1u << 5 | 1u << 1);
	CHECK(!lan95xx_receive(s.e, frame, 40));
	wr(&s, MAC_CR, 1u << 18 | 1u << 16 | 0xc);
	CHECK(lan95xx_receive(s.e, frame, 40));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(load_le32(buf) & (1u << 15 | 1u << 11), 1u << 15 | 1u << 11);
	wr(&s, HW_CFG, 1u << 12 | 1u << 6);
	CHECK(!lan95xx_receive(s.e, frame, 100));
	CHECK(rd(&s, INT_STS) & 1u << 11);
	wr(&s, INT_STS, 1u << 11);
	/* Too long: 1519 bytes; 1522 with a VLAN tag that VLAN1 names. */
	make_frame(frame, bcast, 0x8100, 1522);
	wr(&s, HW_CFG, 1u << 12);
	CHECK(lan95xx_receive(s.e, frame, 1522));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK(load_le32(buf) & 1u << 7);
	wr(&s, VLAN1, 0x8100);
	CHECK(lan95xx_receive(s.e, frame, 1522));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(load_le32(buf) & (1u << 15 | 1u << 7), 0);
	/* Pad stripping: an 802.3 frame with a length of 20. */
	make_frame(frame, bcast, 20, 64);
	wr(&s, MAC_CR, 1u << 18 | 1u << 8 | 0xc);
	CHECK(lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(load_le32(buf) >> 16, 34);

	/* The FIFO's 20 KB: the frame that does not fit is dropped. */
	make_frame(frame, bcast, 0x0800, 1514);
	for (n = 0; n < 13; n++)
		CHECK(lan95xx_receive(s.e, frame, 1514));
	CHECK(!lan95xx_receive(s.e, frame, 1514));
	CHECK(rd(&s, INT_STS) & 1u << 11);
	{
		uint8_t stats[32];

		CHECK_EQ(control(s.d, 0xc0, 0xa2, 0, 0, 32, stats, &got), USB_ACK);
		CHECK_EQ(got, 32);
		CHECK_EQ(load_le32(stats + 28), 1); /* FIFO dropped */
		CHECK_EQ(load_le32(stats + 4), 2);  /* CRC errors */
	}
	wr(&s, RX_CFG, 1); /* flush */
	CHECK_EQ(rd(&s, RX_FIFO_INF), 0);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_NAK);
	/* RXEN off: RXSTOP_INT, and nothing comes in. */
	wr(&s, MAC_CR, 1u << 18 | 0x8);
	CHECK(rd(&s, INT_STS) & 1u << 16);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* A frame to dst of len bytes; under 18 bytes, without its FCS. */
static size_t any_frame(uint8_t *f, const uint8_t *dst, uint16_t type,
                        size_t len)
{
	if (len >= 18)
		return make_frame(f, dst, type, len);
	memset(f, 0, 18);
	memcpy(f, dst, 6);
	f[12] = (uint8_t)(type >> 8);
	f[13] = (uint8_t)type;
	return len;
}

/* Receive a frame and read it back: its status word, frame in buf. */
static uint32_t rx_one(Setup *s, const uint8_t *frame, size_t len, uint8_t *buf)
{
	size_t got;

	CHECK(lan95xx_receive(s->e, frame, len));
	CHECK_EQ(bulk_read(s, buf, &got), USB_ACK);
	CHECK(got >= 4);
	return load_le32(buf);
}

/* The receive counters (Table 5-14), read and so cleared. */
static void rx_counters(Setup *s, uint32_t *c)
{
	uint8_t b[32];
	size_t got;

	CHECK_EQ(control(s->d, 0xc0, 0xa2, 0, 0, 32, b, &got), USB_ACK);
	CHECK_EQ(got, 32);
	for (int i = 0; i < 8; i++)
		c[i] = load_le32(b + 4 * i);
}

/* The RX status word (Table 5-40), the counters, and pad stripping. */
static void test_rx_status(void)
{
	static const uint8_t multi[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
	static const struct {
		const uint8_t *dst;
		uint16_t type;
		size_t len;
		uint32_t status; /* but the length */
	} frames[] = {
		{own, 0x0800, 64, 1u << 5},
		{multi, 0x0800, 64, 1u << 10 | 1u << 5},
		{almost, 0x0800, 64, 1u << 10 | 1u << 5},
		/* A length field: up to 46 bytes of data are padded. */
		{own, 82, 100, 0},
		{own, 83, 100, 1u << 12},
		{own, 81, 100, 1u << 12},
		{own, 20, 64, 0},
		{own, 46, 64, 0},
		{own, 47, 64, 1u << 12},
		{own, 46, 65, 1u << 12},
		{own, 1500, 1518, 0},
		{own, 1500, 1517, 1u << 12},
		{own, 1501, 1518, 1u << 5},
		/* Runts, which PASSBAD lets through. */
		{own, 0x0800, 63, 1u << 15 | 1u << 11 | 1u << 5},
		{own, 1, 18, 1u << 15 | 1u << 12 | 1u << 11},
		{own, 1, 17, 1u << 15 | 1u << 11},
		{own, 0x0800, 14, 1u << 15 | 1u << 11 | 1u << 5},
		{own, 0x0800, 13, 1u << 15 | 1u << 11},
		{own, 0x0800, 6, 1u << 15 | 1u << 11},
		/* Too long: over 1518 bytes, 1522 with a VLAN tag. */
		{own, 0x0800, 1518, 1u << 5},
		{own, 0x0800, 1519, 1u << 15 | 1u << 7 | 1u << 5},
		{own, 0x8101, 1522, 1u << 5},
		{own, 0x9101, 1522, 1u << 5},
		{own, 0x9101, 1523, 1u << 15 | 1u << 7 | 1u << 5},
		{own, 0x0800, 2048, 1u << 15 | 1u << 7 | 1u << 5},
	};
	static const struct {
		uint16_t type;
		size_t len, stripped;
	} pads[] = {
		{45, 64, 59},
		{46, 65, 65},
		{42, 60, 60},
		{1, 17, 17},
	};
	Setup s = setup(true);
	uint8_t frame[2100], buf[2100];
	uint32_t c[8], st;
	size_t got;

	link_up(&s);
	wr(&s, VLAN1, 0x8101);
	wr(&s, VLAN2, 0x9101);
	wr(&s, MAC_CR, 1u << 18 | 1u << 16 | 0xc); /* PRMS, PASSBAD */
	for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
		size_t len =
			any_frame(frame, frames[i].dst, frames[i].type, frames[i].len);

		st = rx_one(&s, frame, len, buf);
		CHECK_EQ(st, (uint32_t)len << 16 | frames[i].status);
		CHECK(memcmp(buf + 4, frame, len) == 0);
	}
	/* Under 6 bytes, no address: nothing comes. */
	CHECK(!lan95xx_receive(s.e, frame, 5));
	/* The receive watchdog cuts a frame at 2048 bytes. */
	make_frame(frame, own, 0x0800, 2049);
	st = rx_one(&s, frame, 2049, buf);
	CHECK_EQ(st & (0x3fffu << 16 | 1u << 4), 2048u << 16 | 1u << 4);

	/* The counters: good, CRC error, runt, too long; with DRP. */
	rx_counters(&s, c);
	wr(&s, HW_CFG, 1u << 6);
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, own, 0x0800, 64), frame), 64));
	frame[20] ^= 1;
	CHECK(!lan95xx_receive(s.e, frame, 64));
	CHECK(
		!lan95xx_receive(s.e, (any_frame(frame, own, 0x0800, 63), frame), 63));
	CHECK(!lan95xx_receive(s.e, (make_frame(frame, own, 0x0800, 1519), frame),
	                       1519));
	rx_counters(&s, c);
	CHECK_EQ(c[0], 1); /* good */
	CHECK_EQ(c[1], 1); /* CRC errors */
	CHECK_EQ(c[2], 1); /* runts */
	CHECK_EQ(c[4], 1); /* too long */
	CHECK_EQ(c[6], 3); /* bad */
	CHECK_EQ(c[7], 0); /* dropped: the FIFO had room */
	rx_counters(&s, c);
	CHECK_EQ(c[0] | c[1] | c[2] | c[4] | c[6], 0);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(got, 68);

	/* Pad stripping only below 46 bytes of data. */
	wr(&s, HW_CFG, 0);
	wr(&s, MAC_CR, 1u << 18 | 1u << 16 | 1u << 8 | 0xc);
	for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
		size_t len = any_frame(frame, own, pads[i].type, pads[i].len);

		st = rx_one(&s, frame, len, buf);
		CHECK_EQ(st >> 16 & 0x3fff, pads[i].stripped);
	}
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* The RX FIFO: around its end, RXDOFF's zeros, flushes, its 20 KB. */
static void test_rx_fifo(void)
{
	Setup s = setup(true);
	uint8_t frame[2100], buf[2100];
	size_t got, n;

	link_up(&s);
	/* RXDOFF's bytes are zeros, over what the FIFO held there. */
	make_frame(frame, own, 0x0800, 100);
	CHECK(lan95xx_receive(s.e, frame, 100));
	wr(&s, RX_CFG, 1); /* the next frame goes where this one was */
	wr(&s, HW_CFG, 3u << 9);
	CHECK(lan95xx_receive(s.e, frame, 100));
	CHECK(lan95xx_receive(s.e, frame, 100));
	for (int i = 0; i < 2; i++) {
		CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
		CHECK_EQ(got, 107);
		CHECK(buf[4] == 0 && buf[5] == 0 && buf[6] == 0);
		CHECK(memcmp(buf + 7, frame, 100) == 0);
	}
	/* Frames of other sizes, each read before the next: around the end. */
	wr(&s, HW_CFG, 0);
	for (size_t i = 0; i < 15; i++) {
		make_frame(frame, own, (uint16_t)(0x0800 + i), 1514 - i);
		CHECK_EQ(rx_one(&s, frame, 1514 - i, buf) >> 16 & 0x3fff, 1514 - i);
		CHECK(memcmp(buf + 4, frame, 1514 - i) == 0);
	}
	/* A flush ends a burst half sent; with BIR, a NAK follows. */
	wr(&s, HW_CFG, 1u << 12);
	make_frame(frame, own, 0x0800, 1514);
	CHECK(lan95xx_receive(s.e, frame, 1514));
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_ACK);
	CHECK_EQ(n, 512);
	wr(&s, RX_CFG, 1);
	CHECK_EQ(rd(&s, RX_FIFO_INF), 0);
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_NAK);
	/* And the ZLP due after a full last packet. */
	make_frame(frame, own, 0x0800, 508);
	CHECK(lan95xx_receive(s.e, frame, 508));
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_ACK);
	CHECK_EQ(n, 512);
	wr(&s, RX_CFG, 1);
	CHECK_EQ(usb_device_in(s.d, 1, buf, 512, &n), USB_NAK);
	/* Ten frames of 2044 bytes fill the 20 KB exactly. */
	make_frame(frame, own, 0x0800, 2044);
	for (int i = 0; i < 10; i++)
		CHECK(lan95xx_receive(s.e, frame, 2044));
	CHECK_EQ(rd(&s, RX_FIFO_INF), 20480);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* Address filtering (Table 5-45). */
static void test_filter(void)
{
	Setup s = setup(true);
	uint8_t frame[64], other[6] = {0x02, 0, 0, 0, 0, 1};
	uint8_t multi[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}, near[6];
	uint8_t buf[512];
	size_t got;

	link_up(&s);
	wr(&s, ADDRL, 0x33221102u); /* 02:11:22:33:44:55 */
	wr(&s, ADDRH, 0x5544u);
	wr(&s, MAC_CR, 0xc); /* perfect filtering */
	CHECK(lan95xx_receive(s.e, (make_frame(frame, own, 0x800, 64), frame), 64));
	CHECK(!lan95xx_receive(s.e, (make_frame(frame, other, 0x800, 64), frame),
	                       64));
	CHECK(!lan95xx_receive(s.e, (make_frame(frame, multi, 0x800, 64), frame),
	                       64));
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, bcast, 0x800, 64), frame), 64));
	wr(&s, MAC_CR, 0xc | 1u << 11); /* BCAST: no broadcasts */
	CHECK(!lan95xx_receive(s.e, frame, 64));
	/* Inverse filtering. */
	wr(&s, MAC_CR, 0xc | 1u << 17);
	CHECK(
		!lan95xx_receive(s.e, (make_frame(frame, own, 0x800, 64), frame), 64));
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, other, 0x800, 64), frame), 64));
	/* Pass all multicast. */
	wr(&s, MAC_CR, 0xc | 1u << 19);
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, multi, 0x800, 64), frame), 64));
	/*
	 * Hash filtering: 01:00:5e:00:00:01 hashes to 0x1f, bit 31 of HASHL
	 * (the top six bits of the CRC register, as Linux's ether_crc()).
	 */
	wr(&s, MAC_CR, 0xc | 1u << 13);
	make_frame(frame, multi, 0x800, 64);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	wr(&s, HASHH, 0xffffffffu);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	wr(&s, HASHL, 1u << 31);
	CHECK(lan95xx_receive(s.e, frame, 64));
	/* Other hash bits: 01:00:5e:00:00:02 hashes to 8, :04 to 35. */
	wr(&s, HASHH, 0);
	wr(&s, HASHL, 1u << 8);
	multi[5] = 2;
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, multi, 0x800, 64), frame), 64));
	multi[5] = 4;
	CHECK(!lan95xx_receive(s.e, (make_frame(frame, multi, 0x800, 64), frame),
	                       64));
	wr(&s, HASHH, 1u << 3);
	CHECK(lan95xx_receive(s.e, frame, 64));
	/* Without HPFILT a multicast frame goes by the address. */
	wr(&s, MAC_CR, 0xc);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	/* Unicast goes by the hash with HO and HPFILT; other hashes to 32. */
	wr(&s, HASHH, 1u << 0);
	make_frame(frame, other, 0x800, 64);
	wr(&s, MAC_CR, 0xc | 1u << 13);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	wr(&s, MAC_CR, 0xc | 1u << 15);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	wr(&s, MAC_CR, 0xc | 1u << 15 | 1u << 13);
	CHECK(lan95xx_receive(s.e, frame, 64));
	/* The address filter compares all six bytes. */
	wr(&s, MAC_CR, 0xc);
	memcpy(near, own, 6);
	near[5] ^= 1;
	CHECK(
		!lan95xx_receive(s.e, (make_frame(frame, near, 0x800, 64), frame), 64));
	/* ff:ff:ff:ff:ff:fe is multicast: BCAST does not drop it. */
	wr(&s, MAC_CR, 0xc | 1u << 19 | 1u << 11);
	CHECK(lan95xx_receive(s.e, (make_frame(frame, almost, 0x800, 64), frame),
	                      64));
	/* Promiscuous: everything comes. */
	wr(&s, MAC_CR, 0xc | 1u << 18);
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, other, 0x800, 64), frame), 64));
	/* RXALL: filtered frames come, marked Filtering Fail. */
	wr(&s, RX_CFG, 1);
	wr(&s, MAC_CR, 0xc | 1u << 31);
	CHECK(
		lan95xx_receive(s.e, (make_frame(frame, other, 0x800, 64), frame), 64));
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK(load_le32(buf) & 1u << 30);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

/* The interrupt endpoint (5.3.1.3), and the MAC's loopback. */
static void test_interrupts_and_loopback(void)
{
	Setup s = setup(true);
	uint8_t buf[512], frame[64];
	size_t n, got;

	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_NAK);
	wr(&s, INT_EP_CTL, 1u << 31);
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_ACK);
	CHECK(n == 4 && load_le32(buf) == 0);
	wr(&s, INT_EP_CTL, 1u << 15 | 1u << 18);
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_NAK);
	link_up(&s);
	mdio_write(&s, 30, 0x0040);
	mdio_write(&s, 0, 0x3200); /* restart negotiation */
	run(&s, 2000 * MS);
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_ACK);
	CHECK_EQ(load_le32(buf) & 1u << 15, 1u << 15);
	mdio_read(&s, 29);
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_NAK);
	make_frame(frame, bcast, 0x0800, 64);
	CHECK(lan95xx_receive(s.e, frame, 64));
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_ACK);
	CHECK_EQ(load_le32(buf), 1u << 18);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK_EQ(usb_device_in(s.d, 3, buf, 16, &n), USB_NAK);
	CHECK_EQ(usb_device_in(s.d, 4, buf, 16, &n), USB_STALL);

	/* Loopback: what goes out comes back in, not onto the cable. */
	wr(&s, MAC_CR, 1u << 21 | 1u << 20 | 1u << 18 | 0xc);
	CHECK(!lan95xx_receive(s.e, frame, 64));
	{
		uint8_t out[72];

		store_le32(out, 60u | 3u << 12);
		store_le32(out + 4, 60);
		memcpy(out + 8, frame, 60);
		CHECK_EQ(bulk_write(&s, out, 68), USB_ACK);
	}
	CHECK_EQ(sent_count, 0);
	CHECK_EQ(bulk_read(&s, buf, &got), USB_ACK);
	CHECK(got == 68 && memcmp(buf + 4, frame, 64) == 0);
	CHECK_EQ(s.m->stop, MACHINE_STOP_NONE);
	teardown(&s);
}

int main(void)
{
	RUN_TEST(test_descriptors);
	RUN_TEST(test_registers);
	RUN_TEST(test_register_bits);
	RUN_TEST(test_unmodelled);
	RUN_TEST(test_eeprom);
	RUN_TEST(test_resets);
	RUN_TEST(test_lite_reset);
	RUN_TEST(test_phy);
	RUN_TEST(test_phy_registers);
	RUN_TEST(test_phy_modes);
	RUN_TEST(test_phy_unmodelled);
	RUN_TEST(test_phy_link);
	RUN_TEST(test_tx);
	RUN_TEST(test_rx);
	RUN_TEST(test_rx_status);
	RUN_TEST(test_rx_fifo);
	RUN_TEST(test_filter);
	RUN_TEST(test_interrupts_and_loopback);
	return test_summary();
}
