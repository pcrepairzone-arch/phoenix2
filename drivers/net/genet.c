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

/* ── Cache maintenance for non-cache-coherent GENET DMA ────────────────── */
/* BCM2711 GENET DMA is NOT cache-coherent with the ARM Cortex-A72.
 * Without these ops:
 *   TX: memcpy writes frame into D-cache.  DRAM still holds BSS zeros.
 *       TDMA DMA-reads DRAM → gets zeros → UniMAC sees src=00:00:00:00:00:00
 *       → frame silently dropped before RGMII.
 *   RX: RDMA writes received frame to DRAM.  CPU reads stale D-cache (zeros
 *       from BSS init) → ARP/IP/DHCP handler sees all-zeros → no replies.
 * CACHE_LINE = 64 bytes (Cortex-A72 L1/L2 cache line size).               */
#define CACHE_LINE        64U
#define CACHE_CLEAN(ptr, bytes) do {                                        \
    uintptr_t _a = (uintptr_t)(ptr) & ~(uintptr_t)(CACHE_LINE - 1);       \
    uintptr_t _e = (uintptr_t)(ptr) + (uintptr_t)(bytes);                  \
    for (; _a < _e; _a += CACHE_LINE)                                       \
        __asm__ volatile("dc cvac, %0" :: "r"(_a) : "memory");             \
    __asm__ volatile("dsb sy" ::: "memory");                                \
} while (0)
#define CACHE_INVAL(ptr, bytes) do {                                        \
    uintptr_t _a = (uintptr_t)(ptr) & ~(uintptr_t)(CACHE_LINE - 1);       \
    uintptr_t _e = (uintptr_t)(ptr) + (uintptr_t)(bytes);                  \
    for (; _a < _e; _a += CACHE_LINE)                                       \
        __asm__ volatile("dc civac, %0" :: "r"(_a) : "memory");            \
    __asm__ volatile("dsb sy" ::: "memory");                                \
} while (0)

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
/* g_rx_write_ptr removed in boot368: EtherGENET-6 leaves WRITE_PTR=0 always */
static uint16_t g_tx_pidx      = 0;  /* producer index into TX ring           */
static uint16_t g_tx_cidx      = 0;  /* consumer index (completed TX) tracking*/
static int      g_genet_ok     = 0;  /* set to 1 after successful init        */
static uint32_t g_rx_count     = 0;  /* frames successfully delivered to caller*/
static uint32_t g_rx_fcs       = 0;  /* frames dropped due to CRC/FCS error   */

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

    /* Reset UMAC core.
     * boot314: delay must be 200 µs (Linux udelay(200)), not 2 µs.
     * Too-short reset leaves the UMAC in partial reset; RDMA never works. */
    GW4(GENET_UMAC_CMD, 0);
    GW4(GENET_UMAC_CMD, GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    genet_delay_us(200);
    GW4(GENET_UMAC_CMD, 0);
    genet_delay_us(200);   /* post-reset settle (Linux adds a second udelay) */

    /* Clear MIB counters */
    GW4(GENET_UMAC_MIB_CTRL,
        GENET_UMAC_MIB_RESET_RUNT | GENET_UMAC_MIB_RESET_TX | GENET_UMAC_MIB_RESET_RX);
    GW4(GENET_UMAC_MIB_CTRL, 0);

    /* Set max frame length (1536 bytes) */
    GW4(GENET_UMAC_MAX_FRAME_LEN, 1536);

    /* RBUF_CTRL: explicit write — do NOT read-modify-write.
     * boot346 root-cause analysis: the GPU/VidCore firmware leaves
     * RBUF_CTRL bits 15, 14 and 6 set (= 0xc040) before handing off
     * to the ARM kernel.  Those bits throttle the RBUF→RDMA handoff
     * path so PROD_INDEX never advances regardless of ring config.
     *   boot333/334 (RDMA OK):    RBUF_CTRL=0x0002 — boot314 code
     *                             wrote an explicit 0x2, clearing GPU bits
     *   boot340–345 (RDMA BROKEN): RMW preserved 0xc040 → 0xc042
     * Fix: force RBUF_CTRL to exactly ALIGN_2B | BAD_DIS = 0x6.
     *   ALIGN_2B (bit 1): 2-byte IP-header alignment prepend
     *   BAD_DIS  (bit 2): pass CRC-bad frames to RDMA (boot313)
     *   64B_EN   (bit 0): must stay 0 — changes DMA handoff (boot307/324) */
    /* RBUF_CTRL: ALIGN_2B only — matches the empirically working boot.
     * boot314 (boots 333/334 confirmed RDMA OK, DHCP bound) had RBUF_CTRL=0x2.
     * Adding BAD_DIS (bit 2) in boot328 broke RDMA — do not set it. */
    GW4(GENET_RBUF_CTRL, GENET_RBUF_ALIGN_2B);
    debug_print("[GENET] RBUF_CTRL=0x%x (expect 0x2)\n", (unsigned)GR4(GENET_RBUF_CTRL));
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

    /* MDF_CTRL = 0: disable MAC destination filter entirely.
     * Root cause of pidx=0 since boot302 (confirmed in Mac chatlog boot324):
     * MACs were written to entries 0 and 1, but the old code enabled entries
     * 16/17 via (1u<<17)|(1u<<16) — those entries hold zero MACs, so every
     * frame was silently dropped before RBUF, including the loopback frame.
     * PROMISC in UMAC_CMD does NOT bypass MDF on GENETv5 (Linux confirms this:
     * in promisc mode Linux explicitly writes MDF_CTRL=0).
     * With MDF_CTRL=0 all frames reach RBUF; PROMISC in UMAC_CMD then passes
     * them all to RDMA.  The MDF_ADDR entries above are kept for future use
     * if we ever want selective hardware filtering.                          */
    GW4(GENET_UMAC_MDF_CTRL, 0);

    /* UMAC_CMD: set PROMISC + SPEED_1000, keep TX/RX off for now.
     * boot323: PROMISC must be SET here.  boot314 (confirmed working — ARP
     * and ICMP on the wire) had UMAC_CMD=0x18 at this point = PROMISC |
     * SPEED_1000.  Between boot314 and boot341, this was changed to CLEAR
     * PROMISC and rely on the MDF filter instead.  But the MDF filter does
     * not pass frames: boot341 shows pidx=0 for 30+ seconds after link-up
     * even though DHCP DISCOVERs are sent (TX works).  With PROMISC cleared
     * the MAC filter drops every incoming frame before it reaches RDMA.
     * Reinstating PROMISC matches boot314 exactly and unblocks RX.          */
    uint32_t cmd = GR4(GENET_UMAC_CMD);
    cmd &= ~GENET_UMAC_CMD_SPEED;
    cmd |= GENET_UMAC_CMD_PROMISC;
    cmd |= __SHIFTIN(GENET_UMAC_CMD_SPEED_1000, GENET_UMAC_CMD_SPEED);
    GW4(GENET_UMAC_CMD, cmd);
}

/* ── DMA ring init ─────────────────────────────────────────────────────── */
/* Derived from genet_init_rings() in NetBSD bcmgenet.c (BSD-2).          */
static void genet_init_rings(int qid)
{
    uint32_t val;

    /* boot309: disable RDMA before re-initialising the ring.
     * The RPi4 bootloader leaves GENET RDMA enabled with stale descriptor
     * addresses.  Re-initialising the ring while RDMA is running causes a
     * race: hardware may DMA into the old bootloader buffers or miss our
     * new buffer addresses entirely.  Disabling first ensures clean state. */
    GW4(GENET_RX_DMA_RING_CFG, 0);
    GW4(GENET_RX_DMA_CTRL,     0);
    DSB();
    genet_delay_us(10);

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
    /* END_ADDR = per-ring descriptor count × 3 words - 1 = 191.
     * The mac files reference (the version that produced working boots
     * 333/334) uses TX_DESC_COUNT×3-1=191, matching the NetBSD/inet6
     * EtherGENET-6 driver.  Using 767 (GENET_TOTAL_DESC=256) was wrong. */
    GW4(GENET_TX_DMA_END_ADDR_LO(qid),
        TX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4 - 1);  /* = 191 */
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
    /* boot366 / EtherGENET-6 line 533 (Inet6Sources bcmgenet RISCOS path):
     * Sync g_rx_cidx to the current hardware PROD_INDEX BEFORE writing
     * PROD_INDEX=0.  On a clean Pi 4 power-on the bootloader does not use
     * GENET so PROD=0, making this a no-op.  On warm reboot or reinit the
     * software consumer index stays in sync with actual hardware state and
     * does not diverge when the ring resets.                                 */
    g_rx_cidx = GR4(GENET_RX_DMA_PROD_INDEX(qid)) & 0xffff;
    GW4(GENET_RX_DMA_PROD_INDEX(qid),   0);
    GW4(GENET_RX_DMA_CONS_INDEX(qid),   0);
    GW4(GENET_RX_DMA_RING_BUF_SIZE(qid),
        __SHIFTIN(RX_DESC_COUNT, GENET_RX_DMA_RING_BUF_SIZE_DESC_COUNT) |
        __SHIFTIN(GENET_BUF_SIZE, GENET_RX_DMA_RING_BUF_SIZE_BUF_LENGTH));
    GW4(GENET_RX_DMA_START_ADDR_LO(qid), 0);
    GW4(GENET_RX_DMA_START_ADDR_HI(qid), 0);
    GW4(GENET_RX_DMA_END_ADDR_LO(qid),
        RX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4 - 1);  /* = 191 */
    GW4(GENET_RX_DMA_END_ADDR_HI(qid), 0);
    GW4(GENET_RX_DMA_XON_XOFF_THRES(qid),
        __SHIFTIN(5,                    GENET_RX_DMA_XON_XOFF_THRES_LO) |
        __SHIFTIN(RX_DESC_COUNT >> 4,   GENET_RX_DMA_XON_XOFF_THRES_HI));
    GW4(GENET_RX_DMA_READ_PTR_LO(qid), 0);
    GW4(GENET_RX_DMA_READ_PTR_HI(qid), 0);

    /* Populate all RX descriptors before enabling DMA.
     * STATUS is hardware-owned — write 0 (do NOT write BUFLEN here; that
     * confused RDMA in boot326-328).  Buffer capacity comes from the
     * RING_BUF_SIZE.BUF_LENGTH field written above, not from STATUS.        */
    DSB();
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        uint32_t paddr = (uint32_t)(uintptr_t)g_rx_buf[i];
        GW4(GENET_RX_DESC_STATUS(i),     0);
        GW4(GENET_RX_DESC_ADDRESS_LO(i), paddr);
        GW4(GENET_RX_DESC_ADDRESS_HI(i), 0);
    }
    DSB();

    /* boot368: WRITE_PTR left at 0, matching EtherGENET-6 (Inet6Sources RISCOS
     * port, lines 540-541).  The reference driver writes WRITE_PTR=0 at init
     * and never updates it per-frame or at re-arm.  Hardware uses PROD_INDEX /
     * CONS_INDEX arithmetic directly for flow control; WRITE_PTR=0 is the
     * correct quiescent value.  Our boot322-367 value of 192 (64 slots × 3
     * words) was not needed and may have contributed to sporadic STATUS=0
     * phantom frames seen in boot367.
     * WRITE_PTR_LO and WRITE_PTR_HI are already 0 from the writes above. */

    /* Enable ring and DMA now that all descriptor slots have valid addresses */
    GW4(GENET_RX_DMA_RING_CFG, __BIT(qid));  /* enable this ring */

    val  = GR4(GENET_RX_DMA_CTRL);
    val |= GENET_RX_DMA_CTRL_EN;
    val |= GENET_RX_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE);
    GW4(GENET_RX_DMA_CTRL, val);

    /* g_rx_cidx already synced above to hardware PROD before ring reset
     * (EtherGENET-6 line 533 pattern) — do NOT overwrite to 0 here.       */
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

    debug_print("[GENET] UMAC TX+RX enabled cmd=0x%08x\n",
        (unsigned)GR4(GENET_UMAC_CMD));

    /* Mark driver OK now so genet_send/genet_poll_rx work below */
    g_genet_ok = 1;

    /* boot367: loopback self-test REMOVED.
     *
     * boot342–366 used LCL_LOOP_EN to verify RDMA health at init time.
     * boot366 analysis revealed that setting and then clearing LCL_LOOP_EN
     * triggers the BCM2711 GENETv5 64-slot hardware ring scan: RDMA advances
     * PROD_INDEX through all 64 slots at ~2–3/second WITHOUT DMA'ing any data.
     * This "phantom scan" runs for ~17 seconds after LCL_LOOP_EN is cleared,
     * contaminating the first 17 seconds of real traffic with STATUS=0 phantom
     * ring advances.  Real frames still arrive (in non-phantom slots) but the
     * phantom slots cause the ring to appear fuller than it is, backing up the
     * MAC FIFO and producing 15–25 second RTTs.
     *
     * Linux bcmgenet.c and EtherGENET-6 (Inet6Sources) both omit the loopback
     * test and never set LCL_LOOP_EN.  Without it the BCM2711 ring scan does
     * not trigger: PROD stays 0 until the first real frame DMA after RGMII_LINK
     * is asserted, and RTTs should drop to <1 ms.
     *
     * The loopback test can be re-enabled as a factory diagnostic via a boot
     * flag once the phantom scan mechanism is better understood.             */

    /* Init PHY and start autoneg */
    genet_phy_init();

    debug_print("[GENET] init complete (boot369 no-loopback no-writeptr)\n");
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

/* genet_rx_ring_reset() was removed in boot366.
 * It reset PROD_INDEX=0 from inside genet_apply_link() after UMAC TX_RX_EN
 * was already active, which triggered the BCM2711 GENETv5 64-slot hardware
 * initialisation scan — the RDMA engine advanced PROD_INDEX through all 64
 * slots without DMA'ing any data, taking ~17-20 seconds and holding the MAC
 * FIFO (causing 20-second RTTs).  EtherGENET-6 (Inet6Sources) confirms:
 * never reset PROD_INDEX after the MAC is active.                            */

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
    cmd |= GENET_UMAC_CMD_PROMISC;     /* boot323: keep PROMISC on link-up */
    cmd |= GENET_UMAC_CMD_TX_RX_EN;   /* boot326/boot314: GENETv5 MAC data-path gate */
    cmd |= GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    GW4(GENET_UMAC_CMD, cmd);

    /* boot366: ring_reset REMOVED.
     *
     * boot365's genet_rx_ring_reset() wrote PROD_INDEX=0 here, which triggered
     * the BCM2711 GENETv5 64-slot hardware initialization scan: the RDMA engine
     * cycled through all 64 ring slots advancing PROD_INDEX without DMA'ing any
     * data (~17-20 seconds).  During the scan the UniMAC FIFO held all arriving
     * frames, causing 20-second RTTs and ~42% phantom-driven packet loss.
     *
     * Why the scan fires here but not in genet_init_rings():
     *   UMAC_CMD TX_RX_EN (bit 29) gates the BCM2711 scan.  In init_rings TX_RX_EN
     *   is not yet set (it is first set in the loopback test above), so writing
     *   PROD_INDEX=0 there is safe.  By the time apply_link() runs TX_RX_EN is
     *   active; any subsequent PROD_INDEX=0 write triggers the full 64-slot scan.
     *
     * EtherGENET-6 (Inet6Sources Drivers/EtherGENET-6/c/bcmgenet RISCOS port of
     * NetBSD bcmgenet.c) confirms: after rings are initialised, never reset
     * PROD_INDEX again.  Just assert RGMII_LINK and let hardware run.           */

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

    /* boot313/327: post-link RDMA state dump.
     * Key values: CTRL=0x00020001 (EN+RBUF_EN), CFG=0x00010000 (ring16 enabled)
     * d0_stat=0x08000000 = BUFLEN=2048 (untouched = no frame yet received)
     * If d0_stat changed = hardware wrote a frame into slot 0.              */
    {
        const int qid2 = GENET_DMA_DEFAULT_QUEUE;
        debug_print("[GENET] post-link RDMA: CTRL=0x%08x CFG=0x%08x"
                    " PROD=%u CONS=%u d0_stat=0x%08x d0_addr=0x%08x\n",
            (unsigned)GR4(GENET_RX_DMA_CTRL),
            (unsigned)GR4(GENET_RX_DMA_RING_CFG),
            (unsigned)(GR4(GENET_RX_DMA_PROD_INDEX(qid2)) & 0xffff),
            (unsigned)(GR4(GENET_RX_DMA_CONS_INDEX(qid2)) & 0xffff),
            (unsigned)GR4(GENET_RX_DESC_STATUS(0)),
            (unsigned)GR4(GENET_RX_DESC_ADDRESS_LO(0)));
    }
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

    /* boot338/347: flush D-cache to DRAM so TDMA reads our actual frame data,
     * not the stale BSS zeros.  Must happen AFTER memcpy, BEFORE PROD bump.
     * Without this: TDMA DMA-reads DRAM zeros → UniMAC sees src=00:00:00:00:00:00
     * → frame silently dropped before RGMII → nothing appears on wire.         */
    CACHE_CLEAN(g_tx_buf[idx], len);

    /* boot339: log first 32 TX frames for diagnostics.
     * Split into two calls (≤7 args each) — bare-metal debug_print va_list
     * does not reliably read stack-spilled args beyond 7 registers.            */
    if (g_tx_pidx < 32) {
        const uint8_t *f = g_tx_buf[idx];
        uint16_t etype = (uint16_t)((f[12] << 8) | f[13]);
        debug_print("[GENET] TX#%u src=%02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned)g_tx_pidx,
            f[6],f[7],f[8],f[9],f[10],f[11]);
        debug_print("[GENET]      dst=%02x:%02x:%02x:%02x:%02x:%02x"
                    " type=0x%04x len=%u\n",
            f[0],f[1],f[2],f[3],f[4],f[5],
            etype, (unsigned)len);
    }

    uint32_t paddr = (uint32_t)(uintptr_t)g_tx_buf[idx];
    uint32_t status = (len << 16) |
                      GENET_TX_DESC_STATUS_SOP |
                      GENET_TX_DESC_STATUS_EOP |
                      GENET_TX_DESC_STATUS_CRC;

    GW4(GENET_TX_DESC_ADDRESS_LO(idx), paddr);
    GW4(GENET_TX_DESC_ADDRESS_HI(idx), 0);
    GW4(GENET_TX_DESC_STATUS(idx),     status);
    /* boot334/347: do NOT update TX WRITE_PTR per frame.
     * EtherGENET-6 genet_start_locked() only writes PROD_INDEX to kick DMA;
     * WRITE_PTR is set once at ring init and never touched again.  Our earlier
     * per-frame WRITE_PTR update collapsed to idx×3, wrapping back to 0 on
     * descriptor 0 and corrupting the hardware's ring view on every TX.       */
    DSB();

    /* Advance producer index to kick DMA */
    g_tx_pidx = (g_tx_pidx + 1) & 0xffff;
    GW4(GENET_TX_DMA_PROD_INDEX(qid), g_tx_pidx);

    return 0;
}

/* ── Public: genet_tx_diag ─────────────────────────────────────────────── */
/* Return TX DMA producer and consumer indices (both 16-bit free-running).
 * boot329: called from lib.c ARP handler to verify TDMA is consuming frames
 * after each send — if cons stays stuck at the same value across multiple
 * sends, the TX DMA engine is not advancing (hardware-side TX is broken).  */
void genet_tx_diag(uint32_t *prod_out, uint32_t *cons_out)
{
    const int qid = GENET_DMA_DEFAULT_QUEUE;
    if (prod_out) *prod_out = g_tx_pidx & 0xffff;
    if (cons_out) *cons_out = GR4(GENET_TX_DMA_CONS_INDEX(qid)) & 0xffff;
}

/* ── Public: genet_poll_rx ─────────────────────────────────────────────── */
/* Poll for one received frame.  Returns frame length (> 0) on success,
 * 0 if no frame ready or frame skipped (error/oversized), -1 if not init.
 * Consumer index always advances so the ring never stalls on bad frames.  */
int genet_poll_rx(void *buf, uint32_t maxlen)
{
    if (!g_genet_ok) return -1;

    const int qid = GENET_DMA_DEFAULT_QUEUE;

    uint32_t pidx  = GR4(GENET_RX_DMA_PROD_INDEX(qid)) & 0xffff;
    uint32_t total = (pidx - g_rx_cidx) & 0xffff;
    if (total == 0)
        return 0;

    int idx = g_rx_cidx & (RX_DESC_COUNT - 1);

    /* boot338/347: invalidate D-cache for this slot before reading frame
     * data.  RDMA wrote to DRAM; the Cortex-A72 cache may hold stale zeros
     * from the BSS or the previous re-arm.  DC CIVAC forces a refetch.    */
    CACHE_INVAL(g_rx_buf[idx], GENET_BUF_SIZE);
    DSB();

    uint32_t status  = GR4(GENET_RX_DESC_STATUS(idx));

    uint32_t raw_len = __SHIFTOUT(status, GENET_RX_DESC_STATUS_BUFLEN);

    /* boot349: log STATUS so we can see what hardware wrote.
     * Only log first 16 calls to avoid spamming the UART.               */
    static uint32_t s_rx_log_count = 0;
    if (s_rx_log_count < 16) {
        s_rx_log_count++;
        debug_print("[GENET] poll_rx idx=%d status=0x%08x raw_len=%u total=%u\n",
                    idx, (unsigned)status, (unsigned)raw_len, (unsigned)total);
    }

    /* Advance consumer — this is the ONLY re-arm operation needed.
     *
     * boot366: EtherGENET-6 (NetBSD bcmgenet.c RISCOS port, Inet6Sources)
     * reveals that the reference driver NEVER writes STATUS, ADDRESS_LO/HI,
     * or WRITE_PTR per re-arm.  Only CONS_INDEX is updated per consumed frame.
     * Buffer addresses are written ONCE at init (genet_init_rings) and left
     * permanently — hardware reuses the same physical address for each new
     * frame it DMAs into that slot.
     *
     * Root cause of STATUS=0 phantoms in boot364/365: our per-rearm writes
     * of STATUS=0, ADDRESS_LO/HI, and WRITE_PTR were triggering BCM2711
     * GENETv5 internal RDMA recalculation, causing it to advance PROD_INDEX
     * ~64 times (full ring scan) without DMA'ing any data.  WRITE_PTR is
     * set once at init (to 192) and NEVER updated — hardware uses PROD/CONS
     * arithmetic directly for flow control, not WRITE_PTR.
     *
     * Reference: EtherGENET-6/c/bcmgenet genet_rxintr() RISCOS path:
     *   sc->sc_rx.cidx = (sc->sc_rx.cidx + 1) & 0xffff;
     *   WR4(sc, GENET_RX_DMA_CONS_INDEX(qid), sc->sc_rx.cidx);
     *   [no STATUS write, no ADDRESS write, no WRITE_PTR write]             */
    g_rx_cidx = (g_rx_cidx + 1) & 0xffff;
    GW4(GENET_RX_DMA_CONS_INDEX(qid), g_rx_cidx);

    if (status & GENET_RX_DESC_STATUS_ALL_ERRS) {
        if (status & GENET_RX_DESC_STATUS_CRC_ERR)
            g_rx_fcs++;
        debug_print("[GENET] RX hw error status=0x%08x\n", (unsigned)status);
        return 0;
    }

    if (raw_len <= (uint32_t)ETHER_ALIGN)
        return 0;
    uint32_t frame_len = raw_len - ETHER_ALIGN;

    if (frame_len > maxlen) {
        debug_print("[GENET] RX oversized %u > %u\n",
                    (unsigned)frame_len, (unsigned)maxlen);
        return 0;
    }

    memcpy(buf, g_rx_buf[idx] + ETHER_ALIGN, frame_len);

    if (g_rx_count < 32) {
        const uint8_t *f = (const uint8_t *)buf;
        uint16_t etype = (uint16_t)((f[12] << 8) | f[13]);
        debug_print("[GENET] RX#%u src=%02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned)g_rx_count,
            f[6],f[7],f[8],f[9],f[10],f[11]);
        debug_print("[GENET]      dst=%02x:%02x:%02x:%02x:%02x:%02x"
                    " type=0x%04x len=%u\n",
            f[0],f[1],f[2],f[3],f[4],f[5],
            etype, (unsigned)frame_len);
    }

    g_rx_count++;
    return (int)frame_len;
}

/* ── Public: genet_rx_available ────────────────────────────────────────── */
/* Returns number of frames currently in the RDMA ring awaiting poll_rx.   */
int genet_rx_available(void)
{
    if (!g_genet_ok) return 0;
    const int qid = GENET_DMA_DEFAULT_QUEUE;
    uint32_t pidx = GR4(GENET_RX_DMA_PROD_INDEX(qid)) & 0xffff;
    return (int)((pidx - g_rx_cidx) & 0xffff);
}

/* ── Public: genet_rx_count_raw ────────────────────────────────────────── */
uint32_t genet_rx_count_raw(void)  { return g_rx_count; }

/* ── Public: genet_rx_fcs_raw ──────────────────────────────────────────── */
uint32_t genet_rx_fcs_raw(void)    { return g_rx_fcs; }

/* ── Public: genet_tx_cons_raw ─────────────────────────────────────────── */
uint32_t genet_tx_cons_raw(void)
{
    const int qid = GENET_DMA_DEFAULT_QUEUE;
    return GR4(GENET_TX_DMA_CONS_INDEX(qid)) & 0xffff;
}

