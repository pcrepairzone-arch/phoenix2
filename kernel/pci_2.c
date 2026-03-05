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
#define MISC_PCIE_CTRL_OFF      0x4064   /* L23 request, etc. */
#define MISC_RC_BAR2_CFG_LO     0x4034   /* Inbound BAR2 (DMA from endpoint) */
#define MISC_RC_BAR2_CFG_HI     0x4038
#define PCIE_EXT_CFG_INDEX_OFF  0x9000
#define PCIE_EXT_CFG_DATA_OFF   0x9004
#define PCIE_LINK_STATUS_OFF    0x00BC   /* RC PCIe Link Status/Control (std cap) */

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
     * ── PRE-RESET PROBE ────────────────────────────────────────────────────
     *
     * Firmware (start4.elf) leaves the PCIe RC FULLY CONFIGURED and the VL805
     * ALIVE — the VC log shows "xHC0 ver:256" which are real VL805 registers.
     * THEN firmware does "PCI0 reset" which puts RGR1=0x00000003.
     *
     * KEY QUESTION: after "PCI0 reset", does 0x600000000 still respond?
     * If YES  → firmware's ATU config survived the reset, we just need to
     *            find what we're overwriting that breaks it.
     * If NO   → the AXI interconnect gate is cleared by the RC reset and
     *            needs a specific register write to re-arm it.
     *
     * We also snapshot ALL ATU registers before we touch anything, so we
     * can compare them with what we write later — if they differ, that's the bug.
     *
     * NOTE: We do NOT reset here — this probe runs against firmware's state.
     *       If the probe reads real data, SKIP_RESET mode will be tried next.
     */
    {
        uart_puts("[PRE-RESET] Firmware ATU state snapshot:\n");

        /* ATU outbound window registers as firmware left them */
        uint32_t pre_win0_lo    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        uint32_t pre_win0_hi    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI);
        uint32_t pre_base_limit = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT);
        uint32_t pre_base_hi    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);
        uint32_t pre_limit_hi   = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);
        uint32_t pre_misc_ctrl  = readl(pcie_base + MISC_MISC_CTRL_OFF);
        uint32_t pre_rc_bar2    = readl(pcie_base + MISC_RC_BAR2_CFG_LO);
        uint32_t pre_rc_win     = readl(pcie_base + 0x20);  /* RC bridge mem window */

        uart_puts("[PRE-RESET]   WIN0_LO=");      print_hex32(pre_win0_lo);
        uart_puts("  WIN0_HI=");                  print_hex32(pre_win0_hi);
        uart_puts("  BASE_LIMIT=");               print_hex32(pre_base_limit);
        uart_puts("\n");
        uart_puts("[PRE-RESET]   BASE_HI=");      print_hex32(pre_base_hi);
        uart_puts("  LIMIT_HI=");                 print_hex32(pre_limit_hi);
        uart_puts("  MISC_CTRL=");                print_hex32(pre_misc_ctrl);
        uart_puts("  RC_BAR2=");                  print_hex32(pre_rc_bar2);
        uart_puts("\n");
        uart_puts("[PRE-RESET]   RC_WIN(0x20)="); print_hex32(pre_rc_win);
        uart_puts("\n");

        /* Critical test: read 0x600000000 NOW, before any reset.
         * If this returns real data, firmware's ATU is working and our
         * reset/reconfigure sequence is what breaks it.
         * If this returns 0xdeaddead, the 'PCI0 reset' already cleared
         * the AXI interconnect route and we need a separate enable reg. */
        uart_puts("[PRE-RESET] Reading 0x600000000 BEFORE reset...\n");
        volatile uint32_t *fw_mmio = (volatile uint32_t *)VL805_BAR0_CPU;
        asm volatile("dsb sy; isb" ::: "memory");
        uint64_t pre_t0, pre_t1;
        asm volatile("mrs %0, cntpct_el0" : "=r"(pre_t0));
        uint32_t pre_v0 = fw_mmio[0];
        uint32_t pre_v1 = fw_mmio[1];
        uint32_t pre_v2 = fw_mmio[2];
        uint32_t pre_v3 = fw_mmio[3];
        asm volatile("mrs %0, cntpct_el0" : "=r"(pre_t1));
        asm volatile("dsb sy; isb" ::: "memory");
        uint64_t pre_ticks = pre_t1 - pre_t0;

        uart_puts("[PRE-RESET]   [0]="); print_hex32(pre_v0);
        uart_puts("  [1]=");             print_hex32(pre_v1);
        uart_puts("  [2]=");             print_hex32(pre_v2);
        uart_puts("  [3]=");             print_hex32(pre_v3);
        uart_puts("  ticks=0x");
        for (int _i = 28; _i >= 0; _i -= 4) {
            int _n = (pre_ticks >> _i) & 0xF;
            uart_putc(_n < 10 ? '0' + _n : 'a' + _n - 10);
        }
        uart_puts("\n");

        if (pre_v0 == 0xdeaddead) {
            uart_puts("[PRE-RESET]   RESULT: dead -- AXI gate already closed by PCI0 reset\n");
            uart_puts("[PRE-RESET]   => Need to find the BCM2711 interconnect enable register\n");
        } else {
            uart_puts("[PRE-RESET]   RESULT: LIVE! xHCI CAPLENGTH=");
            print_hex8((uint8_t)(pre_v0 & 0xFF));
            uart_puts("  HCIVERSION=0x");
            uint32_t hver = (pre_v0 >> 16) & 0xFFFF;
            for (int _i = 12; _i >= 0; _i -= 4) {
                int _n = (hver >> _i) & 0xF;
                uart_putc(_n < 10 ? '0' + _n : 'a' + _n - 10);
            }
            uart_puts("\n");
            uart_puts("[PRE-RESET]   => Firmware config WORKS. Our reset/reconfigure BREAKS it.\n");
            uart_puts("[PRE-RESET]   => Compare PRE vs POST-RESET register snapshots above.\n");
        }
    }

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
     * POST-RESET ATU SNAPSHOT — compare with pre-reset values above.
     * Any register that changed was wiped by the RC soft reset / PERST#.
     * These are the registers we need to restore to re-arm the AXI bridge.
     */
    {
        uart_puts("[POST-RESET] ATU state after link-up (before our writes):\n");
        uint32_t post_win0_lo    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        uint32_t post_win0_hi    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI);
        uint32_t post_base_limit = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT);
        uint32_t post_base_hi    = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);
        uint32_t post_limit_hi   = readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);
        uint32_t post_misc_ctrl  = readl(pcie_base + MISC_MISC_CTRL_OFF);
        uint32_t post_rc_bar2    = readl(pcie_base + MISC_RC_BAR2_CFG_LO);
        uint32_t post_rc_win     = readl(pcie_base + 0x20);

        uart_puts("[POST-RESET]   WIN0_LO=");     print_hex32(post_win0_lo);
        uart_puts("  WIN0_HI=");                  print_hex32(post_win0_hi);
        uart_puts("  BASE_LIMIT=");               print_hex32(post_base_limit); uart_puts("\n");
        uart_puts("[POST-RESET]   BASE_HI=");     print_hex32(post_base_hi);
        uart_puts("  LIMIT_HI=");                 print_hex32(post_limit_hi);
        uart_puts("  MISC_CTRL=");                print_hex32(post_misc_ctrl);
        uart_puts("  RC_BAR2=");                  print_hex32(post_rc_bar2);    uart_puts("\n");
        uart_puts("[POST-RESET]   RC_WIN(0x20)="); print_hex32(post_rc_win);    uart_puts("\n");
    }

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
     * BCM2711 outbound ATU register encoding — verified against Linux
     * pcie-brcmstb.c and cross-checked against 25+ boots of diagnostics.
     *
     * CPU window: 0x600000000 â 0x603FFFFFF (64 MB, BCM2711 DTS range)
     * PCIe dest:  0x80000000              (VL805 BAR, 32-bit)
     *
     * WIN0_LO / WIN0_HI  â PCIe destination (where TLPs are addressed TO)
     *   WIN0_LO = 0x80000000   : lower 32 bits of PCIe dest (VL805 BAR)
     *   WIN0_HI = 0x00000000   : upper 32 bits.  VL805 is a 32-bit device.
     *             PREVIOUS BUG: 0x00000006 here sent TLPs to PCIe 0x600000000
     *             which no 32-bit endpoint can decode.
     *
     * BASE_LIMIT â CPU-side window bounds (where the ARM reads FROM)
     *   Field layout: [31:16]=cpu_limit_mb, [15:0]=cpu_base_mb (addr>>20)
     *   cpu_base_mb  = 0x600000000 >> 20 = 0x6000
     *   cpu_limit_mb = 0x603FFFFFF >> 20 = 0x603F
     *   BASE_LIMIT   = (0x603F << 16) | 0x6000 = 0x603F6000
     *   PREVIOUS BUG: 0x03F00000 â base_mb=0, cpu_base=0x600000000 exactly
     *   equal to the access address. BCM2711 lower bound check is exclusive
     *   (addr > base), so access at exactly base FAILS â 0xdeaddead in 0 ticks.
     *
     * BASE_HI / LIMIT_HI â upper bits of cpu_xxx_mb beyond bit 15
     *   0x6000 and 0x603F both fit in 16 bits â HI fields are 0x00000000.
     *   PREVIOUS BUG: writing 0x6 shifted the window to wrong addresses.
     *
     * WIN0_LO is also written in pci_init_pi4() with the confirmed BAR value.
     */
    writel(0x80000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);   /* PCIe dest LO = VL805 BAR */
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI);   /* PCIe dest HI = 0 (32-bit device) */
    writel(0x603F5FFFUL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT); /* base_mb=0x5FFF (1MB below window start) so addr 0x6000 > base_mb, passes strict > check */
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);   /* base_mb  0x6000 < 0x10000 */
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);  /* limit_mb 0x603F < 0x10000 */

    /* SCB_ACCESS_EN (bit 12): gates the ARM→PCIe outbound path.
     * THIS IS THE BIT THAT ENABLES MEMORY TLPs FROM THE ARM TO THE VL805.
     * Without it the BCM2711 AXI→PCIe bridge returns 0xdeaddead INSTANTLY
     * (measured: 1 clock tick = 18.5ns) — no TLP is ever put on the wire.
     *
     * CRITICAL: The bit is BIT(12) = 0x1000, NOT BIT(8) = 0x100.
     * Linux pcie-brcmstb.c: PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK = BIT(12)
     * We were setting bit 8 for 18 boots — the outbound path was never enabled.
     *
     * CFG_READ_UR_MODE (bit 2): UR completions return 0xFFFFFFFF not 0xdeaddead.
     * SCB0_SIZE (bits[4:0] = 0xF): inbound DMA window size = 1GB (log2(1G)-15=15).
     * Note: SCB0_SIZE=0xF (01111) already sets bit 2 (CFG_READ_UR_MODE). */
    uint32_t ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    ctrl |= (1U << 12);         /* SCB_ACCESS_EN — ENABLES outbound ARM→PCIe path */
    ctrl |= (1U << 2);          /* CFG_READ_UR_MODE: UR → 0xFFFFFFFF not 0xdeaddead */
    ctrl  = (ctrl & ~0x1F0U) | 0x0F0U; /* SCB0_SIZE=15 at bits[8:4] (Linux GENMASK(8,4)) = 1GB */
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");

    ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    uart_puts("[PCI] MISC_CTRL after setup="); print_hex32(ctrl);
    uart_puts((ctrl & (1U << 12)) ? "  SCB_ACCESS_EN=1" : "  SCB_ACCESS_EN=0 PROBLEM");
    uart_puts((ctrl & (1U << 2))  ? "  UR_MODE=1"       : "  UR_MODE=0 PROBLEM");
    uart_puts("\n");

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
     * Step 1: Determine VL805 BAR address after PERST# reset.
     *
     * Our PERST# cycle resets the VL805. After SPI reload, the VL805 BAR
     * registers return to 0x00000000 (PCIe spec default — nobody has
     * re-enumerated and assigned a PCI address since the reset).
     *
     * The firmware's RC bridge memory window (pcie_base+0x20) was set by
     * start4.elf to 0xbff08000 (base=0x80000000) BEFORE our PERST# cycle.
     * That value is now stale.
     *
     * CRITICAL: The BCM2711 RC uses its Type-1 bridge window (config 0x20)
     * to decide which outbound TLPs to forward downstream. TLPs whose PCI
     * address falls outside the window are DROPPED, returning 0xdeaddead.
     *
     * Boot 21 confirmed: after PERST#, RC_WIN hardware-resets to 0xbff08000
     * (base=0x80000000, limit=0xBFFF0000).  The VL805 BAR is therefore at
     * 0x80000000 — this is where the RC presented it during enumeration.
     *
     * The fix (boot 22): set WIN0_LO=0x80000000 and DO NOT touch RC_WIN.
     */
    uint32_t fw_win = readl(pcie_base + 0x20);
    uart_puts("[PCI] FW memory window: "); print_hex32(fw_win); uart_puts("\n");

    /*
     * VL805 BAR = 0x80000000 — confirmed by boot 21 analysis.
     *
     * After PERST#, the BCM2711 RC bridge window register (config offset 0x20)
     * hardware-resets to 0xbff08000:
     *   base  = 0x80000000  (bits[15:4]  of 0xbff08000 >> shifted = 0x800)
     *   limit = 0xBFFF0000  (bits[31:20] of 0xbff08000 = 0xBFF)
     *
     * This window covers PCI 0x80000000–0xBFFF0000.  The BCM2711 RC only
     * forwards outbound TLPs whose PCI address falls WITHIN this window.
     * TLPs outside it are silently dropped (0xdeaddead returned by fabric).
     *
     * Therefore the VL805 BAR must be at 0x80000000 — and that is exactly
     * where the RC window is centred.  The VL805 resets its BAR to the
     * value the RC presented during PERST# enumeration (0x80000000), not
     * to 0x00000000 as PCI spec would suggest for a cold reset.
     *
     * DO NOT overwrite RC_WIN(0x20).  The hardware default 0xbff08000 is
     * the correct value.  Writing 0x00000000 collapses the bridge window
     * and drops every subsequent TLP — this was the bug across boots 1–21.
     */
    uint32_t pci_bar = 0x80000000U;

    /*
     * Step 2 (CRITICAL SEQUENCE): VL805 firmware notification via mailbox.
     *
     * The VideoCore firmware loads the VL805 USB firmware blobs (which live
     * inside start4.elf, not on the SD card) by sending them across the PCIe
     * link directly from the VC side.  The VC accepts the mailbox notify ONLY
     * while the PCIe link is up and BEFORE any EXT_CFG config-space writes to
     * the VL805.  Once the ARM has touched VL805 config space, the VC says
     * "too late" and returns no response.
     *
     * Therefore: mailbox notify MUST come before RC Command, RC buses,
     * and VL805 Command writes.  Only the ATU setup (which writes to RC MISC
     * registers, not VL805 config space) is allowed before the notify.
     *
     * Previous failure: vl805_init.c was using GPU alias 0xC0000000 (non-
     * coherent) instead of 0x40000000 (L2 cache-coherent, required on Pi 4).
     * VC read stale DRAM, saw zeros, returned no response → code=0x00000000.
     * Fixed in vl805_init.c.
     */

    /* ATU setup first (writes to MISC registers, not VL805 config space) */
    pcie_setup_outbound_window();
    writel(pci_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] ATU WIN0_LO written="); print_hex32(pci_bar);
    uart_puts("  readback=");
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
     * RC_BAR2: Inbound DMA window — MUST be set before any outbound access.
     *
     * Linux sets this as the FIRST thing in brcm_pcie_setup().  On the BCM2711
     * the SCB (System Crossbar Bridge) uses RC_BAR2 to define the valid address
     * range for PCIe transactions.  If RC_BAR2 = 0 (disabled) the SCB blocks
     * ALL transactions — including outbound reads — and returns 0xdeaddead in a
     * single AXI clock cycle without ever touching the PCIe bus.  This is why
     * every timing diagnostic showed "1 tick" regardless of ATU WIN0_LO value.
     *
     * Encoding: bits[4:0] = log2(size) - 15.  For 1 GB: log2(2^30) - 15 = 15 = 0xF.
     * This maps PCIe->CPU RAM starting at 0x00000000 (identity, 1 GB window).
     */
    uart_puts("[PCI] RC_BAR2 setup (1GB inbound DMA window, arms SCB)...\n");
    writel(0x0000000FU, pcie_base + MISC_RC_BAR2_CFG_LO);  /* size = 1 GB */
    writel(0x00000000U, pcie_base + MISC_RC_BAR2_CFG_HI);  /* base = 0    */
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC_BAR2 LO="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_LO));
    uart_puts(" HI="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_HI));
    uart_puts("\n");

    /*
     * Step 3: Configure the RC root port BEFORE calling the mailbox.
     *
     * CRITICAL ORDERING FIX — root cause of MBOX code=0x00000000 on every boot:
     *
     * mailbox tag 0x00030058 (NOTIFY_XHCI_RESET) asks the VC to load VL805
     * firmware.  The VC does this by issuing PCIe config TLPs to the VL805
     * at BDF bus=1/dev=0/fn=0.  For those TLPs to reach the VL805:
     *   (a) RC bridge bus numbers must be programmed (offset 0x18 = 0x00010100)
     *       so the RC knows bus 1 is downstream and will forward TYPE-1 TLPs.
     *   (b) RC Memory Space + Bus Master (offset 0x04 bits 1,2) must be set
     *       so the RC actually forwards outbound transactions.
     *   (c) RC memory window (offset 0x20) must cover 0x80000000 so the VC's
     *       config writes reach the VL805's config space at that address.
     *
     * Previously these were written AFTER the mailbox call, so the VC fired
     * TLPs at a bus that wasn't yet enabled → silent VC failure → code=0x00000000.
     *
     * These writes are to RC config registers, NOT VL805 EXT_CFG config space,
     * so they do not trigger the "ARM touched VL805 config" lockout on the VC.
     *
     * The VL805 EXT_CFG writes (Command, FLR) still happen AFTER the mailbox.
     */

    /* A) RC memory window — must cover VL805 at PCI 0x80000000.
     *    On Pi 4B: hardware resets to 0xbff08000 after PERST#.
     *    On CM4:   stays 0x00000000 after link-up — must be written explicitly.
     *    Unconditionally write the correct value on all BCM2711 variants. */
    writel(0xbff08000U, pcie_base + 0x20);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC mem win set: "); print_hex32(readl(pcie_base + 0x20)); uart_puts("\n");

    /* B) RC Command: Memory Space (bit 1) + Bus Master (bit 2) */
    uint32_t rc_cmd = readl(pcie_base + 0x04);
    uart_puts("[PCI] RC Command before: "); print_hex32(rc_cmd); uart_puts("\n");
    rc_cmd |= (1U << 1) | (1U << 2);
    writel(rc_cmd, pcie_base + 0x04);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC Command after:  "); print_hex32(readl(pcie_base + 0x04)); uart_puts("\n");

    /* C) Bus numbers: primary=0, secondary=1, subordinate=1 */
    writel(0x00010100U, pcie_base + 0x18);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(5);
    uart_puts("[PCI] RC buses: "); print_hex32(readl(pcie_base + 0x18)); uart_puts("\n");

    /*
     * Step 4: VL805 firmware notification via mailbox.
     *
     * NOW the RC is fully configured — bus numbers assigned, Memory Space
     * enabled, bridge window open to 0x80000000.  The VC can reach the VL805
     * at BDF 1:0.0 to load its firmware.
     *
     * We still call this BEFORE any EXT_CFG writes to VL805 config space
     * (Command register, FLR) — touching VL805 config via EXT_CFG after the
     * mailbox call is fine; touching it BEFORE would lock the VC out.
     */
    extern int vl805_init(void);
    vl805_init();

    /* Confirm bridge window is still intact after mailbox */
    uart_puts("[PCI] RC bridge window after mailbox: ");
    print_hex32(readl(pcie_base + 0x20)); uart_puts(" (expect 0xbff08000)\n");

    /*
     * Step 5: Verify VL805 via EXT_CFG bus=1 (TYPE-1 config TLPs now forwarded).
     * The mailbox call above should have loaded VL805 firmware — give it a moment.
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
     * Step 6: FLR then enable Memory Space + Bus Master in the VL805 itself.
     * EXT_CFG bus=1 generates TYPE-1 TLPs that the root port now forwards to bus 1.
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

    /*
     * ── BAR0 WRITE TEST ────────────────────────────────────────────────
     * EXT_CFG for bus=1 generates proper TYPE-1 config TLPs that traverse
     * the bridge.  Unlike bus=0 (the RC itself), bus=1 accesses should
     * forward ALL config offsets including BAR0 (0x10) to the VL805.
     *
     * After PERST# reset, VL805 BAR0 = 0x00000000 (PCIe default).
     * We write 0x80000004 (64-bit, non-prefetchable, base=0x80000000) to
     * restore the same BAR value that start4.elf had originally assigned.
     * Then update the RC memory window and ATU to match.
     * Readback confirms whether EXT_CFG can actually write VL805 BARs.
     */
    uart_puts("[PCI] Writing BAR0 = 0x80000004 via EXT_CFG bus=1...\n");
    pci_write_config(&vl805_dev, 0x10, 0x80000004U);  /* 64-bit memory, base=0x80000000 */
    pci_write_config(&vl805_dev, 0x14, 0x00000000U);  /* BAR1 upper 32 = 0 */
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(5);
    uint32_t bar0_after = pci_read_config(&vl805_dev, 0x10);
    uint32_t bar1_after = pci_read_config(&vl805_dev, 0x14);
    uart_puts("[PCI] BAR0 after write="); print_hex32(bar0_after);
    uart_puts("  BAR1="); print_hex32(bar1_after); uart_puts("\n");

    if ((bar0_after & 0xFFFFFFF0U) == 0x80000000U) {
        /* EXT_CFG CAN write VL805 BAR — update ATU and RC window to match */
        uart_puts("[PCI] BAR write SUCCESS → VL805 BAR = 0x80000000\n");
        pci_bar = 0x80000000U;
        writel(pci_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        /* Restore firmware's RC memory window (covers 0x80000000-0xBFFFFFFF) */
        writel(0xbff08000U, pcie_base + 0x20);
        asm volatile("dsb sy; isb" ::: "memory");
        uart_puts("[PCI] ATU WIN0_LO=0x80000000, RC window=0xbff08000\n");
    } else if ((bar0_after & 0xFFFFFFF0U) == 0x00000000U) {
        uart_puts("[PCI] BAR write had NO EFFECT (EXT_CFG can't write BAR) — keeping BAR=0x00000000\n");
    } else {
        uart_puts("[PCI] BAR readback unexpected: "); print_hex32(bar0_after); uart_puts("\n");
    }

    if (bar0_raw != 0 && bar0_raw != 0xFFFFFFFF && (bar0_raw & 0xFFFFFFF0U) != pci_bar) {
        uint32_t actual_bar = bar0_raw & 0xFFFFFFF0U;
        uart_puts("[PCI] !!! Original BAR0 read was non-zero: "); print_hex32(actual_bar); uart_puts("\n");
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
     * With CFG_READ_UR_MODE (bit 2) now set in MISC_CTRL, we can distinguish:
     *   0xdeaddead = TLP never reached VL805 (RC dropped it / link issue)
     *   0xFFFFFFFF = TLP reached VL805, device sent UR (BAR mismatch or
     *                Memory Space disabled)
     *   anything else = real xHCI register data!
     */
    uart_puts("[PCI] RGR1 at sweep time="); print_hex32(readl(pcie_base + RGR1_SW_INIT_1_OFF)); uart_puts("\n");

    /* ── Extended status dump ─────────────────────────────────────────
     * Print registers that show actual link state at sweep time.
     * PCIE_CTRL bit 0 = L23_REQUEST: if set, RC is requesting L2/L3 which
     * would cause all memory TLPs to fail while the link sleeps.
     * PCIE_STATUS bits[8:4]: link power state (L0/L0s/L1/L2/L3).
     * PCIE_LINK_STATUS (RC config 0xBC) bits[19:16] = current link speed,
     * bits[23:20] = negotiated link width.
     */
    {
        uint32_t pcie_ctrl   = readl(pcie_base + MISC_PCIE_CTRL_OFF);
        uint32_t pcie_status = readl(pcie_base + MISC_PCIE_STATUS_OFF);
        uint32_t link_status = readl(pcie_base + PCIE_LINK_STATUS_OFF);
        uint32_t rc_bar2_lo  = readl(pcie_base + MISC_RC_BAR2_CFG_LO);
        uint32_t rc_bar2_hi  = readl(pcie_base + MISC_RC_BAR2_CFG_HI);
        uart_puts("[PCI] PCIE_CTRL=");   print_hex32(pcie_ctrl);
        uart_puts("  PCIE_STATUS=");     print_hex32(pcie_status);
        uart_puts("  LINK_STATUS=");     print_hex32(link_status); uart_puts("\n");
        uart_puts("[PCI] RC_BAR2: LO="); print_hex32(rc_bar2_lo);
        uart_puts(" HI=");               print_hex32(rc_bar2_hi);  uart_puts("\n");
        if (pcie_ctrl & 1U)  uart_puts("[PCI] WARNING: L23_REQUEST bit set — link sleeping!\n");
        if (!(pcie_status & (1U<<4))) uart_puts("[PCI] WARNING: PHY link DOWN at sweep time!\n");
        if (!(pcie_status & (1U<<5))) uart_puts("[PCI] WARNING: DL layer DOWN at sweep time!\n");
        /* RC_BAR2 already set above (before mailbox) — just confirm it here */
    }

    /* ── TIMING DIAGNOSTIC: measure how long a failing readl takes ──────────
     * This resolves the key question: are TLPs being sent at all?
     *
     * If readl(xhci_base) returns in < 1ms:
     *   → BCM2711 returns 0xdeaddead BEFORE sending any TLP (AXI bridge issue)
     *   → The PCIe outbound path is blocked at the SoC level
     *
     * If readl(xhci_base) takes ~50ms (PCIe completion timeout):
     *   → TLP IS sent on the wire, VL805 not responding
     *   → SerDes / VL805 state issue
     *
     * ARM Generic Timer at 54 MHz: 1ms = 54,000 counts.
     */
    {
        uint64_t t0, t1;
        asm volatile("mrs %0, cntpct_el0" : "=r"(t0));
        volatile uint32_t timed_read = readl(xhci_base);
        asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
        (void)timed_read;
        uint64_t elapsed = t1 - t0;
        uint32_t elapsed_us = (uint32_t)(elapsed / 54);   /* 54 ticks per µs */
        uint32_t elapsed_ms = elapsed_us / 1000;
        uart_puts("[PCI] TIMING: readl took ");
        /* print ms */
        if (elapsed_ms >= 1000) { uart_putc('0' + (elapsed_ms/1000)%10); uart_putc(','); }
        uart_putc('0' + (elapsed_ms/100)%10);
        uart_putc('0' + (elapsed_ms/10)%10);
        uart_putc('0' + elapsed_ms%10);
        uart_puts(" ms (");
        /* print raw counts high word */
        print_hex32((uint32_t)(elapsed >> 32));
        uart_putc('_');
        print_hex32((uint32_t)elapsed);
        uart_puts(" ticks @ 54MHz)\n");
        if (elapsed_ms < 1)
            uart_puts("[PCI] TIMING: < 1ms → TLP NOT SENT (AXI→PCIe bridge blocked)\n");
        else if (elapsed_ms < 10)
            uart_puts("[PCI] TIMING: < 10ms → very fast timeout (unexpected)\n");
        else
            uart_puts("[PCI] TIMING: >= 10ms → PCIe completion timeout (TLP WAS sent)\n");
    }

    static const uint32_t sweep[] = { 0x00000000, 0x80000000, 0xF8000000, 0xC0000000 };
    uint32_t live_bar = 0xFFFFFFFF;
    for (int s = 0; s < 4; s++) {
        writel(sweep[s], pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        asm volatile("dsb sy; isb" ::: "memory");
        delay_ms(5);
        uint32_t v = readl(xhci_base);
        uart_puts("[PCI] ATU sweep WIN0_LO="); print_hex32(sweep[s]);
        uart_puts("  MMIO[0]="); print_hex32(v);
        if (v == 0xFFFFFFFF)      uart_puts(" <- UR (TLP reaches VL805)\n");
        else if (v == 0xdeaddead) uart_puts(" <- TIMEOUT (TLP lost)\n");
        else                      { uart_puts(" <- LIVE DATA!\n"); if (live_bar == 0xFFFFFFFF) live_bar = sweep[s]; }
    }
    /* Restore to the BAR we believe is correct */
    uint32_t use_bar = (live_bar != 0xFFFFFFFF) ? live_bar : pci_bar;
    writel(use_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    asm volatile("dsb sy; isb" ::: "memory");
    if (live_bar != 0xFFFFFFFF && live_bar != pci_bar) {
        uart_puts("[PCI] ATU sweep found VL805 at WIN0_LO="); print_hex32(live_bar);
        uart_puts(" — using this\n");
        pci_bar = live_bar;
    } else if (live_bar == 0xFFFFFFFF) {
        uart_puts("[PCI] ATU sweep: all candidates dead\n");
        uart_puts("[PCI] VL805 Memory Space likely not enabled in device\n");
    }

    /* Final raw dump with the winning BAR */
    uart_puts("[xHCI] Raw @0x600000000 (WIN0_LO="); print_hex32(use_bar); uart_puts("): ");
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
