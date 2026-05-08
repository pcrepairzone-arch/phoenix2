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
#include "irq.h"                                /* irq_set_handler, irq_unmask */
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

/* ── GIC interrupt ID for GENET ────────────────────────────────────────── */
/* BCM2711 DTS (bcm2711.dtsi): ethernet@7d580000 interrupts =
 *   <GIC_SPI 157 IRQ_TYPE_LEVEL_HIGH>  — "mac_0"  (RX/TX/MDIO events)
 *   <GIC_SPI 158 IRQ_TYPE_LEVEL_HIGH>  — "mac_1"  (secondary)
 * GIC INTID = SPI_number + 32, so mac_0 → INTID 189.
 * RXDMA_DONE (INTRL2 bit 13) is delivered on mac_0.                      */
#define GENET_GIC_INTID   189

/* ── MMIO access ───────────────────────────────────────────────────────── */
#define GR4(reg)          (*(volatile uint32_t *)(GENET_BASE + (reg)))
#define GW4(reg,val)      do { *(volatile uint32_t *)(GENET_BASE + (reg)) = (val); } while (0)

/* Memory barrier before touching DMA descriptors */
#define DSB()             __asm__ volatile("dsb sy" ::: "memory")

/* ── Cache maintenance for non-cache-coherent GENET DMA ────────────────── */
/* BCM2711 GENET DMA is NOT cache-coherent with the ARM Cortex-A72.
 * Linux bcmgenet.c calls dma_map_single(DMA_TO_DEVICE) before TX and
 * dma_map_single(DMA_FROM_DEVICE) before RX, which translates to DC CVAC
 * (clean) and DC CIVAC (clean+invalidate) on arm64.  Without these ops:
 *
 *   TX: memcpy writes frame into D-cache.  DRAM still holds BSS zeros.
 *       TDMA DMA-reads DRAM → gets zeros → UniMAC sees src=00:00:00:00:00:00
 *       → UniMAC silently drops the frame before RGMII → nothing on wire.
 *       (boot337 confirmed: mib=7 consumed by TDMA but zero Pi frames in pcap)
 *
 *   RX: RDMA writes received frame to DRAM.  CPU reads D-cache (stale zeros
 *       from BSS init) → ARP/IP handler sees all-zeros → no replies sent.
 *       (boot337 confirmed: rx=45 counted but mib never advanced past 7)
 *
 * Note: UniMAC loopback mode (LCL_LOOP_EN) bypasses the source MAC validity
 * check, which is why the loopback self-test passes even without cache ops.
 *
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
static uint32_t g_rx_write_ptr = 0;  /* tracked RDMA WRITE_PTR (word units)   */
static uint16_t g_tx_pidx      = 0;  /* producer index into TX ring           */
static uint16_t g_tx_cidx      = 0;  /* consumer index (completed TX) tracking*/
static int      g_genet_ok     = 0;  /* set to 1 after successful init        */
static uint32_t g_rx_count     = 0;  /* frames successfully delivered to caller*/
static uint32_t g_rx_fcs       = 0;  /* frames dropped due to CRC/FCS error   */

/* ── IRQ-driven RX flag ─────────────────────────────────────────────────── */
/* Set by genet_rx_irq_handler() when RXDMA_DONE fires.  Cleared by the
 * WIMP drain loop in lib.c.  Declared volatile so the compiler does not
 * cache the value in a register across iterations of the outer WIMP loop. */
volatile int g_genet_rx_pending = 0;

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

    /* Reset UMAC core (2 µs — matches boot342 working value) */
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

    /* RBUF_CTRL: ALIGN_2B only — matches the empirically working boot.
     * boot332: remove RBUF_64B_EN (bit 0).  The one boot on record where
     * wire RX worked had RBUF_CTRL=0x2 at post-link time (ALIGN_2B only).
     * boot330/331 set bit 0 (64B_EN) and got pidx=0 throughout.  64B_EN
     * enables a 64-byte receive-side buffer header; if the hardware uses
     * this to gate RDMA writes and our descriptor layout doesn't match,
     * it silently drops every frame. */
    GW4(GENET_RBUF_CTRL, GENET_RBUF_ALIGN_2B);
    debug_print("[GENET] RBUF_CTRL=0x%x (expect 0x2)\n", (unsigned)GR4(GENET_RBUF_CTRL));
    GW4(GENET_RBUF_TBUF_SIZE_CTRL, 1);

    /* boot334: clear any UMAC_TX_FLUSH left by VideoCore firmware.
     * EtherGENET-6 genet_stop_locked() writes TX_FLUSH=1 to drain the FIFO
     * on shutdown.  If VC didn't complete a clean stop the flush bit stays
     * set.  With TX_FLUSH=1: TDMA DMA engine reads the descriptor and
     * advances CONS (DMA-level — the frame IS consumed from the ring), but
     * UMAC discards it before it reaches RGMII or the internal loopback MAC.
     * This explains boot333 RDMA=BROKEN: TDMA CONS advanced (DMA worked) yet
     * nothing reached RX or the wire.  Log the value first for the boot log. */
    debug_print("[GENET] UMAC_TX_FLUSH before clear=0x%x\n",
        (unsigned)GR4(GENET_UMAC_TX_FLUSH));
    GW4(GENET_UMAC_TX_FLUSH, 0);

    /* boot339: clear SYS_TBUF_FLUSH_CTRL (offset 0x00c) — separate from the
     * UMAC TX_FLUSH (0xb34) above.  Linux bcmgenet.c clears this register in
     * bcmgenet_init().  If VideoCore left it set, TX frames are flushed from
     * the TX buffer before they reach the UniMAC TX path → TDMA still advances
     * CONS (DMA-level) but frames never exit to RGMII.  Unlike UMAC_TX_FLUSH,
     * this register is in the SYS block and may survive the UMAC SW_RESET.   */
    debug_print("[GENET] TBUF_FLUSH_CTRL before clear=0x%x\n",
        (unsigned)GR4(GENET_SYS_TBUF_FLUSH_CTRL));
    GW4(GENET_SYS_TBUF_FLUSH_CTRL, 0);
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

    /* boot337: disable MDF entirely — PROMISC alone is not sufficient to
     * bypass MDF; Linux explicitly writes MDF_CTRL=0 when enabling promisc.
     * Previous code wrote (1u<<17)|(1u<<16) which enables entries 16 and 17,
     * not entries 0 and 1 where we programmed the unicast and broadcast MACs.
     * Entries 16/17 held uninitialized zeros → no frame ever matched → every
     * received frame was silently dropped before reaching RBUF/RDMA → pidx=0
     * on every boot since boot302.  Writing 0 disables all MDF entries and
     * allows PROMISC to work as intended. */
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

    /* boot330 (= boot341/342 behaviour): populate all RX descriptors.
     * STATUS must be 0 — it is hardware-owned (GENET fills length/flags on
     * frame arrival).  Writing BUFLEN here (boot326) confused the hardware.
     * WRITE_PTR must be set to DESC_COUNT×3=192 AFTER populating all slots
     * to tell hardware "64 buffers are armed."  WRITE_PTR=0 at init means
     * "ring empty" — that's what caused RDMA=BROKEN in boot322-329. */
    DSB();
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        /* boot337: raw ARM physical address — BCM2711 GENET is cache-coherent
         * and uses the same address space as the CPU (no VC4 bus offset). */
        uint32_t paddr = (uint32_t)(uintptr_t)g_rx_buf[i];
        GW4(GENET_RX_DESC_STATUS(i),     0);      /* hardware fills on RX  */
        GW4(GENET_RX_DESC_ADDRESS_LO(i), paddr);
        GW4(GENET_RX_DESC_ADDRESS_HI(i), 0);
    }
    DSB();

    /* Arm all 64 slots: WRITE_PTR = 64 × 3 words = 192.
     * Hardware tracks (WRITE_PTR - its_fill_ptr) to know how many slots are
     * available.  Each genet_poll_rx re-arm increments g_rx_write_ptr by 3. */
    g_rx_write_ptr = (uint32_t)(RX_DESC_COUNT * GENET_DMA_DESC_SIZE / 4);
    GW4(GENET_RX_DMA_WRITE_PTR_LO(qid), g_rx_write_ptr);
    GW4(GENET_RX_DMA_WRITE_PTR_HI(qid), 0);

    /* Enable ring and DMA now that all descriptor slots have valid addresses */
    GW4(GENET_RX_DMA_RING_CFG, __BIT(qid));  /* enable this ring */

    val  = GR4(GENET_RX_DMA_CTRL);
    val |= GENET_RX_DMA_CTRL_EN;
    val |= GENET_RX_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE);
    GW4(GENET_RX_DMA_CTRL, val);

    g_rx_cidx = 0;
    g_tx_pidx = 0;
    g_tx_cidx = 0;

    /* Diagnostic readback — mirrors boot314 output so we can compare logs  */
    debug_print("[GENET] RX ring: WRITE_PTR=%u PROD=%u CONS=%u\n",
        (unsigned)(GR4(GENET_RX_DMA_WRITE_PTR_LO(qid)) & 0xffff),
        (unsigned)(GR4(GENET_RX_DMA_PROD_INDEX(qid))   & 0xffff),
        (unsigned)(GR4(GENET_RX_DMA_CONS_INDEX(qid))   & 0xffff));
    /* boot337: addr_lo = raw ARM physical address of g_rx_buf[0] (~0x08xxxxxx) */
    debug_print("[GENET] RX desc[0] addr_lo=0x%08x status=0x%08x\n",
        (unsigned)GR4(GENET_RX_DESC_ADDRESS_LO(0)),
        (unsigned)GR4(GENET_RX_DESC_STATUS(0)));
}

/* ── INTRL2 helpers ─────────────────────────────────────────────────────── */

/* genet_disable_intr — mask all GENET INTRL2 sources and clear any pending.
 * Called at init before the ring is ready.  Ensures no stale IRQ fires
 * between genet_init_rings() and genet_enable_rx_irq().                    */
static void genet_disable_intr(void)
{
    GW4(GENET_INTRL2_CPU_SET_MASK, 0xffffffff);  /* mask all sources      */
    GW4(GENET_INTRL2_CPU_CLEAR,    0xffffffff);  /* clear any pending     */
}

/* ── IRQ handler ────────────────────────────────────────────────────────── */

/* genet_rx_irq_handler — GIC IRQ handler for GENET mac_0 (INTID 189).
 *
 * Called from irq_dispatch() when the GIC delivers INTID 189.
 * Sequence:
 *   1. Read INTRL2_CPU_STAT to find which sources are pending.
 *   2. Write INTRL2_CPU_CLEAR with the same value to acknowledge them.
 *      (must clear before re-enabling at GIC level, i.e. before returning
 *       from irq_dispatch — irq_eoi is called by irq_dispatch after us.)
 *   3. If RXDMA_DONE is set, raise g_genet_rx_pending.
 *      WIMP's outer loop checks this flag and drains the ring.
 *
 * We do NOT call genet_poll_rx() here.  genet_poll_rx() copies frame data
 * into a caller-supplied buffer; the caller (WIMP) owns that buffer and the
 * ARP/ICMP processing state.  Setting a flag and returning is the correct
 * pattern for a bottom-half / deferred handler.
 *
 * Single-core bare-metal: no locking needed.  WIMP cannot be in
 * genet_poll_rx() while this handler runs — the IRQ preempts WIMP at a
 * bounded boundary and returns.  genet_send() only touches the TX ring and
 * is safe to be interrupted mid-flight.                                    */
static void genet_rx_irq_handler(int vec, void *priv)
{
    (void)vec;
    (void)priv;

    uint32_t stat = GR4(GENET_INTRL2_CPU_STAT);
    GW4(GENET_INTRL2_CPU_CLEAR, stat);     /* ack all pending INTRL2 bits */

    if (stat & GENET_IRQ_RXDMA_DONE) {
        g_genet_rx_pending = 1;
    }
    /* TXDMA_DONE and MDIO_DONE are not used — no handler needed.  Any other
     * bits in stat are harmless: we cleared them above.                    */
}

/* genet_enable_rx_irq — wire RXDMA_DONE through INTRL2 → GIC → CPU.
 *
 * Call order (from genet_init, after rings are armed):
 *   genet_disable_intr()   — masks all INTRL2; clears pending
 *   genet_enable_rx_irq()  — register C handler, unmask GIC, unmask INTRL2
 *
 * After this returns, any frame received by RDMA triggers:
 *   INTRL2_CPU_STAT[13] → GIC SPI 157 (INTID 189) → exc_irq_handler →
 *   irq_dispatch → genet_rx_irq_handler → g_genet_rx_pending=1 → WIMP.  */
static void genet_enable_rx_irq(void)
{
    /* Step 1: register C handler with the GIC dispatch table */
    irq_set_handler(GENET_GIC_INTID, genet_rx_irq_handler, NULL);

    /* Step 2: tell the GIC distributor to forward INTID 189 to CPU 0 */
    irq_unmask(GENET_GIC_INTID);

    /* Step 3: unmask RXDMA_DONE in GENET INTRL2 so the block asserts the
     * GIC SPI line when RDMA finishes writing a frame.
     * INTRL2_CPU_CLEAR_MASK (0x214): writing a 1 to bit N clears (unmasks)
     * that bit in the mask register — i.e. allows the source to reach GIC.
     * The inverse (SET_MASK / 0x210) re-masks sources.                    */
    GW4(GENET_INTRL2_CPU_CLEAR_MASK, GENET_IRQ_RXDMA_DONE);

    debug_print("[GENET] RXDMA_DONE IRQ enabled (INTID %d, INTRL2 bit 13)\n",
                GENET_GIC_INTID);
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

    /* Mask all INTRL2 sources first, then enable just RXDMA_DONE via GIC */
    genet_disable_intr();
    genet_enable_rx_irq();

    /* Enable UMAC transmitter, receiver, and data-path gate.
     * boot333: TX_RX_EN (bit 29) is the GENETv5 MAC data-path gate — it
     * must be set at init time, not deferred to apply_link.  Without it:
     *   • TX frames are silently swallowed by UMAC (not visible on wire)
     *   • RX frames never reach RDMA (pidx stays 0)
     * The pcap from boot332 confirmed zero Pi frames on wire despite
     * DHCP DISCOVERs being logged — TX_RX_EN was only set in apply_link
     * which runs ~40 s after boot, after DHCP had already given up. */
    uint32_t cmd = GR4(GENET_UMAC_CMD);
    cmd |= GENET_UMAC_CMD_TX_RX_EN | GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    GW4(GENET_UMAC_CMD, cmd);

    debug_print("[GENET] UMAC TX+RX+gate enabled cmd=0x%08x\n",
        (unsigned)GR4(GENET_UMAC_CMD));

    /* Mark driver OK now so genet_send/genet_poll_rx work below */
    g_genet_ok = 1;

    /* ── RDMA loopback self-test ─────────────────────────────────────────
     * boot342: Enable UMAC MAC loopback (LCL_LOOP_EN), send one frame,
     * and poll RX PROD_INDEX for up to 10 ms.
     *
     * If PROD_INDEX advances → RDMA is alive; the loopback path works
     *   and any remaining pidx=0 issue is in the wire path (RGMII/PHY).
     * If PROD_INDEX stays 0 → RDMA is broken regardless of OOB/link state;
     *   the ring init is wrong at a more fundamental level.
     *
     * boot314 had this test and it passed; we lost it in the regression.
     * It is the definitive split between "ring init broken" vs "wire broken".
     ──────────────────────────────────────────────────────────────────── */
    {
        /* boot330 (= boot342): simple loopback — LCL_LOOP_EN only.
         * boot342 did NOT set TX_RX_EN or manipulate OOB for the loopback,
         * and it passed.  All the extra bits we added (boot326+) were wrong. */
        uint32_t lpcmd = GR4(GENET_UMAC_CMD);
        lpcmd |= GENET_UMAC_CMD_LCL_LOOP_EN;
        GW4(GENET_UMAC_CMD, lpcmd);
        genet_delay_us(100);

        static uint8_t s_lp_frame[64];
        memset(s_lp_frame, 0, sizeof(s_lp_frame));
        s_lp_frame[0] = 0xff; s_lp_frame[1] = 0xff; s_lp_frame[2] = 0xff;
        s_lp_frame[3] = 0xff; s_lp_frame[4] = 0xff; s_lp_frame[5] = 0xff;
        memcpy(s_lp_frame + 6, g_genet_mac, 6);
        s_lp_frame[12] = 0x90; s_lp_frame[13] = 0x00;
        genet_send(s_lp_frame, sizeof(s_lp_frame));

        uint32_t lp_pidx = 0;
        for (int i = 0; i < 1000; i++) {
            genet_delay_us(10);
            lp_pidx = GR4(GENET_RX_DMA_PROD_INDEX(qid)) & 0xffff;
            if (lp_pidx) break;
        }
        debug_print("[GENET] loopback: pidx=%u wptr=0x%x RDMA=%s UMAC_CMD=0x%08x TX_FLUSH=0x%x\n",
            (unsigned)lp_pidx,
            (unsigned)(GR4(GENET_RX_DMA_WRITE_PTR_LO(qid)) & 0xffff),
            lp_pidx ? "OK" : "BROKEN",
            (unsigned)GR4(GENET_UMAC_CMD),
            (unsigned)GR4(GENET_UMAC_TX_FLUSH));

        lpcmd = GR4(GENET_UMAC_CMD);
        lpcmd &= ~GENET_UMAC_CMD_LCL_LOOP_EN;
        GW4(GENET_UMAC_CMD, lpcmd);

        if (lp_pidx) {
            static uint8_t s_drain[GENET_BUF_SIZE];
            genet_poll_rx(s_drain, sizeof(s_drain));
        }
    }

    /* Init PHY and start autoneg */
    genet_phy_init();

    debug_print("[GENET] init complete (boot343 IRQ-driven RX,"
                " MDF_CTRL=0 + cache ops + ID_MODE_DISABLE=1)\n");
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
    /* boot331: restore TX_RX_EN (bit 29) on link-up.
     * boot330 removed it following the boot342 git analysis, but the one boot
     * we have on record where wire RX actually worked (bootlog349 second boot)
     * had UMAC_CMD=0x2000001b — bit 29 SET.  Without it frames do not flow
     * (boot330 shows pidx=0 throughout despite correct ring init). */
    cmd |= GENET_UMAC_CMD_TX_RX_EN;
    cmd |= GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    GW4(GENET_UMAC_CMD, cmd);

    /* Update EXT_RGMII_OOB_CTRL on link-up.
     * boot339: ID_MODE_DISABLE is KEPT SET (MAC adds no delay — see long
     * comment below).  Only OOB_DISABLE and RGMII_LINK are touched here.
     *   • OOB_DISABLE: clear — enable OOB signalling.
     *   • RGMII_LINK:  set   — assert link-up to MAC.                      */
    uint32_t oob = GR4(GENET_EXT_RGMII_OOB_CTRL);
    debug_print("[GENET] apply_link: OOB before=0x%08x\n", (unsigned)oob);

    /* boot339: DO NOT clear ID_MODE_DISABLE.  Keep it SET (= MAC adds no TX
     * clock delay) as genet_init() configured it.
     *
     * BCM54213PE on Pi 4 operates in RGMII-ID mode: the PHY adds internal
     * delay on both TX_CLK and RX_CLK.  The MAC must therefore NOT add its
     * own delay → ID_MODE_DISABLE=1.
     *
     * boot331 cleared ID_MODE_DISABLE based on bootlog349 showing
     * OOB=0x00f00050 (bit 16 clear) when "RX worked."  But bootlog349 only
     * confirmed RDMA=OK (pidx advancing); TX was never verified in that boot.
     * With ID_MODE_DISABLE=0 the MAC also delays TX_CLK → double delay:
     * PHY delay + MAC delay = RGMII TX misalignment → PHY discards every
     * frame → nothing on wire.  This explains boot338: TDMA CONS advances
     * (DMA consumed) but zero Pi frames ever appear in Wireshark.
     *
     * boot314 (confirmed ARP+ICMP on wire) had ID_MODE_DISABLE=1 after
     * apply_link — the apply_link OOB code did not exist then; the init
     * setting of bit 16 was preserved.  Reverting to that state. */
    /* REMOVED: oob &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE; */
    oob &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;  /* enable OOB signalling   */
    oob |=  GENET_EXT_RGMII_OOB_RGMII_LINK;   /* assert link-up to MAC   */
    GW4(GENET_EXT_RGMII_OOB_CTRL, oob);
    debug_print("[GENET] apply_link: OOB after=0x%08x UMAC_CMD=0x%08x\n",
        (unsigned)GR4(GENET_EXT_RGMII_OOB_CTRL),
        (unsigned)GR4(GENET_UMAC_CMD));

    /* boot339: dump PHY state at link-up to confirm 1 Gbps negotiated */
    {
        uint16_t phy_bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
        phy_bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR); /* latch-low */
        uint32_t spd_mhz = (spd == GENET_UMAC_CMD_SPEED_1000) ? 1000u :
                           (spd == GENET_UMAC_CMD_SPEED_100)  ?  100u : 10u;
        debug_print("[GENET] apply_link PHY: BMSR=0x%04x STAT1000=0x%04x"
                    " LPA=0x%04x spd=%u\n",
            (unsigned)phy_bmsr, (unsigned)stat1000,
            (unsigned)lpa,      (unsigned)spd_mhz);
    }

    /* post-link state dump — RX and TX DMA sides.
     * RX: PROD should advance once first frame arrives.
     * TX: CONS should advance past 0 once DHCP DISCOVER is consumed by TDMA.
     *     If tx_cons stays 0 after DHCP, TDMA is not consuming = TX broken. */
    {
        const int qid2 = GENET_DMA_DEFAULT_QUEUE;
        debug_print("[GENET] post-link RX: CTRL=0x%08x CFG=0x%08x"
                    " PROD=%u CONS=%u d0_stat=0x%08x d0_addr=0x%08x\n",
            (unsigned)GR4(GENET_RX_DMA_CTRL),
            (unsigned)GR4(GENET_RX_DMA_RING_CFG),
            (unsigned)(GR4(GENET_RX_DMA_PROD_INDEX(qid2)) & 0xffff),
            (unsigned)(GR4(GENET_RX_DMA_CONS_INDEX(qid2)) & 0xffff),
            (unsigned)GR4(GENET_RX_DESC_STATUS(0)),
            (unsigned)GR4(GENET_RX_DESC_ADDRESS_LO(0)));
        debug_print("[GENET] post-link TX: CTRL=0x%08x CFG=0x%08x"
                    " PROD=%u CONS=%u UMAC_CMD=0x%08x\n",
            (unsigned)GR4(GENET_TX_DMA_CTRL),
            (unsigned)GR4(GENET_TX_DMA_RING_CFG),
            (unsigned)(GR4(GENET_TX_DMA_PROD_INDEX(qid2)) & 0xffff),
            (unsigned)(GR4(GENET_TX_DMA_CONS_INDEX(qid2)) & 0xffff),
            (unsigned)GR4(GENET_UMAC_CMD));
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
    DSB();

    /* boot338: raw ARM physical address (no VC4 bus offset).
     * Flush D-cache to DRAM so TDMA reads our actual frame data, not the
     * stale BSS zeros.  Must happen AFTER memcpy, BEFORE PROD_INDEX bump. */
    CACHE_CLEAN(g_tx_buf[idx], len);

    uint32_t paddr = (uint32_t)(uintptr_t)g_tx_buf[idx];
    uint32_t status = (len << 16) |
                      GENET_TX_DESC_STATUS_SOP |
                      GENET_TX_DESC_STATUS_EOP |
                      GENET_TX_DESC_STATUS_CRC;

    /* boot339: log first 32 TX frames — split into two debug_print calls so
     * the total vararg count never exceeds 7 (fits in AArch64 x1-x7 regs).
     * Exceeding 7 args spills to stack; our bare-metal debug_print va_list
     * does not reliably read stack args, causing bytes 3-5 of MACs to print
     * as 00.  Two calls of ≤7 args each print correctly. */
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

    GW4(GENET_TX_DESC_ADDRESS_LO(idx), paddr);
    GW4(GENET_TX_DESC_ADDRESS_HI(idx), 0);
    GW4(GENET_TX_DESC_STATUS(idx),     status);
    /* boot334: do NOT update TX WRITE_PTR per frame.
     * EtherGENET-6 genet_start_locked() (the authoritative reference) only
     * writes PROD_INDEX to kick DMA; WRITE_PTR is set once at ring init and
     * never touched again.  Our per-frame WRITE_PTR update was wrong — it
     * set WRITE_PTR to idx×3 which collapsed back to 0 on descriptor 0 and
     * corrupted the hardware's view of the ring on every subsequent TX. */
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

    /* boot338: invalidate D-cache for this RX slot before reading status or
     * frame data.  RDMA wrote to DRAM; without DC CIVAC the CPU would read
     * stale cache lines (BSS zeros) instead of the received frame.          */
    CACHE_INVAL(g_rx_buf[idx], GENET_BUF_SIZE);
    DSB();  /* ensure all RDMA writes visible before we read status */
    uint32_t status = GR4(GENET_RX_DESC_STATUS(idx));
    uint32_t raw_len = __SHIFTOUT(status, GENET_RX_DESC_STATUS_BUFLEN);

    /* Advance consumer index regardless of errors */
    g_rx_cidx = (g_rx_cidx + 1) & 0xffff;
    GW4(GENET_RX_DMA_CONS_INDEX(qid), g_rx_cidx);

    /* Re-arm this descriptor slot: restore buffer address and STATUS=0.
     * boot330 (= boot342): STATUS is hardware-owned — RDMA fills it with
     * length/flags on arrival.  Writing BUFLEN here (boot326) was wrong;
     * the working boot342 writes STATUS=0 to return the slot to hardware.
     * boot337: raw ARM physical address. */
    uint32_t paddr = (uint32_t)(uintptr_t)g_rx_buf[idx];
    GW4(GENET_RX_DESC_ADDRESS_LO(idx), paddr);
    GW4(GENET_RX_DESC_ADDRESS_HI(idx), 0);
    GW4(GENET_RX_DESC_STATUS(idx), 0);  /* hardware-owned; 0 = slot available */
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
        if (status & GENET_RX_DESC_STATUS_CRC_ERR)
            g_rx_fcs++;
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

    /* boot339: log first 32 RX frames — split into two calls (≤7 args each)
     * to avoid AArch64 vararg stack spill in bare-metal debug_print.          */
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

/* ── Public: genet_rx_count_raw ────────────────────────────────────────── */
/* Software counter of successfully received frames (incremented in poll_rx
 * when a frame passes all error checks and is copied to caller's buffer).  */
uint32_t genet_rx_count_raw(void)
{
    return g_rx_count;
}

/* ── Public: genet_tx_cons_raw ─────────────────────────────────────────── */
/* Returns hardware TX DMA consumer index — the count of descriptors the
 * TDMA engine has consumed (i.e. frames committed to UMAC at the DMA level).
 * Used as the 'mib=' heartbeat field to confirm TX DMA is advancing.       */
uint32_t genet_tx_cons_raw(void)
{
    if (!g_genet_ok) return 0;
    return GR4(GENET_TX_DMA_CONS_INDEX(GENET_DMA_DEFAULT_QUEUE)) & 0xffff;
}

/* ── Public: genet_rx_fcs_raw ──────────────────────────────────────────── */
/* Software counter of RX frames dropped due to CRC/FCS errors.            */
uint32_t genet_rx_fcs_raw(void)
{
    return g_rx_fcs;
}
