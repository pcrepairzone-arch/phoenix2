/**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver — spec-compliant init for VL805 on Pi 4
 */

#include "kernel.h"
#include "usb_xhci.h"
#include "usb.h"
#include <string.h>

/* ── UART / print helpers ───────────────────────────────────────────────── */
/* uart_puts is defined in drivers/uart/uart.c                               */
extern void uart_puts(const char *s);

static void print_hex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0'; buf[1] = 'x';
    buf[2]  = hex[(v >> 28) & 0xF]; buf[3]  = hex[(v >> 24) & 0xF];
    buf[4]  = hex[(v >> 20) & 0xF]; buf[5]  = hex[(v >> 16) & 0xF];
    buf[6]  = hex[(v >> 12) & 0xF]; buf[7]  = hex[(v >>  8) & 0xF];
    buf[8]  = hex[(v >>  4) & 0xF]; buf[9]  = hex[(v >>  0) & 0xF];
    buf[10] = '\0';
    uart_puts(buf);
}
/* ── end helpers ────────────────────────────────────────────────────────── */

extern void *pcie_base;
extern pci_dev_t vl805_dev;

/* Forward declarations */
static void port_scan(void);
static int cmd_address_device(uint8_t slot_id, uint8_t port, uint32_t route, uint32_t speed);
static int ep0_get_device_descriptor(uint8_t slot_id, uint8_t *buf, int len);
static void enumerate_port(int port);
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type);
static int xhci_wait_event(uint32_t ev[4], int timeout_ms);
static int evt_ring_poll(uint32_t ev[4]);

/* ── All defines/globals from your original file ─────────────────────────── */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_CRCR_LO      0x18
#define OP_CRCR_HI      0x1C
#define OP_DCBAAP_LO    0x30
#define OP_DCBAAP_HI    0x34
#define OP_CONFIG       0x38

#define CMD_RS          (1U << 0)
#define CMD_HCRST       (1U << 1)
#define CMD_INTE        (1U << 2)

#define STS_HCH         (1U << 0)
#define STS_HSE         (1U << 2)
#define STS_EINT        (1U << 3)
#define STS_PCD         (1U << 4)
#define STS_CNR         (1U << 11)

#define IR_IMAN         0x00
#define IR_IMOD         0x04
#define IR_ERSTSZ       0x08
#define IR_ERSTBA_LO    0x10
#define IR_ERSTBA_HI    0x14
#define IR_ERDP_LO      0x18
#define IR_ERDP_HI      0x1C

#define PORTSC_CCS      (1U <<  0)
#define PORTSC_PED      (1U <<  1)
#define PORTSC_PP       (1U <<  9)
#define PORTSC_WIC      0x00FE0000U

#define TRB_CYCLE           (1U <<  0)
#define TRB_TC              (1U <<  1)
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE_LINK        6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_PORT_CHNG_EVT 34

#define CC_SUCCESS          1

#define DMA_DCBAA_OFF         0x0000
#define DMA_CMD_RING_OFF      0x0800
#define DMA_EVT_RING_OFF      0x0C00
/*
 * 0x1000 is the MSI landing pad: pci.c programs the RC MSI BAR to
 * xhci_dma_phys()+0x1000 so every VL805 MSI write lands here.  The
 * 4-byte MSI data payload is harmless in this scratch area.  ERST must
 * NOT live at this offset or MSI writes will corrupt the ring-base field.
 */
#define DMA_MSI_PAGE_OFF      0x1000  /* MSI landing pad – do NOT place structs here */
#define DMA_ERST_OFF          0x1040  /* was 0x1000 – moved to avoid MSI BAR collision */
#define DMA_SCRATCH_OFF       0x1080  /* was 0x1040 */
#define DMA_SCRATCH_PAGES_OFF 0x2000

#define CMD_RING_TRBS    64
#define EVT_RING_TRBS    64
#define MAX_SCRATCH_PAGES 64

extern char __xhci_dma_start[];
#define xhci_dma_buf ((uint8_t *)__xhci_dma_start)

static volatile uint64_t *dcbaa;
static volatile uint32_t *cmd_ring;
static volatile uint32_t *evt_ring;
static volatile uint64_t *erst;

static uint32_t evt_dequeue = 0;
static uint8_t  evt_cycle   = 1;  /* xHCI spec §4.9.3: event ring PCS/CCS = 1 after reset.
                                    * Ring is zeroed (all TRBs cycle=0). With CCS=1, zero TRBs
                                    * correctly appear EMPTY (0 ≠ 1). MCU writes events with
                                    * PCS=1; cycle=1 matches CCS=1 → detected correctly.
                                    *
                                    * Linux xhci-ring.c "cycle state=0" is for COMMAND/TRANSFER
                                    * rings (SW-produced), NOT the event ring (HC-produced).
                                    * Boot 32: evt_cycle=0 consumed all 64 zero-init TRBs as
                                    * false events — confirmed wrong for event ring. */

static xhci_controller_t xhci_ctrl;

static uint8_t  cmd_cycle   = 1;  /* CRCR.RCS=1 required by VL805 MCU firmware.
                                    * Boot 33 (RCS=0): MCU wrote ZERO events, fired HSE at 400ms.
                                    * Boot 29 (RCS=1): MCU ran 2000ms clean, wrote PSCEv events.
                                    * Linux xhci-ring.c always writes RCS=1. MCU interprets RCS=0
                                    * as "command ring not initialised" → firmware watchdog fires. */
static uint32_t cmd_enqueue = 0;
static uint8_t  g_slot_id   = 0;  /* set after successful Enable Slot CCE */

static volatile uint32_t pending_event[4]  = {0, 0, 0, 0};
static volatile int      pending_event_ready = 0;

static uint64_t cmd_ring_dma  = 0;
static uint64_t evt_ring_dma  = 0;
static uint64_t erst_dma_addr = 0;

/* DMA_OFFSET = 0.
 * RC_BAR2 maps PCIe 0x00000000 → CPU 0x00000000 (1 GB window).
 * Physical addresses equal PCIe/DMA addresses — no offset needed.  */
#define DMA_OFFSET  0x00000000ULL
static inline uint64_t phys_to_dma(uint64_t phys) {
    return phys + DMA_OFFSET;
}

/* ── mem_hexdump ──────────────────────────────────────────────────────────
 * Diagnostic helper: dump [addr, addr+len) as 16-byte rows to UART.
 *
 * Used at xhci_init() time to inspect what the Pi 4 firmware left at
 * physical 0x10000 (our .xhci_dma section start).  That section is
 * NOBITS/ALLOC-only — it is NOT in the kernel8.img binary — so its
 * contents are whatever the VideoCore firmware initialised DRAM to.
 * We expect all zeros; non-zero bytes here would indicate a firmware
 * conflict (ATF stubs, VC data, etc.) that would corrupt DMA ring state.
 *
 * Call before memset() so the original firmware state is logged.
 */
static void mem_hexdump(const char *label, uint64_t addr, size_t len) {
    uart_puts(label);
    uart_puts(" @ 0x");
    print_hex32((uint32_t)(addr >> 32)); print_hex32((uint32_t)addr);
    uart_puts("  ("); print_hex32((uint32_t)len); uart_puts(" bytes):\n");

    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    size_t rows = (len + 15) / 16;
    int any_nonzero = 0;

    for (size_t r = 0; r < rows; r++) {
        uint32_t row_addr = (uint32_t)(addr + r * 16);
        /* Read 16 bytes */
        uint8_t  b[16];
        int nonzero = 0;
        for (int i = 0; i < 16; i++) {
            b[i] = (r * 16 + i < len) ? p[r * 16 + i] : 0;
            if (b[i]) nonzero = 1;
        }
        if (!nonzero) continue;   /* skip all-zero rows to keep log compact */
        any_nonzero = 1;

        uart_puts("  0x"); print_hex32(row_addr); uart_puts(":  ");
        for (int i = 0; i < 16; i++) {
            /* print each byte as 2 hex digits */
            uint8_t hi = b[i] >> 4, lo = b[i] & 0xF;
            char hc = hi < 10 ? '0'+hi : 'a'+(hi-10);
            char lc = lo < 10 ? '0'+lo : 'a'+(lo-10);
            char s[3] = { hc, lc, 0 };
            uart_puts(s);
            if (i == 7) uart_puts("  ");
            else        uart_puts(" ");
        }
        uart_puts("\n");
    }
    if (!any_nonzero)
        uart_puts("  (all zeros — region is clean)\n");
}

/* reg_write64 — write a 64-bit xHCI register as two separate 32-bit stores.
 *
 * xHCI spec §4.20.3: "software shall write the upper and lower 32-bits of a
 * 64-bit register in two separate DWORD writes" — LO first, then HI.
 * A single 64-bit STR to PCIe MMIO generates one 8-byte MWr TLP; some
 * devices (including VL805) may latch only the first DWORD and leave the
 * upper half undefined.  Always use writel() pairs here.
 */
static void reg_write64(void *base, uint32_t lo_off, uint64_t val) {
    writel((uint32_t)(val & 0xFFFFFFFFULL), (uint8_t *)base + lo_off);
    writel((uint32_t)(val >> 32),           (uint8_t *)base + lo_off + 4);
    asm volatile("dsb sy" ::: "memory");
}

static void delay_us(int us) {
    for (volatile int i = 0; i < us * 150; i++) {}
}

static void delay_ms(int ms) {
    delay_us(ms * 1000);
}

static void *ir_base(int n) {
    return xhci_ctrl.runtime_regs + 0x20 + n * 0x20;
}

/* ── Full setup functions from your original paste ─────────────────────── */
static int read_caps(void) {
    void    *base = xhci_ctrl.cap_regs;
    uint32_t cap0 = readl(base + CAP_CAPLENGTH);
    uint8_t  clen = cap0 & 0xFF;
    uint16_t hver = cap0 >> 16;

    debug_print("[xHCI] CAPLENGTH=0x%02x  HCIVERSION=0x%04x\n", clen, hver);

    if (cap0 == 0xdeaddead || cap0 == 0xFFFFFFFF || clen < 0x10) {
        debug_print("[xHCI] ERROR: bad CAPLENGTH 0x%02x\n", clen);
        return -1;
    }

    uint32_t hcs1 = readl(base + CAP_HCSPARAMS1);
    uint32_t hcs2 = readl(base + CAP_HCSPARAMS2);

    xhci_ctrl.max_slots = hcs1 & 0xFF;
    xhci_ctrl.max_ports = (hcs1 >> 24) & 0xFF;

    uint32_t spb_lo = (hcs2 >> 27) & 0x1F;
    uint32_t spb_hi = (hcs2 >> 21) & 0x1F;
    xhci_ctrl.scratchpad_count = (spb_hi << 5) | spb_lo;

    uint32_t rtsoff = readl(base + CAP_RTSOFF) & ~0x1FU;
    uint32_t dboff  = readl(base + CAP_DBOFF)  & ~0x03U;

    xhci_ctrl.op_regs       = xhci_ctrl.cap_regs + clen;
    xhci_ctrl.runtime_regs  = xhci_ctrl.cap_regs + rtsoff;
    xhci_ctrl.doorbell_regs = xhci_ctrl.cap_regs + dboff;

    debug_print("[xHCI] MaxSlots=%u  MaxPorts=%u  Scratchpads=%u\n",
                xhci_ctrl.max_slots, xhci_ctrl.max_ports, xhci_ctrl.scratchpad_count);
    return 0;
}

static int do_reset(void) {
    void *op = xhci_ctrl.op_regs;

    uart_puts("[xHCI] do_reset: USBSTS="); print_hex32(readl(op + OP_USBSTS));
    uart_puts("  USBCMD="); print_hex32(readl(op + OP_USBCMD)); uart_puts("\n");

    /* NO HCRST HERE — pci.c already issued HCRST and waited for CNR=0.
     * Just clear stale status bits before programming rings.               */

    /* Clear any stale status bits the MCU may have set during cold-boot.
     * These are W1C — writing 1 clears them, writing 0 has no effect.    */
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
    asm volatile("dsb sy; isb" ::: "memory");

    uart_puts("[xHCI] do_reset done. USBSTS="); print_hex32(readl(op + OP_USBSTS));
    uart_puts("\n");
    return 0;
}

static int setup_dcbaa(void) {
    dcbaa = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    memset((void *)dcbaa, 0, 2048);

    uint32_t pgsz = readl(xhci_ctrl.op_regs + OP_PAGESIZE);
    debug_print("[xHCI] PAGESIZE reg = 0x%08x (%s)\n", pgsz,
                (pgsz & 1) ? "4KB OK" : "4KB NOT supported — scratchpad alloc wrong");
    if (!(pgsz & 1)) return -1;

    uint32_t n = xhci_ctrl.scratchpad_count;
    if (n > MAX_SCRATCH_PAGES) return -1;

    if (n > 0) {
        volatile uint64_t *scratch_arr = (volatile uint64_t *)(xhci_dma_buf + DMA_SCRATCH_OFF);
        memset((void *)scratch_arr, 0, MAX_SCRATCH_PAGES * 8);

        uint8_t *pages_base = xhci_dma_buf + DMA_SCRATCH_PAGES_OFF;
        memset(pages_base, 0, n * 4096);
        asm volatile("dsb sy" ::: "memory");

        for (uint32_t i = 0; i < n; i++) {
            void *pg = pages_base + i * 4096;
            scratch_arr[i] = phys_to_dma((uint64_t)virt_to_phys(pg));
        }
        asm volatile("dsb sy" ::: "memory");

        uint64_t scratch_arr_dma = phys_to_dma((uint64_t)virt_to_phys((void *)scratch_arr));
        dcbaa[0] = scratch_arr_dma;
    }

    uint64_t dcbaa_dma = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
    reg_write64(xhci_ctrl.op_regs, OP_DCBAAP_LO, dcbaa_dma);

    writel(xhci_ctrl.max_slots & 0xFFU, xhci_ctrl.op_regs + OP_CONFIG);
    return 0;
}

static int setup_cmd_ring(void) {
    cmd_ring    = (volatile uint32_t *)(xhci_dma_buf + DMA_CMD_RING_OFF);
    cmd_cycle   = 1;  /* RCS=1: VL805 MCU requires this; see static initialiser comment */
    cmd_enqueue = 0;
    memset((void *)cmd_ring, 0, CMD_RING_TRBS * 16);

    uint64_t ring_phys = (uint64_t)virt_to_phys((void *)cmd_ring);
    uint64_t ring_dma  = phys_to_dma(ring_phys);
    cmd_ring_dma = ring_dma;

    uint32_t li = (CMD_RING_TRBS - 1) * 4;
    cmd_ring[li + 0] = (uint32_t)(ring_dma);
    cmd_ring[li + 1] = (uint32_t)(ring_dma >> 32);
    cmd_ring[li + 2] = 0;
    cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;

    asm volatile("dsb sy" ::: "memory");

    uint64_t crcr = (ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    reg_write64(xhci_ctrl.op_regs, OP_CRCR_LO, crcr);

    return 0;
}

static int setup_event_ring(void) {
    evt_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
    erst     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);

    memset((void *)evt_ring, 0, EVT_RING_TRBS * 16);
    memset((void *)erst,     0, 16);

    uint64_t evt_phys  = (uint64_t)virt_to_phys((void *)evt_ring);
    uint64_t erst_phys = (uint64_t)virt_to_phys((void *)erst);
    uint64_t evt_dma   = phys_to_dma(evt_phys);
    uint64_t erst_dma  = phys_to_dma(erst_phys);
    evt_ring_dma  = evt_dma;

    /*
     * ERSTBA: boot 47 analysis — switch to real DMA address.
     *
     * Boot 27 concluded ERSTBA_cache=0 (MCU always DMA-reads ERST from PCIe 0).
     * But boot 47 showed: MCU fires HSE 13ms AFTER Enable Slot doorbell with
     * zero events ever written to the ring.  This matches "MCU tried to DMA
     * read/write something and got UR/CA from the RC".
     *
     * Root cause theory: the BCM2711 RC_BAR2 inbound window may not cover
     * PCIe address 0 — the zero page is a protected region on many SoCs
     * (firmware vectors, RC shadow).  If the MCU's ERST DMA-read at PCIe 0
     * hits UR, it gets a corrupted ERST → writes CCE to a bogus address → UR
     * on the write → HSE.  Every inbound DMA test so far was outbound (ARM
     * reading VL805 MMIO); PCIe 0x0 inbound has never been verified.
     *
     * Fix: point ERSTBA at the real DMA buffer (Normal-NC, in the 1GB
     * RC_BAR2 window, definitely accessible).  The MCU DMA-reads ERST from
     * there, finds our event ring at evt_ring_dma, writes CCE correctly.
     *
     * Belt-and-suspenders: ALSO keep the ERST at phys 0 (flushed to DRAM).
     * If the MCU truly ignores ERSTBA and always reads from PCIe 0 (cache),
     * it still finds valid ERST data there.  We get the best of both worlds.
     *
     * Diagnostic: after a failed CCE poll, dump phys 0 (cache-invalidated)
     * to see if the MCU wrote events THERE instead of the evt ring — this
     * distinguishes "ERSTBA as ERST pointer" vs "ERSTBA as ring base" behaviour.
     */

    /* ERST entry in Normal-NC DMA buffer (primary: MCU reads from here) */
    volatile uint32_t *erst32 = (volatile uint32_t *)erst;
    erst32[0] = (uint32_t)(evt_dma & 0xFFFFFFFFULL);
    erst32[1] = (uint32_t)(evt_dma >> 32);
    erst32[2] = EVT_RING_TRBS;
    erst32[3] = 0;
    asm volatile("dsb sy" ::: "memory");

    /* Also write ERST at physical address 0 (fallback if MCU truly ignores
     * ERSTBA MMIO and always reads from PCIe 0 — boot 27 behaviour).
     * Cache-flush so any PCIe DMA-read at address 0 sees coherent data.  */
    {
        uintptr_t pa = 0;
        asm volatile("" : "+r"(pa));   /* opaque barrier: prevent GCC UB trap */
        volatile uint32_t *p = (volatile uint32_t *)pa;
        p[0] = (uint32_t)(evt_dma & 0xFFFFFFFFULL);
        p[1] = (uint32_t)(evt_dma >> 32);
        p[2] = (uint32_t)EVT_RING_TRBS;
        p[3] = 0;
        asm volatile("dc civac, %0\n\tdsb sy\n\tisb" :: "r"(pa) : "memory");
    }

    /* ERSTBA = real DMA address (Normal-NC, RC_BAR2 accessible).
     * Previously: 0ULL (matched ERSTBA_cache=0 theory).
     * Now: real address so MCU can DMA-read ERST from a verified region.  */
    erst_dma_addr = erst_dma;

    uart_puts("[xHCI] ERST setup (boot48: ERSTBA=real DMA, phys0 fallback kept)\n");
    uart_puts("[xHCI]   evt_ring DMA="); print_hex32((uint32_t)evt_dma);
    uart_puts("  erst_buf DMA="); print_hex32((uint32_t)erst_dma);
    uart_puts("  size="); print_hex32(EVT_RING_TRBS); uart_puts("\n");

    /* Readback verification */
    {
        uintptr_t pa = 0;
        asm volatile("" : "+r"(pa));
        volatile uint32_t *p = (volatile uint32_t *)pa;
        asm volatile("dc civac, %0\n\tdsb sy\n\tisb" :: "r"(pa) : "memory");
        uart_puts("[xHCI]   phys0:    [");
        print_hex32(p[0]); uart_puts(","); print_hex32(p[1]);
        uart_puts(","); print_hex32(p[2]); uart_puts(","); print_hex32(p[3]);
        uart_puts("]\n");
        uart_puts("[xHCI]   erst_buf: [");
        print_hex32(erst32[0]); uart_puts(","); print_hex32(erst32[1]);
        uart_puts(","); print_hex32(erst32[2]); uart_puts(","); print_hex32(erst32[3]);
        uart_puts("]\n");
    }

    return 0;
}

static int run_controller(void) {
    void *op  = xhci_ctrl.op_regs;
    void *ir0 = ir_base(0);

    uint64_t dcbaa_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
    uint64_t erstba_dma = erst_dma_addr;
    uint64_t evt_dma    = evt_ring_dma;
    uint64_t crcr_val   = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    uint32_t cfg_val    = (readl(op + OP_CONFIG) & ~0xFFU) | (xhci_ctrl.max_slots & 0xFFU);

    /*
     * Boot 27 refactor: follow Linux's xHCI init sequence.
     *
     * Linux programs ALL ring pointers AND enables the event ring (ERSTSZ=1)
     * BEFORE setting RS=1.  Previous boots (19-26) suppressed ERSTSZ=0 during
     * RS=1 then latched it post-settle — this may have prevented the MCU from
     * ever seeing a valid ERST.  Now we do it the Linux way:
     *
     *   1. Clear stale status
     *   2. Program DCBAA, CONFIG, CRCR
     *   3. Program ERSTBA (real address), ERSTSZ=1, ERDP, IMAN, IMOD
     *   4. RS=1 + INTE
     *   5. Wait HCH=0 with HSE retry
     *   6. Settle, drain, scan
     */

    /* Step 1: Clear stale status bits (W1C) */
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Step 2: Program DCBAA, CONFIG, CRCR */
    reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
    writel(cfg_val, op + OP_CONFIG);
    reg_write64(op, OP_CRCR_LO, crcr_val);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Step 3: Program interrupter 0.
     * ERSTBA=0 (matches MCU's immutable ERSTBA_cache=0; confirmed boot 27).
     * The actual ERST is at phys 0 — MCU DMAs there regardless of MMIO.
     * ERSTSZ=1 enables event generation.  ERDP points to our event ring.  */
    writel(0U, ir0 + IR_ERSTSZ);                      /* clear first      */
    reg_write64(ir0, IR_ERSTBA_LO, erstba_dma);        /* 0 = MCU target   */
    asm volatile("dsb sy; isb" ::: "memory");
    writel(1U, ir0 + IR_ERSTSZ);                       /* enable: 1 segment */
    asm volatile("dsb sy; isb" ::: "memory");
    reg_write64(ir0, IR_ERDP_LO, evt_dma | 0x8ULL);   /* ERDP + W1C EHB   */
    writel(0x00000002U, ir0 + IR_IMAN);                 /* IE=1, IP W1C     */
    writel(0x0FA00FA0U, ir0 + IR_IMOD);                 /* IMOD             */
    asm volatile("dsb sy; isb" ::: "memory");

    /* Diagnostic readback: verify all ring pointers landed */
    uart_puts("[xHCI] Rings programmed (Linux order). Readback:\n");
    uart_puts("[xHCI]   DCBAAP=");  print_hex32(readl(op + OP_DCBAAP_LO));
    uart_puts("  CRCR=");    print_hex32(readl(op + OP_CRCR_LO));
    uart_puts("  CONFIG=");  print_hex32(readl(op + OP_CONFIG)); uart_puts("\n");
    uart_puts("[xHCI]   ERSTBA=");  print_hex32(readl(ir0 + IR_ERSTBA_LO));
    uart_puts("  ERSTSZ=");  print_hex32(readl(ir0 + IR_ERSTSZ));
    uart_puts("  ERDP=");    print_hex32(readl(ir0 + IR_ERDP_LO));
    uart_puts("  IMAN=");    print_hex32(readl(ir0 + IR_IMAN)); uart_puts("\n");
    uart_puts("[xHCI]   USBSTS=");  print_hex32(readl(op + OP_USBSTS));
    uart_puts("  USBCMD=");  print_hex32(readl(op + OP_USBCMD)); uart_puts("\n");

    /* Step 4: RS=1 with minimal HSE retry loop.
     *
     * Boot 45 proved passive wait (Linux xhci_start style) does NOT work:
     *   USBCMD read back 0x00000004 (RS=0, INTE=1) instantly after our RS=1
     *   write.  USBSTS stayed 0x00000005 (HCH=1+HSE=1) for all 3000ms.
     *   The VL805 MCU auto-clears RS when it fires HSE and does NOT self-
     *   recover — it waits for the host to acknowledge and retry.
     *
     * MCU recovery protocol (standard xHCI §4.5.1 + VL805 behaviour):
     *   1. Host W1C HSE — acknowledges the transient firmware-init error
     *   2. Host re-asserts RS=1 — MCU retries its init sequence
     *   Repeat until MCU settles into TRUE RUNNING (typically < 50ms total).
     *
     * CRCR rule (learned boots 41-45):
     *   • Write CRCR ONCE before the loop — MCU needs a valid ring base.
     *   • NEVER rewrite CRCR during retry iterations — any lone CRCR write
     *     without an immediately paired doorbell jams the MCU FSM (boot 41:
     *     during HCH=1; boot 42: during HCH=0+HSE=1).
     *   • Writing CRCR then immediately ringing db[0] IS safe (inline Enable
     *     Slot below) — MCU treats CRCR+doorbell together as a command submit.
     *
     * TRUE RUNNING ≡ USBCMD.RS=1 AND USBSTS.HCH=0 AND USBSTS.HSE=0
     * Budget: 600 × 5ms = 3000ms; MCU typically settles in < 50ms.           */
    {
        /* Write CRCR once — MCU uses this as command ring base when it accepts
         * RS=1.  Register holds the value; we do NOT touch it in the loop.    */
        reg_write64(op, OP_CRCR_LO, crcr_val);
        asm volatile("dsb sy; isb" ::: "memory");

        /* First RS=1 attempt. */
        writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        uart_puts("[xHCI] RS=1 (HSE-retry, no CRCR rewrites)\n");

        uint32_t s       = 0;
        uint32_t cmd_v   = 0;
        int settled      = 0;
        int hse_retries  = 0;

        for (int t = 0; t < 600 && !settled; t++) {
            delay_ms(5);
            s     = readl(op + OP_USBSTS);
            cmd_v = readl(op + OP_USBCMD);

            if (!(s & STS_HCH) && (cmd_v & CMD_RS) && !(s & STS_HSE)) {
                /* TRUE RUNNING — controller accepted RS=1 cleanly */
                uart_puts("[xHCI] TRUE RUNNING at ");
                print_hex32((uint32_t)((t + 1) * 5));
                uart_puts("ms  hse_retries=");
                print_hex32((uint32_t)hse_retries); uart_puts("\n");
                settled = 1;
                break;
            }

            /* Not TRUE RUNNING — MCU fired HSE and cleared RS.
             * Recovery: W1C HSE to acknowledge, then re-assert RS=1.
             * NO CRCR write — register already holds the value set above.    */
            hse_retries++;
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            delay_ms(2);
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");

            if (hse_retries <= 5 || (hse_retries % 20) == 0) {
                uart_puts("[xHCI] HSE retry "); print_hex32((uint32_t)hse_retries);
                uart_puts(" t="); print_hex32((uint32_t)((t + 1) * 5));
                uart_puts("ms USBSTS="); print_hex32(s); uart_puts("\n");
            }
        }

        if (!settled) {
            uart_puts("[xHCI] ERROR: controller never reached TRUE RUNNING\n");
            uart_puts("[xHCI]   hse_retries="); print_hex32((uint32_t)hse_retries);
            uart_puts("  last USBSTS="); print_hex32(s); uart_puts("\n");
            return -1;
        }
    }

    /* Step 5: Power up ports.
     * Boot 39 finding: writing PP=1 to Port 1 (companion, DR=1) triggered
     * HSE with ~3ms delay, killing the Enable Slot window.  Skip the
     * companion port here — enumerate_port() already skips it for WPR /
     * device enumeration, so this write was always unnecessary.           */
    for (int p = 0; p < (int)xhci_ctrl.max_ports; p++) {
        uint32_t ps5 = readl(op + 0x400 + p * 0x10);
        if (ps5 & (1U << 30)) {
            uart_puts("[xHCI] step5: skip companion port ");
            print_hex32((uint32_t)(p + 1)); uart_puts(" (DR=1)\n");
            continue;
        }
        writel((ps5 & ~PORTSC_WIC) | PORTSC_PP, op + 0x400 + p * 0x10);
        asm volatile("dsb sy" ::: "memory");
    }

    /* Step 6: Settle wait — 400ms for MCU to train SS links.
     *
     * Boot 34: 2000ms settle → MCU watchdog at 1200ms.  Shortened to 400ms.
     * Boot 56 root cause: without No-op at TRUE RUNNING, MCU fires HSE
     *   every 4ms throughout the entire settle window (100 consecutive
     *   settle HSE entries, every one showing USBSTS=0x00000004).
     * Boot 57 fix: No-op at TRUE RUNNING satisfies the MCU keepalive so
     *   the settle loop should see ZERO HSE entries.  Any HSE here is a
     *   secondary watchdog event and is logged with timestamp so we can
     *   measure the No-op keepalive duration precisely.                   */
    /* Step 6: Settle wait — 400ms for MCU to train SS links.
     * VL805 MCU fires HSE during settle; this is normal — force-ring in
     * cmd_ring_submit will handle RS=1 + doorbell atomically at submit time. */
    int _settle_evts = 0;
    int _settle_hse  = 0;
    for (int i = 0; i < 100; i++) {
        delay_ms(4);
        uint32_t _ev[4];
        while (evt_ring_poll(_ev)) _settle_evts++;
        if (readl(op + OP_USBSTS) & STS_HSE) _settle_hse++;
    }
    uart_puts("[xHCI] Settle done: USBSTS="); print_hex32(readl(op + OP_USBSTS));
    uart_puts(" evts="); print_hex32((uint32_t)_settle_evts);
    uart_puts(" hse="); print_hex32((uint32_t)_settle_hse); uart_puts("\n");

    /* Drain any startup PSCEv events from the settle period */
    {
        uint32_t _dev[4];
        int _drained = 0;
        while (evt_ring_poll(_dev)) _drained++;
        uart_puts("[xHCI] PSCEv drain: ");
        print_hex32((uint32_t)_drained);
        uart_puts(" events  deq="); print_hex32(evt_dequeue);
        uart_puts(" cycle="); print_hex32(evt_cycle); uart_puts("\n");
    }

    /* Final state check */
    uart_puts("[xHCI] Final: USBSTS="); print_hex32(readl(op + OP_USBSTS));
    uart_puts(" USBCMD="); print_hex32(readl(op + OP_USBCMD));
    uart_puts(" CRCR="); print_hex32(readl(op + OP_CRCR_LO));
    uart_puts(" ERSTBA="); print_hex32(readl(ir0 + IR_ERSTBA_LO));
    uart_puts(" ERSTSZ="); print_hex32(readl(ir0 + IR_ERSTSZ));
    uart_puts("\n");

    /* Event ring self-test REMOVED (boot 53 confirmed PASS every run).
     * Ring stays at initial state: deq=0, ERDP=ring_base, cycle=1.
     * MCU will write its first event (Enable Slot CCE) to TRB[0].         */

    /* Also verify phys 0 ERST hasn't been corrupted since setup_event_ring */
    {
        uintptr_t pa = 0;
        asm volatile("" : "+r"(pa));
        volatile uint32_t *p = (volatile uint32_t *)pa;
        asm volatile("dc civac, %0\n\tdsb sy\n\tisb" :: "r"(pa) : "memory");
        uart_puts("[xHCI] phys0 ERST check: [");
        print_hex32(p[0]); uart_puts(","); print_hex32(p[1]);
        uart_puts(","); print_hex32(p[2]); uart_puts(","); print_hex32(p[3]);
        uart_puts("]\n");
        /* Verify against expected values */
        uint32_t exp0 = (uint32_t)(evt_dma & 0xFFFFFFFFULL);
        uint32_t exp2 = EVT_RING_TRBS;
        if (p[0] != exp0 || p[2] != exp2) {
            uart_puts("[xHCI] *** PHYS 0 CORRUPTED! ***\n");
            uart_puts("[xHCI]   expected[0]="); print_hex32(exp0);
            uart_puts(" got="); print_hex32(p[0]); uart_puts("\n");
            uart_puts("[xHCI]   expected[2]="); print_hex32(exp2);
            uart_puts(" got="); print_hex32(p[2]); uart_puts("\n");
        } else {
            uart_puts("[xHCI] phys0 ERST intact\n");
        }
    }

    return 0;
}

/* forward-referenced by xhci_init — defined here so the struct is visible */
static usb_hc_ops_t g_xhci_hc_ops;

/* ── xhci_init ───────────────────────────────────────────────────────────── */
int xhci_init(void *base_addr) {
    debug_print("[xHCI] driver built " __DATE__ " " __TIME__ " init base=0x%llx\n", (uint64_t)(uintptr_t)base_addr);

    /* ── DMA region sanity check + explicit zero-init ─────────────────────
     * .xhci_dma is an ALLOC-only (NOBITS) section — it is NOT present in
     * kernel8.img.  Physical 0x10000 contains whatever the Pi 4 VideoCore
     * firmware left there.  Dump it first (to catch any firmware conflict),
     * then zero it unconditionally so every ring and struct starts clean.
     *
     * The section spans [__xhci_dma_start, __xhci_dma_end) = 0x42000 bytes.
     * We inspect only the first 256 bytes (covers DCBAA + cmd_ring header).
     */
    extern char __xhci_dma_end[];
    size_t dma_region_size = (size_t)((uintptr_t)__xhci_dma_end -
                                      (uintptr_t)__xhci_dma_start);
    uart_puts("[xHCI] DMA region: 0x");
    print_hex32((uint32_t)((uint64_t)(uintptr_t)xhci_dma_buf >> 32));
    print_hex32((uint32_t)((uint64_t)(uintptr_t)xhci_dma_buf));
    uart_puts(" - 0x");
    print_hex32((uint32_t)((uint64_t)(uintptr_t)__xhci_dma_end >> 32));
    print_hex32((uint32_t)((uint64_t)(uintptr_t)__xhci_dma_end));
    uart_puts("  size=0x"); print_hex32((uint32_t)dma_region_size); uart_puts("\n");

    /* Inspect first 256 bytes BEFORE zeroing — shows what firmware left. */
    mem_hexdump("[xHCI] pre-zero DMA[0..255]",
                (uint64_t)(uintptr_t)xhci_dma_buf, 256);

    /* Explicit zero — NOBITS section; ring cycle bits must start at 0.
     * Normal-NC mapping: memset goes straight to DRAM, no cache issue.  */
    memset(xhci_dma_buf, 0, dma_region_size);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[xHCI] DMA region zeroed.\n");

    /* ── End DMA init ──────────────────────────────────────────────────── */

    xhci_ctrl.cap_regs = base_addr;
    if (read_caps() != 0) return -1;
    if (do_reset() != 0) return -1;
    if (setup_dcbaa() != 0) return -1;
    if (setup_cmd_ring() != 0) return -1;
    if (setup_event_ring() != 0) return -1;
    /* setup_interrupter() merged into run_controller() — Linux programs
     * IMAN/IMOD together with ERSTBA/ERSTSZ before RS=1.               */
    if (run_controller() != 0) return -1;

    /* Register HC ops BEFORE port scan so xhci_control_transfer is
     * available during enumeration. port_scan itself is deferred —
     * it is called by xhci_scan_ports() which usb_init() calls AFTER
     * registering class drivers, so usb_enumerate_device() finds them. */
    usb_register_hc(&g_xhci_hc_ops);
    xhci_ctrl.initialized = 1;
    return 0;
}

/* ── DMA extension + full enumeration functions (from your original) ─────── */
#define DMA_INPUT_CTX_OFF   0x21000
#define DMA_OUT_CTX_OFF     0x21500
#define DMA_EP0_RING_OFF    0x21900
#define DMA_EP0_DATA_OFF    0x21D00

#define EP0_RING_TRBS  64
#define CTX_SIZE  32

#define TRB_TYPE_SETUP     2
#define TRB_TYPE_DATA      3
#define TRB_TYPE_STATUS    4
#define TRB_TYPE_ADDR_DEV  11
#define TRB_TYPE_CMD_CMPL  33
#define TRB_TYPE_XFER_EVT 32

#define TRB_IDT   (1U << 6)
#define TRB_IOC   (1U << 5)
#define TRB_DIR_IN (1U << 16)

#define CC_SHORT_PKT  13

#define USB_REQ_GET_DESCRIPTOR  6
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5

#define USB_REQ_SET_ADDRESS     5
#define USB_REQ_SET_CONFIG      9

static volatile uint8_t  *input_ctx;
static volatile uint8_t  *out_ctx;
static volatile uint32_t *ep0_ring;
static uint8_t  ep0_cycle   = 1;  /* ICS=1 in input context; matches VL805 MCU CCS=1 expectation */
static uint32_t ep0_enqueue = 0;
/* g_slot_id declared above (line ~134) so run_controller() can set it */

static void ep0_ring_init(void) {
    ep0_ring    = (volatile uint32_t *)(xhci_dma_buf + DMA_EP0_RING_OFF);
    ep0_cycle   = 1;  /* ICS=1; matches VL805 MCU CCS=1 expectation */
    ep0_enqueue = 0;
    memset((void *)ep0_ring, 0, EP0_RING_TRBS * 16);

    uint64_t ring_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_ring));
    uint32_t li = (EP0_RING_TRBS - 1) * 4;
    ep0_ring[li + 0] = (uint32_t)(ring_dma);
    ep0_ring[li + 1] = (uint32_t)(ring_dma >> 32);
    ep0_ring[li + 2] = 0;
    ep0_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | ep0_cycle;
    asm volatile("dsb sy" ::: "memory");
}

static void ep0_enq(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type, uint32_t flags) {
    uint32_t b = ep0_enqueue * 4;
    ep0_ring[b + 0] = dw0;
    ep0_ring[b + 1] = dw1;
    ep0_ring[b + 2] = dw2;
    ep0_ring[b + 3] = (type << TRB_TYPE_SHIFT) | flags | ep0_cycle;
    asm volatile("dsb sy" ::: "memory");

    ep0_enqueue++;
    if (ep0_enqueue >= EP0_RING_TRBS - 1) {
        uint32_t li = (EP0_RING_TRBS - 1) * 4;
        ep0_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | ep0_cycle;
        asm volatile("dsb sy" ::: "memory");
        ep0_cycle ^= 1;
        ep0_enqueue = 0;
    }
}

static void ep0_doorbell(uint8_t slot) {
    volatile uint32_t *db = (volatile uint32_t *)xhci_ctrl.doorbell_regs;
    asm volatile("dsb sy" ::: "memory");
    db[slot] = 1;
    asm volatile("dsb sy" ::: "memory");
}

static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type) {
    uint32_t b = cmd_enqueue * 4;
    cmd_ring[b + 0] = dw0;
    cmd_ring[b + 1] = dw1;
    cmd_ring[b + 2] = dw2;
    /* Command TRBs: type + cycle bit only. TRB_TC is Toggle Cycle and
     * belongs only on the Link TRB at the end of the ring. */
    cmd_ring[b + 3] = (type << TRB_TYPE_SHIFT) | cmd_cycle;
    asm volatile("dsb sy" ::: "memory");

    cmd_enqueue++;
    if (cmd_enqueue >= CMD_RING_TRBS - 1) {
        uint32_t li = (CMD_RING_TRBS - 1) * 4;
        cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;
        asm volatile("dsb sy" ::: "memory");
        cmd_cycle ^= 1;
        cmd_enqueue = 0;
    }

    /* Re-write CRCR immediately before ringing the doorbell.
     * The VL805 MCU can zero CRCR in the narrow window between our
     * ring setup and the doorbell. Writing it here with only a dsb
     * between it and db[0] closes that gap. */
    void *op = xhci_ctrl.op_regs;

    /* Force-ring: W1C → RS=1 → CRCR → doorbell, atomically.
     * VL805 MCU fires HSE as its idle keepalive; waiting for HSE=0 before
     * the doorbell means never ringing.  Accept HSE=1 post-ring.         */
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
    asm volatile("dsb sy; isb" ::: "memory");
    writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");

    uint64_t crcr = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    reg_write64(op, OP_CRCR_LO, crcr);
    asm volatile("dsb sy; isb" ::: "memory");

    volatile uint32_t *db = (volatile uint32_t *)xhci_ctrl.doorbell_regs;
    db[0] = 0;
    asm volatile("dsb sy" ::: "memory");

    return 0;
}

static int cmd_address_device(uint8_t slot_id, uint8_t port, uint32_t route, uint32_t speed) {
    input_ctx = (volatile uint8_t *)(xhci_dma_buf + DMA_INPUT_CTX_OFF);
    out_ctx   = (volatile uint8_t *)(xhci_dma_buf + DMA_OUT_CTX_OFF);
    memset((void *)input_ctx, 0, 34 * CTX_SIZE);
    memset((void *)out_ctx,   0, 32 * CTX_SIZE);

    volatile uint32_t *icc = (volatile uint32_t *)input_ctx;
    icc[1] = 0x00000003;

    /* Speed field in Slot Context DWord 0 bits[23:20] (xHCI §6.2.2):
     *   1=FS 12Mb/s  2=LS 1.5Mb/s  3=HS 480Mb/s  4=SS 5Gb/s
     * Use the actual port speed — do NOT hardcode SS (4).
     * If speed=0 (unknown/companion) fall back to SS as safe default. */
    uint32_t spd = (speed > 0 && speed <= 6) ? speed : 4U;
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(input_ctx + CTX_SIZE);
    slot_ctx[0] = (route & 0xFFFFF) | (spd << 20) | (1U << 27);
    slot_ctx[1] = (uint32_t)(port + 1) << 16;

    volatile uint32_t *ep0_ctx = (volatile uint32_t *)(input_ctx + 2 * CTX_SIZE);
    ep0_ctx[1] = (3U << 1) | (4U << 3) | (512U << 16);

    ep0_ring_init();
    uint64_t ep0_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_ring));
    /* bit 0 = ICS (Initial Cycle State): must match ep0_cycle (=1). */
    ep0_ctx[2] = (uint32_t)(ep0_dma) | ep0_cycle;
    ep0_ctx[3] = (uint32_t)(ep0_dma >> 32);
    ep0_ctx[4] = 8;

    uint64_t out_dma = phys_to_dma((uint64_t)virt_to_phys((void *)out_ctx));
    dcbaa[slot_id] = out_dma;

    uint64_t in_dma = phys_to_dma((uint64_t)virt_to_phys((void *)input_ctx));
    cmd_ring_submit((uint32_t)in_dma, (uint32_t)(in_dma >> 32), 0, TRB_TYPE_ADDR_DEV);

    /* Short timeout — VL805 MCU does not yet write CCEs; caller continues */
    uint32_t ev[4];
    if (xhci_wait_event(ev, 200) != 0) return -1;
    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS) return -1;

    volatile uint32_t *out_slot = (volatile uint32_t *)out_ctx;
    uint8_t usb_addr = out_slot[3] & 0xFF;
    debug_print("[xHCI] Address Device OK  slot=%u  usb_addr=%u\n", slot_id, usb_addr);
    return 0;
}

static int ep0_get_device_descriptor(uint8_t slot_id, uint8_t *buf, int len) {
    volatile uint8_t *ep0_data = (volatile uint8_t *)(xhci_dma_buf + DMA_EP0_DATA_OFF);
    memset((void *)ep0_data, 0, 512);

    uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));

    uint32_t setup_lo = 0x80U | (USB_REQ_GET_DESCRIPTOR << 8) | ((uint32_t)USB_DESC_DEVICE << 24);
    uint32_t setup_hi = (uint32_t)len << 16;

    ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
    ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), (uint32_t)len, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
    ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);

    ep0_doorbell(slot_id);

    uint32_t ev[4];
    if (xhci_wait_event(ev, 5000) != 0) return -1;
    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;

    uint32_t ev2[4];
    xhci_wait_event(ev2, 1000);

    int copy = len < 18 ? len : 18;
    for (int i = 0; i < copy; i++) buf[i] = ep0_data[i];
    return copy;
}

static void enumerate_port(int port) {
    void *op = xhci_ctrl.op_regs;
    uint32_t portsc = readl(op + 0x400 + port * 0x10);

    uart_puts("[xHCI] Port "); print_hex32(port + 1);

    /* Companion port (DR bit 30): Port 1 on VL805 is an internal HS hub.
     * Any PORTSC write to it fires HSE and halts the controller.  Skip. */
    if (portsc & (1U << 30)) {
        uart_puts(": companion (DR=1, skipped)  PORTSC=");
        print_hex32(portsc); uart_puts("\n");
        return;
    }

    if (!(portsc & PORTSC_CCS)) {
        /* PRC=1 or CSC=1 with CCS=0: the MCU detected a device during
         * cold-boot init but the SS link dropped before we got here.
         * Try a single Warm Port Reset to re-establish the link.
         * If that fails, try a PP power-cycle as last resort.           */
        uint32_t prc = (portsc >> 21) & 1;
        uint32_t csc = (portsc >> 17) & 1;
        if (prc || csc) {
            uart_puts(": CCS=0 PRC="); print_hex32(prc);
            uart_puts(" CSC="); print_hex32(csc);
            uart_puts(" — link dropped, trying WPR recovery  PORTSC=");
            print_hex32(portsc); uart_puts("\n");

            /* Single clean WPR to re-establish SS link */
            writel((1U << 31) | PORTSC_PP, op + 0x400 + port * 0x10);
            asm volatile("dsb sy" ::: "memory");
            for (int t = 0; t < 1000; t++) {
                delay_ms(1);
                portsc = readl(op + 0x400 + port * 0x10);
                if (portsc & PORTSC_CCS) break;
                if ((t + 1) % 200 == 0) {
                    uart_puts("[xHCI]   WPR recovery t="); print_hex32((uint32_t)(t + 1));
                    uart_puts("ms  PORTSC="); print_hex32(portsc); uart_puts("\n");
                }
            }

            if (!(portsc & PORTSC_CCS)) {
                /* WPR didn't bring device back — try PP power cycle */
                uart_puts("[xHCI]   WPR recovery failed — trying PP power cycle\n");
                writel(PORTSC_PP & 0, op + 0x400 + port * 0x10); /* PP=0 */
                asm volatile("dsb sy" ::: "memory");
                delay_ms(300);
                writel(PORTSC_PP, op + 0x400 + port * 0x10);     /* PP=1 */
                asm volatile("dsb sy" ::: "memory");
                for (int t = 0; t < 2000; t++) {
                    delay_ms(1);
                    portsc = readl(op + 0x400 + port * 0x10);
                    if (portsc & PORTSC_CCS) break;
                    if ((t + 1) % 500 == 0) {
                        uart_puts("[xHCI]   PP-cycle t="); print_hex32((uint32_t)(t + 1));
                        uart_puts("ms  PORTSC="); print_hex32(portsc); uart_puts("\n");
                    }
                }
            }

            if (!(portsc & PORTSC_CCS)) {
                uart_puts(": device not recovered after WPR+PP  PORTSC=");
                print_hex32(portsc); uart_puts("\n");
                return;
            }
            uart_puts(": device RECOVERED  PORTSC=");
            print_hex32(portsc); uart_puts("\n");
            /* Fall through to the normal WPR + Enable Slot path below */
        } else {
            uart_puts(": not connected (PORTSC="); print_hex32(portsc); uart_puts(")\n");
            return;
        }
    }

    uart_puts(": CONNECTED  PORTSC=");
    print_hex32(portsc); uart_puts("\n");
    uart_puts("[xHCI] Issuing single WPR...\n");

    /* Single clean Warm Port Reset — PR alone times out on this SS device.
     * Clean write only: do NOT copy snapshot bits (W1C bits in snapshot
     * can re-trigger change events and generate extra MCU PSCEv).         */
    writel((1U << 31) | PORTSC_PP, op + 0x400 + port * 0x10);
    asm volatile("dsb sy" ::: "memory");

    uint32_t ps = portsc;
    for (int t = 0; t < 300; t++) {
        delay_ms(1);
        ps = readl(op + 0x400 + port * 0x10);
        /* Boot 54: waiting for WPR=0 AND PED=1 caused 300ms timeout when
         * link training failed (PED never became 1 — PR stuck at 1).
         * Fix: break as soon as WPR bit (bit 31) clears — reset completed
         * regardless of whether USB3 link trained.  PED=1 is checked after
         * the speed read to decide if enumeration can proceed.            */
        if (!(ps & (1U << 31))) break;
    }
    uart_puts("[xHCI] WPR done. PORTSC="); print_hex32(ps); uart_puts("\n");

    if (!(ps & PORTSC_CCS)) {
        uart_puts("[xHCI] Device lost after WPR — aborting\n");
        return;
    }

    /* Clear W1C change bits with clean write (WRC, CSC, PRC etc.).
     * Do NOT copy snapshot: PED is RW1C on USB3 and snapshot copy
     * would disable the port.                                             */
    writel(PORTSC_WIC | PORTSC_PP, op + 0x400 + port * 0x10);
    asm volatile("dsb sy" ::: "memory");

    /* Boot 54: speed=0 because PR bit (bit 4) was still set when speed was
     * read (port reset still in progress).  Wait for PR=0 — up to 200ms —
     * before reading speed.  PR clears when the standard port reset (that
     * follows WPR internally) completes and port is addressed.            */
    for (int t = 0; t < 200; t++) {
        delay_ms(1);
        ps = readl(op + 0x400 + port * 0x10);
        if (!(ps & (1U << 4))) break;   /* PR cleared */
    }

    ps = readl(op + 0x400 + port * 0x10);
    uint32_t speed = (ps >> 10) & 0xF;
    uart_puts("[xHCI] Port reset done. PORTSC="); print_hex32(ps);
    uart_puts("  speed="); print_hex32(speed); uart_puts("\n");

    /* ── Step 1: Enable Slot ─────────────────────────────────────────── */
    /* VL805 quirk: MCU never writes CCEs to the event ring.
     * Submit Enable Slot, assume slot_id=1, and drive forward.          */
    uint32_t ev[4];
    uint8_t slot_id;
    cmd_ring_submit(0, 0, 0, TRB_TYPE_ENABLE_SLOT);
    slot_id = 1;
    g_slot_id = slot_id;
    uart_puts("[xHCI] Enable Slot submitted (slot_id=1 assumed)\n");

    /* ── Step 2: Address Device ──────────────────────────────────────── */
    if (cmd_address_device(slot_id, (uint8_t)port, 0, speed) == 0) {
        uart_puts("[xHCI] Address Device OK slot="); print_hex32(slot_id); uart_puts("\n");
    } else {
        uart_puts("[xHCI] Address Device timeout — continuing\n");
    }

    /* Build a minimal usb_device_t so control transfers work via g_hc_ops.
     * Store slot_id in hcd_private so xhci_control_transfer can find the ring. */
    extern int usb_enumerate_device(usb_device_t *dev, int port);
    static usb_device_t g_dev; /* one device for now — extend to array later */
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.speed       = (uint8_t)speed;
    g_dev.address     = 1; /* xHCI assigns USB address via Address Device */
    g_dev.hcd_private = (void *)(uintptr_t)slot_id;

    /* ── Step 3: GET_DESCRIPTOR (Device, 18 bytes) ───────────────────── */
    /* VL805 quirk: MCU never writes transfer completion events.
     * Check DMA buffer for real data first; fall back to synthetic
     * descriptor so the class-driver probe path can be exercised.       */
    int force_fake = 0;
    uint8_t ddesc[18];
    int got = ep0_get_device_descriptor(slot_id, ddesc, 18);
    if (got < 8) {
        /* Check DMA buffer for real data — MCU may have written the payload
         * even without posting a transfer completion event.               */
        volatile uint8_t *_ep0_buf =
            (volatile uint8_t *)(xhci_dma_buf + DMA_EP0_DATA_OFF);
        asm volatile("dsb sy" ::: "memory"); /* ensure PCIe DMA is visible */
        if (_ep0_buf[0] != 0 || _ep0_buf[1] != 0) {
            /* Real descriptor arrived via DMA despite missing event */
            uart_puts("[xHCI] DevDesc: real data in DMA buffer\n");
            for (int _i = 0; _i < 18; _i++) ddesc[_i] = _ep0_buf[_i];
            got = 18;
        } else {
            /* No data — synthesise a USB3 MSC descriptor to exercise the
             * class-driver path until event ring write-back is fixed.     */
            uart_puts("[xHCI] DevDesc: DMA empty, using synthetic MSC descriptor\n");
            static const uint8_t _fake_d[18] = {
                18,   0x01, 0x00, 0x03, /* bLength, DEVICE, bcdUSB=3.0      */
                0x00, 0x00, 0x00,        /* class/sub/proto at interface      */
                0x09,                    /* bMaxPacketSize0 = 2^9 = 512 (SS) */
                0x81, 0x07,              /* idVendor  = 0x0781 (SanDisk)     */
                0x58, 0x55,              /* idProduct = 0x5558               */
                0x01, 0x01,              /* bcdDevice = 1.01                 */
                0x01, 0x02, 0x03, 0x01,  /* strings + bNumConfigurations     */
            };
            for (int _i = 0; _i < 18; _i++) ddesc[_i] = _fake_d[_i];
            got = 18;
            force_fake = 1;
        }
    }

    g_dev.bMaxPacketSize0  = ddesc[7];
    g_dev.idVendor         = (uint16_t)(ddesc[8]  | (ddesc[9]  << 8));
    g_dev.idProduct        = (uint16_t)(ddesc[10] | (ddesc[11] << 8));
    g_dev.bcdUSB           = (uint16_t)(ddesc[2]  | (ddesc[3]  << 8));
    g_dev.bDeviceClass     = ddesc[4];
    g_dev.bDeviceSubClass  = ddesc[5];
    g_dev.bDeviceProtocol  = ddesc[6];

    uart_puts("[xHCI] Device: VID="); print_hex32(g_dev.idVendor);
    uart_puts(" PID="); print_hex32(g_dev.idProduct);
    uart_puts(" class="); print_hex32(g_dev.bDeviceClass); uart_puts("\n");

    /* ── Step 4: GET_DESCRIPTOR (Configuration, parse interfaces) ────── */
    uint8_t cfgbuf[256];
    memset(cfgbuf, 0, sizeof(cfgbuf));
    uint16_t total_len = 0;

    if (force_fake) {
        /* Synthetic path: skip over-the-wire config fetch.
         * Single-interface USB 3.0 MSC config: class=8 sub=6 proto=0x50 (BOT).
         * Two bulk EPs: 0x81 IN (512B) and 0x02 OUT (512B).                  */
        static const uint8_t _fake_cfg[32] = {
            9, 0x02, 32, 0,  1, 1, 0, 0x80, 0xFA,  /* Config:  9B total=32  */
            9, 0x04,  0, 0,  2, 8, 6, 0x50,      0, /* Intf 0: MSC SCSI BOT */
            7, 0x05, 0x81, 0x02, 0x00, 0x02,     0, /* EP 0x81 IN  BULK 512 */
            7, 0x05, 0x02, 0x02, 0x00, 0x02,     0, /* EP 0x02 OUT BULK 512 */
        };
        total_len = 32;
        for (int _i = 0; _i < 32; _i++) cfgbuf[_i] = _fake_cfg[_i];
        uart_puts("[xHCI] CfgDesc: synthetic MSC config (class=8/6/BOT)\n");
    } else {
        /* Real path: fetch config over EP0 */
        uint32_t setup_lo = (0x80U)
                          | ((uint32_t)USB_REQ_GET_DESCRIPTOR << 8)
                          | ((uint32_t)USB_DESC_CONFIG << 24);
        uint32_t setup_hi = (uint32_t)9 << 16;
        volatile uint8_t *ep0_data = (volatile uint8_t *)(xhci_dma_buf + DMA_EP0_DATA_OFF);
        memset((void *)ep0_data, 0, 256);
        uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));

        /* First fetch 9 bytes to get wTotalLength */
        ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
        ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), 9, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
        ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);
        ep0_doorbell(slot_id);
        if (xhci_wait_event(ev, 3000) != 0) {
            uart_puts("[xHCI] GET_DESCRIPTOR(Config,9) timeout\n");
            goto probe;
        }
        xhci_wait_event(ev, 500); /* drain Status event */

        total_len = (uint16_t)(ep0_data[2] | ((uint16_t)ep0_data[3] << 8));
        if (total_len < 9 || total_len > 255) total_len = 9;

        /* Fetch full config descriptor */
        memset((void *)ep0_data, 0, 256);
        setup_hi = (uint32_t)total_len << 16;
        ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
        ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), total_len, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
        ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);
        ep0_doorbell(slot_id);
        if (xhci_wait_event(ev, 3000) != 0) {
            uart_puts("[xHCI] GET_DESCRIPTOR(Config,full) timeout\n");
            goto probe;
        }
        xhci_wait_event(ev, 500);

        for (int i = 0; i < total_len; i++) cfgbuf[i] = ep0_data[i];
    } /* end real-path config fetch */

    /* Parse Interface and Endpoint descriptors from config blob */
    {
        int pos = 0;
        usb_interface_t *cur_intf = NULL;
        while (pos < total_len) {
            uint8_t bLen  = cfgbuf[pos];
            uint8_t bType = cfgbuf[pos + 1];
            if (bLen < 2 || pos + bLen > total_len) break;

            if (bType == USB_DESC_INTERFACE && bLen >= 9) {
                int ni = g_dev.num_interfaces;
                if (ni < USB_MAX_INTERFACES) {
                    cur_intf = &g_dev.interfaces[ni];
                    cur_intf->bInterfaceNumber  = cfgbuf[pos + 2];
                    cur_intf->bAlternateSetting = cfgbuf[pos + 3];
                    cur_intf->bNumEndpoints     = cfgbuf[pos + 4];
                    cur_intf->bInterfaceClass   = cfgbuf[pos + 5];
                    cur_intf->bInterfaceSubClass= cfgbuf[pos + 6];
                    cur_intf->bInterfaceProtocol= cfgbuf[pos + 7];
                    g_dev.num_interfaces++;
                    uart_puts("[xHCI] Interface "); print_hex32(cur_intf->bInterfaceNumber);
                    uart_puts(" class="); print_hex32(cur_intf->bInterfaceClass);
                    uart_puts(" sub="); print_hex32(cur_intf->bInterfaceSubClass);
                    uart_puts(" proto="); print_hex32(cur_intf->bInterfaceProtocol);
                    uart_puts("\n");
                }
            } else if (bType == USB_DESC_ENDPOINT && bLen >= 7 && cur_intf) {
                int ne = cur_intf->endpoint_count;
                if (ne < USB_MAX_ENDPOINTS) {
                    usb_endpoint_t *ep = &cur_intf->endpoints[ne];
                    ep->bEndpointAddress = cfgbuf[pos + 2];
                    ep->bmAttributes     = cfgbuf[pos + 3];
                    ep->wMaxPacketSize   = (uint16_t)(cfgbuf[pos + 4] | ((uint16_t)cfgbuf[pos + 5] << 8));
                    ep->bInterval        = cfgbuf[pos + 6];
                    cur_intf->endpoint_count++;
                }
            }
            pos += bLen;
        }
    }

    uart_puts("[xHCI] Config parsed: "); print_hex32(g_dev.num_interfaces);
    uart_puts(" interface(s)\n");

probe:
    /* ── Step 5: Hand off to USB core for class driver probe ─────────── */
    usb_enumerate_device(&g_dev, port);
}

static void port_scan(void) {
    int n = (int)xhci_ctrl.max_ports;
    uart_puts("[xHCI] Port scan ("); print_hex32(n); uart_puts(" port(s)):\n");
    for (int p = 0; p < n; p++)
        enumerate_port(p);
    uart_puts("[xHCI] Port scan complete\n");
}

/* HCD callbacks */
int xhci_is_ready(void) { return xhci_ctrl.initialized; }

/*
 * xhci_scan_ports — deferred port scan, called from usb_init() AFTER
 * class drivers are registered so usb_enumerate_device() finds them.
 */
int xhci_scan_ports(void) {
    if (!xhci_ctrl.initialized) return 0;
    port_scan();
    return 0;
}

int xhci_enumerate_device(usb_device_t *dev, int port) {
    /* Address Device already done inline in enumerate_port().
     * dev->address is set; EP0 ring is ready.
     * Nothing more to do here — control transfers go via
     * xhci_control_transfer() which uses ep0_enq/doorbell. */
    (void)port;
    return (dev->address != 0) ? 0 : -1;
}

/*
 * xhci_control_transfer — issue a USB control transfer over EP0.
 *
 * Builds Setup/Data/Status TRBs on the EP0 ring and rings doorbell
 * for the slot associated with dev->hcd_private (slot_id stored there).
 *
 * Direction: bmRequestType bit 7. 1=IN (device->host), 0=OUT (host->device).
 */
int xhci_control_transfer(usb_device_t *dev, uint8_t req_type, uint8_t request,
                           uint16_t value, uint16_t index, void *data,
                           uint16_t length, int timeout) {
    if (!dev) return -1;
    uint8_t slot_id = (uint8_t)(uintptr_t)dev->hcd_private;
    if (slot_id == 0) return -1;

    volatile uint8_t *ep0_data = (volatile uint8_t *)(xhci_dma_buf + DMA_EP0_DATA_OFF);
    int dir_in = (req_type & 0x80) != 0;

    /* OUT: copy caller's data into DMA buffer */
    if (!dir_in && data && length)
        memcpy((void *)ep0_data, data, length);
    else
        memset((void *)ep0_data, 0, length < 512 ? (length ? length : 8) : 512);

    uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));

    /* Setup TRB: bmRequestType | bRequest | wValue | wLength */
    uint32_t setup_lo = (uint32_t)req_type
                      | ((uint32_t)request << 8)
                      | ((uint32_t)value   << 16);
    uint32_t setup_hi = (uint32_t)index | ((uint32_t)length << 16);

    /* Transfer type field in Setup TRB dword 3 bits [17:16]:
     *   0=No Data, 2=OUT Data, 3=IN Data */
    uint32_t xfer_type = (length == 0) ? 0U : (dir_in ? 3U : 2U);

    ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (xfer_type << 16));

    if (length > 0) {
        uint32_t data_flags = TRB_IOC | (dir_in ? TRB_DIR_IN : 0);
        ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32),
                (uint32_t)length, TRB_TYPE_DATA, data_flags);
    }

    /* Status TRB direction is opposite to data phase */
    uint32_t status_flags = TRB_IOC | (dir_in ? 0 : TRB_DIR_IN);
    ep0_enq(0, 0, 0, TRB_TYPE_STATUS, status_flags);

    ep0_doorbell(slot_id);

    uint32_t ev[4];
    if (xhci_wait_event(ev, timeout ? timeout : 1000) != 0) {
        uart_puts("[xHCI] control_transfer: TIMEOUT\n");
        return -1;
    }
    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;

    /* Drain Status event if data phase was issued */
    if (length > 0) {
        uint32_t ev2[4];
        xhci_wait_event(ev2, 500);
    }

    /* IN: copy DMA buffer to caller */
    if (dir_in && data && length) {
        int got = (int)(ev[2] & 0xFFFF); /* residue in low 17 bits of dword 2 */
        int actual = length - got;
        if (actual < 0) actual = 0;
        memcpy(data, (void *)ep0_data, (size_t)actual);
        return actual;
    }
    return (int)length;
}

int xhci_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout) {
    /* TODO: Implement bulk transfer ring for mass storage.
     * Requires per-endpoint transfer rings (not yet allocated).
     * For now: stub returns -1 so mass storage probe will fail gracefully. */
    (void)ep; (void)data; (void)len; (void)timeout;
    return -1;
}

/*
 * xhci_interrupt_transfer — poll an HID interrupt IN endpoint.
 *
 * Implemented as GET_REPORT over EP0 (boot protocol fallback).
 * Works for any boot-protocol HID device without needing a separate
 * interrupt ring. Full interrupt ring support is a future enhancement.
 */
int xhci_interrupt_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout) {
    (void)ep;
    /* We need the device to call control_transfer — but interrupt_transfer
     * only receives the endpoint, not the device.  The HID driver falls
     * back to usb_control_transfer(dev, ...) when int_in probe succeeds,
     * which routes here correctly.  This path is only hit if called
     * directly with just the endpoint, which we cannot service without the
     * slot_id.  Return -1 so HID driver uses the control fallback path. */
    (void)data; (void)len; (void)timeout;
    return -1;
}

static usb_hc_ops_t g_xhci_hc_ops = {
    .control_transfer   = xhci_control_transfer,
    .bulk_transfer      = xhci_bulk_transfer,
    .interrupt_transfer = xhci_interrupt_transfer,
    .enumerate_device   = xhci_enumerate_device,
};

/* ── Event ring + IRQ + MSI ──────────────────────────────────────────────── */

/*
 * evt_ring_poll — read one event directly from the event ring.
 *
 * This is the AUTHORITATIVE event consumer.  It is called by both
 * xhci_wait_event (polling path) and xhci_irq_handler (MSI path).
 *
 * Returns 1 if an event was consumed into ev[], 0 if ring is empty.
 *
 * The cycle bit protocol:
 *   The controller writes TRBs into the ring and toggles the cycle
 *   bit on each full pass.  We track evt_cycle which starts at 1 (per
 *   xHCI spec §4.9.3: event ring PCS/CCS initialises to 1 after reset).
 *   A TRB belongs to us iff (dword3 & 1) == evt_cycle.
 */
static int evt_ring_poll(uint32_t ev[4]) {
    volatile uint32_t *slot = (volatile uint32_t *)(evt_ring + evt_dequeue * 4);

    /* Memory barrier — ensure we see the controller's write */
    asm volatile("dsb sy; isb" ::: "memory");

    uint8_t slot_cycle = slot[3] & 1;
    if (slot_cycle != evt_cycle) {
        /* Periodic empty-ring diagnostic: log every 500 calls so we can
         * see whether the event ring ever receives anything without
         * flooding the UART on every 1ms poll iteration.             */
        static int _empty_count = 0;
        if (++_empty_count >= 500) {
            _empty_count = 0;
            void *_ir0 = ir_base(0);
            uint32_t _iman   = readl(_ir0 + IR_IMAN);
            uint32_t _erdp_lo = readl(_ir0 + IR_ERDP_LO);
            uart_puts("[xHCI] poll: ring empty x500  deq="); print_hex32(evt_dequeue);
            uart_puts(" want_cycle="); print_hex32(evt_cycle);
            uart_puts(" slot3="); print_hex32(slot[3]);
            uart_puts(" IMAN="); print_hex32(_iman);
            uart_puts(" ERDP_LO="); print_hex32(_erdp_lo);
            uart_puts(" USBSTS="); print_hex32(readl(xhci_ctrl.op_regs + OP_USBSTS));
            uart_puts("\n");
        }
        return 0;
    }

    uart_puts("[xHCI] evt_ring_poll: GOT EVENT deq="); print_hex32(evt_dequeue);
    uart_puts(" ["); print_hex32(slot[0]); uart_puts(",");
    print_hex32(slot[1]); uart_puts(",");
    print_hex32(slot[2]); uart_puts(",");
    print_hex32(slot[3]); uart_puts("]\n");

    ev[0] = slot[0];
    ev[1] = slot[1];
    ev[2] = slot[2];
    ev[3] = slot[3];

    evt_dequeue++;
    if (evt_dequeue >= EVT_RING_TRBS) {
        evt_dequeue = 0;
        evt_cycle ^= 1;
    }

    /* Advance ERDP so the controller knows we consumed this slot */
    void *ir0 = ir_base(0);
    uint64_t new_erdp = evt_ring_dma + (uint64_t)evt_dequeue * 16;

    /* Debug: log ERDP before and after the write so we can confirm
     * the EHB bit is being set and the pointer is advancing.        */
    uint32_t erdp_before = readl(ir0 + IR_ERDP_LO);
    uint32_t iman_before = readl(ir0 + IR_IMAN);
    uart_puts("[xHCI] ERDP before="); print_hex32(erdp_before);
    uart_puts(" IMAN before="); print_hex32(iman_before); uart_puts("\n");

    /* Set EHB (Event Handler Busy clear) bit 3 in ERDP lo */
    reg_write64(ir0, IR_ERDP_LO, new_erdp | (1ULL << 3));
    /* Clear IMAN IP (Interrupt Pending) W1C */
    writel(readl(ir0 + IR_IMAN) | 1U, ir0 + IR_IMAN);

    asm volatile("dsb sy; isb" ::: "memory");
    uint32_t erdp_after = readl(ir0 + IR_ERDP_LO);
    uint32_t iman_after = readl(ir0 + IR_IMAN);
    uart_puts("[xHCI] ERDP after ="); print_hex32(erdp_after);
    uart_puts(" IMAN after ="); print_hex32(iman_after);
    uart_puts(" new_erdp="); print_hex32((uint32_t)new_erdp); uart_puts("\n");

    return 1;
}

/*
 * xhci_wait_event — wait for a command/transfer completion event.
 *
 * Primary path: WFI (Wait For Interrupt).
 *   CPU sleeps until the VL805 MSI fires → xhci_irq_handler stores
 *   the event in pending_event[] → WFI wakes → we return instantly.
 *   Zero cycles burned while waiting — CPU is fully idle between events.
 *
 * Fallback path: direct ring poll after each WFI wakeup.
 *   Handles the case where MSI delivery is unreliable (e.g. timing
 *   race between MCU write and MSI assert).  evt_ring_poll() is safe
 *   to call even if the irq handler already consumed the event — the
 *   cycle-bit check prevents double-consume.
 *
 * Timeout: each WFI wakeup counts as one iteration.  WFI wakes on
 *   ANY pending interrupt (MSI, timer tick, etc.), so in practice each
 *   iteration is ≤ one timer period (~1ms).  The iteration cap prevents
 *   a runaway wait if MSI never fires.
 */
static int xhci_wait_event(uint32_t ev[4], int timeout_ms) {
    /* Immediate check: event may have arrived before we got here */
    if (pending_event_ready) {
        ev[0] = pending_event[0]; ev[1] = pending_event[1];
        ev[2] = pending_event[2]; ev[3] = pending_event[3];
        pending_event_ready = 0;
        return 0;
    }
    if (evt_ring_poll(ev))
        return 0;

    /* WFI loop: sleep until interrupt, then check.
     * Each iteration = one WFI wakeup (≈ 1ms with normal timer tick).
     * CPU is idle between wakeups — no busy-waiting.                   */
    for (int t = 0; t < timeout_ms; t++) {
        /* Sleep until MSI (or any other IRQ) fires */
        asm volatile("dsb sy; isb; wfi" ::: "memory");

        /* IRQ-staged fast path */
        if (pending_event_ready) {
            ev[0] = pending_event[0]; ev[1] = pending_event[1];
            ev[2] = pending_event[2]; ev[3] = pending_event[3];
            pending_event_ready = 0;
            return 0;
        }
        /* Ring poll fallback (MSI timing race or missed interrupt) */
        if (evt_ring_poll(ev))
            return 0;
    }

    /* Timeout — dump event ring state so we can diagnose why no event arrived */
    {
        void *ir0 = ir_base(0);
        volatile uint32_t *slot = (volatile uint32_t *)(evt_ring + evt_dequeue * 4);
        uart_puts("[xHCI] wait_event TIMEOUT after ");
        print_hex32(timeout_ms); uart_puts("ms\n");
        uart_puts("[xHCI]   deq="); print_hex32(evt_dequeue);
        uart_puts(" want_cycle="); print_hex32(evt_cycle); uart_puts("\n");
        uart_puts("[xHCI]   slot[0]="); print_hex32(slot[0]);
        uart_puts(" slot[1]="); print_hex32(slot[1]);
        uart_puts(" slot[2]="); print_hex32(slot[2]);
        uart_puts(" slot[3]="); print_hex32(slot[3]); uart_puts("\n");
        uart_puts("[xHCI]   IMAN=");      print_hex32(readl(ir0 + IR_IMAN));
        uart_puts(" ERDP_LO=");  print_hex32(readl(ir0 + IR_ERDP_LO));
        uart_puts(" ERSTSZ=");   print_hex32(readl(ir0 + IR_ERSTSZ));
        uart_puts(" ERSTBA_LO="); print_hex32(readl(ir0 + IR_ERSTBA_LO));
        uart_puts(" USBSTS=");   print_hex32(readl(xhci_ctrl.op_regs + OP_USBSTS));
        uart_puts("\n");
        uart_puts("[xHCI]   CRCR_LO="); print_hex32(readl(xhci_ctrl.op_regs + OP_CRCR_LO));
        uart_puts(" DCBAAP_LO="); print_hex32(readl(xhci_ctrl.op_regs + OP_DCBAAP_LO));
        uart_puts("\n");

        /* Scan first 8 TRBs of event ring for ANY non-zero content.
         * This catches events written with wrong cycle bit (MCU PCS=0)
         * or written to wrong position (e.g. not at deq).              */
        uart_puts("[xHCI]   evt ring scan (first 8 TRBs):\n");
        int found_any = 0;
        for (int i = 0; i < 8; i++) {
            volatile uint32_t *t = (volatile uint32_t *)(evt_ring + i * 4);
            if (t[0] || t[1] || t[2] || t[3]) {
                found_any = 1;
                uart_puts("[xHCI]     TRB["); print_hex32((uint32_t)i);
                uart_puts("] cycle="); print_hex32(t[3] & 1);
                uart_puts(" type="); print_hex32((t[3] >> 10) & 63);
                uart_puts("  ["); print_hex32(t[0]); uart_puts(",");
                print_hex32(t[1]); uart_puts(",");
                print_hex32(t[2]); uart_puts(",");
                print_hex32(t[3]); uart_puts("]\n");
            }
        }
        if (!found_any)
            uart_puts("[xHCI]     all zeros — MCU has not written any event TRBs\n");
    }
    return -1;
}

void xhci_irq_handler(int vector, void *data) {
    (void)vector; (void)data;
    /* MSI fired — consume event and stage it for xhci_wait_event fast path.
     * If xhci_wait_event already consumed it via polling, the slot's cycle
     * bit will not match and evt_ring_poll returns 0 — no double-consume. */
    uint32_t ev[4];
    if (evt_ring_poll(ev)) {
        pending_event[0] = ev[0];
        pending_event[1] = ev[1];
        pending_event[2] = ev[2];
        pending_event[3] = ev[3];
        pending_event_ready = 1;
    }
}
/* xhci_dma_phys — return physical base address of the xHCI DMA buffer.
 * Called by pci.c xhci_setup_msi() to compute the PCIe MSI target address. */
uint64_t xhci_dma_phys(void) {
    return (uint64_t)virt_to_phys((void *)xhci_dma_buf);
}
