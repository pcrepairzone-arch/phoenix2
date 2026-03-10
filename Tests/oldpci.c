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
 *    PCIE_EXT_CFG_INDEX  (RC+0x9000): selects target BDF + dword offset
 *    PCIE_EXT_CFG_DATA   (RC+0x8000): fixed 4KB config space window base
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
#define PCIE_EXT_CFG_INDEX_OFF  0x9000   /* selects target BDF + offset */
#define PCIE_EXT_CFG_DATA_OFF   0x8000   /* 4KB config window — fixed base, NOT sliding */
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
/*
 * EXT_CFG config access — matches Linux pcie-brcmstb.c brcm_pcie_map_bus():
 *   INDEX = bus<<20 | dev<<15 | fn<<12 | (offset & ~3)
 *   DATA  = EXT_CFG_DATA_BASE + (offset & ~3)   ← fixed base, full offset
 *
 * The full dword-aligned offset goes into BOTH the INDEX register AND is
 * added to the fixed DATA base (RC+0x8000).  The INDEX selects which device
 * and which dword; DATA+offset is where the actual read/write lands.
 * This is NOT a sliding window — DATA base is always RC+0x8000.
 */
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset) {
    uint32_t aligned = offset & ~3U;
    uint32_t idx = ((uint32_t)dev->bus  << 20) |
                   ((uint32_t)dev->dev  << 15) |
                   ((uint32_t)dev->func << 12) |
                   aligned;
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    return readl(pcie_base + PCIE_EXT_CFG_DATA_OFF + aligned);
}

void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value) {
    uint32_t aligned = offset & ~3U;
    uint32_t idx = ((uint32_t)dev->bus  << 20) |
                   ((uint32_t)dev->dev  << 15) |
                   ((uint32_t)dev->func << 12) |
                   aligned;
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    writel(value, pcie_base + PCIE_EXT_CFG_DATA_OFF + aligned);
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
     * SKIP-RESET DECISION
     *
     * start4.elf initialises the VL805 (loads firmware, enables xHCI) then
     * does "PCI0 reset" (RGR1=0x3) before handing off to our kernel.
     * This PCI0 reset clears the ATU registers (WIN0_LO, MISC_CTRL, etc.)
     * but does NOT wipe the VL805's own firmware or its BAR assignment.
     *
     * If RC_WIN (offset 0x20) is non-zero, firmware completed full PCIe
     * init and the VL805 has its BAR programmed.  We can skip PERST# and
     * go straight to ATU re-arm, avoiding the fatal combination of:
     *   (a) PERST# wiping VL805 firmware from SRAM
     *   (b) vl805.bin absent from SD → mailbox tag 0x00030058 fails
     *   (c) VL805 stuck at bare-metal reset, xHCI unresponsive
     *
     * PCIE_STATUS bits 4+5 (pcie_link_up + pcie_dl_up) survive start4.elf's
     * "PCI0 reset".  If both set at entry: VL805 is alive, BAR assigned by
     * the VC — skip PERST# and mailbox entirely.
     * If link is down: cold boot (CM4 rpiboot etc) — do full PERST# + mailbox.
     *
     * RC_WIN (offset 0x20) is NOT a reliable signal: start4.elf's PCI0 reset
     * clears it to 0 before handoff regardless of board or firmware state.
     */
    uint32_t entry_status = readl(pcie_base + MISC_PCIE_STATUS_OFF);
    int link_up_at_entry  = (entry_status & (1U << 4)) && (entry_status & (1U << 5));
    uint32_t fw_rc_win    = readl(pcie_base + 0x20);  /* logging only */
    uart_puts("[PCI] PCIE_STATUS at entry="); print_hex32(entry_status);
    uart_puts(link_up_at_entry
        ? "  -> link UP — firmware left VL805 running, SKIPPING PERST#\n"
        : "  -> link DOWN — cold boot, doing PERST# reset\n");
    uart_puts("[PCI] RC_WIN at entry="); print_hex32(fw_rc_win); uart_puts("\n");

    if (!link_up_at_entry) {
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
    }

    if (!link_up_at_entry) {
        /* Step 3: wait for PCIe link layer to come up after PERST# (~650 ms) */
        uint32_t reg;
        int link_ms = 0;
        for (int t = 200; t > 0; t--) {
            delay_ms(10);
            link_ms += 10;
            reg = readl(pcie_base + MISC_PCIE_STATUS_OFF);
            if ((reg & (1U << 4)) && (reg & (1U << 5))) {
                uart_puts("[PCI]   LINK UP after ");
                if (link_ms >= 100) uart_putc('0' + link_ms / 100);
                uart_putc('0' + (link_ms / 10) % 10);
                uart_putc('0' + link_ms % 10);
                uart_puts(" ms\n");
                break;
            }
            if (t == 1) { uart_puts("[PCI]   LINK FAILED\n"); return -1; }
        }

        /* Minimum wait after link-up — actual SPI-ready polling happens later
         * in pci_init_pi4() once EXT_CFG is armed and we can read VL805 Command. */
        uart_puts("[PCI]   Waiting 100ms minimum after link-up...\n");
        delay_ms(100);

        uart_puts("[PCI] RGR1 post-link=");
        print_hex32(readl(pcie_base + RGR1_SW_INIT_1_OFF));
        uart_puts("\n");
    } else {
        uart_puts("[PCI]   Link already up — no wait needed\n");
    }

    /*
     * POST-RESET ATU SNAPSHOT — only meaningful after PERST#.
     * Shows what the hardware reset-defaults to before our writes.
     */
    if (!link_up_at_entry) {
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

    /* Return 2 = skip-reset (firmware already inited VL805, no PERST# done)
     *        1 = did PERST# reset (cold boot, need mailbox to reload firmware) */
    return link_up_at_entry ? 2 : 1;
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
     * CRITICAL ORDERING: Enable SCB_ACCESS_EN (MISC_CTRL bit 12) BEFORE
     * writing any WIN0 registers.
     *
     * On BCM2711 the MISC_CPU_2_PCIE_MEM_WIN0_* registers are guarded by
     * the SCB (System Crossbar Bridge) access enable gate.  Writing them
     * with SCB_ACCESS_EN=0 causes an AXI SLVERR on the write bus, which
     * triggers a synchronous data abort.  With no exception vectors
     * installed the CPU hangs silently — exactly what we see on CM4.
     *
     * Linux pcie-brcmstb.c writes MISC_CTRL first, then WIN0.  We must
     * do the same.
     */
    uart_puts("[PCI] ATU: MISC_CTRL before="); print_hex32(readl(pcie_base + MISC_MISC_CTRL_OFF)); uart_puts("\n");
    uint32_t ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    ctrl |= (1U << 12);         /* SCB_ACCESS_EN — gate must open first */
    ctrl |= (1U << 2);          /* CFG_READ_UR_MODE */
    /* SCB0_SIZE in bits[8:4]: encoding is ilog2(size) - 12.
     * For 1GB (2^30): 30 - 12 = 18 = 0x12  → field value 0x120
     * (Previous value of 15 = 0xF gave only 2^(15+12) = 128MB — our DMA
     *  buffer at offset 0x080b5000 ≈ 128MB sits just outside that window.) */
    ctrl  = (ctrl & ~0x1F0U) | 0x120U; /* SCB0_SIZE=18 = 1GB inbound */
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] ATU: MISC_CTRL after ="); print_hex32(readl(pcie_base + MISC_MISC_CTRL_OFF)); uart_puts("\n");

    /*
     * Now safe to write the CPU-side window registers.
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



     *
     * WIN0_LO is also written in pci_init_pi4() with the confirmed BAR value.
     */
    /*
     * Now safe to write the CPU-side window registers.
     *
     * BCM2711 Pi 4B DTS outbound window (from bcm2711-rpi-4-b.dts ranges):
     *   CPU  address: 0x600000000  (34-bit)
     *   PCI  address: 0xC0000000   (32-bit, VL805 BAR assigned here by firmware)
     *   Size:         0x40000000   (1 GB)
     *
     * WIN0_LO / WIN0_HI — PCI destination address (where TLPs are sent TO):
     *   WIN0_LO = 0xC0000000  lower 32 bits of PCI dest
     *   WIN0_HI = 0x00000000  upper 8 bits — VL805 is 32-bit, upper = 0
     *
     * BASE_LIMIT — CPU source window bounds, PCI bridge register format:
     *   bits[31:20] = limit[31:20] i.e. (cpu_limit >> 20) & 0xFFF
     *   bits[15:4]  = base[31:20]  i.e. (cpu_base  >> 20) & 0xFFF
     *   cpu_base  = 0x600000000: (0x600000000 >> 20) & 0xFFF = 0x000
     *   cpu_limit = 0x63FFFFFFF: (0x63FFFFFFF >> 20) & 0xFFF = 0x3FF
     *   BASE_LIMIT = (0x3FF << 20) | (0x000 << 4) = 0x3FF00000
     *   NOTE: the upper 4 bits of the MB number (the "0x6" prefix) go into
     *   BASE_HI / LIMIT_HI — they do NOT fit in BASE_LIMIT's 12-bit fields.
     *
     * BASE_HI / LIMIT_HI — upper 32 bits of the 40-bit CPU address:
     *   cpu_addr >> 32 = 0x600000000 >> 32 = 0x6
     *   Both BASE_HI and LIMIT_HI = 0x00000006
     *   These registers read back as 0 (write-only / read-as-zero on BCM2711)
     *   but the written value IS used by the ATU hardware.
     */
    uart_puts("[PCI] ATU: writing WIN0_LO (PCI dest = 0xC0000000)...\n");
    writel(0xC0000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] ATU: WIN0_LO="); print_hex32(readl(pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO)); uart_puts("\n");

    uart_puts("[PCI] ATU: writing WIN0_HI (0x0 — PCI dest is 32-bit)...\n");
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_HI);
    asm volatile("dsb sy; isb" ::: "memory");

    uart_puts("[PCI] ATU: writing BASE_LIMIT (0x3FF00000)...\n");
    writel(0x3FF00000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT);
    asm volatile("dsb sy; isb" ::: "memory");

    uart_puts("[PCI] ATU: writing BASE_HI/LIMIT_HI (0x6 = upper32 of 0x600000000)...\n");
    /*
     * BASE_HI / LIMIT_HI = cpu_addr >> 32 = 0x6.
     * These read back as 0 on BCM2711 (write-only registers) but are
     * functionally active — without them the ATU window is at 0x000000000
     * and every access to 0x600000000 gets a fabric abort.
     */
    writel(0x00000006UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);
    writel(0x00000006UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);
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

    int link_result = pcie_bring_up_link();
    if (link_result <= 0) return;
    int fw_already_inited = (link_result == 2);

    /*
     * Step 1: Determine VL805 BAR address.
     *
     * Skip-reset path (fw_already_inited=1):
     *   start4.elf ran, assigned VL805 BAR, left RC_WIN=0xbff08000.
     *   RC_WIN encodes PCI base in bits[15:4]: 0xbff08000 → base=0x80000000.
     *   Use pci_bar=0x80000000 and keep RC_WIN as-is.
     *
     * PERST# reset path (fw_already_inited=0):
     *   VL805 was PERST# reset, its BAR reverted to 0x80000000 (the value
     *   the RC had been presenting it at). RC_WIN hardware-default = 0xbff08000.
     *   Same pci_bar=0x80000000.
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
    /*
     * VL805 PCI BAR address = 0xC0000000.
     * This is what the Pi 4B DTS specifies (CPU 0x600000000 ↔ PCI 0xC0000000)
     * and what Linux/firmware assigns.  The RC bridge window and ATU WIN0_LO
     * both use this value.
     */
    uint32_t pci_bar = 0xC0000000U;

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
     * RC_BAR2: Inbound DMA window.
     *
     * BCM2711 dma-ranges: PCIe addr 0xC0000000 → CPU addr 0x0, size 1GB.
     * All DMA addresses the VL805 uses must be CPU_phys + 0xC0000000
     * (handled in usb_xhci.c via phys_to_dma()).
     *
     * ENCODING: RC_BAR2_LO = base_addr | size_mask  (NOT a log2 field)
     *   base  = 0xC0000000  (PCIe start of inbound window)
     *   mask  = size - 1    = 0x40000000 - 1 = 0x3FFFFFFF  (1GB)
     *   LO    = 0xC0000000 | 0x3FFFFFFF = 0xFFFFFFFF
     *   HI    = upper 32 bits of base = 0x0
     *
     * From Linux brcm_pcie.c pcie_set_rc_bar2():
     *   writel(lower_32_bits(mask) | lower_32_bits(dma_addr), RC_BAR2_LO)
     *   writel(upper_32_bits(dma_addr), RC_BAR2_HI)
     */
    uart_puts("[PCI] RC_BAR2 setup (1GB inbound DMA, PCIe base 0xC0000000)...\n");
    writel(0xFFFFFFFFU, pcie_base + MISC_RC_BAR2_CFG_LO);  /* 0xC0000000 | 0x3FFFFFFF */
    writel(0x00000000U, pcie_base + MISC_RC_BAR2_CFG_HI);
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

    /* A) RC memory window.
     * After PERST# + SPI auto-load the VL805 BAR = 0x00000000 (reset default).
     * We cannot assign a BAR via EXT_CFG (silently dropped by BCM2711).
     * Instead we match the ATU and RC window to BAR=0:
     *   WIN0_LO = 0x00000000  (TLPs sent to PCI 0x00000000 = VL805 BAR)
     *   RC window covers PCI 0x00000000–0x0FFFFFFF
     */
    writel(0xFFF0C000U, pcie_base + 0x20);
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

    /* Initialise vl805_dev BDF — bus=1 dev=0 fn=0 per DTB usb@0,0 node */
    vl805_dev.bus = 1; vl805_dev.dev = 0; vl805_dev.func = 0;

    /*
     * Step 4a: Poll VL805 Command register until SPI firmware load completes.
     *
     * After PERST# the VL805 autonomously loads firmware from its SPI flash.
     * Firmware sets bit 20 of the Command register (0x00100000) when ready.
     * Until that bit is set, BAR writes are silently dropped and MMIO reads
     * return 0xdeaddead.  A fixed 500ms wait is not reliable across all boots.
     *
     * Poll up to 2 seconds in 10ms increments.  On a warm board ~600–700ms
     * is typical; on a cold board or after a long PERST# it can take longer.
     */
    if (!fw_already_inited) {
        uart_puts("[VL805] Polling for SPI firmware ready (Command bit 20)...\n");
        int spi_ready = 0;
        int spi_ms = 0;
        for (int t = 200; t > 0; t--) {
            uint32_t cmd_poll = pci_read_config(&vl805_dev, 0x04);
            if (cmd_poll & (1U << 20)) {
                spi_ready = 1;
                uart_puts("[VL805] SPI firmware ready after ");
                if (spi_ms >= 1000) uart_putc('0' + spi_ms / 1000);
                if (spi_ms >= 100)  uart_putc('0' + (spi_ms / 100) % 10);
                uart_putc('0' + (spi_ms / 10) % 10);
                uart_putc('0' + spi_ms % 10);
                uart_puts("ms  Command=");
                print_hex32(cmd_poll);
                uart_puts("\n");
                break;
            }
            delay_ms(10);
            spi_ms += 10;
        }
        if (!spi_ready) {
            uart_puts("[VL805] WARNING: SPI firmware not ready after 2000ms — proceeding anyway\n");
        }
    }

    /*
     * Step 4b: ATU outbound window.
     * WIN0_LO=0xC0000000 was written in pcie_setup_outbound_window().
     * RC bridge window = 0xFFF0C000 covers PCI 0xC0000000–0xFFFFFFFF.
     * Once the SPI poll confirms firmware ready the BAR write in Step 6
     * assigns BAR=0xC0000000 and the ATU is already aligned.
     */
    if (fw_already_inited)
        uart_puts("[VL805] Firmware already loaded by start4.elf\n");

    uart_puts("[PCI] RC bridge window: ");
    print_hex32(readl(pcie_base + 0x20)); uart_puts("\n");

    /*
     * Step 5: Verify VL805 via EXT_CFG bus=1 (TYPE-1 config TLPs now forwarded).
     * The mailbox call above should have loaded VL805 firmware — give it a moment.
     */
    /*
     * Step 5: Verify VL805 via EXT_CFG.
     */
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
    uart_puts("[PCI] Writing BAR0 = 0xC0000004 via EXT_CFG bus=1...\n");
    pci_write_config(&vl805_dev, 0x10, 0xC0000004U);  /* 64-bit memory, base=0x80000000 */
    pci_write_config(&vl805_dev, 0x14, 0x00000000U);  /* BAR1 upper 32 = 0 */
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(5);
    uint32_t bar0_after = pci_read_config(&vl805_dev, 0x10);
    uint32_t bar1_after = pci_read_config(&vl805_dev, 0x14);
    uart_puts("[PCI] BAR0 after write="); print_hex32(bar0_after);
    uart_puts("  BAR1="); print_hex32(bar1_after); uart_puts("\n");

    if ((bar0_after & 0xFFFFFFF0U) == 0xC0000000U) {
        /* EXT_CFG CAN write VL805 BAR — update ATU and RC window to match */
        uart_puts("[PCI] BAR write SUCCESS → VL805 BAR = 0xC0000000\n");
        pci_bar = 0xC0000000U;
        writel(pci_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        /* Restore firmware's RC memory window (covers 0x80000000-0xBFFFFFFF) */
        writel(0xFFF0C000U, pcie_base + 0x20);
        asm volatile("dsb sy; isb" ::: "memory");
        uart_puts("[PCI] ATU WIN0_LO=0xC0000000, RC window=0xFFF0C000\n");
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

    /* ── TIMING DIAGNOSTIC ───────────────────────────────────────────────────
     * Measure how long a readl of xhci_base takes to determine whether TLPs
     * are being sent at all, or whether the BCM2711 fabric returns 0xdeaddead
     * before the TLP even hits the wire.
     *
     * ARM Generic Timer at 54 MHz: 1ms = 54,000 counts.
     *   < 1,000 ticks  (~18µs) → fabric abort before TLP sent
     *   54,000–5,400,000 ticks  → PCIe completion timeout (TLP was sent)
     *
     * CRITICAL: must dsb+isb between the load and the second timer read to
     * prevent speculative execution of the timer read before the load issues.
     */
    {
        uint64_t t0, t1;
        volatile uint32_t timed_read;
        asm volatile("dsb sy; isb" ::: "memory");
        asm volatile("mrs %0, cntpct_el0" : "=r"(t0));
        timed_read = readl(xhci_base);
        asm volatile("dsb sy; isb" ::: "memory");   /* ← barrier BEFORE t1 read */
        asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
        (void)timed_read;
        uint64_t elapsed = t1 - t0;
        uint32_t elapsed_us = (uint32_t)(elapsed / 54);
        uint32_t elapsed_ms = elapsed_us / 1000;
        uart_puts("[PCI] TIMING: readl took ");
        if (elapsed_ms >= 1000) { uart_putc('0' + (elapsed_ms/1000)%10); uart_putc(','); }
        uart_putc('0' + (elapsed_ms/100)%10);
        uart_putc('0' + (elapsed_ms/10)%10);
        uart_putc('0' + elapsed_ms%10);
        uart_puts(" ms (0x");
        print_hex32((uint32_t)(elapsed >> 32));
        uart_putc('_');
        print_hex32((uint32_t)elapsed);
        uart_puts(" ticks @ 54MHz)\n");
        if (elapsed < 1000ULL)
            uart_puts("[PCI] TIMING: < 1000 ticks → AXI fabric abort (TLP never sent)\n");
        else if (elapsed < 54000ULL)
            uart_puts("[PCI] TIMING: < 1ms → very fast, unexpected\n");
        else
            uart_puts("[PCI] TIMING: >= 1ms → PCIe completion timeout (TLP WAS sent!)\n");
    }

    /*
     * ATU sweep: try WIN0_LO candidates.  For each, also temporarily adjust
     * the RC bridge window (config 0x20) to cover that PCIe address range —
     * the bridge window gates TLP forwarding independently of the ATU.
     *
     * RC bridge window register (config offset 0x20) encoding (PCI spec §3.2.5.6):
     *   bits[31:20] = Memory Limit  (upper 12 bits, 1MB aligned, inclusive)
     *   bits[15:4]  = Memory Base   (upper 12 bits, 1MB aligned, inclusive)
     *   => To cover PCI 0x80000000–0x8FFFFFFF: writel(0x8ff08000, base+0x20)
     *   => To cover PCI 0x00000000–0x0FFFFFFF: writel(0x0ff00000, base+0x20)
     *
     * We save and restore the original RC_WIN value around each probe.
     */
    uint32_t orig_rc_win = readl(pcie_base + 0x20);
    static const uint32_t sweep[] = { 0x00000000, 0xC0000000, 0x80000000, 0xF8000000 };
    static const uint32_t sweep_rc_win[] = {
        0x0FF00000,   /* cover PCI 0x00000000–0x0FFFFFFF (VL805 reset BAR) */
        0xFFF0C000,   /* cover PCI 0xC0000000–0xFFFFFFFF (DTS mapping) */
        0x8ff08000,   /* cover PCI 0x80000000–0x8FFFFFFF (firmware mapping) */
        0xfff0f800,   /* cover PCI 0xF8000000–0xFFFFFFFF */
    };
    uint32_t live_bar = 0xFFFFFFFF;
    for (int s = 0; s < 4; s++) {
        /* Set RC bridge window to cover this PCIe address range */
        writel(sweep_rc_win[s], pcie_base + 0x20);
        writel(sweep[s], pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
        asm volatile("dsb sy; isb" ::: "memory");
        delay_ms(5);

        /* Time this read with proper barriers */
        uint64_t st0, st1;
        volatile uint32_t v;
        asm volatile("dsb sy; isb" ::: "memory");
        asm volatile("mrs %0, cntpct_el0" : "=r"(st0));
        v = readl(xhci_base);
        asm volatile("dsb sy; isb" ::: "memory");
        asm volatile("mrs %0, cntpct_el0" : "=r"(st1));
        uint64_t sticks = st1 - st0;

        uart_puts("[PCI] ATU sweep WIN0_LO="); print_hex32(sweep[s]);
        uart_puts(" RC_WIN="); print_hex32(sweep_rc_win[s]);
        uart_puts(" ticks=0x"); print_hex32((uint32_t)sticks);
        uart_puts(" MMIO[0]="); print_hex32(v);
        if (v == 0xFFFFFFFF)      uart_puts(" <- UR\n");
        else if (v == 0xdeaddead) uart_puts(sticks < 1000 ? " <- fabric abort\n" : " <- PCIe timeout\n");
        else                      { uart_puts(" <- LIVE DATA!\n"); if (live_bar == 0xFFFFFFFF) live_bar = sweep[s]; }
    }

    /* Restore original RC_WIN or use the live BAR */
    uint32_t use_bar = (live_bar != 0xFFFFFFFF) ? live_bar : pci_bar;
    uint32_t use_rc_win = (live_bar != 0xFFFFFFFF) ? sweep_rc_win[/* find index */0] : orig_rc_win;
    /* Re-find the matching rc_win for use_bar */
    for (int s = 0; s < 4; s++) {
        if (sweep[s] == use_bar) { use_rc_win = sweep_rc_win[s]; break; }
    }
    writel(use_rc_win, pcie_base + 0x20);
    writel(use_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
    asm volatile("dsb sy; isb" ::: "memory");
    if (live_bar != 0xFFFFFFFF && live_bar != pci_bar) {
        uart_puts("[PCI] ATU sweep found VL805 at WIN0_LO="); print_hex32(live_bar);
        uart_puts(" — using this\n");
        pci_bar = live_bar;
    } else if (live_bar == 0xFFFFFFFF) {
        uart_puts("[PCI] ATU sweep: all candidates dead\n");
        uart_puts("[PCI] Check ticks: fabric abort = ATU/SCB issue; PCIe timeout = BAR/Memory Space issue\n");
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
