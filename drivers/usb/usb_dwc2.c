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
#define HCINT_ALL_ERR   (HCINT_STALL | HCINT_TXERR | HCINT_BBERR | \
                         HCINT_DTERR | HCINT_AHBERR | HCINT_FRMOR)

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

        /* Fatal errors */
        if (ints & HCINT_ALL_ERR) {
            debug_print("[DWC2] hc_xfer error: HCINT=0x%08x HCCHAR=0x%08x\n",
                        (unsigned)ints, (unsigned)dwc2_rd(HCCHAR(ch)));
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
 * speed: 0=HS, 1=FS, 2=LS (from HPRT_SPD field)                     */
static int dwc2_enumerate_device(int speed)
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

    /* If HID class or composite (HID likely in interface), note next step */
    if (cls == 0x03 || cls == 0x00) {
        uart_puts("[DWC2] HID device detected — EP0 GET_DESCRIPTOR(Config) next\n");
    }

    return 0;
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

    if (dwc2_init() < 0) {
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

    /* EP0 enumeration: read device descriptor */
    if (dwc2_enumerate_device(speed) < 0) {
        uart_puts("[DWC2] Device enumeration failed\n");
        return -1;
    }

    uart_puts("[DWC2] boot139: EP0 enumeration complete\n");
    uart_puts("[DWC2] Next: GET_DESCRIPTOR(Config) → HID interrupt-IN poll\n");
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
