/*
 * genet.c — Phoenix bare-metal BCM2711 GENETv5 Ethernet driver.
 * boot302: polling-mode TX/RX, no IRQs.
 *
 * Derived from:
 *   NetBSD sys/dev/ic/bcmgenet.c,v 1.7 2020/06/27
 *   Copyright (c) 2020 Jared McNeill <jmcneill@invisible.ca>
 *   BSD 2-Clause licence — see bcmgenetreg.h
 *
 *   RISC OS Open EtherGENET-6: glue.c, egemodule.c
 *   Copyright (c) 2019-2020 RISC OS Open Ltd
 *   BSD 3-Clause licence
 *
 * Phoenix-specific changes: removed OS module framework, DCI, mbuf stack,
 * callout timers, and mutex layer.  Replaced with static buffers, direct
 * MMIO, mailbox MAC read, and a simple poll/send API.
 *
 * Hardware: BCM2711 GENETv5 at 0xFD580000, PHY BCM54213PE at MDIO addr 1.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kernel.h"                             /* debug_print, uart_puts   */
#include "drivers/gpu/mailbox_property.h"       /* mbox_get_mac_address()   */
#include "drivers/net/genet.h"
#include "drivers/net/bcmgenetreg.h"

/* ── Bit-field helpers (from bcmgenetreg.h convention) ─────────────────── */
#define __BIT(n)          (1u << (n))
#define __BITS(hi,lo)     (((2u << (hi)) - 1u) & ~((1u << (lo)) - 1u))
#define __SHIFTOUT(v,m)   (((v) & (m)) / ((m) & (-(m))))
#define __SHIFTIN(v,m)    (((v) * ((m) & (-(m)))) & (m))

/* ── Hardware base address ─────────────────────────────────────────────── */
/* BCM2711 GENETv5 is at 0xFD580000 regardless of periph_base.            */
#define GENET_BASE        0xFD580000UL

/* ── MMIO access ───────────────────────────────────────────────────────── */
#define GR4(reg)          (*(volatile uint32_t *)(GENET_BASE + (reg)))
#define GW4(reg,val)      do { *(volatile uint32_t *)(GENET_BASE + (reg)) = (val); } while (0)

/* Memory barrier before touching DMA descriptors */
#define DSB()             __asm__ volatile("dsb sy" ::: "memory")

/* ── PHY constants ─────────────────────────────────────────────────────── */
#define GENET_PHY_ADDR    1                     /* BCM54213PE on Pi 4       */
/* Standard MII register numbers */
#define MII_BMCR          0x00                  /* Basic Mode Control       */
#define  BMCR_AUTOEN      0x1000                /* Auto-Negotiation Enable  */
#define  BMCR_ANRESTART   0x0200                /* AN Restart               */
#define  BMCR_RESET       0x8000
#define MII_BMSR          0x01                  /* Basic Mode Status        */
#define  BMSR_LSTATUS     0x0004                /* Link Status              */
#define  BMSR_ANEGCOMPLETE 0x0020               /* Autoneg Complete         */
#define MII_PHYID1        0x02
#define MII_PHYID2        0x03

/* ── Descriptor count ──────────────────────────────────────────────────── */
#define RX_DESC_COUNT     GENET_DMA_DESC_COUNT  /* 64 */
#define TX_DESC_COUNT     GENET_DMA_DESC_COUNT  /* 64 */
#define RX_NEXT(i)        (((i) + 1) & (RX_DESC_COUNT - 1))
#define TX_NEXT(i)        (((i) + 1) & (TX_DESC_COUNT - 1))

/* ETHER_ALIGN: GENET prepends 2 bytes for 4-byte alignment of IP header.
 * Received length includes these 2 bytes; actual frame starts at offset 2. */
#define ETHER_ALIGN       2

/* ── Static DMA packet buffers ─────────────────────────────────────────── */
/* Phoenix is bare-metal: virtual address == physical address.              */
static uint8_t g_rx_buf[RX_DESC_COUNT][GENET_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t g_tx_buf[TX_DESC_COUNT][GENET_BUF_SIZE] __attribute__((aligned(64)));

/* ── Driver state ──────────────────────────────────────────────────────── */
uint8_t g_genet_mac[6];

static uint16_t g_rx_cidx      = 0;  /* consumer index into RX ring           */
static uint32_t g_rx_write_ptr = 0;  /* tracked RDMA WRITE_PTR (word units)   */
static uint16_t g_tx_pidx      = 0;  /* producer index into TX ring           */
static uint16_t g_tx_cidx      = 0;  /* consumer index (completed TX) tracking*/
static int      g_genet_ok     = 0;  /* set to 1 after successful init        */

/* ── Tiny busy-wait delay ──────────────────────────────────────────────── */
static void genet_delay_us(uint32_t us)
{
    /* ARM system counter: CNTPCT_EL0 at typically 54 MHz on Pi 4 */
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t end = cnt + (freq / 1000000ULL) * us;
    do {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    } while (cnt < end);
}

/* ── MDIO (MII management bus) ─────────────────────────────────────────── */
static int genet_mdio_wait(void)
{
    for (int i = 0; i < 1000; i++) {
        if (!(GR4(GENET_MDIO_CMD) & GENET_MDIO_START_BUSY))
            return 0;
        genet_delay_us(10);
    }
    debug_print("[GENET] MDIO timeout\n");
    return -1;
}

static uint16_t genet_mdio_read(int phy, int reg)
{
    GW4(GENET_MDIO_CMD,
        GENET_MDIO_START_BUSY | GENET_MDIO_READ |
        __SHIFTIN(phy, GENET_MDIO_PMD)  |
        __SHIFTIN(reg, GENET_MDIO_REG));
    if (genet_mdio_wait() < 0) return 0xffff;
    return (uint16_t)(GR4(GENET_MDIO_CMD) & 0xffff);
}

static void genet_mdio_write(int phy, int reg, uint16_t val)
{
    GW4(GENET_MDIO_CMD,
        GENET_MDIO_START_BUSY | GENET_MDIO_WRITE |
        __SHIFTIN(phy, GENET_MDIO_PMD)  |
        __SHIFTIN(reg, GENET_MDIO_REG)  |
        val);
    genet_mdio_wait();
}

/* ── PHY init ──────────────────────────────────────────────────────────── */
static int genet_phy_init(void)
{
    /* Read PHY ID to confirm it's there */
    uint16_t id1 = genet_mdio_read(GENET_PHY_ADDR, MII_PHYID1);
    uint16_t id2 = genet_mdio_read(GENET_PHY_ADDR, MII_PHYID2);

    if (id1 == 0xffff && id2 == 0xffff) {
        debug_print("[GENET] no PHY at addr %d\n", GENET_PHY_ADDR);
        return -1;
    }
    debug_print("[GENET] PHY id=%04x:%04x at addr %d\n", id1, id2, GENET_PHY_ADDR);

    /* Reset PHY */
    genet_mdio_write(GENET_PHY_ADDR, MII_BMCR, BMCR_RESET);
    for (int i = 0; i < 100; i++) {
        genet_delay_us(1000);
        if (!(genet_mdio_read(GENET_PHY_ADDR, MII_BMCR) & BMCR_RESET))
            break;
    }

    /* Enable and restart auto-negotiation */
    genet_mdio_write(GENET_PHY_ADDR, MII_BMCR, BMCR_AUTOEN | BMCR_ANRESTART);
    debug_print("[GENET] PHY autoneg started\n");
    return 0;
}

/* ── Soft reset ────────────────────────────────────────────────────────── */
/* Derived from genet_reset() in NetBSD bcmgenet.c (BSD-2, Jared McNeill) */
static void genet_reset(void)
{
    uint32_t val;

    /* Flush RX buffer */
    val  = GR4(GENET_SYS_RBUF_FLUSH_CTRL);
    val |= GENET_SYS_RBUF_FLUSH_RESET;
    GW4(GENET_SYS_RBUF_FLUSH_CTRL, val);
    genet_delay_us(10);
    val &= ~GENET_SYS_RBUF_FLUSH_RESET;
    GW4(GENET_SYS_RBUF_FLUSH_CTRL, val);
    genet_delay_us(10);
    GW4(GENET_SYS_RBUF_FLUSH_CTRL, 0);
    genet_delay_us(10);

    /* Reset UMAC core */
    GW4(GENET_UMAC_CMD, 0);
    GW4(GENET_UMAC_CMD, GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    genet_delay_us(2);
    GW4(GENET_UMAC_CMD, 0);

    /* Clear MIB counters */
    GW4(GENET_UMAC_MIB_CTRL,
        GENET_UMAC_MIB_RESET_RUNT | GENET_UMAC_MIB_RESET_TX | GENET_UMAC_MIB_RESET_RX);
    GW4(GENET_UMAC_MIB_CTRL, 0);

    /* Set max frame length (1536 bytes) */
    GW4(GENET_UMAC_MAX_FRAME_LEN, 1536);

    /* Enable 64-byte receive context + 2-byte IP alignment header */
    GW4(GENET_RBUF_CTRL, GENET_RBUF_64B_EN | GENET_RBUF_ALIGN_2B);
    GW4(GENET_RBUF_TBUF_SIZE_CTRL, 1);
}

/* ── MAC address filter ─────────────────────────────────────────────────── */
/* Accept our unicast MAC and broadcast; drop everything else.             */
/* Derived from genet_setup_rxfilter() in NetBSD bcmgenet.c (BSD-2).      */
static void genet_setup_rxfilter(const uint8_t *mac)
{
    /* MDF entry 0: our unicast MAC */
    uint32_t addr0 = (mac[3]       ) | (mac[2] <<  8) |
                     (mac[1] << 16 ) | (mac[0] << 24);
    uint32_t addr1 = (mac[5]       ) | (mac[4] <<  8);
    GW4(GENET_UMAC_MDF_ADDR0(0), addr0);
    GW4(GENET_UMAC_MDF_ADDR1(0), addr1);

    /* MDF entry 1: broadcast FF:FF:FF:FF:FF:FF */
    GW4(GENET_UMAC_MDF_ADDR0(1), 0xffffffff);
    GW4(GENET_UMAC_MDF_ADDR1(1), 0x0000ffff);

    /* Enable both entries (bits 17 and 16) */
    GW4(GENET_UMAC_MDF_CTRL, (1u << 17) | (1u << 16));

    /* UMAC_CMD: clear promisc, set speed=1000, keep TX/RX off for now */
    uint32_t cmd = GR4(GENET_UMAC_CMD);
    cmd &= ~(GENET_UMAC_CMD_PROMISC | GENET_UMAC_CMD_SPEED);
    cmd |= __SHIFTIN(GENET_UMAC_CMD_SPEED_1000, GENET_UMAC_CMD_SPEED);
    GW4(GENET_UMAC_CMD, cmd);
}

/* ── DMA ring init ─────────────────────────────────────────────────────── */
/* Derived from genet_init_rings() in NetBSD bcmgenet.c (BSD-2).          */
static void genet_init_rings(int qid)
{
    uint32_t val;

    /* ── TX ring ──────────────────────────────────────────────────────── */
    GW4(GENET_TX_SCB_BURST_SIZE, 0x08);
    GW4(GENET_TX_DMA_READ_PTR_LO(qid), 0);
    GW4(GENET_TX_DMA_READ_PTR_HI(qid), 0);
    GW4(GENET_TX_DMA_CONS_INDEX(qid),  0);
    GW4(GENET_TX_DMA_PROD_INDEX(qid),  0);
    GW4(GENET_TX_DMA_RING_BUF_SIZE(qid),
        __SHIFTIN(TX_DESC_COUNT, GENET_TX_DMA_RING_BUF_SIZE_DESC_COUNT) |
        __SHIFTIN(GENET_BUF_SIZE, GENET_TX_DMA_RING_BUF_SIZE_BUF_LENGTH));
    GW4(GENET_TX_DMA_START_ADDR_LO(qid), 0);
    GW4(GENET_TX_DMA_START_ADDR_HI(qid), 0);
    GW4(GENET_TX_DMA_END_ADDR_LO(qid),
        TX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4 - 1);
    GW4(GENET_TX_DMA_END_ADDR_HI(qid), 0);
    GW4(GENET_TX_DMA_MBUF_DONE_THRES(qid), 1);
    GW4(GENET_TX_DMA_FLOW_PERIOD(qid),     0);
    GW4(GENET_TX_DMA_WRITE_PTR_LO(qid),    0);
    GW4(GENET_TX_DMA_WRITE_PTR_HI(qid),    0);

    GW4(GENET_TX_DMA_RING_CFG, __BIT(qid));  /* enable this ring */

    val  = GR4(GENET_TX_DMA_CTRL);
    val |= GENET_TX_DMA_CTRL_EN;
    val |= GENET_TX_DMA_CTRL_RBUF_EN(qid);
    GW4(GENET_TX_DMA_CTRL, val);

    /* ── RX ring ──────────────────────────────────────────────────────── */
    GW4(GENET_RX_SCB_BURST_SIZE, 0x08);
    GW4(GENET_RX_DMA_WRITE_PTR_LO(qid), 0);
    GW4(GENET_RX_DMA_WRITE_PTR_HI(qid), 0);
    GW4(GENET_RX_DMA_PROD_INDEX(qid),   0);
    GW4(GENET_RX_DMA_CONS_INDEX(qid),   0);
    GW4(GENET_RX_DMA_RING_BUF_SIZE(qid),
        __SHIFTIN(RX_DESC_COUNT, GENET_RX_DMA_RING_BUF_SIZE_DESC_COUNT) |
        __SHIFTIN(GENET_BUF_SIZE, GENET_RX_DMA_RING_BUF_SIZE_BUF_LENGTH));
    GW4(GENET_RX_DMA_START_ADDR_LO(qid), 0);
    GW4(GENET_RX_DMA_START_ADDR_HI(qid), 0);
    GW4(GENET_RX_DMA_END_ADDR_LO(qid),
        RX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4 - 1);
    GW4(GENET_RX_DMA_END_ADDR_HI(qid), 0);
    GW4(GENET_RX_DMA_XON_XOFF_THRES(qid),
        __SHIFTIN(5,                    GENET_RX_DMA_XON_XOFF_THRES_LO) |
        __SHIFTIN(RX_DESC_COUNT >> 4,   GENET_RX_DMA_XON_XOFF_THRES_HI));
    GW4(GENET_RX_DMA_READ_PTR_LO(qid), 0);
    GW4(GENET_RX_DMA_READ_PTR_HI(qid), 0);

    GW4(GENET_RX_DMA_RING_CFG, __BIT(qid));  /* enable this ring */

    val  = GR4(GENET_RX_DMA_CTRL);
    val |= GENET_RX_DMA_CTRL_EN;
    val |= GENET_RX_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE);
    GW4(GENET_RX_DMA_CTRL, val);

    /* Pre-populate all RX descriptors with buffer addresses.
     * boot341: STATUS must be 0 at init.  The STATUS word (offset +0 in each
     * descriptor) is HARDWARE-WRITTEN — GENET fills it (frame length, OWN,
     * EOP, SOP, error bits) when a frame arrives.  Software must not pre-set
     * any bits here.  Slot availability is communicated to hardware via
     * WRITE_PTR only.  Writing OWN|EOP=0xC000 (as boot340 did) puts garbage
     * in the hardware-owned status word and appears to prevent RDMA from
     * filling those slots — PROD_INDEX never advances.  boot314 had STATUS=0
     * and worked (confirmed from bootlog333 readback: status=0x00000000).    */
    DSB();
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        uint32_t paddr = (uint32_t)(uintptr_t)g_rx_buf[i];
        GW4(GENET_RX_DESC_STATUS(i),     0);      /* hardware fills on RX  */
        GW4(GENET_RX_DESC_ADDRESS_LO(i), paddr);
        GW4(GENET_RX_DESC_ADDRESS_HI(i), 0);
    }
    DSB();

    /* RDMA WRITE_PTR: free-running word-address counter.
     * After arming all 64 slots WRITE_PTR = 64 * (12/4) = 192.
     * Hardware tracks (WRITE_PTR - its_internal_read_ptr) to know how many
     * slots are available.  Each subsequent refill increments by 3 (one
     * descriptor's worth of 32-bit words) — see genet_poll_rx.             */
    g_rx_write_ptr = (uint32_t)(RX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4);
    GW4(GENET_RX_DMA_WRITE_PTR_LO(qid), g_rx_write_ptr);
    GW4(GENET_RX_DMA_WRITE_PTR_HI(qid), 0);

    g_rx_cidx = 0;
    g_tx_pidx = 0;
    g_tx_cidx = 0;

    /* Diagnostic readback — mirrors boot314 output so we can compare logs  */
    debug_print("[GENET] RX ring: WRITE_PTR=%u PROD=%u CONS=%u\n",
        (unsigned)(GR4(GENET_RX_DMA_WRITE_PTR_LO(qid)) & 0xffff),
        (unsigned)(GR4(GENET_RX_DMA_PROD_INDEX(qid))   & 0xffff),
        (unsigned)(GR4(GENET_RX_DMA_CONS_INDEX(qid))   & 0xffff));
    debug_print("[GENET] RX desc[0] addr_lo=0x%08x status=0x%08x\n",
        (unsigned)GR4(GENET_RX_DESC_ADDRESS_LO(0)),
        (unsigned)GR4(GENET_RX_DESC_STATUS(0)));
}

/* ── IRQ mask: mask everything (polling mode) ───────────────────────────── */
static void genet_disable_intr(void)
{
    GW4(GENET_INTRL2_CPU_SET_MASK, 0xffffffff);
    GW4(GENET_INTRL2_CPU_CLEAR,    0xffffffff);
}

/* ── Public: genet_init ────────────────────────────────────────────────── */
void genet_init(void)
{
    const int qid = GENET_DMA_DEFAULT_QUEUE;

    /* Verify hardware revision — must be GENETv5 */
    uint32_t rev = GR4(GENET_SYS_REV_CTRL);
    uint32_t maj = __SHIFTOUT(rev, SYS_REV_MAJOR);
    uint32_t min = __SHIFTOUT(rev, SYS_REV_MINOR);
    if (maj == 0) maj = 1; else if (maj == 5 || maj == 6) maj--;
    debug_print("[GENET] GENETv%u.%u detected\n", (unsigned)maj, (unsigned)min);
    if (maj != 5) {
        debug_print("[GENET] unsupported version — skipping init\n");
        return;
    }

    /* Get board MAC address from VideoCore OTP */
    if (mbox_get_mac_address(g_genet_mac) < 0) {
        debug_print("[GENET] mailbox MAC read failed — using fallback\n");
        /* Locally administered fallback: DE:AD:BE:EF:00:01 */
        g_genet_mac[0] = 0xde; g_genet_mac[1] = 0xad;
        g_genet_mac[2] = 0xbe; g_genet_mac[3] = 0xef;
        g_genet_mac[4] = 0x00; g_genet_mac[5] = 0x01;
    }
    debug_print("[GENET] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                g_genet_mac[0], g_genet_mac[1], g_genet_mac[2],
                g_genet_mac[3], g_genet_mac[4], g_genet_mac[5]);

    /* Select external RGMII-ID PHY mode (Pi 4 BCM54213PE) */
    GW4(GENET_SYS_PORT_CTRL, GENET_SYS_PORT_MODE_EXT_GPHY);

    /* Configure RGMII OOB signalling.
     * boot308/339: BCM54213PE operates in RGMII-ID mode — the PHY adds
     * clock delay on both TX and RX.  The MAC must NOT add its own delay,
     * so ID_MODE_DISABLE must be SET (= disable internal MAC clock delay).
     * Clearing ID_MODE_DISABLE (the boot302 mistake) causes double delay:
     * PHY delay + MAC delay → RGMII RX clock misalignment → UMAC CRC
     * fails on every received frame → RDMA sees nothing → pidx stays 0.
     * This matches Linux bcmgenet.c and NetBSD bcmgenet.c for 1000BASE-T. */
    uint32_t oob = GR4(GENET_EXT_RGMII_OOB_CTRL);
    oob |= GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;   /* PHY provides delay  */
    oob |= GENET_EXT_RGMII_OOB_RGMII_MODE_EN;     /* enable RGMII OOB    */
    GW4(GENET_EXT_RGMII_OOB_CTRL, oob);

    /* Soft-reset UMAC and RX/TX paths */
    genet_reset();

    /* Write MAC address into UMAC */
    uint32_t m0 = ((uint32_t)g_genet_mac[0] << 24) |
                  ((uint32_t)g_genet_mac[1] << 16) |
                  ((uint32_t)g_genet_mac[2] <<  8) |
                   (uint32_t)g_genet_mac[3];
    uint32_t m1 = ((uint32_t)g_genet_mac[4] <<  8) |
                   (uint32_t)g_genet_mac[5];
    GW4(GENET_UMAC_MAC0, m0);
    GW4(GENET_UMAC_MAC1, m1);

    /* Set up MAC address filter (our addr + broadcast) */
    genet_setup_rxfilter(g_genet_mac);

    /* Set up TX/RX descriptor rings */
    genet_init_rings(qid);

    /* Mask all interrupts — polling only for boot302 */
    genet_disable_intr();

    /* Enable UMAC transmitter and receiver */
    uint32_t cmd = GR4(GENET_UMAC_CMD);
    cmd |= GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    GW4(GENET_UMAC_CMD, cmd);

    debug_print("[GENET] UMAC TX+RX enabled\n");

    /* Init PHY and start autoneg */
    genet_phy_init();

    g_genet_ok = 1;
    debug_print("[GENET] init complete (boot341 polling mode)\n");
}

/* ── Public: genet_link_up ─────────────────────────────────────────────── */
int genet_link_up(void)
{
    if (!g_genet_ok) return 0;
    uint16_t bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
    /* Read twice — link bit is latched-low on some PHYs */
    bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
    return (bmsr & BMSR_LSTATUS) ? 1 : 0;
}

/* ── Public: genet_rx_pidx_raw ─────────────────────────────────────────── */
/* Returns the raw RX DMA producer index from hardware — used by the WIMP
 * heartbeat log to confirm RX DMA is advancing (i.e. packets arriving).   */
uint32_t genet_rx_pidx_raw(void)
{
    if (!g_genet_ok) return 0;
    return GR4(GENET_RX_DMA_PROD_INDEX(GENET_DMA_DEFAULT_QUEUE)) & 0xffff;
}

/* ── Public: genet_apply_link ──────────────────────────────────────────── */
/* Read PHY-negotiated speed via MDIO and program UMAC_CMD speed field to
 * match.  Must be called when link comes UP (boot308 fix): if UMAC speed
 * doesn't match the PHY's RGMII clock, pidx never advances on RX.
 *
 * MII register 5  (LPA)      — advertised by partner at 10/100
 * MII register 10 (STAT1000) — advertised by partner at 1000 Mbps         */
void genet_apply_link(void)
{
    if (!g_genet_ok) return;

    /* MII reg 10 (1000BASE-T Status): bits 11:10 = partner 1000Mbps caps */
#define MII_STAT1000  10
    uint16_t stat1000 = genet_mdio_read(GENET_PHY_ADDR, MII_STAT1000);
    /* MII reg 5 (LPA): bits 8:5 = partner 100/10 caps */
#define MII_LPA       5
    uint16_t lpa      = genet_mdio_read(GENET_PHY_ADDR, MII_LPA);

    uint32_t spd;
    if (stat1000 & ((1u << 11) | (1u << 10))) {
        spd = GENET_UMAC_CMD_SPEED_1000;
        debug_print("[GENET] link speed: 1000 Mbps\n");
    } else if (lpa & ((1u << 8) | (1u << 7))) {
        spd = GENET_UMAC_CMD_SPEED_100;
        debug_print("[GENET] link speed: 100 Mbps\n");
    } else {
        spd = GENET_UMAC_CMD_SPEED_10;
        debug_print("[GENET] link speed: 10 Mbps\n");
    }

    uint32_t cmd = GR4(GENET_UMAC_CMD);
    cmd &= ~GENET_UMAC_CMD_SPEED;
    cmd |= __SHIFTIN(spd, GENET_UMAC_CMD_SPEED);
    GW4(GENET_UMAC_CMD, cmd);

    /* Update EXT_RGMII_OOB_CTRL on link-up — mirrors Circle bcm54213.cpp
     * mii_setup() / adjust_link() sequence:
     *   • ID_MODE_DISABLE: set at 1Gbps (PHY provides delay, MAC must not);
     *                      clear at 10/100 (no delay needed).
     *   • OOB_DISABLE:     clear — enable OOB (link/speed) signalling.
     *   • RGMII_LINK:      set — tell MAC the physical link is established.
     *     Without RGMII_LINK set the MAC does not forward received frames
     *     to RDMA, so PROD_INDEX never advances regardless of other fixes.
     * boot341: prints OOB before/after to confirm correct bits at runtime.  */
    uint32_t oob = GR4(GENET_EXT_RGMII_OOB_CTRL);
    debug_print("[GENET] apply_link: OOB before=0x%08x\n", (unsigned)oob);
    if (spd == GENET_UMAC_CMD_SPEED_1000)
        oob |= GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    else
        oob &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    oob &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;  /* enable OOB signalling   */
    oob |=  GENET_EXT_RGMII_OOB_RGMII_LINK;   /* assert link-up to MAC   */
    GW4(GENET_EXT_RGMII_OOB_CTRL, oob);
    debug_print("[GENET] apply_link: OOB after=0x%08x UMAC_CMD=0x%08x\n",
        (unsigned)GR4(GENET_EXT_RGMII_OOB_CTRL),
        (unsigned)GR4(GENET_UMAC_CMD));
}

/* ── Public: genet_send ────────────────────────────────────────────────── */
/* Transmit one Ethernet frame (header + payload, no FCS — GENET appends). */
int genet_send(const void *buf, uint32_t len)
{
    if (!g_genet_ok) return -1;
    if (len == 0 || len > GENET_MAX_FRAME) return -1;

    const int qid = GENET_DMA_DEFAULT_QUEUE;

    /* Check TX ring has room (one free slot) */
    uint32_t cidx = GR4(GENET_TX_DMA_CONS_INDEX(qid)) & 0xffff;
    uint32_t used = (g_tx_pidx - (uint16_t)cidx) & 0xffff;
    if (used >= (uint32_t)(TX_DESC_COUNT - 1)) {
        debug_print("[GENET] TX ring full\n");
        return -1;
    }

    int idx = g_tx_pidx & (TX_DESC_COUNT - 1);

    /* Copy frame into our static TX buffer */
    memcpy(g_tx_buf[idx], buf, len);
    DSB();

    uint32_t paddr = (uint32_t)(uintptr_t)g_tx_buf[idx];
    uint32_t status = (len << 16) |
                      GENET_TX_DESC_STATUS_SOP |
                      GENET_TX_DESC_STATUS_EOP |
                      GENET_TX_DESC_STATUS_CRC;

    GW4(GENET_TX_DESC_ADDRESS_LO(idx), paddr);
    GW4(GENET_TX_DESC_ADDRESS_HI(idx), 0);
    GW4(GENET_TX_DESC_STATUS(idx),     status);
    /* boot320: update TDMA WRITE_PTR to the word address of this descriptor
     * so hardware knows which SRAM slot contains the new TX frame.
     * Without this, WRITE_PTR stays 0 and hardware reuses descriptor 0.  */
    GW4(GENET_TX_DMA_WRITE_PTR_LO(qid),
        (uint32_t)idx * (GENET_DMA_DESC_SIZE / 4));
    GW4(GENET_TX_DMA_WRITE_PTR_HI(qid), 0);
    DSB();

    /* Advance producer index to kick DMA */
    g_tx_pidx = (g_tx_pidx + 1) & 0xffff;
    GW4(GENET_TX_DMA_PROD_INDEX(qid), g_tx_pidx);

    return 0;
}

/* ── Public: genet_poll_rx ─────────────────────────────────────────────── */
/* Returns received frame length (>0), 0 if nothing ready, -1 on error.   */
/* Derived from genet_rxintr() in NetBSD bcmgenet.c (BSD-2, Jared McNeill)*/
int genet_poll_rx(void *buf, uint32_t maxlen)
{
    if (!g_genet_ok) return -1;

    const int qid = GENET_DMA_DEFAULT_QUEUE;

    /* Check how many new packets the DMA engine has written */
    uint32_t pidx  = GR4(GENET_RX_DMA_PROD_INDEX(qid)) & 0xffff;
    uint32_t total = (pidx - g_rx_cidx) & 0xffff;
    if (total == 0)
        return 0;

    int idx = g_rx_cidx & (RX_DESC_COUNT - 1);

    DSB();  /* ensure descriptor write from GENET is visible */
    uint32_t status = GR4(GENET_RX_DESC_STATUS(idx));
    uint32_t raw_len = __SHIFTOUT(status, GENET_RX_DESC_STATUS_BUFLEN);

    /* Advance consumer index regardless of errors */
    g_rx_cidx = (g_rx_cidx + 1) & 0xffff;
    GW4(GENET_RX_DMA_CONS_INDEX(qid), g_rx_cidx);

    /* Re-arm this descriptor slot.
     * boot341: STATUS=0 (hardware fills it on the next received frame).
     * Do NOT write OWN|EOP here — see genet_init_rings for why.          */
    uint32_t paddr = (uint32_t)(uintptr_t)g_rx_buf[idx];
    GW4(GENET_RX_DESC_STATUS(idx),     0);
    GW4(GENET_RX_DESC_ADDRESS_LO(idx), paddr);
    GW4(GENET_RX_DESC_ADDRESS_HI(idx), 0);
    /* Advance WRITE_PTR by one descriptor (3 words) using the tracked
     * free-running counter g_rx_write_ptr.
     * boot340 bug: calculated next*3 from g_rx_cidx, collapsing WRITE_PTR
     * from 192 down to 3 after the first frame — hardware saw only 1 slot
     * available.  After slot 63 it would collapse to 0 (no slots).
     * boot341 fix: WRITE_PTR is a free-running counter; just += 3 each time
     * and let hardware use START/END_ADDR for ring-wrap internally.         */
    g_rx_write_ptr = (g_rx_write_ptr + (GENET_DMA_DESC_SIZE / 4)) & 0xffff;
    GW4(GENET_RX_DMA_WRITE_PTR_LO(qid), g_rx_write_ptr);
    GW4(GENET_RX_DMA_WRITE_PTR_HI(qid), 0);

    /* Check for hardware errors */
    if (status & GENET_RX_DESC_STATUS_ALL_ERRS) {
        debug_print("[GENET] RX error status=0x%08x\n", (unsigned)status);
        return 0;
    }

    /* raw_len includes the 2-byte ETHER_ALIGN header; strip it */
    if (raw_len <= (uint32_t)ETHER_ALIGN)
        return 0;
    uint32_t frame_len = raw_len - ETHER_ALIGN;

    if (frame_len > maxlen) {
        debug_print("[GENET] RX frame too large (%u > %u)\n",
                    (unsigned)frame_len, (unsigned)maxlen);
        return 0;
    }

    /* Copy from the 2-byte offset position in the DMA buffer */
    memcpy(buf, g_rx_buf[idx] + ETHER_ALIGN, frame_len);

    return (int)frame_len;
}
