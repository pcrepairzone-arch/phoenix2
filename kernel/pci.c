/*
 * pci.c - VL805 xHCI for Phoenix RISC OS
 *
 * Supports both Raspberry Pi 4 and Pi 5:
 *
 *   Pi 4 (BCM2711):
 *     PCIe RC base  = 0xFD500000
 *     USB chip      = VL805 xHCI (external, via PCIe)
 *
 *   Pi 5 (BCM2712):
 *     PCIe RC base  = 0x1F00000000
 *     USB chip      = Built-in xHCI via RP1 (0x1F00100000)
 *
 * ─── HOW BCM2711 PCIe CONFIG SPACE WORKS ────────────────────────────────
 *
 *  The BCM2711 RC uses an EXT_CFG sideband to provide access to the
 *  downstream endpoint's config space:
 *
 *    PCIE_EXT_CFG_INDEX  (RC+0x9000): selects the target slot
 *    PCIE_EXT_CFG_DATA   (RC+0x9004): base of a 4KB config space window
 *
 *  Index format:  bits[27:20]=bus, bits[19:12]=devfn, rest=ignored
 *  Data  format:  DATA + (config_offset & 0xFFF) = that config register
 *
 *  IMPORTANT LIMITATION: The BCM2711 RC only forwards Vendor/Device ID
 *  (offset 0x00) and Command/Status (offset 0x04) through EXT_CFG to the
 *  downstream endpoint.  All other offsets — including Class Code (0x08),
 *  BAR0 (0x10), BAR1 (0x14) — return the RC's OWN register values (which
 *  are 0x00000000 for those fields, since the RC has no BARs).  BAR writes
 *  via EXT_CFG also go to the RC and are silently ignored.
 *
 *  This is why Class/Rev always reads 0x00000000, and why BAR writes
 *  never stick — they never reach the VL805.
 *
 * ─── HOW BAR ASSIGNMENT ACTUALLY WORKS ON BCM2711 ───────────────────────
 *
 *  The RC has a dedicated outbound ATU (Address Translation Unit) that maps
 *  a CPU address window to a PCIe bus address window.  The correct mapping
 *  from the BCM2711 DTS (bcm2711-rpi-4-b.dts ranges property) is:
 *    CPU  window: 0x600000000 – 0x603FFFFFF  (64 MB)
 *    PCI  window: 0x000000000 – 0x03FFFFFFF  (64 MB)
 *
 *  After the firmware's 'PCI0 reset', the VL805 power-on-resets and its
 *  BAR0 returns to 0x00000000 (the PCI spec default).  This means the VL805
 *  MMIO window sits at PCI address 0x00000000, which maps to CPU 0x600000000
 *  via the ATU.  No BAR programming is needed or possible via EXT_CFG.
 *
 *  NOTE: Using PCI base 0xC0000000 (a common mistake) causes reads to bounce
 *  off the RC's own inbound DMA window (PCIe 0xC0000000 → CPU 0x00000000),
 *  returning data from system RAM instead of the VL805 registers.
 *
 * ─── PERST# NOTE ─────────────────────────────────────────────────────────
 *
 *  RGR1_SW_INIT_1 reads as 0x00000000 on every boot — PERST# is already
 *  deasserted by start4.elf before it jumps to our kernel.  Writing bit 2
 *  clear is a no-op but harmless.  The VL805 is live and responding when
 *  we enter kernel_main.
 *
 * ─── KEY FIXES IN THIS VERSION ───────────────────────────────────────────
 *
 *  1. BAR0 is NOT programmed via EXT_CFG (impossible on BCM2711).
 *     Instead, after ATU setup, xhci_base is set directly to VL805_BAR0_CPU.
 *  2. Command register (Memory Space + Bus Master) IS written via EXT_CFG
 *     because EXT_CFG does forward offset 0x04 to the VL805.
 *  3. Bus-1 scanning removed — on BCM2711, EXT_CFG uses the VL805's actual
 *     config space at index 0 (bus=0,dev=0).  Bridge bus-number writes are
 *     also dropped since the downstream bus topology is fixed in hardware.
 *  4. "xHCI online" banner only printed on success.
 *  5. All MMIO reads checked for 0xFFFFFFFF (device disappeared).
 */

#include "kernel.h"
#include "pci.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void *ioremap(uint64_t phys_addr, size_t size);
extern int   get_pi_model(void);

/* ── Globals ────────────────────────────────────────────────────── */

static void    *pcie_base = NULL;
static pci_dev_t vl805_dev;
static void    *xhci_base = NULL;
static void    *xhci_op   = NULL;

/* ── Print helpers ──────────────────────────────────────────────── */

static void print_hex32(uint32_t val) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        int n = (val >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

static void print_hex8(uint8_t val) {
    uart_puts("0x");
    for (int i = 4; i >= 0; i -= 4) {
        int n = (val >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

static __attribute__((unused)) void print_hex64(uint64_t val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int n = (val >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

/* ── Delay ──────────────────────────────────────────────────────── */

static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 10000; i++) {}
    asm volatile("dsb sy; isb" ::: "memory");
}

/* ── PCIe register offsets ──────────────────────────────────────── */

#define RGR1_SW_INIT_1_OFF      0x9210
#define MISC_PCIE_STATUS_OFF    0x4068
#define PCIE_EXT_CFG_INDEX_OFF  0x9000
#define PCIE_EXT_CFG_DATA_OFF   0x9004

/* BCM2711 outbound ATU registers — configure CPU→PCIe address translation */
#define MISC_MISC_CTRL_OFF              0x4008
#define MISC_CPU_2_PCIE_MEM_WIN0_LO     0x400C
#define MISC_CPU_2_PCIE_MEM_WIN0_HI     0x4010
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI    0x4080
#define MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI   0x4084

/*
 * PCIE_MISC_HARD_PCIE_HARD_DEBUG (BCM2711, offset 0x4204)
 *
 * Bit 21: L1SS_ENABLE — enables PCIe L1 Sub-States (ASPM L1ss).
 *         When set, the RC is allowed to put the link into L1 substates
 *         where CLKREQ# is deasserted and the PCIe reference clock stops.
 *         In that state ALL outbound memory TLPs are silently dropped —
 *         this is why every boot returned 0xdeaddead from every PCI address
 *         despite the ATU, Command, bus numbers and VL805 Command all being
 *         correct.  The ATU sweep (0x00000000, 0x80000000, 0xC0000000) all
 *         dead confirmed the link was in L1ss blocking every TLP.
 *
 *         Linux pcie-brcmstb.c defines:
 *           PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK = BIT(21)
 *         and clears it unconditionally in brcm_pcie_setup().
 *
 * Bit  0: CLKREQ_DEBUG_ENABLE — separate debug mode; also cleared by Linux.
 *         (Was already 0 in our boots, not the blocker.)
 */
#define MISC_HARD_DEBUG_OFF         0x4204
#define HARD_DEBUG_L1SS_MASK        (1U << 21)
#define HARD_DEBUG_CLKREQ_MASK      (1U <<  0)

/*
 * PCIE_RC_CFG_PRIV1_ID_VAL3 (BCM2711, offset 0x043C)
 * RC ships with wrong PCI Class Code.  Linux fixes it to 0x060400
 * (PCI-to-PCI Bridge).  Bits[31:24]=revision (preserved), [23:0]=class.
 */
#define RC_CFG_PRIV1_ID_VAL3_OFF    0x043C
#define RC_CORRECT_CLASS            0x060400U

#define PI4_PCIE_RC_BASE     0xFD500000ULL
#define PI5_PCIE_RC_BASE     0x1F00000000ULL
#define PI5_RP1_XHCI_BASE    0x1F00100000ULL

/*
 * VL805_BAR0_CPU: the CPU-side address of the VL805 MMIO window.
 * This is where xHCI registers appear after the ATU is configured.
 * ATU maps CPU:0x600000000 → PCIe:0x00000000 (= VL805 BAR0 default after reset).
 */
#define VL805_BAR0_CPU       0x600000000ULL

/* ── PCI config access ──────────────────────────────────────────── */

/*
 * On BCM2711, EXT_CFG reaches the downstream EP at any bus/dev/func.
 * For the VL805 (always bus=0, dev=0, func=0 from EXT_CFG's perspective),
 * only offsets 0x00 (ID) and 0x04 (Command) are reliably forwarded.
 */
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset) {
    uint32_t idx = ((uint32_t)dev->bus  << 20) |
                   ((uint32_t)dev->dev  << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    return readl(pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFF));
}

void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value) {
    uint32_t idx = ((uint32_t)dev->bus  << 20) |
                   ((uint32_t)dev->dev  << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    writel(value, pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFF));
    asm volatile("dsb sy; isb" ::: "memory");
}

/* ── PCIe link ──────────────────────────────────────────────────── */

static int pcie_bring_up_link(void) {
    uart_puts("\n[PCI] Bringing up PCIe link...\n");

    /* Dump RC state as firmware left it */
    uint32_t rgr1  = readl(pcie_base + RGR1_SW_INIT_1_OFF);
    uint32_t mctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    uint32_t hdbg  = readl(pcie_base + MISC_HARD_DEBUG_OFF);
    uart_puts("[PCI] RGR1="); print_hex32(rgr1);
    uart_puts("  MISC_CTRL="); print_hex32(mctrl);
    uart_puts("  HARD_DEBUG="); print_hex32(hdbg);
    uart_puts("\n");

    /*
     * BCM2711 RGR1_SW_INIT_1 — empirically confirmed register behaviour:
     *
     *   RGR1 = 0x00000000  →  LINK UP  (RC active, PERST# deasserted)
     *   RGR1 = 0x00000001  →  LINK FAILED (boots 16-17)
     *
     * Firmware leaves RGR1 = 0x00000003 (both bits set).
     * Clearing BOTH bits (→ 0x00000000) produces a working link.
     * Clearing only bit 1 (→ 0x00000001) fails.
     *
     * Sequence:
     *   Step 1 — Clear bit 1 (RC soft reset): RC comes out of reset.
     *   Step 2 — Clear bit 0 (PERST#): VL805 is released / link trains.
     */
    rgr1 &= ~(1U << 1);
    writel(rgr1, pcie_base + RGR1_SW_INIT_1_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(1);

    rgr1 &= ~(1U << 0);
    writel(rgr1, pcie_base + RGR1_SW_INIT_1_OFF);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Step 3: wait for PCIe link layer to come up (~50–100 ms typical) */
    uint32_t reg;
    int link_ms = 0;
    for (int t = 200; t > 0; t--) {
        delay_ms(10);
        link_ms += 10;
        reg = readl(pcie_base + MISC_PCIE_STATUS_OFF);
        if ((reg & (1U << 4)) && (reg & (1U << 5))) {
            uart_puts("[PCI]   LINK UP after ");
            /* print link_ms decimal */
            if (link_ms >= 100) uart_putc('0' + link_ms / 100);
            uart_putc('0' + (link_ms / 10) % 10);
            uart_putc('0' + link_ms % 10);
            uart_puts(" ms\n");
            break;
        }
        if (t == 1) { uart_puts("[PCI]   LINK FAILED\n"); return -1; }
    }

    uart_puts("[PCI]   Waiting 500ms for VL805 SPI firmware load...\n");
    delay_ms(500);

    /* Print RGR1 so we can verify the PERST# state at MMIO access time */
    uart_puts("[PCI] RGR1 post-link=");
    print_hex32(readl(pcie_base + RGR1_SW_INIT_1_OFF));
    uart_puts("\n");

    /*
     * CRITICAL: Disable L1 Sub-States (ASPM L1ss) — bit 21 of HARD_DEBUG.
     *
     * The boot log showed HARD_DEBUG = 0x00200000 after link-up.
     * 0x00200000 = BIT(21) = L1SS_ENABLE.
     *
     * With L1ss enabled the RC is free to put the PCIe link into L1 power
     * saving sub-states.  In L1ss the CLKREQ# signal is deasserted, the PCIe
     * reference clock stops, and the endpoint (VL805) powers down its
     * transceiver.  Any outbound memory read TLP issued by the ARM CPU is
     * silently discarded — the RC returns 0xdeaddead instead.
     *
     * This was the root cause of every failed MMIO read across 14 boots:
     * the ATU sweep of 0x00000000 / 0x80000000 / 0xC0000000 all returned
     * 0xdeaddead because the link was in L1ss the moment we tried.
     *
     * Linux pcie-brcmstb.c (PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK
     * = BIT(21)) clears this unconditionally after link-up.
     */
    hdbg = readl(pcie_base + MISC_HARD_DEBUG_OFF);
    uart_puts("[PCI] HARD_DEBUG before L1ss fix="); print_hex32(hdbg); uart_puts("\n");
    hdbg &= ~HARD_DEBUG_L1SS_MASK;    /* disable L1ss — keep link in L0 */
    hdbg &= ~HARD_DEBUG_CLKREQ_MASK;  /* also clear CLKREQ debug (Linux clears both) */
    writel(hdbg, pcie_base + MISC_HARD_DEBUG_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] HARD_DEBUG after  L1ss fix="); print_hex32(readl(pcie_base + MISC_HARD_DEBUG_OFF));
    uart_puts(" (L1ss disabled — link stays in L0)\n");

    /* RC Class Code fixup (BCM2711 ships with wrong class code) */
    uint32_t ccr = readl(pcie_base + RC_CFG_PRIV1_ID_VAL3_OFF);
    ccr = (ccr & 0xFF000000U) | RC_CORRECT_CLASS;
    writel(ccr, pcie_base + RC_CFG_PRIV1_ID_VAL3_OFF);

    return 0;
}

/* ── FLR ────────────────────────────────────────────────────────── */

static void vl805_flr(pci_dev_t *dev) {
    uint32_t cap_ptr = pci_read_config(dev, 0x34) & 0xFC;
    while (cap_ptr && cap_ptr < 0x100) {
        uint32_t cap = pci_read_config(dev, cap_ptr);
        if ((cap & 0xFF) == 0x10) {
            uint32_t devcap = pci_read_config(dev, cap_ptr + 4);
            if (devcap & (1U << 28)) {
                uint32_t devctl = pci_read_config(dev, cap_ptr + 8);
                devctl |= (1U << 15);
                pci_write_config(dev, cap_ptr + 8, devctl);
                delay_ms(100);
                uart_puts("[PCI] FLR complete\n");
            }
            return;
        }
        cap_ptr = (cap >> 8) & 0xFC;
    }
    uart_puts("[PCI] No FLR support — skipping\n");
}

/* ── BCM2711 outbound ATU setup ─────────────────────────────────── */

static void pcie_setup_outbound_window(void) {
    uart_puts("[PCI] Configuring outbound ATU window...\n");

    /*
     * BCM2711 outbound ATU register semantics (from Linux pcie-brcmstb.c):
     *
     *   WIN0_LO  = lower 32 bits of the PCIe destination base address.
     *              *** NO size encoding — raw address only. ***
     *              brcm_pcie_encode_ibar_size() is for INBOUND SCB windows
     *              only (PCIe→CPU DMA).  Writing 0x0b into WIN0_LO corrupts
     *              the PCIe address to 0x3 (hardware masks bits[3:2]) and
     *              generates misaligned TLPs that the VL805 ignores.
     *   WIN0_HI  = upper 32 bits of PCIe destination (always 0 — 32-bit bus)
     *
     *   BASE_LIMIT = packed {cpu_limit_mb[15:0], cpu_base_mb[15:0]}
     *                where cpu_*_mb = cpu_addr >> 20  (address in 1 MB units)
     *   BASE_HI    = cpu_base_mb >> 16
     *   LIMIT_HI   = cpu_limit_mb >> 16
     *
     * For CPU window 0x600000000 – 0x603FFFFFFF (64 MB):
     *   cpu_base_mb  = 0x600000000 >> 20 = 0x6000
     *   cpu_limit_mb = 0x603FFFFFFF >> 20 = 0x603F
     *   BASE_LIMIT   = (0x603F << 16) | 0x6000 = 0x603F6000
     *   BASE_HI      = 0x6000 >> 16 = 0x0
     *   LIMIT_HI     = 0x603F >> 16 = 0x0
     *
     * Window size is determined purely by (LIMIT - BASE), not by WIN0_LO.
     *
     * Linux write order: MISC_CTRL first, then WIN0 registers.
     * pci_init_pi4() sets the final WIN0_LO PCIe destination after this call.
     */

    /* 1. SCB_ACCESS_EN + CFG_READ_UR_MODE — must be set BEFORE WIN0 registers
     *    (matches Linux brcm_pcie_setup() ordering).
     *    SCB_ACCESS_EN  BIT(12): enables ARM CPU→PCIe outbound memory TLPs.
     *    CFG_READ_UR_MODE BIT(13): UR completion for non-existent config reads.
     */
    uint32_t ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    ctrl |= (1U << 12);   /* SCB_ACCESS_EN */
    ctrl |= (1U << 13);   /* CFG_READ_UR_MODE */
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    uart_puts("[PCI] MISC_CTRL after setup="); print_hex32(ctrl);
    uart_puts(ctrl & (1U << 12) ? "  SCB_ACCESS_EN=1 OK\n" : "  SCB_ACCESS_EN=0 PROBLEM\n");

    /* 2. Outbound window registers — raw PCIe address, no size encoding */
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI);
    writel(0x603F6000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT);
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);
    asm volatile("dsb sy; isb" ::: "memory");

    uart_puts("[PCI] Outbound ATU window ready\n");
}

/* ── xHCI common functions ──────────────────────────────────────── */

static int xhci_read_capabilities(void) {
    if (!xhci_base) {
        uart_puts("[xHCI] ERROR: xhci_base NULL\n");
        return -1;
    }

    /* First 32-bit read: if 0xFFFFFFFF the window isn't mapped */
    uint32_t first = readl(xhci_base);
    if (first == 0xFFFFFFFF) {
        uart_puts("[xHCI] ERROR: reads 0xFFFFFFFF — ATU window not active\n");
        uart_puts("[xHCI] Check: ATU programmed? Memory Space + Bus Master enabled?\n");
        return -1;
    }

    uint8_t cap_len = readb(xhci_base + 0x00);
    uart_puts("[xHCI] Cap length = ");
    print_hex8(cap_len);
    uart_puts("\n");

    /* Valid xHCI cap lengths are 0x10–0x40. VL805 is always 0x20. */
    if (cap_len < 0x10 || cap_len > 0x40) {
        uart_puts("[xHCI] ERROR: implausible cap length\n");
        uart_puts("[xHCI] If 0xFF: VL805 not responding — Memory Space not enabled.\n");
        uart_puts("[xHCI] If other: wrong PCI base in ATU (looping back via inbound DMA window).\n");
        return -1;
    }

    xhci_op = xhci_base + cap_len;
    return 0;
}

static int xhci_reset_controller(void) {
    uart_puts("[xHCI] Resetting controller...\n");

    /* Wait for CNR (Controller Not Ready, bit 11 of USBSTS) to clear */
    for (int t = 100; t > 0; t--) {
        uint32_t sts = readl(xhci_op + 0x04);
        if (sts == 0xFFFFFFFF) { uart_puts("[xHCI] ERROR: USBSTS=0xFFFFFFFF\n"); return -1; }
        if (!(sts & (1U << 11))) break;
        delay_ms(5);
    }

    uint32_t cmd = readl(xhci_op + 0x00);
    cmd |= (1U << 1);   /* HCRST */
    writel(cmd, xhci_op + 0x00);
    asm volatile("dsb sy; isb" ::: "memory");

    for (int t = 100; t > 0; t--) {
        delay_ms(5);
        uint32_t v = readl(xhci_op + 0x00);
        if (v == 0xFFFFFFFF) { uart_puts("[xHCI] ERROR: device vanished\n"); return -1; }
        if (!(v & (1U << 1))) { uart_puts("[xHCI] Reset complete\n"); return 0; }
    }
    uart_puts("[xHCI] ERROR: reset timed out\n");
    return -1;
}

static int xhci_start_controller(void) {
    uart_puts("[xHCI] Starting controller...\n");

    /* Wait for CNR to clear post-reset */
    for (int t = 200; t > 0; t--) {
        uint32_t sts = readl(xhci_op + 0x04);
        if (sts == 0xFFFFFFFF) { uart_puts("[xHCI] ERROR: USBSTS=0xFFFFFFFF\n"); return -1; }
        if (!(sts & (1U << 11))) break;
        delay_ms(5);
    }

    uint32_t cmd = readl(xhci_op + 0x00);
    cmd |= (1U << 0);   /* RS — Run/Stop */
    writel(cmd, xhci_op + 0x00);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(10);

    uint32_t sts = readl(xhci_op + 0x04);
    if (sts & (1U << 0))
        uart_puts("[xHCI] WARNING: HCH still set after RS=1\n");
    else
        uart_puts("[xHCI] Controller running!\n");
    return 0;
}

static void xhci_scan_ports(void) {
    uart_puts("[xHCI] Scanning ports...\n");
    uint32_t hcs1 = readl(xhci_base + 0x04);
    int max_ports = (hcs1 >> 24) & 0xFF;
    if (max_ports == 0 || max_ports > 16) max_ports = 4;

    uart_puts("[xHCI] MaxPorts = ");
    uart_putc('0' + (max_ports % 10));
    uart_puts("\n");

    void *port_base = xhci_op + 0x400;
    for (int p = 0; p < max_ports; p++) {
        uint32_t portsc = readl(port_base + (p * 0x10));
        uart_puts("[xHCI] Port ");
        uart_putc('0' + p);
        uart_puts(": ");
        print_hex32(portsc);
        if (portsc & 1) uart_puts(" CONNECTED");
        if (portsc & 2) uart_puts(" ENABLED");
        uart_puts("\n");
    }
}

/* ── Pi 4 init path (VL805 via PCIe) ───────────────────────────── */

static void pci_init_pi4(void) {
    uart_puts("[PCI] Pi 4 path: VL805 via PCIe\n");

    pcie_base = ioremap(PI4_PCIE_RC_BASE, 0x10000);
    if (!pcie_base) { uart_puts("[PCI] ERROR: ioremap(Pi4 PCIe RC) failed\n"); return; }

    if (pcie_bring_up_link() != 0) return;

    /*
     * Step 1: Read the firmware's pre-existing RC memory window to discover
     * the real VL805 BAR address BEFORE changing anything.
     *
     * start4.elf enumerates the VL805, assigns its BAR a PCI address (typically
     * 0x80000000), and programs the RC root-port memory window to match.
     * After its 'PCI0 reset' + link-up it jumps to our kernel WITHOUT clearing
     * these registers.  The EXT_CFG BAR read always returns 0 (EXT_CFG limitation),
     * but the RC's own memory window at offset 0x20 tells us the real BAR base.
     *
     * Decode: bits[15:4] of the 32-bit register hold PCI base address [31:20].
     * Example: register = 0xbff08000 -> bits[15:4] = 0x800 -> base = 0x80000000.
     *
     * If the window shows base > limit (register = 0x000XXXXX pattern where
     * limit < base) the firmware left it disabled and the real BAR is 0.
     */
    uint32_t fw_win = readl(pcie_base + 0x20);
    uint32_t fw_base_field = (fw_win >> 4) & 0xFFF;       /* PCI base [31:20] */
    uint32_t fw_limit_field = (fw_win >> 20) & 0xFFF;     /* PCI limit [31:20] */
    uint32_t pci_bar = (fw_base_field <= fw_limit_field) ? (fw_base_field << 20) : 0;

    uart_puts("[PCI] FW memory window: "); print_hex32(fw_win);
    uart_puts("  -> VL805 BAR = "); print_hex32(pci_bar); uart_puts("\n");

    /*
     * Step 2: Configure the outbound ATU window.
     *
     * After pcie_bring_up_link() de-asserts PERST# (clears RGR1 bit 0),
     * VL805 undergoes a full power-on reset.  Its BAR0 returns to the PCI
     * spec default of 0x00000000.  The firmware had assigned BAR0=0x80000000
     * (visible in fw_win above), but that assignment is lost on reset.
     *
     * EXT_CFG cannot reprogram BAR0 — it only forwards offsets 0x00 and
     * 0x04 to the downstream device; BAR writes go to the RC and are silently
     * ignored (see file header).  So VL805 BAR0 = 0x00000000 after reset and
     * there is no way to change it via our EXT_CFG mechanism.
     *
     * Therefore WIN0_LO must target PCIe 0x00000000 so that CPU accesses at
     * 0x600000000 generate TLPs addressed to 0x00000000 where VL805 lives.
     *
     * NOTE: WIN0_LO holds a raw PCIe address — no size encoding.  Window size
     * is set by BASE_LIMIT alone.  (brcm_pcie_encode_ibar_size() applies to
     * inbound DMA windows only, not to this outbound register.)
     */
    pcie_setup_outbound_window();   /* SCB_ACCESS_EN + BASE_LIMIT = 0x603F6000 */
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);  /* PCIe base = VL805 BAR0 default */
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] ATU WIN0_LO written=0x00000000  readback=");
    print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO));
    uart_puts("\n");
    uart_puts("[PCI] ATU WIN0_HI=");
    print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI));
    uart_puts("  BASE_LIMIT=");
    print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT));
    uart_puts("  BASE_HI=");
    print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI));
    uart_puts("  LIMIT_HI=");
    print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI));
    uart_puts("\n");

    /*
     * Step 3: VL805 firmware notification (best-effort, board c03112 ignores it).
     * Pi 4B rev 1.2+ stores VL805 firmware in on-chip SPI flash; vl805.bin not needed.
     */
    extern int vl805_init(void);
    vl805_init();

    /*
     * Step 4: Configure the RC root port via direct MMIO writes to pcie_base+0x00xx.
     *
     * The root port's own Type-1 config space lives at pcie_base+0x0000 directly
     * (NOT via EXT_CFG which reaches downstream devices only).
     *
     * A) RC Command (0x04): enable Memory Space + Bus Master on the root port itself.
     *    Without this the root port discards all outbound memory TLPs -> 0xdeaddead.
     *
     * B) Bus numbers (0x18): secondary=1, subordinate=1 enables forwarding of TYPE-1
     *    config TLPs to bus 1 (required for EXT_CFG bus=1 accesses to reach VL805).
     *
     * C) Memory window (0x20): if the firmware already set a valid window (pci_bar != 0)
     *    we trust it and leave it alone.  Otherwise we open a default window.
     */

    /* A) RC Command */
    uint32_t rc_cmd = readl(pcie_base + 0x04);
    uart_puts("[PCI] RC Command before: "); print_hex32(rc_cmd); uart_puts("\n");
    rc_cmd |= (1U << 1) | (1U << 2);
    writel(rc_cmd, pcie_base + 0x04);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC Command after:  "); print_hex32(readl(pcie_base + 0x04)); uart_puts("\n");

    /* B) Bus numbers */
    writel(0x00010100U, pcie_base + 0x18);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(5);
    uart_puts("[PCI] RC buses: "); print_hex32(readl(pcie_base + 0x18)); uart_puts("\n");

    /* C) Memory window — only override if firmware left it disabled */
    if (pci_bar == 0) {
        uart_puts("[PCI] Opening default RC mem window (FW had none)\n");
        writel(0x03F00000U, pcie_base + 0x20);
        writel(0x03F10001U, pcie_base + 0x24);
        writel(0x00000000U, pcie_base + 0x28);
        writel(0x00000000U, pcie_base + 0x2C);
        asm volatile("dsb sy; isb" ::: "memory");
    } else {
        uart_puts("[PCI] Keeping FW RC mem window (covers VL805 BAR)\n");
    }
    uart_puts("[PCI] RC mem win: "); print_hex32(readl(pcie_base + 0x20)); uart_puts("\n");

    /*
     * Step 5: Verify VL805 via EXT_CFG bus=1 (TYPE-1 config TLPs now forwarded).
     */
    vl805_dev.bus = 1; vl805_dev.dev = 0; vl805_dev.func = 0;
    uint32_t id = 0;
    for (int retry = 0; retry < 10; retry++) {
        id = pci_read_config(&vl805_dev, 0x00);
        if ((id & 0xFFFF) == 0x1106 && (id >> 16) == 0x3483) break;
        uart_puts("[PCI] Bus 1 not ready: "); print_hex32(id); uart_puts("\n");
        delay_ms(10);
    }
    uart_puts("[PCI] VL805 ID: "); print_hex32(id); uart_puts("\n");
    if ((id & 0xFFFF) != 0x1106 || (id >> 16) != 0x3483) {
        uart_puts("[PCI] ERROR: VL805 not found on bus 1\n"); return;
    }

    /*
     * Step 6: Enable VL805 Memory Space + Bus Master.
     *
     * VL805 BAR0 = 0x00000000 after PERST# reset (PCI spec default).
     * Our ATU WIN0_LO = 0x00000000, so CPU reads at 0x600000000 generate
     * PCIe TLPs to address 0x00000000 — exactly where VL805 decodes.
     *
     * We do NOT reprogram BAR0 via EXT_CFG because EXT_CFG only forwards
     * offsets 0x00 and 0x04 to the downstream device; BAR writes at 0x10
     * go to the RC and are silently ignored (see file header).
     *
     * All we need is Memory Space + Bus Master set in the Command register
     * (offset 0x04), which EXT_CFG DOES forward reliably.
     */
    uart_puts("[PCI] 1/4 FLR\n");
    vl805_flr(&vl805_dev);

    uart_puts("[PCI] 2/4 Enable VL805 Memory Space + Bus Master\n");
    uint32_t cmd = pci_read_config(&vl805_dev, 0x04);
    uart_puts("[PCI] VL805 Command before: "); print_hex32(cmd); uart_puts("\n");
    cmd |= (1U << 1) | (1U << 2);
    pci_write_config(&vl805_dev, 0x04, cmd);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(20);
    cmd = pci_read_config(&vl805_dev, 0x04);
    uart_puts("[PCI] VL805 Command after:  "); print_hex32(cmd); uart_puts("\n");

    /*
     * Step 7: Map VL805 MMIO.
     * CPU 0x600000000 -> PCI pci_bar (= VL805 BAR base).
     * The xHCI capability registers begin at offset 0 of the BAR.
     */
    uart_puts("[PCI] 3/4 Map MMIO (CPU 0x600000000 = PCI "); print_hex32(pci_bar);
    uart_puts(")\n");
    xhci_base = ioremap(VL805_BAR0_CPU, 0x10000);
    if (!xhci_base) { uart_puts("[PCI] ERROR: ioremap failed\n"); return; }

    /* ── Diagnostic 1: VL805 BAR0 via EXT_CFG ────────────────────────────
     * EXT_CFG only reliably forwards offsets 0x00 and 0x04 for bus=0, but
     * for bus=1 (with bridge buses programmed) it should forward all offsets.
     * Print raw result so we know what PCI address VL805 is actually at.
     * Expected: 0x80000004 (64-bit BAR, base=0x80000000)
     * If 0x00000000: EXT_CFG can't read BAR registers (known limitation)
     * If 0xFFFFFFFF: VL805 not responding to config reads at all
     */
    uint32_t bar0_raw = pci_read_config(&vl805_dev, 0x10);
    uint32_t bar1_raw = pci_read_config(&vl805_dev, 0x14);
    uart_puts("[PCI] VL805 BAR0="); print_hex32(bar0_raw);
    uart_puts("  BAR1="); print_hex32(bar1_raw); uart_puts("\n");
    if (bar0_raw != 0 && bar0_raw != 0xFFFFFFFF) {
        uint32_t actual_bar = bar0_raw & 0xFFFFFFF0U;
        uart_puts("[PCI] VL805 actual PCI base from BAR0="); print_hex32(actual_bar);
        uart_puts("\n");
        if (actual_bar != pci_bar) {
            uart_puts("[PCI] !!! BAR MISMATCH: ATU targets "); print_hex32(pci_bar);
            uart_puts(" but VL805 is at "); print_hex32(actual_bar); uart_puts("\n");
            uart_puts("[PCI] Correcting ATU WIN0_LO to match real BAR\n");
            pci_bar = actual_bar;
            writel(pci_bar,
                   pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
            asm volatile("dsb sy; isb" ::: "memory");
        }
    }

    /* ── Diagnostic 2: RC inbound BAR registers ───────────────────────────
     * If any inbound BAR maps PCIe 0x80000000 -> CPU RAM, the RC captures
     * outbound TLPs to that address as DMA rather than forwarding to VL805.
     * offsets: BAR0=0x402c/0x4030, BAR1=0x4034/0x4038, BAR2=0x403c/0x4040
     */
    uart_puts("[PCI] RC inbound BARs:\n");
    uart_puts("[PCI]   BAR0: LO="); print_hex32(readl(pcie_base + 0x402c));
    uart_puts(" HI="); print_hex32(readl(pcie_base + 0x4030)); uart_puts("\n");
    uart_puts("[PCI]   BAR1: LO="); print_hex32(readl(pcie_base + 0x4034));
    uart_puts(" HI="); print_hex32(readl(pcie_base + 0x4038)); uart_puts("\n");
    uart_puts("[PCI]   BAR2: LO="); print_hex32(readl(pcie_base + 0x403c));
    uart_puts(" HI="); print_hex32(readl(pcie_base + 0x4040)); uart_puts("\n");

    /* ── Diagnostic 3: ATU sweep ──────────────────────────────────────────
     * Try reading MMIO with WIN0_LO at four candidate PCI base addresses.
     * 0xF8000000 is from the bcm2711-rpi-4-b DTS ranges property (the
     * hardware-defined PCIe aperture start). Previous sweeps missed this.
     * Whichever gives non-0xdeaddead is where the VL805 actually lives.
     */
    uart_puts("[PCI] RGR1 at sweep time="); print_hex32(readl(pcie_base + RGR1_SW_INIT_1_OFF)); uart_puts("\n");
    /*
     * ATU sweep: raw PCIe addresses only — no size encoding in WIN0_LO.
     * Window size is set by BASE_LIMIT; WIN0_LO is a plain PCIe base address.
     * Expected winner: 0x00000000 (VL805 BAR0 default after PERST# reset).
     */
    static const uint32_t sweep_bases[] = {
        0x00000000, 0x80000000, 0xC0000000, 0xF8000000
    };
    uint32_t live_bar = 0xFFFFFFFF;
    for (int s = 0; s < 4; s++) {
        writel(sweep_bases[s], pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        asm volatile("dsb sy; isb" ::: "memory");
        delay_ms(5);
        uint32_t v = readl(xhci_base);
        uart_puts("[PCI] ATU sweep WIN0_LO="); print_hex32(sweep_bases[s]);
        uart_puts("  MMIO[0]="); print_hex32(v); uart_puts("\n");
        if (v != 0xdeaddead && v != 0xFFFFFFFF && live_bar == 0xFFFFFFFF)
            live_bar = sweep_bases[s];
    }
    uint32_t use_base = (live_bar != 0xFFFFFFFF) ? live_bar : 0x00000000;
    writel(use_base, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    asm volatile("dsb sy; isb" ::: "memory");
    if (live_bar != 0xFFFFFFFF) {
        uart_puts("[PCI] ATU sweep found VL805 at PCIe="); print_hex32(live_bar); uart_puts("\n");
    } else {
        uart_puts("[PCI] ATU sweep: all candidates dead\n");
    }

    /* Final raw dump */
    uart_puts("[xHCI] Raw @0x600000000 (PCIe base="); print_hex32(use_base); uart_puts("): ");
    for (int i = 0; i < 4; i++) {
        print_hex32(readl(xhci_base + i * 4));
        uart_puts(" ");
    }
    uart_puts("\n");

    uart_puts("[PCI] 4/4 Full xHCI init (§4.2)\n");
    extern int xhci_init(uint64_t base_addr);
    if (xhci_init(VL805_BAR0_CPU) != 0) {
        uart_puts("[PCI] ERROR: xhci_init failed\n");
        return;
    }

    uart_puts("\n╔════════════════════════════════════════╗\n");
    uart_puts("║   xHCI online — USB ready             ║\n");
    uart_puts("╚════════════════════════════════════════╝\n\n");
}

/* ── Pi 5 init path (RP1 built-in xHCI) ────────────────────────── */

static void pci_init_pi5(void) {
    uart_puts("[PCI] Pi 5 path: RP1 built-in xHCI\n");

    /*
     * On Pi 5 the firmware fully initialises the RP1 xHCI controller.
     * Map the MMIO window and verify the controller is alive.
     */
    xhci_base = ioremap(PI5_RP1_XHCI_BASE, 0x100000);
    if (!xhci_base) { uart_puts("[PCI] ERROR: ioremap(RP1 xHCI) failed\n"); return; }

    if (xhci_read_capabilities() != 0) return;

    /* Don't reset — firmware already started it, and a reset would drop
     * any devices the firmware enumerated.  Just scan ports. */
    uart_puts("[PCI] RP1 xHCI firmware-started — skipping reset\n");
    xhci_scan_ports();

    uart_puts("\n╔════════════════════════════════════════╗\n");
    uart_puts("║   xHCI online — USB ready             ║\n");
    uart_puts("╚════════════════════════════════════════╝\n\n");
}

/* ── pci_init — entry point called from kernel_main ────────────── */

void pci_init(void) {
    int model = get_pi_model();

    uart_puts("\n╔════════════════════════════════════════╗\n");
    if (model == 5)
        uart_puts("║   USB Init (Pi 5 / RP1 xHCI)          ║\n");
    else
        uart_puts("║   USB Init (Pi 4 / VL805 xHCI)        ║\n");
    uart_puts("╚════════════════════════════════════════╝\n");

    if (model == 5)
        pci_init_pi5();
    else
        pci_init_pi4();
}

/* ── Stubs ──────────────────────────────────────────────────────── */

void pci_register_driver(pci_driver_t *driver) { (void)driver; }

void pci_enable_busmaster(pci_dev_t *dev) {
    if (!pcie_base) return;
    uint32_t cmd = pci_read_config(dev, 0x04);
    cmd |= (1U << 2);
    pci_write_config(dev, 0x04, cmd);
}

uint64_t pci_bar_start(pci_dev_t *dev, int bar) {
    (void)dev; (void)bar;
    return 0;
}
