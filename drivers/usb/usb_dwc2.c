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

    /* TODO: enumerate device — read device descriptor via EP0,
     * identify class, bind HID/MSC/CDC class drivers.
     * For now report that a device is present.                            */
    uart_puts("[DWC2] OTG device present — enumeration stub (TODO)\n");
    uart_puts("[DWC2] Connect a USB keyboard/mouse to the USB-C port\n");
    uart_puts("[DWC2]   to use this path while xHCI event ring is debugged\n");
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
