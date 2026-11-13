/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The LAN95xx Ethernet controller; see lan95xx.h. Section, table and
 * page numbers are those of the LAN950x datasheet (DS00001875D).
 */

#include "hw/net/lan95xx.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "core/event.h"
#include "util/byteorder.h"

/* The vendor commands (5.3.3). */
enum { REG_WRITE = 0xa0, REG_READ = 0xa1, GET_STATS = 0xa2 };

/* System CSRs (Table 7-3) and MAC CSRs (Table 7-4). */
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
	DP_CMD = 0x044,
	DP_ADDR = 0x048,
	DP_DATA0 = 0x04c,
	DP_DATA1 = 0x050,
	GPIO_WAKE = 0x064,
	INT_EP_CTL = 0x068,
	BULK_IN_DLY = 0x06c,
	DBG_RX_FIFO_LVL = 0x070,
	DBG_TX_FIFO_PTR = 0x07c,
	MAC_CR = 0x100,
	ADDRH = 0x104,
	ADDRL = 0x108,
	HASHH = 0x10c,
	HASHL = 0x110,
	MII_ACCESS = 0x114,
	MII_DATA = 0x118,
	FLOW = 0x11c,
	VLAN1 = 0x120,
	VLAN2 = 0x124,
	WUFF = 0x128,
	WUCSR = 0x12c,
	COE_CR = 0x130,
	REGS_END = 0x134,
};

/* INT_STS (7.3.2); the interrupt packet's bits (Table 5-2) follow it. */
#define INT_MACRTO (1u << 18)
#define INT_TXSTOP (1u << 17)
#define INT_RXSTOP (1u << 16)
#define INT_PHY (1u << 15)
#define INT_TXE (1u << 14)
#define INT_RXDF (1u << 11)
#define INT_CLEARABLE 0x00077fffu
#define PKT_MACRTO (1u << 19)
#define PKT_RX_FIFO (1u << 18)
#define INTEP_ON (1u << 31)

#define RX_CFG_FLUSH (1u << 0)
#define TX_CFG_ON (1u << 2)
#define TX_CFG_STOP (1u << 1)
#define TX_CFG_FLUSH (1u << 0)

/* HW_CFG (7.3.5). */
#define HW_BIR (1u << 12)
#define HW_RXDOFF_SHIFT 9
#define HW_SBP (1u << 8)
#define HW_DRP (1u << 6)
#define HW_MEF (1u << 5)
#define HW_ETC (1u << 4)
#define HW_LRST (1u << 3)
#define HW_BCE (1u << 1)
#define HW_SRST (1u << 0)

/* PMT_CTL (7.3.8). */
#define PMT_READY (1u << 7)
#define PMT_PHY_RST (1u << 4)
#define PMT_WUPS 0x3u

/* E2P_CMD (7.3.12). */
#define E2P_BUSY (1u << 31)
#define E2P_TIMEOUT (1u << 10)
#define E2P_LOADED (1u << 9)
enum {
	EPC_READ,
	EPC_EWDS,
	EPC_EWEN,
	EPC_WRITE,
	EPC_WRAL,
	EPC_ERASE,
	EPC_ERAL,
	EPC_RELOAD,
};

/* MAC_CR (7.4.1). */
#define MAC_RXALL (1u << 31)
#define MAC_LOOPBK (1u << 21)
#define MAC_MCPAS (1u << 19)
#define MAC_PRMS (1u << 18)
#define MAC_INVFILT (1u << 17)
#define MAC_PASSBAD (1u << 16)
#define MAC_HO (1u << 15)
#define MAC_HPFILT (1u << 13)
#define MAC_BCAST (1u << 11)
#define MAC_PADSTR (1u << 8)
#define MAC_TXEN (1u << 3)
#define MAC_RXEN (1u << 2)

#define MII_BUSY (1u << 0)
#define MII_WRITE (1u << 1)
#define FLOW_FCBSY (1u << 0)
#define WUCSR_WUEN (1u << 2)
#define WUCSR_MPEN (1u << 1)
#define WUCSR_CLEARABLE 0x60u

/* The RX status word (Table 5-40). */
#define RXS_FILTER_FAIL (1u << 30)
#define RXS_ES (1u << 15)
#define RXS_BROADCAST (1u << 13)
#define RXS_LENGTH_ERROR (1u << 12)
#define RXS_RUNT (1u << 11)
#define RXS_MULTICAST (1u << 10)
#define RXS_TOO_LONG (1u << 7)
#define RXS_FRAME_TYPE (1u << 5)
#define RXS_WATCHDOG (1u << 4)
#define RXS_CRC_ERROR (1u << 1)

/* TX command A and B (Tables 5-41 and 5-42). */
#define TXA_FIRST (1u << 13)
#define TXA_LAST (1u << 12)
#define TXB_CK (1u << 14)
#define TXB_NO_CRC (1u << 13)
#define TXB_NO_PAD (1u << 12)

/* PHY registers (Table 7-6) and their bits (7.5). */
enum {
	PHY_BMCR = 0,
	PHY_BMSR = 1,
	PHY_ID1 = 2,
	PHY_ID2 = 3,
	PHY_ANAR = 4,
	PHY_ANLPAR = 5,
	PHY_ANER = 6,
	PHY_MCSR = 17,
	PHY_SMR = 18,
	PHY_SCSIR = 27,
	PHY_ISR = 29,
	PHY_IMR = 30,
	PHY_PSCSR = 31,
};
#define BMCR_RESET (1u << 15)
#define BMCR_LOOPBACK (1u << 14)
#define BMCR_SPEED100 (1u << 13)
#define BMCR_ANENABLE (1u << 12)
#define BMCR_POWERDOWN (1u << 11)
#define BMCR_ANRESTART (1u << 9)
#define BMCR_FULLDPLX (1u << 8)
#define BMCR_CTST (1u << 7)
#define BMSR_ABILITIES 0x7809u /* 100/10 full/half, can negotiate, ext. */
#define BMSR_ANEGCOMPLETE (1u << 5)
#define BMSR_LSTATUS (1u << 2)
#define ANAR_ABILITIES 0x01e0u
#define ANAR_100FULL (1u << 8)
#define ANAR_100HALF (1u << 7)
#define ANAR_10FULL (1u << 6)
#define LPA_ACK (1u << 14)
#define ANER_PAGE (1u << 1)
#define ANER_LP_ABLE (1u << 0)
#define MCSR_ENERGYON (1u << 1)
#define ISR_ENERGYON (1u << 7)
#define ISR_ANEG_DONE (1u << 6)
#define ISR_LINK_DOWN (1u << 4)
#define ISR_LP_ACK (1u << 3)
#define ISR_PAGE (1u << 1)
#define PSCSR_AUTODONE (1u << 12)

/* The partner on the cable: 10 and 100 Mb/s, half and full duplex. */
#define PARTNER_ABILITIES 0x01e1u

#define RX_FIFO_SIZE 20480 /* 20 KB (5.4) */
#define TX_FIFO_SIZE 8192  /* 8 KB */
#define FRAME_MAX 2048     /* the receive watchdog's limit (Table 5-40) */
#define HS_PACKET 512      /* the bulk endpoints' wMaxPacketSize */

#define PHY_RST_NS 2000000ULL      /* PMT_CTL's PHY reset, 2 ms (7.3.8) */
#define BMCR_RST_NS 256000ULL      /* the PHY's soft reset, 256 us (5.6.9.2) */
#define ENERGY_NS 256000000ULL     /* ENERGYON falls after 256 ms (7.5.9) */
#define LINK_NS 2000000000ULL      /* to link up: smirror's choice, 2 s */
#define EECLK_NS 1120ULL           /* EECLK's cycle (LAN9514 Table 4-8) */
#define EPC_TIMEOUT_NS 30000000ULL /* 30 ms; 1.28 us with ETC (7.3.5) */
#define EPC_TIMEOUT_ETC_NS 1280ULL

struct Lan95xx {
	Machine *machine;
	Lan95xxConfig config;
	UsbDevice dev;
	uint8_t device_desc[18];
	uint8_t config_desc[39];
	UsbDeviceClass cls;
	uint32_t regs[REGS_END / 4];
	Event eeprom_done, phy_reset_done, link_up, energy_check;
	bool eeprom_timeout; /* how the EEPROM command in progress ends */
	bool phy_rst;        /* PMT_CTL's PHY reset: every transfer NAKs */
	bool bmcr_rst;       /* the PHY's soft reset, BMCR bit 15 */

	/* The PHY's registers, those of Table 7-6 that hold state. */
	struct {
		uint16_t bmcr, id1, id2, anar, anlpar, aner, mcsr, smr, scsir, isr, imr,
			pscsr;
		bool link, link_fell; /* BMSR's link status latches low */
		bool full, speed100;  /* what the link runs at */
	} phy;

	/* The cable: its partner gets the frames. */
	Lan95xxSend *send;
	void *send_ctx;

	/* The RX FIFO, as Figure 5-4 lays it out, and the burst on bulk IN. */
	uint8_t rx_fifo[RX_FIFO_SIZE];
	size_t rx_head, rx_used;
	unsigned rx_frames;
	uint8_t burst[RX_FIFO_SIZE];
	size_t burst_len, burst_pos;
	bool zlp_due;

	/* The TX FIFO, and the frame its buffers make (5.4.2). */
	uint8_t tx_fifo[TX_FIFO_SIZE];
	size_t tx_head, tx_used;
	bool in_frame;
	uint32_t frame_cmd_b;
	size_t frame_got;
	uint8_t frame[FRAME_MAX + 4];

	/* The statistics counters (Table 5-14), cleared when read. */
	uint32_t rx_stats[8], tx_stats[10];
};

enum { RX_GOOD, RX_CRC, RX_RUNT, RX_ALIGN, RX_LONG, RX_LATE, RX_BAD, RX_DROP };
enum { TX_GOOD, TX_CARRIER = 8, TX_BAD = 9 };

static void stop(Lan95xx *e, const char *what, unsigned addr, uint32_t value)
{
	machine_stop(e->machine, MACHINE_STOP_ERROR,
	             "lan95xx: %s (register 0x%03x, value 0x%08" PRIx32 ")", what,
	             addr, value);
}

static void stop_phy(Lan95xx *e, const char *what, unsigned index)
{
	machine_stop(e->machine, MACHINE_STOP_ERROR, "lan95xx: %s %u", what, index);
}

static uint32_t *reg(Lan95xx *e, unsigned addr)
{
	return &e->regs[addr / 4];
}

static uint64_t now(const Lan95xx *e)
{
	return machine_now(e->machine);
}

static void schedule(Lan95xx *e, Event *ev, uint64_t delay)
{
	event_schedule(&e->machine->events, ev, now(e) + delay);
}

static void cancel(Lan95xx *e, Event *ev)
{
	event_cancel(&e->machine->events, ev);
}

/* A counter that saturates (5.3.1.7): 32 bits for good frames, else 20. */
static void count(uint32_t *counter, bool wide)
{
	if (*counter < (wide ? UINT32_MAX : 0xfffffu))
		(*counter)++;
}

/* The FCS: CRC-32, its bits taken least significant first. */
static uint32_t fcs(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;

	while (n-- > 0) {
		crc ^= *p++;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

/*
 * The hash of a destination address (5.5.4.2): the top six bits of the
 * CRC register, after the address went through it as through the FCS
 * logic: bits in least significant first, the register shifting up.
 */
static unsigned hash_of(const uint8_t *addr)
{
	uint32_t crc = 0xffffffffu;

	for (unsigned i = 0; i < 6; i++)
		for (unsigned bit = 0; bit < 8; bit++) {
			uint32_t top = crc >> 31;

			crc <<= 1;
			if (top ^ ((addr[i] >> bit) & 1u))
				crc ^= 0x04c11db7u;
		}
	return crc >> 26;
}

/* ---- The PHY ---- */

static bool phy_powered(const Lan95xx *e)
{
	return !(e->phy.bmcr & BMCR_POWERDOWN) && !e->phy_rst && !e->bmcr_rst;
}

static void phy_link_down(Lan95xx *e)
{
	cancel(e, &e->link_up);
	if (e->phy.link) {
		e->phy.link = false;
		e->phy.link_fell = true;
		e->phy.isr |= ISR_LINK_DOWN;
	}
	e->phy.anlpar = 0;
	e->phy.aner = 0;
	e->phy.pscsr &= ~PSCSR_AUTODONE;
}

/* Start again to bring the link up: negotiation, or a forced mode. */
static void phy_restart(Lan95xx *e)
{
	phy_link_down(e);
	if (phy_powered(e) && e->send != NULL)
		schedule(e, &e->link_up, LINK_NS);
}

static void link_up(void *ctx)
{
	Lan95xx *e = ctx;
	uint16_t common;

	if (!phy_powered(e) || e->send == NULL)
		return;
	if (e->phy.bmcr & BMCR_ANENABLE) {
		/* The highest common ability (IEEE 802.3 Annex 28B.3). */
		common = e->phy.anar & PARTNER_ABILITIES & ANAR_ABILITIES;
		if (common == 0)
			return;
		e->phy.speed100 = (common & (ANAR_100FULL | ANAR_100HALF)) != 0;
		e->phy.full = (common & ANAR_100FULL) ||
		              (!e->phy.speed100 && (common & ANAR_10FULL));
		e->phy.anlpar = PARTNER_ABILITIES | LPA_ACK;
		e->phy.aner = ANER_LP_ABLE | ANER_PAGE;
		e->phy.pscsr |= PSCSR_AUTODONE;
		e->phy.isr |= ISR_ANEG_DONE | ISR_LP_ACK | ISR_PAGE;
	} else {
		/* Forced: the partner finds the speed by parallel detection. */
		e->phy.speed100 = (e->phy.bmcr & BMCR_SPEED100) != 0;
		e->phy.full = (e->phy.bmcr & BMCR_FULLDPLX) != 0;
	}
	e->phy.link = true;
}

/* ENERGYON goes low when no energy came in 256 ms (7.5.9). */
static void energy_check(void *ctx)
{
	Lan95xx *e = ctx;

	if (e->send == NULL)
		e->phy.mcsr &= ~MCSR_ENERGYON;
}

/*
 * The PHY's registers at reset, from the MODE field (Table 7-7). A soft
 * reset keeps the fields marked NASR: the mode and the address.
 */
static bool phy_defaults(Lan95xx *e, bool soft)
{
	static const uint16_t bmcr_of[8] = {
		0,
		BMCR_FULLDPLX,
		BMCR_SPEED100,
		BMCR_SPEED100 | BMCR_FULLDPLX,
		BMCR_SPEED100 | BMCR_ANENABLE,
		BMCR_SPEED100 | BMCR_ANENABLE,
		0,
		BMCR_SPEED100 | BMCR_ANENABLE,
	};
	unsigned mode;

	if (!soft)
		e->phy.smr = 0x00e1; /* all capable, address 1 */
	mode = e->phy.smr >> 5 & 7;
	if (mode == 6) {
		stop(e, "PHY reset in the reserved mode 110b", PHY_SMR, e->phy.smr);
		return false;
	}
	e->phy.bmcr = bmcr_of[mode];
	e->phy.anar = mode >= 4 && mode <= 5 ? 0x0081 : 0x01e1;
	e->phy.id1 = e->config.phy_id1;
	e->phy.id2 = e->config.phy_id2;
	e->phy.scsir = 0;
	e->phy.isr = 0;
	e->phy.imr = 0;
	e->phy.pscsr = 0x0040;
	e->phy.mcsr &= MCSR_ENERGYON; /* not affected by a soft reset */
	return true;
}

/* A hardware reset of the PHY; power_down as after a USB reset (5.13). */
static void phy_hard_reset(Lan95xx *e, bool power_down)
{
	phy_link_down(e);
	e->phy.link_fell = false;
	cancel(e, &e->phy_reset_done);
	e->bmcr_rst = false;
	phy_defaults(e, false);
	e->phy.mcsr = MCSR_ENERGYON;
	schedule(e, &e->energy_check, ENERGY_NS);
	if (power_down)
		e->phy.bmcr |= BMCR_POWERDOWN;
	phy_restart(e);
}

static void phy_reset_done(void *ctx)
{
	Lan95xx *e = ctx;

	if (e->phy_rst) {
		e->phy_rst = false;
		*reg(e, PMT_CTL) &= ~PMT_PHY_RST;
	}
	e->bmcr_rst = false;
	phy_restart(e);
}

/* BMCR's soft reset, also when the PHY powers up again (5.6.8.1). */
static void phy_soft_reset(Lan95xx *e)
{
	phy_link_down(e);
	if (!phy_defaults(e, true))
		return;
	e->bmcr_rst = true;
	schedule(e, &e->phy_reset_done, BMCR_RST_NS);
}

static uint16_t phy_read(Lan95xx *e, unsigned index)
{
	uint16_t v;

	switch (index) {
	case PHY_BMCR:
		return e->phy.bmcr | (e->bmcr_rst ? BMCR_RESET : 0);
	case PHY_BMSR:
		v = BMSR_ABILITIES;
		if (e->phy.pscsr & PSCSR_AUTODONE)
			v |= BMSR_ANEGCOMPLETE;
		if (e->phy.link && !e->phy.link_fell)
			v |= BMSR_LSTATUS;
		e->phy.link_fell = false;
		return v;
	case PHY_ID1:
		return e->phy.id1;
	case PHY_ID2:
		return e->phy.id2;
	case PHY_ANAR:
		return e->phy.anar;
	case PHY_ANLPAR:
		return e->phy.anlpar;
	case PHY_ANER:
		v = e->phy.aner;
		e->phy.aner &= ~ANER_PAGE; /* latched high until read */
		return v;
	case PHY_MCSR:
		return e->phy.mcsr;
	case PHY_SMR:
		return e->phy.smr;
	case PHY_SCSIR:
		return e->phy.scsir;
	case PHY_ISR:
		v = e->phy.isr;
		e->phy.isr = 0;
		return v;
	case PHY_IMR:
		return e->phy.imr;
	case PHY_PSCSR:
		/* HCDSPEED: 001 10 half, 101 10 full, 010 100 half, 110 full. */
		v = e->phy.pscsr;
		v |= (uint16_t)((e->phy.speed100 ? 2u : 1u) << 2);
		if (e->phy.full)
			v |= 4u << 2;
		return v;
	default:
		stop_phy(e, "read of a PHY register not modelled:", index);
		return 0xffff;
	}
}

static void phy_write(Lan95xx *e, unsigned index, uint16_t v)
{
	uint16_t old;

	switch (index) {
	case PHY_BMCR:
		if (v & (BMCR_LOOPBACK | BMCR_CTST)) {
			stop(e, "PHY loopback and collision test are not modelled",
			     MII_DATA, v);
			return;
		}
		old = e->phy.bmcr;
		e->phy.bmcr = v & 0x7d80;
		if (v & BMCR_RESET) {
			phy_soft_reset(e);
		} else if ((old & BMCR_POWERDOWN) && !(v & BMCR_POWERDOWN)) {
			phy_soft_reset(e);
		} else if (v & BMCR_POWERDOWN) {
			phy_link_down(e);
		} else if (((old ^ v) &
		            (BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX)) ||
		           ((v & BMCR_ANRESTART) && (v & BMCR_ANENABLE))) {
			phy_restart(e);
		}
		return;
	case PHY_ID1:
		e->phy.id1 = v;
		return;
	case PHY_ID2:
		e->phy.id2 = v;
		return;
	case PHY_ANAR:
		e->phy.anar = v & 0x2dff;
		return;
	case PHY_MCSR:
		e->phy.mcsr = (uint16_t)((e->phy.mcsr & MCSR_ENERGYON) | (v & 0x2001));
		return;
	case PHY_SMR:
		e->phy.smr = v & 0x00ff;
		return;
	case PHY_SCSIR:
		e->phy.scsir = v & 0xe400;
		return;
	case PHY_IMR:
		e->phy.imr = v & 0x00ff;
		return;
	case PHY_PSCSR:
		e->phy.pscsr =
			(uint16_t)((e->phy.pscsr & PSCSR_AUTODONE) | (v & 0x0fe0));
		return;
	case PHY_BMSR:
	case PHY_ANLPAR:
	case PHY_ANER:
	case PHY_ISR:
		return; /* read-only */
	default:
		stop_phy(e, "write of a PHY register not modelled:", index);
	}
}

/* ---- Interrupts ---- */

static uint32_t int_sts(Lan95xx *e)
{
	uint32_t v = *reg(e, INT_STS);

	if (e->phy.isr & e->phy.imr & 0xfe)
		v |= INT_PHY;
	return v;
}

/* The interrupt endpoint's packet (Table 5-2). */
static uint32_t int_packet(Lan95xx *e)
{
	uint32_t s = int_sts(e), p = s & 0x3ffffu;

	if (s & INT_MACRTO)
		p |= PKT_MACRTO;
	if (e->rx_frames > 0 || e->burst_pos < e->burst_len)
		p |= PKT_RX_FIFO;
	return p;
}

/* ---- The RX path (5.4.1) ---- */

static unsigned rx_offset(Lan95xx *e)
{
	return *reg(e, HW_CFG) >> HW_RXDOFF_SHIFT & 3;
}

static void rx_flush(Lan95xx *e)
{
	e->rx_head = e->rx_used = 0;
	e->rx_frames = 0;
	e->burst_len = e->burst_pos = 0;
	e->zlp_due = false;
}

static void fifo_put(uint8_t *fifo, size_t size, size_t at, const void *src,
                     size_t n)
{
	const uint8_t *p = src;

	for (size_t i = 0; i < n; i++)
		fifo[(at + i) % size] = p[i];
}

static void fifo_get(const uint8_t *fifo, size_t size, size_t at, void *dst,
                     size_t n)
{
	uint8_t *p = dst;

	for (size_t i = 0; i < n; i++)
		p[i] = fifo[(at + i) % size];
}

static bool is_perfect(Lan95xx *e, const uint8_t *dst)
{
	uint8_t mac[6];

	/* ADDRL holds the first four bytes, ADDRH the last two (Table 7-5). */
	store_le32(mac, *reg(e, ADDRL));
	store_le16(mac + 4, (uint16_t)*reg(e, ADDRH));
	return memcmp(mac, dst, 6) == 0;
}

static bool hash_hit(Lan95xx *e, const uint8_t *dst)
{
	unsigned h = hash_of(dst);
	uint32_t table = *reg(e, h & 0x20 ? HASHH : HASHL);

	return table >> (h & 0x1f) & 1;
}

/* Does the destination pass the address filter (Table 5-45)? */
static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static bool address_passes(Lan95xx *e, const uint8_t *dst)
{
	uint32_t cr = *reg(e, MAC_CR);

	if (cr & MAC_PRMS)
		return true;
	if (memcmp(dst, broadcast, 6) == 0)
		return !(cr & MAC_BCAST);
	if (dst[0] & 1) {
		if (cr & MAC_MCPAS)
			return true;
		if (cr & MAC_HPFILT)
			return hash_hit(e, dst);
	} else if ((cr & MAC_HPFILT) && (cr & MAC_HO)) {
		return hash_hit(e, dst);
	}
	return is_perfect(e, dst) != ((cr & MAC_INVFILT) != 0);
}

/* A frame, FCS included, from the PHY or the MAC's loopback. */
static bool rx_frame(Lan95xx *e, const uint8_t *frame, size_t len)
{
	uint32_t cr = *reg(e, MAC_CR), status = 0;
	size_t longest = 1518, need, at;
	unsigned type = 0, off = rx_offset(e);
	bool pass;
	uint8_t word[4], zero[3] = {0};

	if (!(cr & MAC_RXEN) || len < 6)
		return false;
	if (len > FRAME_MAX) {
		len = FRAME_MAX;
		status |= RXS_WATCHDOG;
	}
	pass = address_passes(e, frame);
	if (!pass && !(cr & MAC_RXALL))
		return false;
	if (!pass)
		status |= RXS_FILTER_FAIL;
	if (memcmp(frame, broadcast, 6) == 0)
		status |= RXS_BROADCAST;
	else if (frame[0] & 1)
		status |= RXS_MULTICAST;
	if (len >= 14) {
		type = load_be16(frame + 12);
		if (type > 1500)
			status |= RXS_FRAME_TYPE;
		/* A VLAN tag allows 4 bytes more (5.5.2). */
		if (type == (*reg(e, VLAN1) & 0xffff) ||
		    type == (*reg(e, VLAN2) & 0xffff))
			longest = 1522;
	}
	if (len < 64)
		status |= RXS_RUNT;
	else if (fcs(frame, len - 4) != load_le32(frame + len - 4))
		status |= RXS_CRC_ERROR;
	if (len > longest)
		status |= RXS_TOO_LONG;
	if (len >= 18 && type <= 1500 &&
	    (type > len - 18 || (len - 18 > 46 && type != len - 18)))
		status |= RXS_LENGTH_ERROR;
	if (status & (RXS_RUNT | RXS_TOO_LONG | RXS_CRC_ERROR))
		status |= RXS_ES;

	/* Runts reach the host only with PASSBAD (Table 5-40, bit 11). */
	if ((status & RXS_RUNT) && !(cr & MAC_PASSBAD))
		return false;
	if (status & RXS_ES) {
		count(&e->rx_stats[RX_BAD], false);
		if (status & RXS_CRC_ERROR)
			count(&e->rx_stats[RX_CRC], false);
		if (status & RXS_RUNT)
			count(&e->rx_stats[RX_RUNT], false);
		if (status & RXS_TOO_LONG)
			count(&e->rx_stats[RX_LONG], false);
	} else {
		count(&e->rx_stats[RX_GOOD], true);
	}
	if ((status & RXS_ES) && (*reg(e, HW_CFG) & HW_DRP)) {
		*reg(e, INT_STS) |= INT_RXDF;
		return false;
	}
	/* Pad stripping: the pad and the FCS go (MAC_CR bit 8). */
	if ((cr & MAC_PADSTR) && len >= 18 && type < 46 && len - 18 > type)
		len = 14 + type;

	need = 4 + ((off + len + 3) & ~(size_t)3);
	if (e->rx_used + need > RX_FIFO_SIZE) {
		count(&e->rx_stats[RX_DROP], false);
		*reg(e, INT_STS) |= INT_RXDF;
		return false;
	}
	status |= (uint32_t)len << 16;
	store_le32(word, status);
	at = e->rx_head + e->rx_used;
	fifo_put(e->rx_fifo, RX_FIFO_SIZE, at, word, 4);
	fifo_put(e->rx_fifo, RX_FIFO_SIZE, at + 4, zero, off);
	fifo_put(e->rx_fifo, RX_FIFO_SIZE, at + 4 + off, frame, len);
	e->rx_used += need;
	e->rx_frames++;
	return true;
}

/* Take the next frame of the RX FIFO onto the end of the burst. */
static void burst_take(Lan95xx *e)
{
	uint8_t word[4];
	size_t len, bytes;

	fifo_get(e->rx_fifo, RX_FIFO_SIZE, e->rx_head, word, 4);
	len = load_le32(word) >> 16 & 0x3fff;
	bytes = 4 + rx_offset(e) + len;
	fifo_get(e->rx_fifo, RX_FIFO_SIZE, e->rx_head, e->burst + e->burst_len,
	         bytes);
	e->burst_len += bytes;
	bytes = (bytes + 3) & ~(size_t)3;
	e->rx_head = (e->rx_head + bytes) % RX_FIFO_SIZE;
	e->rx_used -= bytes;
	e->rx_frames--;
}

/* The size of the next frame in the RX FIFO, with its status word. */
static size_t next_frame_bytes(Lan95xx *e)
{
	uint8_t word[4];

	fifo_get(e->rx_fifo, RX_FIFO_SIZE, e->rx_head, word, 4);
	return 4 + rx_offset(e) + (load_le32(word) >> 16 & 0x3fff);
}

/*
 * The frames of the next bulk IN burst (5.3.1.1): one, or as many as
 * there are with MEF, each from a DWORD boundary, up to the burst cap.
 */
static bool burst_start(Lan95xx *e)
{
	uint32_t hw = *reg(e, HW_CFG), cap = *reg(e, BURST_CAP) & 0xff;
	size_t limit = SIZE_MAX;

	e->burst_len = e->burst_pos = 0;
	if (e->rx_frames == 0)
		return false;
	if ((hw & HW_BCE) && cap > 4)
		limit = cap * HS_PACKET;
	burst_take(e);
	while ((hw & HW_MEF) && e->rx_frames > 0) {
		size_t at = (e->burst_len + 3) & ~(size_t)3;

		if (at + next_frame_bytes(e) > limit)
			break;
		memset(e->burst + e->burst_len, 0, at - e->burst_len);
		e->burst_len = at;
		burst_take(e);
	}
	return true;
}

static UsbResult bulk_in(Lan95xx *e, uint8_t *buf, size_t max, size_t *len)
{
	size_t mps = max < HS_PACKET ? max : HS_PACKET, n;

	if (e->burst_pos == e->burst_len) {
		if (e->zlp_due) {
			e->zlp_due = false;
			return USB_ACK;
		}
		if (!burst_start(e))
			return *reg(e, HW_CFG) & HW_BIR ? USB_NAK : USB_ACK;
	}
	n = e->burst_len - e->burst_pos;
	if (n > mps)
		n = mps;
	memcpy(buf, e->burst + e->burst_pos, n);
	e->burst_pos += n;
	*len = n;
	/* A burst that ends with a whole packet ends with a ZLP after it. */
	if (e->burst_pos == e->burst_len && n == mps)
		e->zlp_due = true;
	return USB_ACK;
}

/* ---- The TX path (5.4.2) ---- */

static void tx_flush(Lan95xx *e)
{
	e->tx_head = e->tx_used = 0;
	e->in_frame = false;
	e->frame_got = 0;
}

/* Loss of sync (5.4.2.5): TXE, and the bulk OUT pipe stalls unless SBP. */
static void tx_error(Lan95xx *e)
{
	*reg(e, INT_STS) |= INT_TXE;
	tx_flush(e);
	if (!(*reg(e, HW_CFG) & HW_SBP))
		e->dev.halted[2][0] = true;
}

static void transmit(Lan95xx *e, uint32_t cmd_b, size_t len)
{
	bool crc = !(cmd_b & TXB_NO_CRC);

	/* Padding to 64 bytes brings the CRC with it (Table 5-42). */
	if (!(cmd_b & TXB_NO_PAD) && len < 60) {
		memset(e->frame + len, 0, 60 - len);
		len = 60;
		crc = true;
	}
	if (crc) {
		store_le32(e->frame + len, fcs(e->frame, len));
		len += 4;
	}
	if (*reg(e, MAC_CR) & MAC_LOOPBK) {
		count(&e->tx_stats[TX_GOOD], true);
		rx_frame(e, e->frame, len);
	} else if (e->phy.link) {
		count(&e->tx_stats[TX_GOOD], true);
		e->send(e->send_ctx, e->frame, len);
	} else {
		count(&e->tx_stats[TX_CARRIER], false);
		count(&e->tx_stats[TX_BAD], false);
	}
}

static bool tx_running(Lan95xx *e)
{
	return (*reg(e, TX_CFG) & TX_CFG_ON) && (*reg(e, MAC_CR) & MAC_TXEN);
}

/* Take the buffers out of the TX FIFO, into frames on the cable. */
static void tx_drain(Lan95xx *e)
{
	while (tx_running(e) && e->tx_used >= 8) {
		uint8_t words[8];
		uint32_t a, b;
		size_t size, off, total, frame_len;

		fifo_get(e->tx_fifo, TX_FIFO_SIZE, e->tx_head, words, 8);
		a = load_le32(words);
		b = load_le32(words + 4);
		size = a & 0x7ff;
		off = a >> 16 & 3;
		total = 8 + ((off + size + 3) & ~(size_t)3);
		frame_len = b & 0x7ff;
		if (size == 0) {
			tx_error(e);
			return;
		}
		if (e->tx_used < total)
			return;
		if (!e->in_frame) {
			if (!(a & TXA_FIRST)) {
				tx_error(e); /* missing FS */
				return;
			}
			e->in_frame = true;
			e->frame_cmd_b = b;
			e->frame_got = 0;
		} else if ((a & TXA_FIRST) || ((b ^ e->frame_cmd_b) & ~TXB_CK)) {
			tx_error(e); /* unexpected FS, or command B changed */
			return;
		}
		if (e->frame_got + size > frame_len) {
			tx_error(e);
			return;
		}
		fifo_get(e->tx_fifo, TX_FIFO_SIZE, e->tx_head + 8 + off,
		         e->frame + e->frame_got, size);
		e->frame_got += size;
		e->tx_head = (e->tx_head + total) % TX_FIFO_SIZE;
		e->tx_used -= total;
		if (e->frame_got == frame_len) {
			if (!(a & TXA_LAST)) {
				tx_error(e); /* missing LS */
				return;
			}
			e->in_frame = false;
			transmit(e, e->frame_cmd_b, frame_len);
		} else if (a & TXA_LAST) {
			tx_error(e); /* unexpected LS */
			return;
		}
	}
}

/* A bulk OUT packet: into the TX FIFO, from a DWORD boundary. */
static UsbResult bulk_out(Lan95xx *e, const uint8_t *buf, size_t len)
{
	size_t padded = (len + 3) & ~(size_t)3;

	if (len > HS_PACKET)
		return USB_NO_RESPONSE; /* longer than wMaxPacketSize */
	/* At least 512 bytes free, or a NAK (5.3.1.2). */
	if (TX_FIFO_SIZE - e->tx_used < HS_PACKET)
		return USB_NAK;
	fifo_put(e->tx_fifo, TX_FIFO_SIZE, e->tx_head + e->tx_used, buf, len);
	e->tx_used += padded;
	tx_drain(e);
	return USB_ACK;
}

/* ---- The EEPROM controller (5.7.4), with no EEPROM ---- */

static void eeprom_done(void *ctx)
{
	Lan95xx *e = ctx;

	*reg(e, E2P_CMD) &= ~E2P_BUSY;
	if (e->eeprom_timeout)
		*reg(e, E2P_CMD) |= E2P_TIMEOUT;
}

/*
 * A command. The data line reads low without an EEPROM (the LAN9514's
 * EEDI has a pull-down): a read shifts in zeros and the reload finds no
 * 0xA5 signature; a write waits for a ready that never comes (7.3.12).
 */
static void eeprom_command(Lan95xx *e, uint32_t cmd)
{
	unsigned op = cmd >> 28 & 7;
	uint64_t delay;

	e->eeprom_timeout = false;
	switch (op) {
	case EPC_READ:
		*reg(e, E2P_DATA) = 0;
		delay = 20 * EECLK_NS; /* start, opcode, 9 address, 8 data bits */
		break;
	case EPC_RELOAD: /* the first byte is not 0xA5: nothing loads */
		delay = 20 * EECLK_NS;
		break;
	case EPC_EWDS:
	case EPC_EWEN:
		delay = 12 * EECLK_NS;
		break;
	default: /* WRITE, WRAL, ERASE, ERAL */
		e->eeprom_timeout = true;
		delay = *reg(e, HW_CFG) & HW_ETC ? EPC_TIMEOUT_ETC_NS : EPC_TIMEOUT_NS;
		break;
	}
	schedule(e, &e->eeprom_done, delay);
}

/* ---- Resets (5.13) ---- */

/* Every register's writable bits and reset value; 0, 0 for none. */
static const struct {
	uint32_t writable, reset;
} reg_info[REGS_END / 4] = {
	[ID_REV / 4] = {0, 0},
	[INT_STS / 4] = {0, 0},
	[RX_CFG / 4] = {0, 0},
	[TX_CFG / 4] = {TX_CFG_ON, 0},
	[HW_CFG / 4] = {0x1ff2, 0},
	[PMT_CTL / 4] = {0x036c, 0x01c0}, /* READY: the EEPROM was tried */
	[LED_GPIO_CFG / 4] = {0x03330777, 0},
	[GPIO_CFG / 4] = {0xffffffff, 0xff000000},
	[AFC_CFG / 4] = {0x00ffffff, 0},
	[E2P_CMD / 4] = {0x700001ff, 0},
	[E2P_DATA / 4] = {0xff, 0},
	[BURST_CAP / 4] = {0xff, 0},
	[DP_SEL / 4] = {0x7, 0x80000000},
	[DP_ADDR / 4] = {0x7fff, 0},
	[DP_DATA0 / 4] = {0xffffffff, 0},
	[DP_DATA1 / 4] = {0x1f, 0},
	[GPIO_WAKE / 4] = {0x07ff07ff, 0},
	[INT_EP_CTL / 4] = {0x800fffff, 0},
	[BULK_IN_DLY / 4] = {0xffff, 0x800},
	[MAC_CR / 4] = {0x80bfbdec, MAC_PRMS},
	[ADDRH / 4] = {0xffff, 0xffff}, /* FF:FF:...: no EEPROM */
	[ADDRL / 4] = {0xffffffff, 0xffffffff},
	[HASHH / 4] = {0xffffffff, 0},
	[HASHL / 4] = {0xffffffff, 0},
	[MII_ACCESS / 4] = {0xffc2, 0},
	[MII_DATA / 4] = {0xffff, 0},
	[FLOW / 4] = {0xffff0006, 0},
	[VLAN1 / 4] = {0xffff, 0xffff},
	[VLAN2 / 4] = {0xffff, 0xffff},
	[WUCSR / 4] = {0x0206, 0},
	[COE_CR / 4] = {0x00010003, 0},
};

/* All but the USB device controller; the PHY powers down after a USB reset. */
static void chip_reset(Lan95xx *e, bool usb_reset)
{
	cancel(e, &e->eeprom_done);
	for (unsigned i = 0; i < REGS_END / 4; i++)
		e->regs[i] = reg_info[i].reset;
	*reg(e, ID_REV) = (uint32_t)e->config.chip_id << 16 | e->config.chip_rev;
	e->phy_rst = false;
	rx_flush(e);
	tx_flush(e);
	memset(e->rx_stats, 0, sizeof(e->rx_stats));
	memset(e->tx_stats, 0, sizeof(e->tx_stats));
	phy_hard_reset(e, usb_reset);
}

/* ---- Registers ---- */

/* GPIO inputs read 1: their pins have pull-ups (LAN9514 Table 2-x). */
static uint32_t gpio_data(uint32_t v, unsigned dir_shift, uint32_t data_mask)
{
	return v | (~(v >> dir_shift) & data_mask);
}

static bool reg_read(Lan95xx *e, unsigned addr, uint32_t *value)
{
	uint32_t v = 0;

	if (addr >= REGS_END || (addr & 3)) {
		stop(e, "read of a reserved register", addr, 0);
		return false;
	}
	switch (addr) {
	case INT_STS:
		v = int_sts(e);
		break;
	case RX_FIFO_INF:
		v = (uint32_t)(e->rx_used + (e->burst_len - e->burst_pos));
		break;
	case TX_FIFO_INF:
		v = (uint32_t)(TX_FIFO_SIZE - e->tx_used);
		break;
	case LED_GPIO_CFG:
		v = gpio_data(*reg(e, addr), 4, 0x7);
		break;
	case GPIO_CFG:
		v = gpio_data(*reg(e, addr), 8, 0xff);
		break;
	case DBG_RX_FIFO_LVL:
	case DBG_RX_FIFO_LVL + 4:
	case DBG_RX_FIFO_LVL + 8:
	case DBG_TX_FIFO_PTR:
		stop(e, "the debug registers are not modelled", addr, 0);
		return false;
	case DP_CMD:
		stop(e, "the data port is not modelled", addr, 0);
		return false;
	case WUFF:
		stop(e, "the wake-up frame filter is not modelled", addr, 0);
		return false;
	default:
		if (reg_info[addr / 4].writable == 0 && addr != ID_REV &&
		    addr != RX_CFG) {
			stop(e, "read of a reserved register", addr, 0);
			return false;
		}
		v = *reg(e, addr);
		break;
	}
	*value = v;
	return true;
}

static void mii_access(Lan95xx *e, uint32_t v)
{
	unsigned phy = v >> 11 & 0x1f, index = v >> 6 & 0x1f;

	/* The internal PHY is the only one, at its address (7.4.6). */
	if (phy != (e->phy.smr & 0x1f)) {
		stop(e, "MII access to an address with no PHY", MII_ACCESS, v);
		return;
	}
	if (v & MII_WRITE)
		phy_write(e, index, (uint16_t)*reg(e, MII_DATA));
	else
		*reg(e, MII_DATA) = phy_read(e, index);
}

static bool reg_write(Lan95xx *e, unsigned addr, uint32_t v)
{
	uint32_t *r, old;

	if (addr >= REGS_END || (addr & 3)) {
		stop(e, "write of a reserved register", addr, v);
		return false;
	}
	r = reg(e, addr);
	old = *r;
	switch (addr) {
	case ID_REV:
	case RX_FIFO_INF:
	case TX_FIFO_INF:
		return true; /* read-only */
	case INT_STS:
		*r &= ~(v & INT_CLEARABLE);
		return true;
	case RX_CFG:
		if (v & RX_CFG_FLUSH)
			rx_flush(e);
		return true;
	case TX_CFG:
		*r = v & TX_CFG_ON;
		if (v & TX_CFG_FLUSH)
			tx_flush(e);
		if (v & TX_CFG_STOP) {
			/* The transmitter stops after the frame: at once here. */
			*r &= ~TX_CFG_ON;
			*reg(e, INT_STS) |= INT_TXSTOP;
		}
		tx_drain(e);
		return true;
	case HW_CFG:
		if (v & HW_SRST) {
			stop(e, "the soft reset (a USB disconnect) is not modelled", addr,
			     v);
			return false;
		}
		*r = v & reg_info[addr / 4].writable;
		if (v & HW_LRST)
			chip_reset(e, false);
		return true;
	case PMT_CTL:
		*r = (old & ~reg_info[addr / 4].writable & ~(v & PMT_WUPS)) |
		     (v & reg_info[addr / 4].writable);
		/* The PHY stays in reset for 2 ms, and transfers NAK. */
		if ((v & PMT_PHY_RST) && !e->phy_rst) {
			*r |= PMT_PHY_RST;
			e->phy_rst = true;
			phy_hard_reset(e, false);
			schedule(e, &e->phy_reset_done, PHY_RST_NS);
		}
		return true;
	case E2P_CMD:
		if (old & E2P_BUSY) {
			stop(e, "E2P_CMD written while the EEPROM is busy", addr, v);
			return false;
		}
		*r = (old & ~0x700001ffu & ~(v & (E2P_TIMEOUT | E2P_LOADED))) |
		     (v & 0x700001ffu);
		if (v & E2P_BUSY) {
			*r |= E2P_BUSY;
			eeprom_command(e, v);
		}
		return true;
	case DP_CMD:
		stop(e, "the data port is not modelled", addr, v);
		return false;
	case MAC_CR:
		*r = v & reg_info[addr / 4].writable;
		/* The receiver halts: RXSTOP_INT, a pulse (7.3.2). */
		if ((old & MAC_RXEN) && !(v & MAC_RXEN))
			*reg(e, INT_STS) |= INT_RXSTOP;
		tx_drain(e);
		return true;
	case MII_ACCESS:
		*r = v & reg_info[addr / 4].writable;
		if (v & MII_BUSY)
			mii_access(e, v);
		return true;
	case FLOW:
		if (v & FLOW_FCBSY) {
			stop(e, "pause frames on demand are not modelled", addr, v);
			return false;
		}
		*r = v & reg_info[addr / 4].writable;
		return true;
	case WUFF:
		stop(e, "the wake-up frame filter is not modelled", addr, v);
		return false;
	case WUCSR:
		if (v & (WUCSR_WUEN | WUCSR_MPEN)) {
			stop(e, "wake-up detection is not modelled", addr, v);
			return false;
		}
		*r = (old & WUCSR_CLEARABLE & ~v) | (v & reg_info[addr / 4].writable);
		return true;
	case COE_CR:
		if (v & reg_info[addr / 4].writable) {
			stop(e, "the checksum offload engines are not modelled", addr, v);
			return false;
		}
		return true;
	default:
		if (reg_info[addr / 4].writable == 0) {
			stop(e, "write of a reserved register", addr, v);
			return false;
		}
		*r = v & reg_info[addr / 4].writable;
		return true;
	}
}

/* ---- USB ---- */

static size_t get_stats(Lan95xx *e, bool tx, uint8_t *data)
{
	uint32_t *c = tx ? e->tx_stats : e->rx_stats;
	size_t n = tx ? 10 : 8;

	for (size_t i = 0; i < n; i++)
		store_le32(data + 4 * i, c[i]);
	memset(c, 0, n * sizeof(*c)); /* cleared when read (5.3.1.7) */
	return 4 * n;
}

static bool eth_request(UsbDevice *d, const UsbSetup *s, uint8_t *data,
                        size_t *len)
{
	Lan95xx *e = d->ctx;
	bool in = s->bm_request_type & USB_DIR_IN;
	unsigned addr = s->w_index;
	uint32_t v;

	if ((s->bm_request_type & (USB_TYPE_MASK | USB_RECIP_MASK)) !=
	    USB_TYPE_VENDOR)
		return false;
	switch (s->b_request) {
	case REG_WRITE:
	case REG_READ:
		if (in != (s->b_request == REG_READ) || s->w_value != 0 ||
		    s->w_length != 4 || addr > 0xfff)
			return false;
		/* MAC CSRs stall while the device is not configured (5.3.3). */
		if (addr >= MAC_CR && d->config == 0)
			return false;
		if (!in)
			return reg_write(e, addr, load_le32(data));
		if (!reg_read(e, addr, &v))
			return false;
		store_le32(data, v);
		*len = 4;
		return true;
	case GET_STATS:
		if (!in || s->w_value != 0 || s->w_index > 1)
			return false;
		*len = get_stats(e, s->w_index == 1, data);
		return true;
	default:
		return false;
	}
}

static UsbResult eth_in(UsbDevice *d, unsigned ep, uint8_t *buf, size_t max,
                        size_t *len)
{
	Lan95xx *e = d->ctx;
	uint32_t p;

	if (ep == 1)
		return bulk_in(e, buf, max, len);
	if (ep != 3 || max < 4)
		return USB_STALL;
	p = int_packet(e);
	if (!(p & *reg(e, INT_EP_CTL) & 0xfffffu) &&
	    !(*reg(e, INT_EP_CTL) & INTEP_ON))
		return USB_NAK;
	store_le32(buf, p);
	*len = 4;
	return USB_ACK;
}

static UsbResult eth_out(UsbDevice *d, unsigned ep, const uint8_t *buf,
                         size_t len)
{
	if (ep != 2)
		return USB_STALL;
	return bulk_out(d->ctx, buf, len);
}

static bool eth_busy(UsbDevice *d)
{
	return ((Lan95xx *)d->ctx)->phy_rst;
}

static void eth_reset(UsbDevice *d)
{
	chip_reset(d->ctx, true);
}

/* The descriptors of 5.3.1.6, at high speed. */
static void make_descriptors(Lan95xx *e)
{
	const Lan95xxConfig *c = &e->config;
	uint8_t *d = e->device_desc, *cfg = e->config_desc;
	static const uint8_t endpoints[3][4] = {
		{0x81, 2, 0x00, 0x02}, /* bulk IN, 512 bytes */
		{0x02, 2, 0x00, 0x02}, /* bulk OUT, 512 bytes */
		{0x83, 3, 0x10, 0x00}, /* interrupt IN, 16 bytes */
	};

	d[0] = 18;
	d[1] = 1;
	store_le16(d + 2, 0x0200);
	d[4] = 0xff; /* vendor-specific */
	d[5] = 0;
	d[6] = c->device_protocol;
	d[7] = 64;
	store_le16(d + 8, c->vendor);
	store_le16(d + 10, c->product);
	store_le16(d + 12, c->release);
	d[14] = d[15] = d[16] = 0; /* no strings without an EEPROM */
	d[17] = 1;

	cfg[0] = 9;
	cfg[1] = 2;
	store_le16(cfg + 2, sizeof(e->config_desc));
	cfg[4] = 1;
	cfg[5] = 1;
	cfg[6] = 0;
	cfg[7] = (uint8_t)(0x80 | (c->self_powered ? 0x40 : 0) |
	                   (c->remote_wakeup ? 0x20 : 0));
	cfg[8] = c->max_power;
	memcpy(cfg + 9, (const uint8_t[]){9, 4, 0, 0, 3, 0xff, 0, 0xff, 0}, 9);
	for (unsigned i = 0; i < 3; i++) {
		uint8_t *ep = cfg + 18 + 7 * i;

		ep[0] = 7;
		ep[1] = 5;
		memcpy(ep + 2, endpoints[i], 4);
		ep[6] = i == 2 ? c->interval : 0;
	}
	e->cls = (UsbDeviceClass){
		.device = e->device_desc,
		.config = e->config_desc,
		.alternates = 1,
		.request = eth_request,
		.in = eth_in,
		.out = eth_out,
		.reset = eth_reset,
		.busy = eth_busy,
	};
}

Lan95xx *lan95xx_create(Machine *m, const Lan95xxConfig *config)
{
	Lan95xx *e = calloc(1, sizeof(*e));

	if (e == NULL)
		return NULL;
	e->machine = m;
	e->config = *config;
	event_init(&e->eeprom_done, eeprom_done, e);
	event_init(&e->phy_reset_done, phy_reset_done, e);
	event_init(&e->link_up, link_up, e);
	event_init(&e->energy_check, energy_check, e);
	make_descriptors(e);
	usb_device_init(&e->dev, &e->cls, e, USB_SPEED_HIGH);
	return e;
}

void lan95xx_destroy(Lan95xx *e)
{
	cancel(e, &e->eeprom_done);
	cancel(e, &e->phy_reset_done);
	cancel(e, &e->link_up);
	cancel(e, &e->energy_check);
	free(e);
}

void lan95xx_reset(Lan95xx *e)
{
	usb_device_reset(&e->dev);
}

UsbDevice *lan95xx_device(Lan95xx *e)
{
	return &e->dev;
}

void lan95xx_plug(Lan95xx *e, Lan95xxSend *send, void *ctx)
{
	bool was = e->send != NULL;

	e->send = send;
	e->send_ctx = ctx;
	if (send != NULL && !was) {
		if (!(e->phy.mcsr & MCSR_ENERGYON)) {
			e->phy.mcsr |= MCSR_ENERGYON;
			e->phy.isr |= ISR_ENERGYON;
		}
		cancel(e, &e->energy_check);
		phy_restart(e);
	} else if (send == NULL && was) {
		phy_link_down(e);
		schedule(e, &e->energy_check, ENERGY_NS);
	}
}

bool lan95xx_receive(Lan95xx *e, const uint8_t *frame, size_t len)
{
	if (e->send == NULL || !e->phy.link || (*reg(e, MAC_CR) & MAC_LOOPBK))
		return false;
	return rx_frame(e, frame, len);
}
