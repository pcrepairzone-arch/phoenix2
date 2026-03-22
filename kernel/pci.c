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

/* Forward declarations for mailbox power state check.
 * Defined in drivers/gpu/mailbox_property.c — same TU that drives
 * fb_init() and all other VC property-channel calls. */
extern int mbox_get_power_state(uint32_t device_id);
#define MBOX_PWR_USB_HCD  3   /* matches MBOX_PWR_USB_HCD in drivers/gpu/mailbox_property.h */

/* kernel.h includes irq.h which defines PCIE_MSI_IRQ_VECTOR (180) and
 * PCIE_INTX_IRQ_VECTOR (175) — used by xhci_setup_msi() below. */

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void *ioremap(uint64_t phys_addr, size_t size);
extern int   get_pi_model(void);

/* ── Globals ────────────────────────────────────────────────────── */

 void    *pcie_base = NULL;
 pci_dev_t vl805_dev;
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
/* UBUS_BAR2_CONFIG_REMAP: CPU-side target address for inbound BAR2 DMA.
 * Linux pcie-brcmstb.c: PCIE_MISC_UBUS_BAR2_CONFIG_REMAP / _REMAP_HI.
 * Bit 0 = ACCESS_ENABLE — MUST be set or UBUS bridge rejects all inbound
 * DMA even if RC_BAR2 is correctly configured.  Without this bit the
 * endpoint sees a UR/CA completion → VL805 MCU sets USBSTS.HSE. */
#define MISC_UBUS_BAR2_CFG_REMAP_LO  0x408c
#define MISC_UBUS_BAR2_CFG_REMAP_HI  0x4090
#define UBUS_BAR2_ACCESS_ENABLE      (1U << 0)
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

/* BCM2711 PCIe RC — MSI inbound address registers.
 * The RC has a fixed MSI target window: any PCIe write to the address
 * programmed in MSI_BAR triggers GIC SPI 148 (INTID 180).
 * We point it at a scratch word in our DMA buffer — the value written
 * doesn't matter, the RC intercepts the TLP and raises the interrupt. */
#define MISC_MSI_BAR_CONFIG_LO      0x4044
#define MISC_MSI_BAR_CONFIG_HI      0x4048
#define MISC_MSI_DATA_CONFIG        0x404C
#define MISC_INTR2_CPU_BASE         0x4300   /* L2 interrupt controller */
#define MISC_INTR2_CPU_STATUS       (MISC_INTR2_CPU_BASE + 0x00)
#define MISC_INTR2_CPU_CLEAR        (MISC_INTR2_CPU_BASE + 0x08)
#define MISC_INTR2_CPU_MASK_SET     (MISC_INTR2_CPU_BASE + 0x10)
#define MISC_INTR2_CPU_MASK_CLR     (MISC_INTR2_CPU_BASE + 0x14)

/* PCIe standard MSI capability ID */
#define PCI_CAP_ID_MSI              0x05

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
     * CRITICAL ORDERING — BCM2711 RC registers are write-protected while
     * RGR1_SW_INIT_1 has its reset bits set.  start4.elf leaves RGR1=0x3
     * (both bit1=bridge-reset and bit0=PERST# asserted).  Any write to
     * MISC_CTRL while RGR1!=0 is silently dropped — readback returns 0.
     * This caused MISC_CTRL restored=0x00000000 on every boot, leaving
     * SCB_ACCESS_EN=0, so PCIE_STATUS always read 0x00000000, so we always
     * took the PERST# path even when the VL805 was alive.
     *
     * Linux pcie-brcmstb.c and U-Boot pcie_brcmstb.c both confirm:
     * deassert bridge reset (bit1) FIRST, wait, then configure MISC_CTRL,
     * then check PCIE_STATUS to decide whether full PERST# is needed.
     *
     * Step A: deassert bridge soft reset (bit1 only — keep PERST# bit0 as-is).
     * This brings the RC register file out of reset so writes take effect.
     */
    rgr1 &= ~(1U << 1);
    writel(rgr1, pcie_base + RGR1_SW_INIT_1_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    delay_ms(1);   /* RC needs ~1ms to come out of bridge reset */

    /*
     * Step B: open the SCB gate so PCIE_STATUS is readable.
     * Now that RGR1 bit1=0 the RC register file accepts writes.
     */
    uint32_t ctrl = readl(pcie_base + MISC_MISC_CTRL_OFF);
    ctrl |= (1U << 12);                  /* SCB_ACCESS_EN — open the gate */
    ctrl |= (1U << 2);                   /* CFG_READ_UR_MODE */
    /*
     * SCB0_SIZE: System Crossbar Bridge inbound window size.
     * Per Linux pcie-brcmstb.c:
     *   PCIE_MISC_MISC_CTRL_SCB0_SIZE_SHIFT = 27
     *   PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK  = GENMASK(31, 27)
     *
     * Encoding: field_value = ilog2(window_bytes) - 15
     *   For 1 GB (2^30): 30 - 15 = 15  →  15 << 27 = 0x78000000
     *
     * BOOT 51 BUG: previously used ~0x1F0 / 0x0F0 which placed SCB0_SIZE
     * in bits[8:4] — the WRONG field.  Bits[31:27] remained 0, meaning an
     * SCB window of 2^(0+15) = 32 KB.  Our DMA buffer starts at 0x10000
     * (64 KB), so EVERY inbound MRd/MWr from the VL805 MCU hit the SCB
     * boundary → UR completion → USBSTS.HSE.  17 retries, never a CCE.
     */
    ctrl  = (ctrl & ~(0xF8000000U | 0x1F0U)) | (15U << 27); /* SCB0_SIZE=15=1GB at bits[31:27] */
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] MISC_CTRL restored="); print_hex32(readl(pcie_base + MISC_MISC_CTRL_OFF)); uart_puts("\n");

    /*
     * Step C: read PCIE_STATUS with the gate open.
     * Bits 4+5 (pcie_link_up + pcie_dl_up) survive start4.elf's PCI0 reset.
     * If both set: VL805 is alive, BAR assigned — skip PERST# entirely.
     * If either clear: cold boot or CM4 rpiboot — need full PERST# + link train.
     */
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
         *   RGR1 = 0x00000001  →  LINK FAILED
         *
         * Bit 1 (bridge soft reset) was already cleared in Step A above.
         * Only bit 0 (PERST#) remains to be cleared to release the VL805.
         * The bit1 write below is a no-op (already 0) but left for clarity.
         */
        rgr1 &= ~(1U << 1);   /* already done in Step A — no-op */
        writel(rgr1, pcie_base + RGR1_SW_INIT_1_OFF);
        asm volatile("dsb sy; isb" ::: "memory");
        delay_ms(1);

        rgr1 &= ~(1U << 0);   /* clear PERST# — VL805 released, link trains */
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

/* FLR (Function Level Reset) — intentionally NOT used on Pi 4.
 * The VL805 is already clean after PERST#; issuing FLR here would
 * restart the MCU firmware init cycle (same hazard as HCRST —
 * journal Rule 1).  Function removed; comment kept as reference. */

/* ── MSI setup ───────────────────────────────────────────────────── */

/*
 * xhci_setup_msi — wire VL805 MSI to GIC SPI 148 (INTID 180).
 *
 * Three steps:
 *   1. Program the BCM2711 RC MSI inbound address window.
 *      The RC intercepts any PCIe memory write to this address and
 *      asserts GIC SPI 148 internally — no external wiring needed.
 *      We use a word in our DMA scratch area as the target physical
 *      address; the written value is irrelevant.
 *
 *   2. Walk the VL805 PCIe capability list to find the MSI capability
 *      (cap ID 0x05) and enable it.  The VL805 will then issue a
 *      memory-write TLP to the MSI BAR address on every xHCI interrupt.
 *
 *   3. Register our handler with the GIC and unmask INTID 180.
 *
 * Must be called after pci_init_pi4() has fully set up the RC and
 * VL805 config space is accessible via EXT_CFG.
 *
 * The handler (xhci_irq_handler) is defined in usb_xhci.c and declared
 * here via extern.
 */
void xhci_setup_msi(void) {
    uart_puts("[MSI] Setting up VL805 MSI -> GIC INTID 180...\n");

    /*
     * Step 1: Program RC MSI BAR.
     *
     * Physical address for MSI target: use the first word of the ERST
     * scratch area (DMA buffer + 0x1000).  This is ordinary cached RAM;
     * the RC only needs a valid PCIe-reachable address.  The RC converts
     * the PCIe write TLP into a GIC interrupt — it never actually lands
     * in RAM as far as the VL805 is concerned.
     *
     * MSI_BAR must be programmed in PCIe address space.  RC_BAR2 now maps
     * PCIe 0x00000000 → CPU 0x00000000 (boot 22), so:
     *   CPU phys 0x080b7000 → PCIe 0x080b7000  (DMA_OFFSET=0)
     * We store the PCIe address in MSI_BAR_LO (32-bit, fits in low 4GB).
     */
    extern uint64_t xhci_dma_phys(void);   /* returns DMA buf physical base */
    uint32_t msi_target_pcie = (uint32_t)(xhci_dma_phys() + 0x1000);
    writel(msi_target_pcie,  pcie_base + MISC_MSI_BAR_CONFIG_LO);
    writel(0x00000000U,      pcie_base + MISC_MSI_BAR_CONFIG_HI);
    writel(0x0000FFE0U,      pcie_base + MISC_MSI_DATA_CONFIG);
    asm volatile("dsb sy; isb" ::: "memory");

    uart_puts("[MSI] RC MSI BAR -> PCIe 0x");
    print_hex32(msi_target_pcie);
    uart_puts("\n");

    /* Unmask the RC L2 MSI interrupt towards the GIC */
    writel(0xFFFFFFFFU, pcie_base + MISC_INTR2_CPU_CLEAR);
    writel(0x00000000U, pcie_base + MISC_INTR2_CPU_MASK_SET);  /* unmask all */
    asm volatile("dsb sy; isb" ::: "memory");

    /*
     * Step 2: Enable MSI in VL805 PCIe config space.
     *
     * Walk the capability list from cap pointer at config offset 0x34.
     * MSI capability (ID=0x05) layout (PCIe spec 6.8.1):
     *   +0  [7:0]  Cap ID (0x05)
     *   +0  [15:8] Next pointer
     *   +2  [15:0] Message Control
     *              bit 0 = MSI Enable
     *              bits[3:1] = Multiple Message Enable (000=1 vector)
     *   +4  [31:0] Message Address Lo
     *   +8  [31:0] Message Address Hi (if 64-bit capable, bit 7 of ctrl)
     *   +12 [15:0] Message Data
     */
    uint32_t cap_ptr = pci_read_config(&vl805_dev, 0x34) & 0xFC;
    int msi_found = 0;
    while (cap_ptr && cap_ptr < 0x100) {
        uint32_t cap = pci_read_config(&vl805_dev, cap_ptr);
        if ((cap & 0xFF) == PCI_CAP_ID_MSI) {
            uart_puts("[MSI] Found MSI cap at offset 0x");
            print_hex8((uint8_t)cap_ptr);
            uart_puts("\n");

            /* Write MSI Address (PCIe target address the VL805 will write to) */
            pci_write_config(&vl805_dev, cap_ptr + 4, msi_target_pcie);

            /* Check 64-bit capability (bit 7 of Message Control word) */
            uint32_t ctrl_word = pci_read_config(&vl805_dev, cap_ptr + 0);
            int is64 = (ctrl_word >> 23) & 1;  /* bit 23 of dword = bit 7 of ctrl */
            if (is64) {
                pci_write_config(&vl805_dev, cap_ptr + 8,  0x00000000U); /* AddrHi=0 */
                pci_write_config(&vl805_dev, cap_ptr + 12, 0x00000000U); /* Data=0   */
            } else {
                pci_write_config(&vl805_dev, cap_ptr + 8,  0x00000000U); /* Data=0   */
            }
            asm volatile("dsb sy; isb" ::: "memory");

            /* Enable MSI: set bit 0 of Message Control.
             * Message Control is at byte offset +2 within the capability,
             * which is bits[31:16] of the dword at cap_ptr+0 on a
             * little-endian read — but pci_read_config reads full dwords.
             * Re-read, set bit 16 (= MSI Enable in the dword), write back. */
            ctrl_word |= (1U << 16);   /* bit 16 of dword = bit 0 of ctrl halfword */
            pci_write_config(&vl805_dev, cap_ptr + 0, ctrl_word);
            asm volatile("dsb sy; isb" ::: "memory");

            uint32_t verify = pci_read_config(&vl805_dev, cap_ptr + 0);
            uart_puts("[MSI] MSI ctrl after enable: "); print_hex32(verify);
            uart_puts(verify & (1U << 16) ? "  MSI ENABLED\n" : "  WARNING: enable bit not set\n");
            msi_found = 1;
            break;
        }
        cap_ptr = (cap >> 8) & 0xFC;
    }

    if (!msi_found) {
        uart_puts("[MSI] WARNING: MSI cap not found — falling back to INTx\n");
        /* Fall back: unmask INTx (PCIE_INTX_IRQ_VECTOR = 175) */
        extern void irq_set_handler(int, void(*)(int, void*), void*);
        extern void irq_unmask(int);
        extern void xhci_irq_handler(int vector, void *data);
        irq_set_handler(PCIE_INTX_IRQ_VECTOR, xhci_irq_handler, NULL);
        irq_unmask(PCIE_INTX_IRQ_VECTOR);
        uart_puts("[MSI] INTx fallback registered on INTID 175\n");
        return;
    }

    /*
     * Step 3: Register handler and unmask at GIC.
     *
     * irq_set_handler registers the C function in the GIC dispatch table.
     * irq_unmask writes GICD_ISENABLER for INTID 180 (SPI 148).
     * irq_dispatch (called from exceptions.S) will call xhci_irq_handler
     * on every MSI from the VL805.
     */
    extern void irq_set_handler(int, void(*)(int, void*), void*);
    extern void irq_unmask(int);
    extern void xhci_irq_handler(int vector, void *data);

    irq_set_handler(PCIE_MSI_IRQ_VECTOR, xhci_irq_handler, NULL);
    irq_unmask(PCIE_MSI_IRQ_VECTOR);

    uart_puts("[MSI] GIC INTID 180 unmasked — VL805 MSI armed\n");
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
    /* SCB0_SIZE at GENMASK(31,27) — Linux pcie-brcmstb.c SCB0_SIZE_SHIFT=27.
     * For 1GB: ilog2(1GB)-15 = 15 → 15<<27 = 0x78000000.
     * Clears old wrong bits[8:4] field at the same time.  */
    ctrl  = (ctrl & ~(0xF8000000U | 0x1F0U)) | (15U << 27);
    writel(ctrl, pcie_base + MISC_MISC_CTRL_OFF);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] ATU: MISC_CTRL after ="); print_hex32(readl(pcie_base + MISC_MISC_CTRL_OFF)); uart_puts("\n");


    /*
     * BCM2711 outbound ATU window — verified against Linux pcie-brcmstb.c
     * and confirmed across 25+ boot diagnostics.
     *
     * DTS range (bcm2711-rpi-4-b.dts):
     *   CPU  address: 0x600000000  (34-bit)
     *   PCI  address: 0x000000000  (VL805 BAR0 resets to 0 after PERST#)
     *   Size:         0x40000000   (1 GB)
     *
     * WIN0_LO / WIN0_HI: PCI destination (where TLPs are addressed TO)
     *   WIN0_LO = 0x00000000  VL805 BAR0 default value after PERST#
     *   WIN0_HI = 0x00000000  VL805 is a 32-bit device
     *
     * BASE_LIMIT: CPU source window, PCI bridge register format:
     *   bits[31:20] = (cpu_limit >> 20) & 0xFFF = 0x3FF
     *   bits[15:4]  = (cpu_base  >> 20) & 0xFFF = 0x000
     *   value = 0x3FF00000
     *
     * BASE_HI / LIMIT_HI: upper 32 bits of the 34-bit CPU address:
     *   0x600000000 >> 32 = 0x6
     *   These read back as 0 on BCM2711 but ARE used by the ATU hardware.
     */
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
    uart_puts((ctrl & (1U << 12))       ? "  SCB_ACCESS_EN=1" : "  SCB_ACCESS_EN=0 PROBLEM");
    uart_puts((ctrl & (1U << 2))        ? "  UR_MODE=1"       : "  UR_MODE=0 PROBLEM");
    /* SCB0_SIZE at bits[31:27]: expect 0xF=15 for 1GB → top 4 bits = 0111 1xxx */
    uart_puts(((ctrl >> 27) & 0x1FU) == 15U ? "  SCB0_SIZE=1GB-OK" : "  SCB0_SIZE-WRONG");
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

/* xhci_reset_controller / xhci_start_controller REMOVED (journal Rule 1).
 *
 * These functions issued HCRST followed by a CNR+RS=1 sequence.
 * On VL805 cold boot, HCRST triggers a second MCU firmware init cycle.
 * The MCU then overwrites CRCR/DCBAA and asserts HCH+HSE ~60us after
 * RS=1 — the root cause of all v32-v34 failures (see journal Session 2).
 *
 * The Pi 4 path (pci_init_pi4) does NOT call either function.
 * The Pi 5 path (pci_init_pi5) uses xhci_read_capabilities + xhci_scan_ports
 * only — also does not call either function.
 *
 * All VL805 xHCI init (rings, RS=1) is handled by usb_xhci.c:xhci_init().
 * DO NOT reinstate HCRST here.  See journal Rules 1 and 2. */

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
     * The VL805 DMAs to PCIe addresses computed by phys_to_dma() in usb_xhci.c,
     * which adds DMA_OFFSET=0xC0000000 to every physical address.
     * So the VL805 targets PCIe 0xC0000000+phys, and RC_BAR2 must accept
     * DMA reads from that PCIe range and map them to CPU 0x00000000+phys.
     *
     * RC_BAR2_CONFIG_LO encoding (BCM2711 TRM):
     *   bits[4:0]  = size: ilog2(window_bytes) - 12
     *                For 1 GB: ilog2(0x40000000) - 12 = 30 - 12 = 18 = 0x12
     *   bits[31:5] = PCIe base address >> 5
     *                For PCIe base 0xC0000000: 0xC0000000 >> 5 = 0x06000000
     *   Combined:   (0x06000000 << 5) | 0x12 = 0xC0000012
     *
     * This maps PCIe 0xC0000000..0xFFFFFFFF → CPU 0x00000000..0x3FFFFFFF.
     * DMA_OFFSET=0xC0000000 in usb_xhci.c then correctly translates:
     *   phys 0x080b5000 → DMA addr 0xC80b5000 → lands at CPU 0x080b5000 ✓
     *
     * NOTE: The previous value 0x00000012 set PCIe base=0, covering
     * PCIe 0x00000000–0x3FFFFFFF.  The VL805 DMAs to 0xC80b5000 which
     * is OUTSIDE that window — all DMA silently drops → empty event ring
     * → timeout on every command. */
    /* Boot 22: RC_BAR2 base moved from 0xC0000000 to 0x00000000.
     *
     * The VL805 MCU's ERSTBA_cache is permanently 0 after HCRST (hardware
     * clears ERSTBA at HCRST completion; MCU reads 0 and caches it).
     * With the old base (0xC0000000), PCIe address 0 fell outside RC_BAR2
     * → every MCU event DMA got a UR completion → HSE → infinite halt loop.
     *
     * Fix: make PCIe address 0 covered by an inbound DMA window.
     *   RC_BAR2 base = 0x00000000 maps PCIe [0, 1GB) → CPU [0, 1GB).
     *   MCU DMA to PCIe 0 → CPU 0 → our ERST table (placed at phys 0).
     *   ERST entry at phys 0 → event ring at phys evt_phys → correct DMA.
     *   No UR, no HSE.
     *
     * All DMA addresses in usb_xhci.c become physical addresses (DMA_OFFSET=0).
     *
     * RC_BAR2_CONFIG_LO encoding (BCM2711 / brcmstb Linux driver):
     *   bits[4:0]  = (ilog2(window_bytes) - 12) | 1
     *                The | 1 is MANDATORY — bit[0] is the BAR enable flag.
     *                Without it (even value) the window is DISABLED and the
     *                VL805 gets UR/CA on every DMA → immediate HSE.
     *                Linux function brcm_pcie_encode_ibar_size() always ORs 1.
     *   bits[31:5] = PCIe base address >> 5
     *
     *   For PCIe base 0x00000000, size 1GB:
     *     size_field = (ilog2(0x40000000) - 12) | 1 = (30 - 12) | 1 = 18 | 1 = 19 = 0x13
     *     base_field = 0x00000000 >> 5 = 0
     *     Combined:   0x00000000 | 0x13 = 0x00000013
     *
     * Maps PCIe 0x00000000..0x3FFFFFFF → CPU 0x00000000..0x3FFFFFFF (1GB).
     * DMA_OFFSET=0 in usb_xhci.c: phys addr == PCIe/DMA addr (identity).
     *   phys 0x080b8c00 → DMA addr 0x080b8c00 → lands at CPU 0x080b8c00 ✓
     *   phys 0x080b9040 → DMA addr 0x080b9040 → MCU reads ERST from there ✓
     *
     * BOOT 49 FIX: was 0x00000012 (bit[0]=0 = window DISABLED).
     *   With 0x12 the VL805 received UR on every inbound DMA (ERST read +
     *   CCE write) → HSE fired 13-15ms after Enable Slot → no CCE ever.
     *   Fix: use 0x00000013 — same 1GB window, bit[0]=1 = window ENABLED.
     */
    uart_puts("[PCI] RC_BAR2 setup (1GB DMA: PCIe 0x00000000 -> CPU 0x0)...\n");
    writel(0x00000013U, pcie_base + MISC_RC_BAR2_CFG_LO);
    writel(0x00000000U, pcie_base + MISC_RC_BAR2_CFG_HI);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] RC_BAR2 LO="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_LO));
    uart_puts(" HI="); print_hex32(readl(pcie_base + MISC_RC_BAR2_CFG_HI)); uart_puts("\n");

    /* UBUS_BAR2_CONFIG_REMAP: tell the UBUS bridge to forward inbound DMA
     * to CPU physical address 0x00000000.  Bit 0 (ACCESS_ENABLE) is the
     * gate that actually enables the window — without it RC_BAR2_CONFIG has
     * no effect and the VL805 gets UR/CA on every DMA → immediate HSE.
     *
     * Linux: PCIE_MISC_UBUS_BAR2_CONFIG_REMAP = cpu_addr | ACCESS_ENABLE
     *        PCIE_MISC_UBUS_BAR2_CONFIG_REMAP_HI = upper_32_bits(cpu_addr)
     * Our cpu_addr = 0x00000000 → REMAP_LO = 0x00000001, REMAP_HI = 0x0 */
    writel(0x00000000U | UBUS_BAR2_ACCESS_ENABLE,
           pcie_base + MISC_UBUS_BAR2_CFG_REMAP_LO);
    writel(0x00000000U, pcie_base + MISC_UBUS_BAR2_CFG_REMAP_HI);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[PCI] UBUS_BAR2_REMAP LO=");
    print_hex32(readl(pcie_base + MISC_UBUS_BAR2_CFG_REMAP_LO));
    uart_puts(" HI=");
    print_hex32(readl(pcie_base + MISC_UBUS_BAR2_CFG_REMAP_HI));
    uart_puts("\n");

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

    /*
     * Mailbox cross-check: ask VideoCore for the USB HCD power state.
     *
     * GET_POWER_STATE (tag 0x00020001) for device 3 (MBOX_PWR_USB_HCD):
     *   returns bit0 = on/off, bit1 = device_exists
     *
     * Why this matters:
     *   start4.elf leaves USB power ON (state=1) after it finishes init.
     *   If we call mbox_power_on_usb() (SET_POWER_STATE on->on) when USB
     *   is already powered, the VideoCore treats it as a USB subsystem reset
     *   request — it issues an internal PERST# equivalent, kicking the VL805
     *   MCU mid-init.  This was the original "skipping reset sets it to 1
     *   which stops proceeding" bug.
     *
     * Rule:
     *   usb_pwr == 1 (already on)  AND  link_up  ->  skip ALL mailbox calls
     *   usb_pwr == 0 (off)         OR  !link_up  ->  do full mbox power-on path
     */
    int usb_pwr = mbox_get_power_state(MBOX_PWR_USB_HCD);
    uart_puts("[PCI] USB HCD power state (mailbox): ");
    uart_putc('0' + (usb_pwr < 0 ? 9 : usb_pwr));  /* '9' = call failed */
    uart_puts(usb_pwr == 1 ? " (ON — VL805 alive, skip mailbox)\n"
            : usb_pwr == 0 ? " (OFF — need mailbox power-on)\n"
                           : " (mailbox error — assuming OFF)\n");

    /*
     * Override fw_already_inited: if power is OFF we must do the full
     * mailbox path regardless of what PCIE_STATUS said.
     * (Defensive: link bits can survive a power glitch on some revisions.)
     */
    if (usb_pwr != 1)
        fw_already_inited = 0;

    if (fw_already_inited) {
        /*
         * Skip-PERST# path: VL805 firmware already loaded by start4.elf,
         * USB power confirmed ON by mailbox.  Do NOT call any SET_POWER_STATE
         * or NOTIFY_XHCI_RESET — either would trigger a VC-side USB reset.
         *
         * Read BAR0 for logging only (EXT_CFG on BCM2711 only forwards
         * offsets 0x00 and 0x04 — BAR0 at 0x10 returns RC's own register,
         * so this will read 0x00000000; that is fine, WIN0_LO already
         * has 0x00000000 from pcie_setup_outbound_window()).
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
            uart_puts("[VL805] BAR0 unreadable via EXT_CFG (expected on BCM2711) — WIN0_LO stays 0x00000000\n");
        }
    } else {
        /*
         * PERST# path: USB power was off or mailbox check failed.
         * Step 1: mbox_power_on_usb() — SET_POWER_STATE device=3 on=1 wait=1.
         *   Without this, the VC has not established USB power rails and the
         *   VL805 MCU cannot complete its cold-boot init → CNR stays stuck at 1.
         * Step 2: vl805_init() — NOTIFY_XHCI_RESET (0x00030058) to reload
         *   VL805 firmware.  Needed on d03114 boards; silently ignored on
         *   c03112 EEPROM boards (VL805 self-loads from EEPROM).
         */
        extern int mbox_power_on_usb(void);
        extern int vl805_init(void);
        uart_puts("[VL805] Powering on USB HCD (SET_POWER_STATE device=3 on=1)...\n");
        if (mbox_power_on_usb() < 0)
            uart_puts("[VL805] WARNING: USB power-on mailbox failed — continuing\n");
        else
            uart_puts("[VL805] USB HCD powered on OK\n");
        delay_ms(50);   /* give VC time to establish USB power rails */
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
     * The VL805 MCU firmware needs Memory Space and Bus Master enabled before
     * it can run its xHCI internal init (it DMAs to its own internal buffers
     * over PCIe).  CNR staying set is NORMAL — no HCRST is issued (journal Rule 1).
     *
     * FLR deliberately skipped — VL805 already clean after PERST#.
     */
    uart_puts("[PCI] 1/4 (FLR skipped — VL805 already clean after PERST#)\n");
    uart_puts("[PCI] 2/4 Enable VL805 Memory Space + Bus Master\n");
    {
        uint32_t cmd = pci_read_config(&vl805_dev, 0x04);
        uart_puts("[PCI] VL805 Command before: "); print_hex32(cmd); uart_puts("\n");
        cmd |= (1U << 1) | (1U << 2);   /* Memory Space | Bus Master */
        pci_write_config(&vl805_dev, 0x04, cmd);
        asm volatile("dsb sy; isb" ::: "memory");
        delay_ms(5);
        cmd = pci_read_config(&vl805_dev, 0x04);
        uart_puts("[PCI] VL805 Command after:  "); print_hex32(cmd); uart_puts("\n");
    }

    /*
     * Map MMIO before xhci_init.
     */
    uart_puts("[PCI] 3/4 Map MMIO (CPU 0x600000000 = PCI "); print_hex32(pci_bar);
    uart_puts(")\n");
    xhci_base = ioremap(VL805_BAR0_CPU, 0x10000);
    if (!xhci_base) { uart_puts("[PCI] ERROR: ioremap failed\n"); return; }

    /*
     * Wait for VL805 MCU cold-boot to complete.
     *
     * After mailbox power-on + Memory Space enable, the MCU runs its
     * firmware init from SPI flash.  USBSTS.CNR (bit 11) stays set
     * until the MCU finishes (~800ms on this board).
     *
     * We must NOT issue HCRST — that restarts the MCU init cycle and
     * puts the MCU into a defensive state where it fights our ring
     * pointers (overwrites CRCR/DCBAA, then asserts HCH+HSE ~60us
     * after RS=1).  Instead we wait here for exactly ONE cold-boot
     * cycle to complete, then xhci_init writes our rings on top of
     * the warm/halted MCU state and sets RS=1.
     *
     * USBSTS is at: xhci_base + CAPLENGTH + 0x04
     * CAPLENGTH is at byte 0 of the capability registers.
     */
    /*
     * Wait for VL805 MCU cold-boot (CNR=0).
     *
     * After NOTIFY_XHCI_RESET the MCU loads firmware and runs its own
     * xHCI init, asserting CNR=1 until done.  This typically takes
     * 500–1500ms on Pi 4 in bare-metal (no start4.elf pre-init).
     *
     * delay_ms(1) is ~0.5ms real time at 1.5 GHz (tight volatile loop),
     * so 10000 iterations ≈ 5 real seconds — enough margin for cold boot.
     *
     * CRITICAL: do NOT proceed to xhci_init while CNR=1.
     * Doing so causes do_reset() to issue HCRST while the MCU is still
     * in cold-boot; the MCU interprets HCRST as a second reset request,
     * restarts its init cycle, and actively zeroes CRCR/DCBAA whenever
     * we write them (observed: CRCR_LO=0 after doorbell).
     */
    uart_puts("[VL805] Waiting for MCU cold-boot (CNR=0, up to ~5s)...\n");
    int cnr_cleared_fast = 0;
    {
        uint8_t caplength = readb(xhci_base);
        volatile uint32_t *usbsts_reg =
            (volatile uint32_t *)((uint8_t *)xhci_base + caplength + 0x04);
        int cnr_cleared = 0;
        for (int w = 0; w < 10000; w++) {
            delay_ms(1);
            if (!(*usbsts_reg & (1U << 11))) {
                uart_puts("[VL805] MCU ready after ");
                print_hex32(w + 1);
                uart_puts(" ticks  USBSTS=");
                print_hex32(*usbsts_reg);
                uart_puts("\n");
                cnr_cleared = 1;
                cnr_cleared_fast = 1;
                break;
            }
        }
        /* CNR not clearing after full timeout (old firmware warm-boot path).
         *
         * OLD FIRMWARE + warm boot (d03114): VL805 was configured by a previous
         * Linux session.  Our PERST# restarted the MCU cold, but on old firmware
         * the MCU waits for VC4 to re-configure it — VC4 already handed off.
         * CNR stays 1 permanently.  RS=1 is silently ignored while CNR=1.
         *
         * Fix: issue HCRST as a last resort.  On a warm-boot VL805 (MCU already
         * has its firmware flashed from SPI), HCRST just resets the register
         * state — it does NOT trigger a full SPI re-flash init cycle the way it
         * does on new firmware after a fresh cold-boot.  After HCRST the MCU
         * re-initialises its register state and CNR should clear within ~500ms.
         *
         * Distinct from journal Rule 1 (NEVER HCRST on new firmware mid-init):
         * Rule 1 applies when CNR cleared fast (new firmware).  Here CNR never
         * cleared → old firmware warm-boot → HCRST is the correct recovery.    */
        if (!cnr_cleared) {
            uart_puts("[VL805] WARNING: CNR stuck — old firmware warm-boot path\n");
            uart_puts("[VL805] Issuing HCRST fallback (warm MCU, not mid-cold-init)...\n");
            uint8_t  caplen2 = readb(xhci_base);
            void    *op_fb   = (uint8_t *)xhci_base + caplen2;
            writel(1U << 1, op_fb + 0x00);          /* USBCMD: HCRST bit 1 */
            asm volatile("dsb sy; isb" ::: "memory");
            /* Wait for HCRST self-clear then CNR=0 (up to 3s total) */
            for (int w = 0; w < 600; w++) {
                delay_ms(5);
                uint32_t cmd_fb = readl(op_fb + 0x00);
                uint32_t sts_fb = readl(op_fb + 0x04);
                if (!(cmd_fb & (1U << 1)) && !(sts_fb & (1U << 11))) {
                    uart_puts("[VL805] HCRST fallback: CNR cleared after ");
                    print_hex32((uint32_t)((w + 1) * 5));
                    uart_puts("ms  USBSTS="); print_hex32(sts_fb); uart_puts("\n");
                    cnr_cleared = 1;
                    /* cnr_cleared_fast stays 0: still old-firmware path,
                     * do NOT issue a second HCRST in the new-fw block below.  */
                    break;
                }
            }
            if (!cnr_cleared) {
                uart_puts("[VL805] HCRST fallback: CNR still set — MCU unresponsive\n");
            }
            uart_puts("[VL805] Post-fallback USBSTS="); print_hex32(*usbsts_reg); uart_puts("\n");
        }
    }

    /*
     * NEW FIRMWARE PATH — HCRST after MCU init (journal Rule 16):
     *
     * With new firmware (start4.elf >= 2026-02-26) the VC4 does XHCI-STOP +
     * PCI0 reset before handing off to the ARM.  After our mailbox power-on
     * + NOTIFY_XHCI_RESET, the VL805 MCU runs a clean firmware init and
     * CNR clears quickly (detected above as cnr_cleared_fast=1).
     *
     * Problem: the MCU configures its own internal ERST pointer (pointing at
     * GPU-side memory) during this init.  When we then write RS=1 without
     * first issuing HCRST, the MCU immediately tries to DMA-write an event
     * to that stale ERST address.  That PCIe write is rejected by the RC
     * (the address falls outside our RC_BAR2 window) and the RC returns
     * a UR/CA completion — HSE fires, HCH stays set.
     *
     * Fix: issue HCRST now, BEFORE programming our rings.  HCRST clears the
     * MCU's internally-configured ring state.  The MCU then re-inits cleanly,
     * CNR clears again, and xhci_init() can program our rings safely.
     *
     * OLD FIRMWARE PATH — HCRST is still FORBIDDEN (journal Rule 1):
     * If CNR never cleared (cnr_cleared_fast=0), the MCU was mid-init when
     * we timed out.  HCRST on a mid-init MCU triggers a second init cycle
     * that fights our ring pointers (overwrites CRCR/DCBAA, asserts
     * HCH+HSE ~60us after RS=1).  Old path: skip HCRST entirely.
     */
    if (cnr_cleared_fast) {
        uint8_t  caplength2 = readb(xhci_base);
        void    *op2        = (uint8_t *)xhci_base + caplength2;

        /*
         * Boot 27 refactor: standard HCRST + CNR wait.
         *
         * HCRST resets all xHC registers to defaults.  After CNR clears,
         * xhci_init() programs ERSTBA with the real DMA buffer address
         * (no phys-0 hack).  ERSTBA_LO reads 0 after HCRST — that's
         * expected; xhci.c writes the real value before RS=1.
         */
        uint32_t rtsoff2   = readl((uint8_t *)xhci_base + 0x18) & ~0x1FU;
        void    *ir0_2     = (uint8_t *)xhci_base + rtsoff2 + 0x20;
        void    *erstba_lo = (uint8_t *)ir0_2 + 0x10;

        uart_puts("[VL805] New firmware: issuing HCRST\n");

        writel(1U << 1, op2 + 0x00);           /* USBCMD: CMD_HCRST = bit 1 */
        asm volatile("dsb sy; isb" ::: "memory");

        int hcrst_done = 0;
        for (int w = 0; w < 200; w++) {
            delay_ms(1);
            if (!(readl(op2 + 0x00) & (1U << 1))) {
                uart_puts("[VL805] HCRST complete after ");
                print_hex32((uint32_t)(w + 1));
                uart_puts("ms  ERSTBA_LO=");
                print_hex32(readl(erstba_lo));
                uart_puts(" (0 expected — xhci.c sets real addr)\n");
                asm volatile("dsb sy; isb" ::: "memory");
                hcrst_done = 1;
                break;
            }
        }
        if (!hcrst_done)
            uart_puts("[VL805] WARNING: HCRST bit did not clear after 200ms — continuing\n");

        /* Wait for CNR=0 again after HCRST (MCU re-inits, up to 2s) */
        for (int w = 0; w < 400; w++) {
            delay_ms(5);
            if (!(readl(op2 + 0x04) & (1U << 11))) {
                uart_puts("[VL805] Post-HCRST MCU ready (");
                print_hex32((uint32_t)((w + 1) * 5));
                uart_puts("ms)  USBSTS=");
                print_hex32(readl(op2 + 0x04));
                uart_puts("\n");
                break;
            }
        }
        uart_puts("[VL805] Post-HCRST USBSTS=");
        print_hex32(readl(op2 + 0x04));
        uart_puts("\n");
    }

    /* Wire MSI before xhci_init so the handler is registered before
     * RS=1 is set. */
    xhci_setup_msi();

    uart_puts("[PCI] 4/4 Full xHCI init\n");
    extern int xhci_init(void *base_addr);
    if (xhci_init(xhci_base) != 0) {
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
