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
    /*
     * CRITICAL: MISC_CTRL is zeroed by start4.elf's "PCI0 reset", which clears
     * SCB_ACCESS_EN (bit 12).  With the SCB gate closed, PCIE_STATUS reads back
     * 0x00000000 regardless of actual link state — causing us to always take the
     * PERST# path even when the VL805 is still alive.
     * Restore SCB_ACCESS_EN + SCB0_SIZE NOW, before reading PCIE_STATUS.
     */
    uint32_t ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    ctrl |= (1U << 12);                  /* SCB_ACCESS_EN — open the gate */
    ctrl |= (1U << 2);                   /* CFG_READ_UR_MODE */
    ctrl  = (ctrl & ~0x1F0U) | 0x0F0U;  /* SCB0_SIZE = 1GB */
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] MISC_CTRL restored="); print_hex32(readl(pcie_base + MISC_MISC_CTRL_OFF)); uart_puts("\n");

    uint32_t entry_status = readl(pcie_base + MISC_PCIE_STATUS_OFF);
    int link_up_at_entry  = (entry_status & (1U << 4)) && (entry_status & (1U << 5));
    uint32_t fw_rc_win    = readl(pcie_base + 0x20);  /* logging only */
    uart_puts("[PCI] PCIE_STATUS at entry="); print_hex32(entry_status);
    uart_puts(link_up_at_entry
        ? "  -> link UP — VL805 alive, SKIPPING PERST#\n"
        : "  -> link DOWN — doing PERST# reset\n");
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
    /* SCB0_SIZE in bits[8:4]: encoding is ilog2(size) - 15.
     * For 1GB (2^30): 30 - 15 = 15 = 0xF  → field value 0x0F0
     * (RC_BAR2_CONFIG_LO uses ilog2-12, but MISC_CTRL SCB0_SIZE uses ilog2-15)
     */
    ctrl  = (ctrl & ~0x1F0U) | 0x0F0U; /* SCB0_SIZE=15 = 1GB inbound (ilog2(1GB)-15=15) */
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
    uart_puts("[PCI] ATU: writing WIN0_LO (PCI dest=0x00000000, VL805 BAR after reset)...\n");
    writel(0x00000000UL, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
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
     * Skip-PERST# path (fw_already_inited=1):
     *   start4.elf fully initialised the VL805 and assigned its BAR.
     *   The VL805 survived start4.elf's PCI0 reset intact — firmware,
     *   BAR assignment and link state all preserved.
     *   Read BAR0 directly from VL805 config space to get the exact value.
     *
     * PERST# path (fw_already_inited=0):
     *   VL805 was reset, BAR reverted to 0xC0000000 per DTS assignment.
     */
    uint32_t fw_win = readl(pcie_base + 0x20);
    uart_puts("[PCI] FW memory window: "); print_hex32(fw_win); uart_puts("\n");

    uint32_t pci_bar = 0x00000000U;  /* VL805 BAR resets to 0 after PERST# */

    /* ATU outbound window — always re-arm (start4.elf's PCI0 reset cleared these) */
    pcie_setup_outbound_window();

    /* RC_BAR2: inbound DMA window.
     * BCM2711 inbound DMA is silicon-fixed: PCIe 0xC0000000..0xFFFFFFFF → CPU 0x0.
     * RC_BAR2_CFG_LO encodes cpu_base (bits[31:5]) | size_enc (bits[4:0]).
     * cpu_base = 0x00000000, size_enc = ilog2(1GB)-12 = 18 = 0x12.
     * NOTE: phys_to_dma() in usb_xhci.c must add 0xC0000000 to all DMA addresses.
     *       WIN0_LO (outbound) is unrelated to this inbound DMA offset. */
    uart_puts("[PCI] RC_BAR2 setup (inbound DMA: PCIe 0x00000000 -> CPU 0x0, 1GB)...\n");
    writel(0x00000012U, pcie_base + MISC_RC_BAR2_CFG_LO);
    writel(0x00000000U, pcie_base + MISC_RC_BAR2_CFG_HI);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC_BAR2 LO="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_LO));
    uart_puts(" HI="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_HI)); uart_puts("\n");

    /* Step 2: Configure RC root port (needed on both paths) */
    /*
     * RC memory window (offset 0x20): bridge window register, PCI bridge format.
     *   bits[31:20] = limit[31:20]  bits[15:4] = base[31:20]
     * VL805 BAR=0x00000000, size ~16MB.  Cover PCI 0x00000000–0x0FFF0000:
     *   base  = 0x00000000 → bits[15:4] = 0x000
     *   limit = 0x0FFF0000 → bits[31:20] = 0x0FF
     *   value = 0x0FF00000
     * DO NOT use 0xFFF0C000 (covers 0xC0000000+ only) or 0xBFF08000
     * (covers 0x80000000+ only) — neither covers PCI 0x00000000.
     */
    writel(0x0FF00000U, pcie_base + 0x20);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC mem win set: "); print_hex32(readl(pcie_base + 0x20)); uart_puts("\n");

    uint32_t rc_cmd = readl(pcie_base + 0x04);
    uart_puts("[PCI] RC Command before: "); print_hex32(rc_cmd); uart_puts("\n");
    rc_cmd |= (1U << 1) | (1U << 2);
    writel(rc_cmd, pcie_base + 0x04);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC Command after:  "); print_hex32(readl(pcie_base + 0x04)); uart_puts("\n");

    writel(0x00010100U, pcie_base + 0x18);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(5);
    uart_puts("[PCI] RC buses: "); print_hex32(readl(pcie_base + 0x18)); uart_puts("\n");

    vl805_dev.bus = 1; vl805_dev.dev = 0; vl805_dev.func = 0;

    if (fw_already_inited) {
        /*
         * Skip-PERST# path: VL805 firmware already loaded by start4.elf.
         * Read BAR0 directly from VL805 config space — this is the actual
         * address the firmware assigned, which may differ from our default.
         */
        uart_puts("[VL805] Firmware already loaded by start4.elf — skipping mailbox\n");
        uint32_t bar0 = pci_read_config(&vl805_dev, 0x10) & 0xFFFFFFF0U;
        if (bar0 != 0 && bar0 != 0xFFFFFFF0U) {
            uart_puts("[VL805] BAR0 from config space: "); print_hex32(bar0); uart_puts("\n");
            pci_bar = bar0;
            /* Align ATU and RC window to the actual BAR */
            writel(pci_bar, pcie_base + MISC_CPU_2_PCIE_MEM_WIN0_LO);
            asm volatile("dsb sy; isb" ::: "memory");
            uart_puts("[PCI] WIN0_LO updated to match firmware BAR\n");
        } else {
            uart_puts("[VL805] BAR0 unreadable — keeping default 0xC0000000\n");
        }
    } else {
        /*
         * PERST# path: mailbox call to reload VL805 firmware (needed on
         * d03114 boards which have no SPI EEPROM).
         */
        extern int vl805_init(void);
        vl805_init();

        /* Poll until VL805 firmware signals ready (Command bit 20) */
        uart_puts("[VL805] Polling for firmware ready (Command bit 20)...\n");
        int spi_ready = 0, spi_ms = 0;
        for (int t = 200; t > 0; t--) {
            uint32_t cmd_poll = pci_read_config(&vl805_dev, 0x04);
            if (cmd_poll & (1U << 20)) {
                spi_ready = 1;
                uart_puts("[VL805] Firmware ready after ");
                if (spi_ms >= 1000) uart_putc('0' + spi_ms / 1000);
                if (spi_ms >= 100)  uart_putc('0' + (spi_ms / 100) % 10);
                uart_putc('0' + (spi_ms / 10) % 10);
                uart_putc('0' + spi_ms % 10);
                uart_puts("ms  Command="); print_hex32(cmd_poll); uart_puts("\n");
                break;
            }
            delay_ms(10);
            spi_ms += 10;
        }
        if (!spi_ready)
            uart_puts("[VL805] WARNING: firmware not ready after 2000ms — proceeding\n");
    }

    uart_puts("[PCI] RC bridge window: ");
    print_hex32(readl(pcie_base + 0x20)); uart_puts("\n");
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
     * Step 6: Enable Memory Space + Bus Master in the VL805.
     *
     * FLR (Function Level Reset) deliberately REMOVED.
     *
     * The VL805 has already been reset twice before this point:
     *   1. start4.elf issues XHCI-STOP + PCI0 reset at handoff
     *   2. pcie_bring_up_link() issues PERST# which reloads SPI firmware
     *
     * Each reset causes the VL805 SPI firmware to run its own internal init
     * sequence and post diagnostic events to an internal event ring, leaving
     * USBSTS.EINT=1.  A third reset (FLR here) adds another round of firmware
     * init, guaranteeing EINT=1 when xhci_init() runs HCRST.
     *
     * When RS=1 is set with EINT=1, the VL805 immediately tries to deliver
     * the pending event using its internal stale ring pointer — not our ERSTBA.
     * That DMA write targets an unmapped address → silent posted-write drop →
     * VL805 internal watchdog fires → HSE at ~10µs, event ring empty.
     *
     * The VL805 is already in a clean post-SPI state after PERST#.  No FLR
     * needed.  Just enable Memory Space + Bus Master and proceed.
     */
    uart_puts("[PCI] 1/4 (FLR skipped — VL805 already clean after PERST#)\n");

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

    uart_puts("[PCI] 4/4 Full xHCI init\n");
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
