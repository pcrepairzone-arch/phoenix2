/*
 * usb_dwc2.c — Synopsys DWC2 USB OTG host driver stub
 *              Pi 4 SoC internal USB controller (USB-C OTG port)
 *
 * Physical address: 0xFE980000 (CPU / identity-map VA)
 * This is the VideoCore 0x7E980000 address translated to ARM CPU space
 * using the Pi 4 peripheral bus offset (0x7E000000 → 0xFE000000).
 *
 * This controller is COMPLETELY SEPARATE from the VL805 xHCI PCIe
 * controller.  It controls only the USB-C OTG port on the Pi 4B.
 * The four USB-A ports are handled exclusively by usb_xhci.c.
 *
 * Current status: host-mode init + basic port detection.
 * Full enumeration (control/bulk transfers) is stubbed for now.
 * This gives us a working USB path for keyboards/mice on the OTG
 * port while the xHCI event ring issue is resolved.
 */

#include "usb_dwc2.h"
#include "usb.h"           /* usb_hc_ops_t, usb_device_t, usb_endpoint_t  */
#include "kernel.h"        /* delay_ms                                      */

/* uart_puts and debug_print are defined in the UART/kernel compilation units.
 * Declare them explicitly here so the compiler doesn't need to find them via
 * a specific header — same pattern used throughout the driver tree.          */
extern void uart_puts(const char *s);
extern void debug_print(const char *fmt, ...);

/* ── DWC2 physical base ─────────────────────────────────────────── */
#define DWC2_BASE   0xFE980000UL    /* CPU physical = VC 0x7E980000  */

/* ── Global register file ────────────────────────────────────────── */
/* Core global registers (offset from DWC2_BASE)                     */
#define GOTGCTL     0x000   /* OTG Control and Status                */
#define GOTGINT     0x004   /* OTG Interrupt                         */
#define GAHBCFG     0x008   /* AHB Configuration                     */
#define GUSBCFG     0x00C   /* USB Configuration                     */
#define GRSTCTL     0x010   /* Reset                                  */
#define GINTSTS     0x014   /* Interrupt Status                       */
#define GINTMSK     0x018   /* Interrupt Mask                         */
#define GRXSTSR     0x01C   /* Receive Status Debug Read             */
#define GRXSTSP     0x020   /* Receive Status Read/Pop               */
#define GRXFSIZ     0x024   /* Receive FIFO Size                     */
#define GNPTXFSIZ   0x028   /* Non-Periodic Transmit FIFO Size       */
#define GUID        0x03C   /* User ID                               */
#define GSNPSID     0x040   /* Synopsys ID                           */
#define GHWCFG1     0x044   /* User HW Config 1                     */
#define GHWCFG2     0x048   /* User HW Config 2                     */
#define GHWCFG3     0x04C   /* User HW Config 3                     */
#define GHWCFG4     0x050   /* User HW Config 4                     */

/* Host mode registers */
#define HCFG        0x400   /* Host Configuration                    */
#define HFIR        0x404   /* Host Frame Interval                   */
#define HFNUM       0x408   /* Host Frame Number/Remaining           */
#define HPTXSTS     0x410   /* Host Periodic TX FIFO/Queue Status    */
#define HAINT       0x414   /* Host All Channels Interrupt           */
#define HAINTMSK    0x418   /* Host All Channels Interrupt Mask      */
#define HPRT        0x440   /* Host Port Control and Status          */

/* HPRT bit definitions */
#define HPRT_CONN       (1U <<  0)  /* Port Connect Status            */
#define HPRT_CONNDET    (1U <<  1)  /* Port Connect Detected (W1C)    */
#define HPRT_ENA        (1U <<  2)  /* Port Enable                    */
#define HPRT_ENACHG     (1U <<  3)  /* Port Enable/Disable Change W1C */
#define HPRT_OVRCURR    (1U <<  4)  /* Port Overcurrent Active        */
#define HPRT_OVRCURRCHG (1U <<  5)  /* Port Overcurrent Change  W1C   */
#define HPRT_RES        (1U <<  6)  /* Port Resume                    */
#define HPRT_SUSP       (1U <<  7)  /* Port Suspend                   */
#define HPRT_RST        (1U <<  8)  /* Port Reset                     */
#define HPRT_LNSTS     (3U <<  10)  /* Port Line Status [11:10]       */
#define HPRT_PWR        (1U <<  12) /* Port Power                     */
#define HPRT_TSTCTL    (0xFU << 13) /* Port Test Control [16:13]      */
#define HPRT_SPD       (3U <<  17)  /* Port Speed [18:17]             */
/* HPRT_SPD values */
#define HPRT_SPD_HS     (0U << 17)  /* High Speed                     */
#define HPRT_SPD_FS     (1U << 17)  /* Full Speed                     */
#define HPRT_SPD_LS     (2U << 17)  /* Low Speed                      */

/* ── Host channel registers (per-channel, 8 channels on Pi 4 DWC2) ──
 * Base = DWC2_BASE + 0x500 + ch * 0x20                               */
#define HC_BASE(n)      (0x500U + (uint32_t)(n) * 0x20U)
#define HCCHAR(n)       (HC_BASE(n) + 0x00U)   /* Characteristics      */
#define HCSPLT(n)       (HC_BASE(n) + 0x04U)   /* Split Control        */
#define HCINT(n)        (HC_BASE(n) + 0x08U)   /* Interrupt            */
#define HCINTMSK(n)     (HC_BASE(n) + 0x0CU)   /* Interrupt Mask       */
#define HCTSIZ(n)       (HC_BASE(n) + 0x10U)   /* Transfer Size        */
#define HCDMA(n)        (HC_BASE(n) + 0x14U)   /* DMA Address          */

/* HCCHAR bit definitions */
#define HCCHAR_CHENA    (1U << 31)  /* Channel Enable                  */
#define HCCHAR_CHDIS    (1U << 30)  /* Channel Disable                 */
#define HCCHAR_ODDFRM   (1U << 29)  /* Odd Frame (periodic only)       */
#define HCCHAR_LSPDDEV  (1U << 17)  /* Low Speed Device                */
#define HCCHAR_EPTYPE_CTRL (0U << 18) /* Control endpoint type         */
#define HCCHAR_EPTYPE_INTR (3U << 18) /* Interrupt endpoint type       */
#define HCCHAR_EPDIR_IN  (1U << 15) /* IN direction                    */
/* OUT direction = bit 15 clear */

/* HCTSIZ PID field [30:29] */
#define HCPID_DATA0     (0U << 29)
#define HCPID_DATA2     (1U << 29)
#define HCPID_DATA1     (2U << 29)
#define HCPID_SETUP     (3U << 29)

/* HCINT status bits */
#define HCINT_XFRC      (1U <<  0)  /* Transfer Complete               */
#define HCINT_CHH       (1U <<  1)  /* Channel Halted                  */
#define HCINT_AHBERR    (1U <<  2)  /* AHB Error                       */
#define HCINT_STALL     (1U <<  3)  /* STALL Response Received         */
#define HCINT_NAK       (1U <<  4)  /* NAK Response Received           */
#define HCINT_ACK       (1U <<  5)  /* ACK Response Received           */
#define HCINT_TXERR     (1U <<  7)  /* Transaction Error               */
#define HCINT_BBERR     (1U <<  8)  /* Babble Error                    */
#define HCINT_FRMOR     (1U <<  9)  /* Frame Overrun                   */
#define HCINT_DTERR     (1U << 10)  /* Data Toggle Error               */
#define HCINT_NYET      (1U <<  6)  /* NYET Response (hub buffer full)  */
#define HCINT_ALL_ERR   (HCINT_STALL | HCINT_TXERR | HCINT_BBERR | \
                         HCINT_DTERR | HCINT_AHBERR | HCINT_FRMOR)

/* HCSPLT register bits (offset already defined as HCSPLT(n) above)    */
#define HCSPLT_SPLTENA   (1U << 31)  /* Split Enable                    */
#define HCSPLT_COMPSPLT  (1U << 16)  /* 0=Start Split, 1=Complete Split */
/* HUBADDR [13:7], PRTADDR [6:0] */
#define HCSPLT_MAKE(hub, port) \
    (HCSPLT_SPLTENA | (((uint32_t)(hub) & 0x7F) << 7) | ((uint32_t)(port) & 0x7F))

/* GRSTCTL */
#define GRSTCTL_CSRST   (1U << 0)   /* Core Soft Reset                */
#define GRSTCTL_AHBIDLE (1U << 31)  /* AHB Master Idle                */

/* GAHBCFG */
#define GAHBCFG_GLBINTRMSK  (1U << 0)  /* Global interrupt mask       */
#define GAHBCFG_DMAEN       (1U << 5)  /* DMA enable                  */

/* GUSBCFG */
#define GUSBCFG_FHMOD   (1U << 29)  /* Force Host Mode                */

/* GINTSTS / GINTMSK */
#define GINT_HPRTINT    (1U << 24)  /* Host port interrupt             */
#define GINT_HCINT      (1U << 25)  /* Host channels interrupt         */
#define GINT_CURMODE    (1U <<  0)  /* Current mode (1=host)           */

/* ── Register access helpers ─────────────────────────────────────── */
static inline uint32_t dwc2_rd(uint32_t off) {
    return *(volatile uint32_t *)(DWC2_BASE + off);
}
static inline void dwc2_wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(DWC2_BASE + off) = val;
    asm volatile("dsb sy" ::: "memory");
}
/* Read-modify-write with explicit mask */
static inline void dwc2_rmw(uint32_t off, uint32_t clr, uint32_t set) {
    dwc2_wr(off, (dwc2_rd(off) & ~clr) | set);
}

/* Cycle-counter delay — safe from any lib.c calling-convention issues */
static void dwc2_delay_ms(int ms) {
    uint64_t t0, t1;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    uint64_t ticks = (uint64_t)ms * 54000ULL; /* 54 MHz */
    do { asm volatile("mrs %0, cntpct_el0" : "=r"(t1)); }
    while ((t1 - t0) < ticks);
}

/* ── DMA coherency helpers ───────────────────────────────────────── *
 * DWC2 is an AHB DMA master — we must maintain data cache coherency  *
 * explicitly since Phoenix OS uses normal cacheable RAM.              */
static void dcache_clean(void *start, size_t len) {
    /* DC CVAC: clean (flush) data cache to PoC so DMA sees CPU writes */
    uintptr_t a   = (uintptr_t)start & ~63UL;
    uintptr_t end = (uintptr_t)start + len;
    for (; a < end; a += 64)
        asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
}
static void dcache_inval(void *start, size_t len) {
    /* DC IVAC: invalidate data cache so CPU re-fetches DMA-written data */
    uintptr_t a   = (uintptr_t)start & ~63UL;
    uintptr_t end = (uintptr_t)start + len;
    for (; a < end; a += 64)
        asm volatile("dc ivac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

/* DMA-accessible buffers: 64-byte aligned to match cache line size   */
static uint8_t __attribute__((aligned(64))) dwc2_setup_buf[8];
static uint8_t __attribute__((aligned(64))) dwc2_data_buf[256];

/* ── Driver state ────────────────────────────────────────────────── */
static int  dwc2_initialised  = 0;
static int  dwc2_port_present = 0;

/* ── Internal helpers ────────────────────────────────────────────── */

static int dwc2_core_reset(void) {
    uart_puts("[DWC2] Core reset: waiting for AHB idle...\n");

    /* Wait for AHB master idle — use cycle counter, not delay_ms */
    uint64_t t0, t1;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    int ahb_idle = 0;
    for (int i = 0; i < 10000; i++) {
        if (dwc2_rd(GRSTCTL) & GRSTCTL_AHBIDLE) { ahb_idle = 1; break; }
        /* 1ms busy wait via cycle counter */
        asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t1) :: "memory");
        uint64_t deadline = t1 + 54000ULL; /* 1ms at 54MHz */
        do { asm volatile("mrs %0, cntpct_el0" : "=r"(t1)); }
        while (t1 < deadline);
    }
    if (!ahb_idle) {
        uart_puts("[DWC2] AHB not idle before reset — GRSTCTL=");
        debug_print("0x%08x\n", (unsigned)dwc2_rd(GRSTCTL));
        return -1;
    }
    uart_puts("[DWC2] AHB idle OK — issuing soft reset...\n");

    /* Issue core soft reset */
    dwc2_wr(GRSTCTL, GRSTCTL_CSRST);
    int reset_done = 0;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    for (int i = 0; i < 100; i++) {
        /* 1ms busy wait */
        asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t1) :: "memory");
        uint64_t deadline = t1 + 54000ULL;
        do { asm volatile("mrs %0, cntpct_el0" : "=r"(t1)); }
        while (t1 < deadline);
        if (!(dwc2_rd(GRSTCTL) & GRSTCTL_CSRST)) { reset_done = 1; break; }
    }
    if (!reset_done) {
        uart_puts("[DWC2] Soft reset did not complete — GRSTCTL=");
        debug_print("0x%08x\n", (unsigned)dwc2_rd(GRSTCTL));
        return -1;
    }
    uart_puts("[DWC2] Soft reset complete — post-reset settle...\n");

    /* 3ms post-reset settle via cycle counter */
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    do { asm volatile("mrs %0, cntpct_el0" : "=r"(t1)); }
    while ((t1 - t0) < 162000ULL); /* 3ms at 54MHz */

    uart_puts("[DWC2] Core reset done\n");
    return 0;
}

static void dwc2_core_init_host(void) {
    /* Force host mode and configure USB */
    uint32_t gusbcfg = dwc2_rd(GUSBCFG);
    gusbcfg |=  GUSBCFG_FHMOD;         /* Force Host Mode */
    gusbcfg &= ~(0xFU << 10);          /* USB Turnaround Time = 0 */
    gusbcfg |=  (5U  << 10);           /* UTT = 5 (recommended for UTMI) */
    dwc2_wr(GUSBCFG, gusbcfg);
    dwc2_delay_ms(25); /* mode switch settle */

    /* AHB config: enable DMA, burst=INCR4 */
    dwc2_wr(GAHBCFG, GAHBCFG_DMAEN | (3U << 1) | GAHBCFG_GLBINTRMSK);

    /* Clear all pending interrupts, mask all (polling mode) */
    dwc2_wr(GINTSTS, 0xFFFFFFFFU);
    dwc2_wr(GINTMSK, 0U);

    /* Host config: full-speed clock = 30/60 MHz */
    dwc2_wr(HCFG, (dwc2_rd(HCFG) & ~0x3U) | 0x1U);
}

static int dwc2_port_power_on(void) {
    /* Apply port power (PP bit) — preserve RW bits, W1C bits stay 0 */
    uint32_t hprt = dwc2_rd(HPRT);
    /* Clear W1C bits before writing, set PP=1 */
    hprt &= ~(HPRT_CONNDET | HPRT_ENACHG | HPRT_OVRCURRCHG);
    hprt |= HPRT_PWR;
    dwc2_wr(HPRT, hprt);
    dwc2_delay_ms(50); /* power stabilisation */

    return (dwc2_rd(HPRT) & HPRT_PWR) ? 0 : -1;
}

static int dwc2_port_reset_and_enable(void) {
    uint32_t hprt;

    /* Assert reset */
    hprt = dwc2_rd(HPRT) & ~(HPRT_CONNDET | HPRT_ENACHG | HPRT_OVRCURRCHG);
    hprt |= HPRT_RST;
    dwc2_wr(HPRT, hprt);
    dwc2_delay_ms(60); /* ≥ 50ms per USB spec for root-hub reset */

    /* Deassert reset */
    hprt = dwc2_rd(HPRT) & ~(HPRT_CONNDET | HPRT_ENACHG | HPRT_OVRCURRCHG | HPRT_RST);
    dwc2_wr(HPRT, hprt);
    dwc2_delay_ms(20); /* recovery */

    /* Check port enabled */
    hprt = dwc2_rd(HPRT);
    if (!(hprt & HPRT_ENA)) {
        uart_puts("[DWC2] Port not enabled after reset\n");
        return -1;
    }
    return 0;
}

/* ── EP0 channel transfer engine ─────────────────────────────────── *
 *                                                                     *
 * dwc2_hc_xfer() — execute one channel transaction (SETUP/IN/OUT).   *
 *                                                                     *
 * The Pi 4 DWC2 uses buffer-DMA mode (GAHBCFG.DMAEn=1).             *
 * In buffer-DMA mode the controller halts the channel on NAK, so     *
 * software must re-enable for retry.  XFRC + CHH = success.         *
 *                                                                     *
 *  ch        : channel index (0 for all control transfers)            *
 *  hcchar    : pre-built HCCHAR value (without CHENA)                *
 *  hctsiz    : pre-built HCTSIZ (PID | pktcnt | xfrsiz)             *
 *  dma_phys  : physical address of transfer buffer (32-bit)          *
 *  req_bytes : number of bytes requested (for bytes_done calc)        *
 *  bytes_done: output — actual bytes transferred (may be NULL)        *
 *                                                                     *
 * Returns 0 on success, -1 on error or 2-second timeout.             */
static int dwc2_hc_xfer(int ch, uint32_t hcchar, uint32_t hctsiz,
                          uint32_t dma_phys, int req_bytes, int *bytes_done)
{
    /* Clear any stale channel interrupts */
    dwc2_wr(HCINT(ch),    0xFFFFFFFFU);
    dwc2_wr(HCINTMSK(ch), 0U);          /* polling — no IRQ needed     */
    dwc2_wr(HCSPLT(ch),   0U);          /* no split transactions        */

    /* Program transfer registers */
    dwc2_wr(HCTSIZ(ch),   hctsiz);
    dwc2_wr(HCDMA(ch),    dma_phys);

    /* Enable channel — starts transfer immediately */
    asm volatile("dsb sy; isb" ::: "memory");
    dwc2_wr(HCCHAR(ch), hcchar | HCCHAR_CHENA);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Poll for completion — 2-second timeout */
    uint64_t t0, t1;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    uint64_t deadline = t0 + 2000ULL * 54000ULL;

    for (;;) {
        asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
        if (t1 >= deadline) {
            uart_puts("[DWC2] hc_xfer: 2s timeout\n");
            /* Attempt graceful halt */
            dwc2_wr(HCCHAR(ch), HCCHAR_CHDIS | HCCHAR_CHENA);
            return -1;
        }

        uint32_t ints = dwc2_rd(HCINT(ch));

        /* Success: transfer complete */
        if (ints & HCINT_XFRC) {
            if (bytes_done) {
                uint32_t remaining = dwc2_rd(HCTSIZ(ch)) & 0x7FFFFU;
                *bytes_done = req_bytes - (int)remaining;
                if (*bytes_done < 0) *bytes_done = 0;
            }
            return 0;
        }

        /* Fatal errors — halt channel before returning so ch0 is not
         * left in CHENA=1 state (boot268 fix: TXERR left ch enabled). */
        if (ints & HCINT_ALL_ERR) {
            debug_print("[DWC2] hc_xfer error: HCINT=0x%08x HCCHAR=0x%08x\n",
                        (unsigned)ints, (unsigned)dwc2_rd(HCCHAR(ch)));
            dwc2_wr(HCCHAR(ch), HCCHAR_CHDIS | HCCHAR_CHENA);
            dwc2_delay_ms(1);  /* wait for CHH */
            return -1;
        }

        /* NAK: buffer-DMA halts channel on NAK — re-enable for retry */
        if ((ints & (HCINT_NAK | HCINT_CHH)) == (HCINT_NAK | HCINT_CHH)) {
            dwc2_wr(HCINT(ch), HCINT_NAK | HCINT_CHH);
            dwc2_delay_ms(1);   /* brief gap before retry */
            dwc2_wr(HCCHAR(ch), hcchar | HCCHAR_CHENA);
            continue;
        }

        /* ACK without XFRC: partial IN (multi-packet) — still running */
        /* Just keep polling — controller re-arms automatically         */
    }
}

/* dwc2_ctrl_xfer() — USB EP0 three-stage control transfer.
 *
 *  dev_addr : USB device address (0 during initial enumeration)
 *  setup    : 8-byte SETUP packet
 *  is_in    : 1 = DATA-IN (device→host), 0 = DATA-OUT (host→device)
 *  data     : data buffer (filled on IN, sent on OUT)
 *  data_len : number of data bytes requested
 *  mps      : EP0 Max Packet Size (8 for LS, 64 for FS/HS)
 *  is_ls    : 1 if Low Speed device
 *
 * Returns bytes transferred on success, -1 on error.                  */
static int dwc2_ctrl_xfer(int dev_addr, const uint8_t *setup,
                           int is_in, void *data, int data_len,
                           int mps, int is_ls)
{
    int ch = 0;     /* channel 0 for all control transfers */

    /* Base HCCHAR: device address, EP0, Control type, MPS */
    uint32_t hcchar_base =
        ((uint32_t)(dev_addr & 0x7F) << 22) |
        HCCHAR_EPTYPE_CTRL                   |
        (is_ls ? HCCHAR_LSPDDEV : 0U)       |
        ((uint32_t)(mps & 0x7FF));

    /* ── SETUP stage: 8-byte OUT, PID=SETUP ──────────────────────── */
    __builtin_memcpy(dwc2_setup_buf, setup, 8);
    dcache_clean(dwc2_setup_buf, 8);        /* flush CPU writes to RAM  */

    uint32_t hcchar_out = hcchar_base;      /* EP dir OUT (bit 15 = 0)  */
    uint32_t hctsiz     = HCPID_SETUP | (1U << 19) | 8U;   /* 1 pkt, 8B */

    if (dwc2_hc_xfer(ch, hcchar_out, hctsiz,
                      (uint32_t)(uintptr_t)dwc2_setup_buf, 8, NULL) < 0) {
        uart_puts("[DWC2] ctrl_xfer: SETUP stage failed\n");
        return -1;
    }
    dwc2_delay_ms(2);   /* inter-stage gap */

    /* ── DATA stage (optional) ───────────────────────────────────── */
    int actual = 0;

    if (data_len > 0) {
        int pktcnt = (data_len + mps - 1) / mps;

        if (is_in) {
            /* DATA-IN: device sends data to host, PID starts at DATA1  */
            uint32_t hcchar_in = hcchar_base | HCCHAR_EPDIR_IN;
            hctsiz = HCPID_DATA1 |
                     ((uint32_t)(pktcnt & 0x3FF) << 19) |
                     ((uint32_t)(data_len & 0x7FFFF));

            /* Invalidate cache so DMA can safely write to RAM          */
            dcache_inval(dwc2_data_buf, (size_t)data_len);

            if (dwc2_hc_xfer(ch, hcchar_in, hctsiz,
                              (uint32_t)(uintptr_t)dwc2_data_buf,
                              data_len, &actual) < 0) {
                uart_puts("[DWC2] ctrl_xfer: DATA-IN stage failed\n");
                return -1;
            }

            /* Invalidate again so CPU sees what DMA wrote              */
            dcache_inval(dwc2_data_buf, (size_t)data_len);

            if (actual > 0 && data)
                __builtin_memcpy(data, dwc2_data_buf, (size_t)actual);

        } else {
            /* DATA-OUT: host sends data to device, PID starts at DATA1 */
            int send = (data_len <= (int)sizeof(dwc2_data_buf))
                       ? data_len : (int)sizeof(dwc2_data_buf);
            if (data) __builtin_memcpy(dwc2_data_buf, data, (size_t)send);
            dcache_clean(dwc2_data_buf, (size_t)send);

            hctsiz = HCPID_DATA1 |
                     ((uint32_t)(pktcnt & 0x3FF) << 19) |
                     ((uint32_t)(send & 0x7FFFF));

            if (dwc2_hc_xfer(ch, hcchar_out, hctsiz,
                              (uint32_t)(uintptr_t)dwc2_data_buf,
                              send, &actual) < 0) {
                uart_puts("[DWC2] ctrl_xfer: DATA-OUT stage failed\n");
                return -1;
            }
        }
    }
    dwc2_delay_ms(2);   /* inter-stage gap */

    /* ── STATUS stage: zero-length, PID=DATA1, opposite direction ── */
    if (is_in) {
        /* STATUS-OUT: host sends ZLP to device */
        hctsiz = HCPID_DATA1 | (1U << 19) | 0U;  /* 1 pkt, 0 bytes    */
        if (dwc2_hc_xfer(ch, hcchar_out, hctsiz,
                          (uint32_t)(uintptr_t)dwc2_setup_buf, 0, NULL) < 0) {
            uart_puts("[DWC2] ctrl_xfer: STATUS-OUT stage failed\n");
            return -1;
        }
    } else {
        /* STATUS-IN: host receives ZLP from device */
        uint32_t hcchar_in = hcchar_base | HCCHAR_EPDIR_IN;
        hctsiz = HCPID_DATA1 | (1U << 19) | 0U;
        if (dwc2_hc_xfer(ch, hcchar_in, hctsiz,
                          (uint32_t)(uintptr_t)dwc2_data_buf, 0, NULL) < 0) {
            uart_puts("[DWC2] ctrl_xfer: STATUS-IN stage failed\n");
            return -1;
        }
    }

    return actual;
}

/* ── USB Device Descriptor ───────────────────────────────────────── */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_dev_desc_t;

/* dwc2_enumerate_device() — send GET_DESCRIPTOR(Device) at address 0
 * and identify the device class from the response.
 *
 * speed:   0=HS, 1=FS, 2=LS (from HPRT_SPD field)
 * out_mps: receives bMaxPacketSize0 (caller's initial MPS for EP0)
 *
 * Returns: device class byte (>=0) on success, -1 on error.           */
static int dwc2_enumerate_device(int speed, int *out_mps)
{
    int is_ls = (speed == 2);
    /* Use MPS=8 for LS (spec-mandated), 64 for FS/HS (safe default)  */
    int mps0  = is_ls ? 8 : 64;

    uart_puts("[DWC2] EP0 GET_DESCRIPTOR(Device, 18 bytes) @ addr 0\n");

    /* Standard SETUP packet: GET_DESCRIPTOR, Device, 18 bytes         */
    static const uint8_t setup_get_dev[8] = {
        0x80,   /* bmRequestType: IN | Standard | Device               */
        0x06,   /* bRequest:      GET_DESCRIPTOR                       */
        0x00,   /* wValue.lo:     descriptor index = 0                 */
        0x01,   /* wValue.hi:     descriptor type = DEVICE (1)         */
        0x00,   /* wIndex.lo:     0                                    */
        0x00,   /* wIndex.hi:     0                                    */
        0x12,   /* wLength.lo:    18                                   */
        0x00    /* wLength.hi:    0                                    */
    };

    uint8_t desc[18];
    __builtin_memset(desc, 0, sizeof(desc));

    int got = dwc2_ctrl_xfer(0, setup_get_dev, 1, desc, 18, mps0, is_ls);
    if (got < 8) {
        debug_print("[DWC2] GET_DESCRIPTOR failed: got=%d\n", got);
        return -1;
    }

    usb_dev_desc_t *dd = (usb_dev_desc_t *)desc;

    debug_print("[DWC2] Device Descriptor (%d bytes):\n", got);
    debug_print("[DWC2]   bcdUSB=0x%04x  bDevClass=0x%02x  bDevSub=0x%02x\n",
                (unsigned)dd->bcdUSB,
                (unsigned)dd->bDeviceClass,
                (unsigned)dd->bDeviceSubClass);
    debug_print("[DWC2]   bMaxPkt0=%u  idVendor=0x%04x  idProduct=0x%04x\n",
                (unsigned)dd->bMaxPacketSize0,
                (unsigned)dd->idVendor,
                (unsigned)dd->idProduct);
    debug_print("[DWC2]   bcdDevice=0x%04x  bNumConfigs=%u\n",
                (unsigned)dd->bcdDevice,
                (unsigned)dd->bNumConfigurations);

    /* Identify device class */
    uint8_t cls = dd->bDeviceClass;
    const char *class_str;
    if      (cls == 0x00) class_str = "Composite (class defined per-interface)";
    else if (cls == 0x02) class_str = "CDC Control";
    else if (cls == 0x03) class_str = "HID";
    else if (cls == 0x08) class_str = "Mass Storage";
    else if (cls == 0x09) class_str = "Hub";
    else if (cls == 0x0A) class_str = "CDC Data";
    else if (cls == 0xE0) class_str = "Wireless Controller";
    else                   class_str = "Unknown";

    uart_puts("[DWC2] Device class: ");
    uart_puts(class_str);
    uart_puts("\n");

    /* Pass back bMaxPacketSize0 so the caller can set up control transfers */
    if (out_mps) *out_mps = (int)dd->bMaxPacketSize0;

    /* If HID class or composite (HID likely in interface), note next step */
    if (cls == 0x03 || cls == 0x00) {
        uart_puts("[DWC2] HID device detected — will enumerate config\n");
    } else if (cls == 0x09) {
        uart_puts("[DWC2] Hub class device detected — will enumerate ports\n");
    }

    return (int)cls;   /* caller uses class to decide next step */
}

/* ══════════════════════════════════════════════════════════════════ *
 *  HUB ENUMERATION + HID POLLING  (boot267)                         *
 * ══════════════════════════════════════════════════════════════════ *
 *                                                                    *
 * When the DWC2 OTG port has a USB hub attached (VID=0x1a40, the    *
 * Terminus hub seen in bootlog266), this code:                       *
 *   1. Assigns the hub a USB address (SET_ADDRESS → addr 1)         *
 *   2. Reads the hub class descriptor to find port count            *
 *   3. Powers and resets each port                                  *
 *   4. For each connected child device:                             *
 *      a. Assigns a USB address (2, 3, …)                          *
 *      b. Reads its device + config descriptor                      *
 *      c. If HID boot-protocol keyboard or mouse:                  *
 *         registers it in dwc2_hid[] for polling                   *
 *   5. dwc2_hid_poll() is called from the WIMP loop every 16 ms    *
 *      and issues GET_REPORT (class request, EP0) for each device,  *
 *      feeding data into mouse_event() / keyboard_event().          *
 *                                                                    *
 * All transfers use dwc2_ctrl_xfer() (channel 0, polling mode).     *
 * No interrupt-IN endpoint is required — GET_REPORT works reliably  *
 * on any properly configured HID device.                            *
 * ══════════════════════════════════════════════════════════════════ */

/* ── Static DMA buffer for large descriptor reads ────────────────── *
 * dwc2_data_buf (256 B, defined above) is used for all DATA-IN DMA. *
 * An additional aligned scratch buffer holds the full config desc.   */
static uint8_t __attribute__((aligned(64))) dwc2_scratch[256];

/* ── DWC2 HID device registry ────────────────────────────────────── */

#define DWC2_MAX_HID        2
#define DWC2_PROTO_KBD      1
#define DWC2_PROTO_MOUSE    2

typedef struct {
    uint8_t  active;
    uint8_t  dev_addr;   /* USB address assigned during enumeration */
    uint8_t  if_num;     /* HID interface number                    */
    uint8_t  protocol;   /* DWC2_PROTO_KBD / DWC2_PROTO_MOUSE      */
    uint8_t  is_ls;      /* 1 = Low Speed device                    */
    uint16_t ep0_mps;    /* EP0 Max Packet Size                     */
    uint8_t  last_report[8]; /* for debounce                        */
} dwc2_hid_t;

static dwc2_hid_t dwc2_hid[DWC2_MAX_HID];
static int        dwc2_hid_cnt = 0;
static uint32_t   dwc2_last_poll_ms[DWC2_MAX_HID]; /* per-device poll stamp */

/* ── ARM system counter → milliseconds (same formula as WIMP) ───── */
static inline uint32_t dwc2_ms(void) {
    uint64_t cnt, freq;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)(cnt / (freq / 1000ULL));
}

/* ── Standard USB control helpers ────────────────────────────────── *
 * All use dwc2_ctrl_xfer() which handles DMA, NAK retry, and cache. */

/* GET_DESCRIPTOR(Configuration, len) */
static int dwc2_get_config_desc(int addr, void *buf, int len, int mps, int is_ls)
{
    uint8_t s[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00,
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return dwc2_ctrl_xfer(addr, s, 1, buf, len, mps, is_ls);
}

/* GET_DESCRIPTOR(Device, len) at any address */
static int dwc2_get_dev_desc(int addr, void *buf, int len, int mps, int is_ls)
{
    uint8_t s[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return dwc2_ctrl_xfer(addr, s, 1, buf, len, mps, is_ls);
}

/* SET_ADDRESS */
static int dwc2_set_address(int cur_addr, int new_addr, int mps, int is_ls)
{
    uint8_t s[8] = { 0x00, 0x05,
                     (uint8_t)(new_addr & 0x7F), 0x00,
                     0x00, 0x00, 0x00, 0x00 };
    return dwc2_ctrl_xfer(cur_addr, s, 0, NULL, 0, mps, is_ls);
}

/* SET_CONFIGURATION */
static int dwc2_set_configuration(int addr, int cfg, int mps, int is_ls)
{
    uint8_t s[8] = { 0x00, 0x09,
                     (uint8_t)(cfg & 0xFF), 0x00,
                     0x00, 0x00, 0x00, 0x00 };
    return dwc2_ctrl_xfer(addr, s, 0, NULL, 0, mps, is_ls);
}

/* ── Hub class helpers ────────────────────────────────────────────── */

/* Hub class GET_DESCRIPTOR (type=0x29) */
static int dwc2_hub_get_class_desc(int hub_addr, void *buf, int len, int mps)
{
    uint8_t s[8] = { 0xA0, 0x06, 0x00, 0x29, 0x00, 0x00,
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return dwc2_ctrl_xfer(hub_addr, s, 1, buf, len, mps, 0 /*hub=FS*/);
}

/* Hub class GET_PORT_STATUS: returns wPortStatus and wPortChange */
static int dwc2_hub_get_port_status(int hub_addr, int port,
                                     uint16_t *status, uint16_t *change,
                                     int mps)
{
    uint8_t s[8] = { 0xA3, 0x00,           /* IN|Class|Other, GET_STATUS */
                     0x00, 0x00,            /* wValue = 0                 */
                     (uint8_t)port, 0x00,   /* wIndex = port              */
                     0x04, 0x00 };          /* wLength = 4                */
    uint8_t data[4] = {0};
    int got = dwc2_ctrl_xfer(hub_addr, s, 1, data, 4, mps, 0);
    if (got < 4) return -1;
    if (status) *status = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    if (change) *change = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    return 0;
}

/* Hub class SET_FEATURE on a port */
static int dwc2_hub_set_feature(int hub_addr, int port, int feature, int mps)
{
    uint8_t s[8] = { 0x23, 0x03,           /* OUT|Class|Other, SET_FEATURE */
                     (uint8_t)(feature & 0xFF), (uint8_t)(feature >> 8),
                     (uint8_t)port, 0x00,
                     0x00, 0x00 };
    return dwc2_ctrl_xfer(hub_addr, s, 0, NULL, 0, mps, 0);
}

/* Hub class CLEAR_FEATURE on a port */
static int dwc2_hub_clear_feature(int hub_addr, int port, int feature, int mps)
{
    uint8_t s[8] = { 0x23, 0x01,           /* OUT|Class|Other, CLEAR_FEATURE */
                     (uint8_t)(feature & 0xFF), (uint8_t)(feature >> 8),
                     (uint8_t)port, 0x00,
                     0x00, 0x00 };
    return dwc2_ctrl_xfer(hub_addr, s, 0, NULL, 0, mps, 0);
}

/* ── HID class helpers ────────────────────────────────────────────── */

/* HID SET_PROTOCOL 0 = boot protocol */
static int dwc2_hid_set_protocol(int addr, int if_num, int mps, int is_ls)
{
    uint8_t s[8] = { 0x21, 0x0B,           /* OUT|Class|Interface, SET_PROTOCOL */
                     0x00, 0x00,            /* wValue = 0 (boot protocol)        */
                     (uint8_t)if_num, 0x00,
                     0x00, 0x00 };
    return dwc2_ctrl_xfer(addr, s, 0, NULL, 0, mps, is_ls);
}

/* HID GET_REPORT (class request on EP0) — used for polling */
static int dwc2_hid_get_report(int addr, int if_num,
                                void *buf, int len, int mps, int is_ls)
{
    uint8_t s[8] = { 0xA1, 0x01,           /* IN|Class|Interface, GET_REPORT */
                     0x00, 0x01,            /* wValue = Input(1), ID=0        */
                     (uint8_t)if_num, 0x00,
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return dwc2_ctrl_xfer(addr, s, 1, buf, len, mps, is_ls);
}

/* ── Config descriptor parser ────────────────────────────────────── *
 * Walk the descriptor chain and find the first HID boot-protocol     *
 * interface (class=0x03, subclass=0x01, proto=1 keyboard or 2 mouse).*
 * Returns 0 on success, fills out_if_num, out_proto, out_cfg_val.    */
static int dwc2_parse_hid_config(const uint8_t *cfg, int total_len,
                                  uint8_t *out_if_num, uint8_t *out_proto,
                                  uint8_t *out_cfg_val)
{
    if (total_len < 9) return -1;
    *out_cfg_val = cfg[5];  /* bConfigurationValue from config header */

    int off = 0;
    while (off < total_len - 1) {
        uint8_t dlen  = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen < 2 || off + dlen > total_len) break;

        if (dtype == 0x04) {  /* Interface descriptor */
            uint8_t iclass = cfg[off + 5];
            uint8_t isub   = cfg[off + 6];
            uint8_t iproto = cfg[off + 7];

            /* HID boot protocol: class=0x03, subclass=0x01, proto=1 or 2 */
            if (iclass == 0x03 && isub == 0x01 &&
                (iproto == DWC2_PROTO_KBD || iproto == DWC2_PROTO_MOUSE)) {
                *out_if_num = cfg[off + 2];
                *out_proto  = iproto;
                return 0;
            }
        }
        off += dlen;
    }
    return -1;  /* no HID boot-protocol interface found */
}

/* ── Child device enumerator ─────────────────────────────────────── *
 * Called after a hub port has been reset and the child device is     *
 * in "Default" state at USB address 0.                               *
 *  new_addr  : USB address to assign (hub_addr+1, then +2 for more)  *
 *  child_is_ls : 1 if port status says LOW_SPEED                     */
static void dwc2_enum_child(int new_addr, int child_is_ls)
{
    int mps = child_is_ls ? 8 : 64;
    uint8_t desc[18] = {0};

    /* Step 1: GET_DESCRIPTOR(Device, 8) at addr 0 to get bMaxPacketSize0 */
    int got = dwc2_get_dev_desc(0, desc, 8, mps, child_is_ls);
    if (got < 8) {
        debug_print("[DWC2] Child@0: GET_DESCRIPTOR failed (got=%d)\n", got);
        return;
    }
    mps = (int)desc[7];  /* update from bMaxPacketSize0 */
    if (mps == 0) mps = child_is_ls ? 8 : 64;

    uint8_t dev_class = desc[4];
    debug_print("[DWC2] Child@0: class=0x%02x mps=%d is_ls=%d\n",
                dev_class, mps, child_is_ls);

    /* Step 2: SET_ADDRESS */
    if (dwc2_set_address(0, new_addr, mps, child_is_ls) < 0) {
        uart_puts("[DWC2] Child: SET_ADDRESS failed\n");
        return;
    }
    dwc2_delay_ms(5);  /* device needs ~2ms to switch address */

    /* Step 3: GET_DESCRIPTOR(Device, 18) at new_addr */
    got = dwc2_get_dev_desc(new_addr, desc, 18, mps, child_is_ls);
    if (got >= 18) {
        dev_class = desc[4];
        uint16_t vid = (uint16_t)(desc[8]  | ((uint16_t)desc[9]  << 8));
        uint16_t pid = (uint16_t)(desc[10] | ((uint16_t)desc[11] << 8));
        debug_print("[DWC2] Child@%d: class=0x%02x VID=0x%04x PID=0x%04x\n",
                    new_addr, dev_class, (unsigned)vid, (unsigned)pid);
    } else {
        debug_print("[DWC2] Child@%d: partial desc (%d B)\n", new_addr, got);
    }

    /* Only proceed for HID (class=0x03) or composite (class=0x00, may have HID if) */
    if (dev_class != 0x03 && dev_class != 0x00) {
        debug_print("[DWC2] Child@%d: not HID (skipping)\n", new_addr);
        return;
    }

    /* Step 4: GET_DESCRIPTOR(Config, 9) — read header to get wTotalLength */
    uint8_t cfghdr[9] = {0};
    got = dwc2_get_config_desc(new_addr, cfghdr, 9, mps, child_is_ls);
    if (got < 4) {
        debug_print("[DWC2] Child@%d: config header failed\n", new_addr);
        return;
    }
    int total_len = (int)(cfghdr[2] | ((int)cfghdr[3] << 8));
    if (total_len > (int)sizeof(dwc2_scratch)) total_len = (int)sizeof(dwc2_scratch);
    if (total_len < 9) total_len = 9;

    /* Step 5: GET_DESCRIPTOR(Config, total_len) — full descriptor */
    got = dwc2_get_config_desc(new_addr, dwc2_scratch, total_len, mps, child_is_ls);
    if (got < 9) {
        debug_print("[DWC2] Child@%d: full config failed\n", new_addr);
        return;
    }

    /* Step 6: Find HID boot-protocol interface */
    uint8_t if_num = 0, protocol = 0, cfg_val = 1;
    if (dwc2_parse_hid_config(dwc2_scratch, got, &if_num, &protocol, &cfg_val) < 0) {
        debug_print("[DWC2] Child@%d: no HID boot-protocol interface\n", new_addr);
        return;
    }
    const char *ps = (protocol == DWC2_PROTO_KBD)   ? "keyboard" :
                     (protocol == DWC2_PROTO_MOUSE)  ? "mouse"    : "?";
    debug_print("[DWC2] Child@%d: HID %s (if=%d cfg=%d)\n",
                new_addr, ps, if_num, cfg_val);

    /* Step 7: SET_CONFIGURATION */
    dwc2_set_configuration(new_addr, cfg_val, mps, child_is_ls);
    dwc2_delay_ms(10);

    /* Step 8: SET_PROTOCOL 0 (boot protocol) */
    dwc2_hid_set_protocol(new_addr, if_num, mps, child_is_ls);

    /* Step 9: Register for polling */
    if (dwc2_hid_cnt < DWC2_MAX_HID) {
        dwc2_hid_t *h = &dwc2_hid[dwc2_hid_cnt];
        __builtin_memset(h, 0, sizeof(*h));
        h->active   = 1;
        h->dev_addr = (uint8_t)new_addr;
        h->if_num   = if_num;
        h->protocol = protocol;
        h->is_ls    = (uint8_t)child_is_ls;
        h->ep0_mps  = (uint16_t)mps;
        dwc2_hid_cnt++;
        debug_print("[DWC2] HID %s registered at USB addr %d\n", ps, new_addr);
        uart_puts("[DWC2] HID device registered — polling will start in WIMP task\n");
    }
}

/* ── Hub enumerator ───────────────────────────────────────────────── *
 * Called from dwc2_start() when the device at address 0 is a hub.   *
 *  hub_mps  : bMaxPacketSize0 from the hub's device descriptor       *
 *  hub_speed: speed detected from HPRT_SPD (0=HS, 1=FS, 2=LS)       */
static void dwc2_enum_hub(int hub_mps, int hub_speed)
{
    (void)hub_speed;  /* currently assumed FS; extend if needed */

    uart_puts("[DWC2] Hub enumeration starting\n");

    /* ── Assign hub USB address 1 ──────────────────────────────── */
    if (dwc2_set_address(0, 1, hub_mps, 0) < 0) {
        uart_puts("[DWC2] Hub SET_ADDRESS failed\n");
        return;
    }
    dwc2_delay_ms(5);
    int hub_addr = 1;
    debug_print("[DWC2] Hub assigned address %d\n", hub_addr);

    /* ── GET_DESCRIPTOR(Config, 9) — get bConfigurationValue ───── */
    uint8_t cfghdr[9] = {0};
    int got = dwc2_get_config_desc(hub_addr, cfghdr, 9, hub_mps, 0);
    if (got < 5) {
        uart_puts("[DWC2] Hub: config descriptor header failed\n");
        return;
    }
    uint8_t cfg_val = cfghdr[5];
    debug_print("[DWC2] Hub: cfg_val=%d wTotalLen=%d\n",
                cfg_val, (int)(cfghdr[2] | ((int)cfghdr[3] << 8)));

    /* ── SET_CONFIGURATION ─────────────────────────────────────── */
    dwc2_set_configuration(hub_addr, cfg_val, hub_mps, 0);
    dwc2_delay_ms(20);

    /* ── GET_DESCRIPTOR(Hub, 0x29) — hub class descriptor ──────── *
     * Layout: [0]=bLength [1]=0x29 [2]=bNbrPorts [3-4]=wHubChar   *
     *         [5]=bPwrOn2PwrGood (×2ms) [6]=bHubContrCurrent       */
    uint8_t hubdesc[16] = {0};
    got = dwc2_hub_get_class_desc(hub_addr, hubdesc, 16, hub_mps);
    int n_ports    = (got >= 3) ? (int)hubdesc[2] : 4;
    int pwr_delay  = (got >= 6) ? (int)hubdesc[5] * 2 : 100;
    if (n_ports < 1 || n_ports > 8) n_ports = 4;  /* sanity */
    if (pwr_delay < 100) pwr_delay = 100;
    debug_print("[DWC2] Hub: %d port(s), power delay=%dms\n", n_ports, pwr_delay);

    /* ── PORT_POWER each port, then wait ──────────────────────── */
    for (int p = 1; p <= n_ports; p++)
        dwc2_hub_set_feature(hub_addr, p, 8 /*PORT_POWER*/, hub_mps);
    dwc2_delay_ms(pwr_delay);

    /* ── Scan each port for connected devices ─────────────────── */
    int next_addr = 2;   /* hub=1, children start at 2 */

    for (int p = 1; p <= n_ports; p++) {
        uint16_t pstatus = 0, pchange = 0;
        if (dwc2_hub_get_port_status(hub_addr, p, &pstatus, &pchange, hub_mps) < 0) {
            debug_print("[DWC2] Port %d: GET_PORT_STATUS failed\n", p);
            continue;
        }
        debug_print("[DWC2] Port %d: status=0x%04x change=0x%04x\n",
                    p, (unsigned)pstatus, (unsigned)pchange);

        /* bit 0 = PORT_CONNECTION */
        if (!(pstatus & 0x0001)) {
            debug_print("[DWC2] Port %d: empty\n", p);
            continue;
        }

        uart_puts("[DWC2] Device detected on hub port — resetting\n");

        /* PORT_RESET (feature=4) */
        dwc2_hub_set_feature(hub_addr, p, 4 /*PORT_RESET*/, hub_mps);
        dwc2_delay_ms(100);

        /* Re-read port status after reset */
        if (dwc2_hub_get_port_status(hub_addr, p, &pstatus, &pchange, hub_mps) < 0)
            continue;
        debug_print("[DWC2] Port %d post-reset: status=0x%04x\n",
                    p, (unsigned)pstatus);

        /* bit 1 = PORT_ENABLE — must be set after reset */
        if (!(pstatus & 0x0002)) {
            debug_print("[DWC2] Port %d: not enabled after reset\n", p);
            continue;
        }

        /* Clear C_PORT_RESET change bit (feature=0x14=20) */
        dwc2_hub_clear_feature(hub_addr, p, 0x14, hub_mps);

        /* Determine child speed: bit 9 = PORT_LOW_SPEED */
        int child_ls = (pstatus & (1U << 9)) ? 1 : 0;
        debug_print("[DWC2] Port %d: child is %s-speed\n",
                    p, child_ls ? "Low" : "Full");

        /* Enumerate the child device */
        if (next_addr < 10)
            dwc2_enum_child(next_addr++, child_ls);
    }

    debug_print("[DWC2] Hub enumeration done: %d HID device(s) registered\n",
                dwc2_hid_cnt);
}

/* ── dwc2_hid_poll() — called from WIMP task every ~16 ms ────────── *
 *                                                                     *
 * Issues HID GET_REPORT (class EP0 request) for each registered HID  *
 * device and forwards the decoded report to mouse_event() /          *
 * keyboard_event() in the input layer.                               *
 *                                                                     *
 * GET_REPORT reliably returns current device state — no periodic      *
 * schedule or interrupt-IN endpoint required.                        */

/* Forward declarations for the input layer (defined in input_stub.c) */
extern void mouse_event(const void *ev);
extern void keyboard_event(const void *ev);
extern void con_putc(char c);

/* Process a mouse boot-protocol report: [buttons, dx, dy, wheel?] */
static void dwc2_proc_mouse(dwc2_hid_t *h, const uint8_t *data, int len)
{
    if (len < 3) return;

    int8_t  dx  = (int8_t)data[1];
    int8_t  dy  = -(int8_t)data[2];  /* USB Y-down → RISC OS Y-up */
    int8_t  wh  = (len >= 4) ? (int8_t)data[3] : 0;
    uint8_t btn = data[0];

    /* Only post if something changed */
    if (!dx && !dy && !wh && btn == h->last_report[0]) return;

    /* Inline mouse_event_t (mirrors drivers/input/mouse.h) */
    struct { int16_t x, y, dx, dy; uint8_t buttons; int8_t wheel; } ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    ev.dx = dx; ev.dy = dy; ev.wheel = wh;
    if (btn & 0x01) ev.buttons |= 0x04;  /* left   → SELECT */
    if (btn & 0x04) ev.buttons |= 0x02;  /* middle → MENU   */
    if (btn & 0x02) ev.buttons |= 0x01;  /* right  → ADJUST */

    if (dx || dy || ev.buttons || wh)
        debug_print("[DWC2-MOUSE] dx=%d dy=%d btn=0x%02x\n",
                    (int)dx, (int)dy, (unsigned)ev.buttons);

    __builtin_memcpy(h->last_report, data,
                     (size_t)((len < 8) ? len : 8));
    mouse_event(&ev);
}

/* Minimal scancode→ASCII tables for keyboard boot reports */
static const char dwc2_sc2asc[] = {
    0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l',  /* 00-0F */
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',  /* 10-1D */
    '1','2','3','4','5','6','7','8','9','0',                  /* 1E-27 */
    '\n',0x1B,'\b','\t',' ','-','=','[',']','\\','#',';',    /* 28-33 */
    '\'','`',',','.','/'}; /* 34-38 */

static const char dwc2_sc2asc_s[] = {
    0,0,0,0,'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n',0x1B,'\b','\t',' ','_','+','{','}','|','~',':',
    '"','~','<','>','?'};

/* Process a keyboard boot-protocol report */
static void dwc2_proc_keyboard(dwc2_hid_t *h, const uint8_t *data, int len)
{
    if (len < 3) return;
    int shift = (data[0] & 0x22) != 0;  /* any Shift key held */

    /* Inline keyboard_event_t (mirrors drivers/input/keyboard.h) */
    struct { uint8_t key_code, key_char, modifiers, pad; } kev;
    __builtin_memset(&kev, 0, sizeof(kev));
    if (data[0] & 0x22) kev.modifiers |= 0x01;  /* MOD_SHIFT */
    if (data[0] & 0x11) kev.modifiers |= 0x02;  /* MOD_CTRL  */
    if (data[0] & 0x44) kev.modifiers |= 0x04;  /* MOD_ALT   */

    for (int i = 2; i < 8 && i < len; i++) {
        uint8_t code = data[i];
        if (code == 0 || code > 0xE7) continue;

        /* Debounce: skip if key was already down in last report */
        int seen = 0;
        for (int j = 2; j < 8; j++) {
            if (h->last_report[j] == code) { seen = 1; break; }
        }
        if (seen) continue;

        /* ESC (code=0x29) always maps to 0x1B */
        char ch = 0;
        if (code == 0x29) {
            ch = 0x1B;
        } else if (code < (uint8_t)sizeof(dwc2_sc2asc)) {
            ch = shift ? dwc2_sc2asc_s[code] : dwc2_sc2asc[code];
        }

        kev.key_char = (uint8_t)ch;
        kev.key_code = (uint8_t)ch;

        if (kev.key_code || kev.key_char) {
            debug_print("[DWC2-KBD] code=0x%02x char='%c' mod=0x%02x\n",
                        code,
                        (ch >= 0x20 && ch < 0x7F) ? ch : '.',
                        kev.modifiers);
            keyboard_event(&kev);
            if (ch >= 0x20) con_putc(ch);
        }
    }

    __builtin_memcpy(h->last_report, data,
                     (size_t)((len < 8) ? len : 8));
}

/* Public: called from WIMP task at ~60 Hz (rate-limited inside) */
void dwc2_hid_poll(void)
{
    if (!dwc2_hid_cnt) return;

    uint32_t now = dwc2_ms();

    for (int i = 0; i < dwc2_hid_cnt; i++) {
        dwc2_hid_t *h = &dwc2_hid[i];
        if (!h->active) continue;

        /* Per-device 16ms gate (≈60 Hz max poll rate) */
        if ((now - dwc2_last_poll_ms[i]) < 16u) continue;
        dwc2_last_poll_ms[i] = now;

        uint8_t report[8] = {0};
        int got = dwc2_hid_get_report(h->dev_addr, h->if_num,
                                       report, 8,
                                       (int)h->ep0_mps, h->is_ls);
        if (got > 0) {
            if (h->protocol == DWC2_PROTO_MOUSE)
                dwc2_proc_mouse(h, report, got);
            else if (h->protocol == DWC2_PROTO_KBD)
                dwc2_proc_keyboard(h, report, got);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

int dwc2_init(void) {
    uart_puts("[DWC2] Initialising SoC USB OTG controller (0xFE980000)...\n");

    /* Sanity: read Synopsys ID register — should be 0x4F54xxxx */
    uint32_t snpsid = dwc2_rd(GSNPSID);
    if ((snpsid >> 16) != 0x4F54) {
        debug_print("[DWC2] Synopsys ID mismatch — controller not present\n");
        debug_print("[DWC2]   GSNPSID=0x%08x (expected 0x4F54xxxx)\n", (unsigned)snpsid);
        return -1;
    }
    debug_print("[DWC2]   GSNPSID=0x%08x  HWcfg2=0x%08x\n",
                snpsid, dwc2_rd(GHWCFG2));

    /* Core soft reset */
    if (dwc2_core_reset() < 0) return -1;

    /* Configure for host mode */
    dwc2_core_init_host();

    /* Power the port */
    if (dwc2_port_power_on() < 0) {
        uart_puts("[DWC2] Port power-on failed\n");
        return -1;
    }

    /* Check if a device is connected */
    uint32_t hprt = dwc2_rd(HPRT);
    if (!(hprt & HPRT_CONN)) {
        uart_puts("[DWC2] No device connected on OTG port\n");
        dwc2_initialised = 1;
        dwc2_port_present = 0;
        return 0;
    }

    uart_puts("[DWC2] Device detected — issuing port reset\n");

    /* Reset and enable the port */
    if (dwc2_port_reset_and_enable() < 0) {
        uart_puts("[DWC2] Port reset/enable failed\n");
        dwc2_initialised = 1;
        dwc2_port_present = 0;
        return 0;
    }

    /* Report speed and HPRT state */
    hprt = dwc2_rd(HPRT);
    uint32_t speed = (hprt & HPRT_SPD) >> 17;
    const char *spd_str = (speed == 0) ? "High-Speed (480 Mbps)" :
                          (speed == 1) ? "Full-Speed (12 Mbps)"  :
                          (speed == 2) ? "Low-Speed (1.5 Mbps)"  : "Unknown";
    uart_puts("[DWC2] Port enabled — speed: ");
    uart_puts(spd_str); uart_puts("\n");
    debug_print("[DWC2]   HPRT=0x%08x  ENA=%u  CONN=%u  SPD=%u\n",
                (unsigned)hprt,
                (unsigned)((hprt >> 2) & 1),
                (unsigned)(hprt & 1),
                (unsigned)speed);
    uart_puts("[DWC2] Device ready for enumeration\n");
    uart_puts("[DWC2]   Next step: read device descriptor via EP0 control transfer\n");

    dwc2_initialised  = 1;
    dwc2_port_present = 1;
    return 0;
}

int dwc2_device_present(void) {
    return dwc2_port_present;
}

int dwc2_start(void) {
    /* Called from the USB subsystem init path, AFTER xHCI is initialised.
     * Runs completely independently — does not touch xHCI state.           */
    uart_puts("[DWC2] Starting SoC USB 2.0 OTG subsystem\n");

    /* Guard against double-init: usb_init.c calls dwc2_init() before
     * dwc2_scan_ports() → dwc2_start(), so dwc2_initialised is already
     * set.  Skip the redundant reset/power-on sequence.               */
    if (!dwc2_initialised && dwc2_init() < 0) {
        uart_puts("[DWC2] Init failed — OTG port unavailable\n");
        return -1;
    }

    if (!dwc2_port_present) {
        uart_puts("[DWC2] OTG port: no device — skipping enumeration\n");
        return 0;
    }

    /* Read device speed from HPRT [18:17] */
    uint32_t hprt  = dwc2_rd(HPRT);
    int      speed = (int)((hprt & HPRT_SPD) >> 17);

    /* EP0 enumeration: read device descriptor, get class + MPS */
    int dev_mps = 64;
    int dev_cls = dwc2_enumerate_device(speed, &dev_mps);
    if (dev_cls < 0) {
        uart_puts("[DWC2] Device enumeration failed\n");
        return -1;
    }

    uart_puts("[DWC2] EP0 enumeration complete\n");

    /* boot267: act on device class */
    if (dev_cls == 0x09) {
        /* USB Hub — enumerate its ports to find keyboards / mice */
        dwc2_enum_hub(dev_mps, speed);
    } else if (dev_cls == 0x03 || dev_cls == 0x00) {
        /* HID or composite device directly on OTG port — rare but handle it */
        uart_puts("[DWC2] Direct HID on OTG port — enumerating as child@1\n");
        /* Treat as "child at addr 0, assign addr 1" */
        dwc2_enum_child(1, (speed == 2) ? 1 : 0);
    } else {
        debug_print("[DWC2] Device class 0x%02x — no further action\n", dev_cls);
    }

    return 0;
}

/* ── Compatibility shims for usb_init.c ─────────────────────────────────────
 *
 * usb_init.c references g_dwc2_hc_ops and dwc2_scan_ports from the old
 * DWC2 stub API.  Provide minimal definitions here so the linker is happy.
 * dwc2_scan_ports() delegates to dwc2_start() which is the real entry point.
 * g_dwc2_hc_ops is a stub ops struct — DWC2 enumeration is not yet
 * implemented so all transfer functions return -1.                          */

static int _dwc2_nop_control(usb_device_t *d, uint8_t rt, uint8_t req,
                              uint16_t val, uint16_t idx, void *buf,
                              uint16_t len, int tmo) {
    (void)d;(void)rt;(void)req;(void)val;(void)idx;(void)buf;(void)len;(void)tmo;
    return -1;
}
static int _dwc2_nop_bulk(usb_endpoint_t *ep, void *buf, size_t len, int tmo) {
    (void)ep;(void)buf;(void)len;(void)tmo; return -1;
}
static int _dwc2_nop_intr(usb_endpoint_t *ep, void *buf, size_t len, int tmo) {
    (void)ep;(void)buf;(void)len;(void)tmo; return -1;
}
static int _dwc2_nop_enum(usb_device_t *dev, int port) {
    (void)dev;(void)port; return 0;
}

usb_hc_ops_t g_dwc2_hc_ops = {
    .control_transfer   = _dwc2_nop_control,
    .bulk_transfer      = _dwc2_nop_bulk,
    .interrupt_transfer = _dwc2_nop_intr,
    .enumerate_device   = _dwc2_nop_enum,
};

int dwc2_scan_ports(void) {
    /* Legacy entry point — delegates to dwc2_start() */
    return dwc2_start();
}
