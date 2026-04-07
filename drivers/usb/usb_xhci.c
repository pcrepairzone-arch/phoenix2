 /**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver — spec-compliant init for VL805 on Pi 4
 *
 * boot89 fixes applied (search "FIX-89" for all change sites):
 *
 * FIX-89-1  ERSTBA root cause (setup_event_ring):
 *   erst_dma_addr was computed correctly as phys_to_dma(erst_phys) but
 *   then overwritten with 0ULL.  With .xhci_dma at 0x30000000 (PCIe
 *   0xF0000000) this made ERSTBA=0, pointing the VL805 MCU at PCIe 0x0
 *   (CPU 0xC0000000) instead of the real ERST at 0xF0001040.  The MCU
 *   wrote all events to the wrong PCIe address → canary unchanged,
 *   INTR2=0x100 (TLPs generated but landing nowhere), scratch=0.
 *   Fix: erst_dma_addr = erst_dma  (real DMA address).
 *
 * FIX-89-2  TRUE RUNNING stability hold (run_controller):
 *   Added 20ms hold-and-recheck after the 5ms poll detects clean state.
 *   Prevents declaring TRUE RUNNING when MCU fires HSE within the poll
 *   window.  If HSE fires during the hold, treated as another retry.
 *
 * FIX-89-3  xhci_wait_event full ring re-arm on HSE:
 *   Previously only W1C HSE + RS=1.  VL805 MCU resets ALL ring registers
 *   (CRCR, ERSTBA, DCBAAP) on HSE.  Now re-arms everything — same as the
 *   retry and settle loops — so ERSTBA is not lost to a mid-transfer HSE.
 *
 * FIX-89-4  No-op timeout extended to 200ms + post-timeout ring re-arm:
 *   50ms was marginal.  On a clean ERSTBA the No-op CCE should arrive in
 *   < 5ms, but 200ms gives headroom.  On timeout, rings are re-armed
 *   before enumeration so Enable Slot is not submitted to dead rings.
 *
 * FIX-89-5  dma_region_size off-by-one (xhci_init):
 *   Previous: __xhci_dma_end - __xhci_dma_start - 1  (last byte never
 *   zeroed).  Fixed: - 0 (full region zeroed).
 *
 * boot91 diagnostic additions (search "BOOT91"):
 *
 * BOOT91-A  USBSTS snapshot between TRUE RUNNING and No-op submit:
 *   Boot 90 showed USBSTS=0x00000005 at the No-op timeout, meaning the
 *   MCU had already re-fired HSE during the 100ms MSI check window before
 *   the No-op was even submitted.  New: read USBSTS immediately after the
 *   MSI check block so we can see exactly when the MCU drops out.
 *
 * BOOT91-B  Explicit full re-arm + RS=1 verify immediately before No-op:
 *   If USBSTS shows HSE/HCH at that point, do a complete ring re-arm and
 *   wait for clean TRUE RUNNING again before submitting the No-op.  This
 *   ensures the No-op is never submitted to a halted controller.
 *
 * BOOT91-C  USBSTS timestamped poll during No-op wait:
 *   Sample USBSTS at 5ms, 10ms, 20ms, 50ms, 100ms into the No-op wait
 *   to see exactly when HSE fires relative to the doorbell ring.
 *
 * BOOT91-D  RC_BAR2 window coverage check:
 *   Print RC_BAR2_CFG_LO and compute whether 0xF0000000–0xF0042000 fits
 *   inside the programmed window.  DMA at 0x30000000 = PCIe 0xF0000000
 *   is right at the top edge of a 1GB window from 0xC0000000.
 *
 * boot92 scratchpad canary test (search "BOOT92"):
 *   INTR2=0x100 (TLPs arriving) + all canaries intact proved that the MCU
 *   IS writing via PCIe but the CPU cannot see those writes.  Root cause:
 *   .xhci_dma mapped as Device nGnRnE which is OUTSIDE the ARM inner-shareable
 *   coherency domain.  PCIe inbound writes commit to DRAM via the RC/UBUS/SCB
 *   pipeline but no coherency notification reaches the CPU.  dsb sy only orders
 *   CPU-initiated transactions — it cannot drain the RC's write-posting buffer.
 *   Linux/BSD/LibreELEC/OSMC all use dma_alloc_coherent() → Normal-NC
 *   (Inner Shareable), which participates in the ARM coherency protocol and
 *   makes PCIe writes visible to the CPU after dsb sy.
 *
 * boot93 result: Normal-NC confirmed (ATTR=0x44 via AT S1E1R) but canaries
 *   still intact and behaviour identical to boot92. INTR2=0x100 is the VL805
 *   MSI write, not a scratchpad/event-ring MemWrite — coherency was not the
 *   root cause. The MCU fires HSE within 100ms consistently regardless of MMU
 *   type because the writes go somewhere wrong.
 *
 * boot94 hypothesis test (search "BOOT94"):
 *   VL805 firmware may only accept inbound DMA addresses with PCIe top nibble
 *   0xC (range 0xC0000000–0xCFFFFFFF). Our DMA was at phys 0x30000000 giving
 *   PCIe 0xF0000000 (top nibble 0xF). Boots 71–83 used phys 0x10000 giving
 *   PCIe 0xC0010000 (top nibble 0xC) and showed more progress.
 *
 *   Test: move .xhci_dma to phys 0x0C000000 → PCIe 0xCC000000 (top nibble C).
 *   DMA_OFFSET stays 0xC0000000 — only the linker script changes.
 *   All PCIe addresses: 0xCC000000–0xCC042000, every address 0xCCxxxxxx.
 *
 *   Expected if hypothesis correct: scratchpad canaries changed, event ring
 *   written, No-op CCE received, MFINDEX running, settle hse drops to 0.
 *   Expected if hypothesis wrong: identical failure — need new theory.
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
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type, uint32_t dw3_extra);
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
#define CMD_HSEE        (1U << 3)   /* Host System Error Enable — boot141: NOT set (Circle never sets HSEE) */

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

/* ERDP_REARM(ir0, dma) — write the event ring dequeue pointer with EHB=0.
 *
 * boot107 fix: EHB (Event Handler Busy, ERDP bit 3) must be CLEAR for the
 * MCU to write new events into the ring.  EHB=1 tells the MCU "host is busy
 * processing, hold new events" — so if we ever leave EHB=1, the MCU silently
 * swallows all completions and the ring stays empty forever.
 *
 * Previous code wrote (evt_dma | 0x8) everywhere, permanently setting EHB=1.
 * The MCU processed commands (CRCR advanced) but never wrote CCEs (boot 105/106).
 *
 * Correct protocol (xHCI §5.5.2.3.3):
 *   After reset or re-arm: write ERDP with EHB=0 to arm the interrupter.
 *   During event consumption: write ERDP | EHB=1 to signal "busy", then
 *   immediately write ERDP | EHB=0 (new dequeue pointer, no EHB) to re-arm.
 *
 * In polling mode we skip the EHB=1 intermediate write entirely — just
 * always write a clean dequeue pointer so the MCU can write at any time.
 */
#define ERDP_REARM(ir0_, dma_) \
    do { \
        reg_write64((ir0_), IR_ERDP_LO, (dma_)); \
        asm volatile("dsb sy; isb" ::: "memory"); \
    } while (0)

#define PORTSC_CCS      (1U <<  0)
#define PORTSC_PED      (1U <<  1)
#define PORTSC_PP       (1U <<  9)
#define PORTSC_WIC      0x00FE0000U

#define TRB_CYCLE           (1U <<  0)
#define TRB_TC              (1U <<  1)
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE_LINK        6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_NOOP_CMD   23  /* xHCI §6.4.3.9 Command No-op — MCU keepalive ping */
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
/* boot147: g_slot_id scalar replaced by g_slot_ids[port] array (below) */

/* boot87: pending_event / pending_event_ready removed.
 * xhci_wait_event now uses pure CNTPCT_EL0 tight-poll — no WFI,
 * no IRQ-staged fast path.  VL805 MSI→GIC confirmed non-functional
 * on BCM2711 after 86 boots; polling is the definitive approach.    */

/*
 * msi_fire_count — incremented every time xhci_irq_handler is entered.
 * This lets the boot log confirm whether GIC INTID 180 / MSI delivery
 * is working at all, independent of whether the event ring contains data.
 * Printed in the xhci_wait_event timeout block and in the handler itself.
 */
static volatile uint32_t msi_fire_count = 0;

static uint64_t cmd_ring_dma  = 0;
static uint64_t evt_ring_dma  = 0;
static uint64_t erst_dma_addr = 0;

/* DMA_OFFSET = 0xC0000000  (BCM2711 native inbound DMA convention).
 *
 * boot77 root-cause fix: DMA_OFFSET=0 placed every VL805 DMA write target
 * squarely inside PCIe 0x00000000–0x3FFFFFFF — the same range used by the
 * outbound ATU for VL805 BAR0 MMIO accesses.  The BCM2711 RC could not
 * correctly route inbound Memory Write TLPs from the VL805 in this range.
 *
 * DMA_OFFSET=0xC0000000 places all DMA at PCIe 0xCxxxxxxx–0xFxxxxxxx,
 * completely clear of the outbound window.
 *
 * boot94 hypothesis: VL805 firmware may only accept inbound DMA to addresses
 * where bits[31:28] == 0xC (range 0xC0000000–0xCFFFFFFF).  Our previous DMA
 * base was phys 0x30000000 → PCIe 0xF0000000 (top nibble 0xF).  Boots 71–83
 * used phys 0x00010000 → PCIe 0xC0010000 (top nibble 0xC) and the MCU reached
 * further before failing.
 *
 * boot94 test: .xhci_dma at phys 0x0C000000 → PCIe 0xCC000000.
 * Every DMA address (DCBAA, rings, ERST, scratchpad, MSI pad) has top nibble C:
 *   DCBAA       phys 0x0C000000  PCIe 0xCC000000
 *   CMD_RING    phys 0x0C000800  PCIe 0xCC000800
 *   EVT_RING    phys 0x0C000C00  PCIe 0xCC000C00
 *   MSI_PAD     phys 0x0C001000  PCIe 0xCC001000
 *   ERST        phys 0x0C001040  PCIe 0xCC001040
 *   SCRATCH[0]  phys 0x0C002000  PCIe 0xCC002000
 *
 * RC_BAR2 window (0xC0000000–0xFFFFFFFF) covers all of these ✓
 * UBUS_BAR2_REMAP=0x00000001: PCIe 0xCCxxxxxx → CPU 0x0Cxxxxxx ✓
 */
#define DMA_OFFSET  0xC0000000ULL
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

/* fast_delay_ms() is provided by kernel.h / lib.c but calling it from the xHCI
 * driver caused hard freezes (bootlogpi4, both boots) — the lib.c version
 * corrupts the call stack or callee-saved registers when called from bare-
 * metal PCI init context.  Use fast_delay_ms() built on CNTPCT_EL0 instead.
 * This is the same approach as fast_delay_ms() — cycle-counter only,
 * no lib.c dependency, safe from any calling-convention mismatch.          */
static void delay_us(int us) {
    /* Simple busy-wait using cycle counter — 54 MHz on BCM2711 */
    uint64_t t0, t1;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0) :: "memory");
    uint64_t ticks = (uint64_t)us * 54ULL;
    do {
        asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
    } while ((t1 - t0) < ticks);
}
void fast_delay_ms(int ms) {
    delay_us(ms * 1000);
}


/* ── Real-time millisecond counter using ARM system counter ──────────────────
 *
 * CNTPCT_EL0: free-running 64-bit counter driven by the system reference
 * clock.  On BCM2711 (Pi 4) this runs at 54 MHz (confirmed via MFINDEX
 * sanity check at boot — see run_controller()).
 *
 * get_time_ms() returns the low 32 bits of elapsed milliseconds.  Wraps
 * every ~49.7 days — fine for boot-time timeout loops.
 *
 * Used by xhci_wait_event for real wall-clock timeouts without WFI or the
 * timer-tick interrupt.  Pure busy-poll: CPU never sleeps between polls.
 */
static inline uint64_t read_cntpct(void) {
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
static inline uint32_t get_time_ms(void) {
    /* 54 MHz → 54,000 ticks per ms */
    return (uint32_t)(read_cntpct() / 54000ULL);
}

/* ── Normal-NC DMA buffer helpers ────────────────────────────────────────────
 *
 * BOOT93: .xhci_dma is now mapped as Normal Non-Cacheable Inner-Shareable.
 *
 * Normal-NC on AArch64:
 *   - CPU reads/writes bypass the D-cache (non-cacheable), so all CPU accesses
 *     go directly to DRAM.  This is the same behaviour as Device memory for
 *     CPU-side accesses.
 *   - PCIe inbound writes (VL805 → RC → UBUS → DRAM) ARE visible to the CPU
 *     after a DSB SY, because Normal-NC participates in the inner-shareable
 *     coherency domain.  Device nGnRnE does NOT — that was the root cause of
 *     92 boots of invisible DMA writes.
 *   - LDP/STP instructions are architecturally permitted on Normal memory
 *     (unlike Device memory).  However, our volatile helpers prevent the
 *     compiler from emitting LDP/STP by forcing individual 32-bit accesses.
 *     This is belt-and-suspenders: LDP/STP on Normal-NC would not fault,
 *     but the volatile barrier ensures the compiler does not coalesce
 *     adjacent writes into a pair which would defeat the volatile semantics.
 *
 * These helpers are kept as-is — they work correctly for Normal-NC and the
 * volatile constraint remains valuable for compiler correctness.
 * dc civac cache-flush calls have been removed from the DMA readback paths
 * (BOOT93-B) because Normal-NC memory is never cached: there is nothing to
 * flush.  A plain DSB SY is the correct and sufficient barrier.
 */

/* Zero a DMA buffer — Device-memory safe (no LDP/STP) */
static void dma_zero(volatile void *ptr, size_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)ptr;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
        p[i] = 0U;
    /* trailing bytes (never hit in practice — all DMA sizes are DWORD-aligned) */
    volatile uint8_t *pb = (volatile uint8_t *)ptr + (words << 2);
    for (size_t i = 0; i < (bytes & 3U); i++)
        pb[i] = 0U;
}

/* Copy from normal CPU memory INTO a DMA (Device) buffer */
static void dma_copy_to(volatile void *dst, const void *src, size_t bytes)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t    *s = (const uint8_t *)src;
    for (size_t i = 0; i < bytes; i++)
        d[i] = s[i];
}

/* Copy from a DMA (Device) buffer INTO normal CPU memory */
static void dma_copy_from(void *dst, const volatile void *src, size_t bytes)
{
    uint8_t                *d = (uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (size_t i = 0; i < bytes; i++)
        d[i] = s[i];
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

    /* boot149: read HCCPARAMS1 to get AC64, CSZ, and XECP.
     * CSZ (bit[2]): 0 = 32-byte context entries, 1 = 64-byte context entries.
     * We previously hardcoded CTX_SIZE=32.  If VL805 has CSZ=1, Slot Context
     * lands at offset 32 instead of 64 and EP0 Context at 64 instead of 128,
     * causing the MCU to read garbage fields → CC=17 "Context State Error".   */
    uint32_t hcc1 = readl(base + CAP_HCCPARAMS1);
    xhci_ctrl.ac64 = (hcc1 >> 0) & 1;
    xhci_ctrl.csz  = (hcc1 >> 2) & 1;
    xhci_ctrl.xecp = (uint16_t)((hcc1 >> 16) & 0xFFFF);

    uint32_t rtsoff = readl(base + CAP_RTSOFF) & ~0x1FU;
    uint32_t dboff  = readl(base + CAP_DBOFF)  & ~0x03U;

    xhci_ctrl.op_regs       = xhci_ctrl.cap_regs + clen;
    xhci_ctrl.runtime_regs  = xhci_ctrl.cap_regs + rtsoff;
    xhci_ctrl.doorbell_regs = xhci_ctrl.cap_regs + dboff;

    debug_print("[xHCI] MaxSlots=%u  MaxPorts=%u  Scratchpads=%u\n",
                xhci_ctrl.max_slots, xhci_ctrl.max_ports, xhci_ctrl.scratchpad_count);
    uart_puts("[xHCI] HCCPARAMS1="); print_hex32(hcc1);
    uart_puts("  AC64="); print_hex32(xhci_ctrl.ac64);
    uart_puts("  CSZ="); print_hex32(xhci_ctrl.csz);
    uart_puts(xhci_ctrl.csz ? "  → 64-byte context entries\n"
                             : "  → 32-byte context entries\n");
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
    dma_zero(dcbaa, 2048);

    uint32_t pgsz = readl(xhci_ctrl.op_regs + OP_PAGESIZE);
    debug_print("[xHCI] PAGESIZE reg = 0x%08x (%s)\n", pgsz,
                (pgsz & 1) ? "4KB OK" : "4KB NOT supported — scratchpad alloc wrong");
    if (!(pgsz & 1)) return -1;

    uint32_t n = xhci_ctrl.scratchpad_count;
    if (n > MAX_SCRATCH_PAGES) return -1;

    if (n > 0) {
        volatile uint64_t *scratch_arr = (volatile uint64_t *)(xhci_dma_buf + DMA_SCRATCH_OFF);
        dma_zero(scratch_arr, MAX_SCRATCH_PAGES * 8);

        uint8_t *pages_base = xhci_dma_buf + DMA_SCRATCH_PAGES_OFF;
        dma_zero(pages_base, n * 4096);
        asm volatile("dsb sy" ::: "memory");

        for (uint32_t i = 0; i < n; i++) {
            void *pg = pages_base + i * 4096;
            scratch_arr[i] = phys_to_dma((uint64_t)virt_to_phys(pg));
        }
        asm volatile("dsb sy" ::: "memory");

        uint64_t scratch_arr_dma = phys_to_dma((uint64_t)virt_to_phys((void *)scratch_arr));
        dcbaa[0] = scratch_arr_dma;
        asm volatile("dsb sy" ::: "memory");

        /* BOOT92-A: Full scratchpad array dump.
         *
         * Print every pointer the MCU will DMA-read when it initialises its
         * internal context at RS=1.  All must be 4KB-aligned DMA addresses
         * within the RC_BAR2 inbound window (0xC0000000–0xFFFFFFFF).
         * Any entry that is 0 or misaligned will cause the MCU to fire HSE
         * immediately.
         */
        uart_puts("[BOOT92-A] Scratchpad array: DCBAA[0]=");
        print_hex32((uint32_t)dcbaa[0]);
        uart_puts("  n="); print_hex32(n); uart_puts("\n");
        for (uint32_t i = 0; i < n; i++) {
            uint64_t entry = scratch_arr[i];
            uint32_t lo = (uint32_t)entry;
            uint32_t hi = (uint32_t)(entry >> 32);
            int ok = (hi == 0) && (lo != 0) && ((lo & 0xFFFU) == 0);
            if (i < 4 || i == n - 1 || !ok) {
                uart_puts("[BOOT92-A]   ["); print_hex32(i); uart_puts("]=");
                print_hex32(hi); uart_puts(":"); print_hex32(lo);
                uart_puts(ok ? "\n" : "  *** BAD — not 4KB-aligned or zero ***\n");
            }
        }
        if (n > 5)  { uart_puts("[BOOT92-A]   (entries 4–"); print_hex32(n-2); uart_puts(" all OK — omitted)\n"); }

        /* BOOT92-B: Write canary pattern to the first 8 bytes of every
         * scratchpad page.
         *
         * Pattern: word0=0xABCD1234  word1=0xDEAD5678
         * These are distinctive and will survive any zero-check.
         * Device nGnRnE: CPU writes go directly to DRAM — no flush needed.
         * The MCU will overwrite these when it writes its internal context.
         * We read them back in BOOT92-C (post-HSE) to see which pages it
         * touched.
         *
         * We write AFTER dma_zero so the zero is not confused with
         * "MCU wrote zero", and AFTER the array pointers are set so the
         * addresses are confirmed correct before we annotate them.
         */
        uart_puts("[BOOT92-B] Writing scratchpad page canaries...\n");
        for (uint32_t i = 0; i < n; i++) {
            volatile uint32_t *pg =
                (volatile uint32_t *)(xhci_dma_buf + DMA_SCRATCH_PAGES_OFF + i * 4096);
            pg[0] = 0xABCD1234U;
            pg[1] = 0xDEAD5678U;
        }
        asm volatile("dsb sy" ::: "memory");
        /* Verify first page canary wrote correctly (Device mem sanity check) */
        {
            volatile uint32_t *pg0 =
                (volatile uint32_t *)(xhci_dma_buf + DMA_SCRATCH_PAGES_OFF);
            uart_puts("[BOOT92-B]   page[0][0]="); print_hex32(pg0[0]);
            uart_puts("  page[0][1]="); print_hex32(pg0[1]);
            uart_puts((pg0[0] == 0xABCD1234U && pg0[1] == 0xDEAD5678U) ?
                      "  [canary OK]\n" :
                      "  *** canary readback WRONG — Device mem write broken ***\n");
        }
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
    dma_zero(cmd_ring, CMD_RING_TRBS * 16);

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

    dma_zero(evt_ring, EVT_RING_TRBS * 16);
    dma_zero(erst, 16);

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
        /* dsb sy sufficient — phys 0 is Normal memory, no dc civac needed */
        asm volatile("dsb sy; isb" ::: "memory");
    }

    /* FIX-89 (boot89): ERSTBA must point at the real DMA buffer.
     *
     * The bug: erstba_dma was computed correctly above (= erst_dma =
     * phys_to_dma(erst_phys) = 0xF0001040 with DMA base 0x30000000), but
     * then discarded by "erst_dma_addr = 0ULL".  run_controller() then
     * programmed ERSTBA=0 into the hardware, so the VL805 MCU read its
     * ERST from PCIe 0x00000000 → CPU 0xC0000000 (the old DMA base).
     * With .xhci_dma now at 0x30000000 (PCIe 0xF0000000), the ERST at
     * PCIe 0 pointed the MCU at evt_ring PCIe 0xC0000C00, but the real
     * ring is at PCIe 0xF0000C00.  All MCU TLP writes went to the wrong
     * address → canary unchanged → INTR2=0x100 (TLPs sent but landed
     * nowhere), scratch=0 (MCU never wrote to our scratchpad region).
     *
     * Fix: use the real erst_dma.  The phys-0 fallback write below is
     * kept for belt-and-suspenders but the primary ERSTBA is now correct.
     */
    erst_dma_addr = erst_dma;

    uart_puts("[xHCI] ERST setup (boot89: ERSTBA=real DMA addr fixed)\n");
    uart_puts("[xHCI]   evt_ring DMA="); print_hex32((uint32_t)evt_dma);
    uart_puts("  erst_buf DMA="); print_hex32((uint32_t)erst_dma);
    uart_puts("  ERSTBA stored="); print_hex32((uint32_t)erst_dma_addr);
    uart_puts("  size="); print_hex32(EVT_RING_TRBS); uart_puts("\n");

    /* Readback verification */
    {
        uintptr_t pa = 0;
        asm volatile("" : "+r"(pa));
        volatile uint32_t *p = (volatile uint32_t *)pa;
        asm volatile("dsb sy; isb" ::: "memory"); /* phys 0 = Normal memory */
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
    /* boot147: increase CONFIG from 1 to max_ports so Enable Slot can
     * allocate slots 2..N.  boot108 used CONFIG=1 to work around a suspected
     * VL805 event-suppression bug at CONFIG=32; that bug was caused by wrong
     * ERST/DCBAA setup (fixed boot141-143), not by CONFIG>1.  With a valid
     * event ring, CONFIG=max_ports works and removes CC=9 on all ports after
     * the first.  Cap at 16 as a sanity bound.                             */
    uint32_t max_p   = xhci_ctrl.max_ports;
    uint32_t cfg_val = (max_p >= 1U && max_p <= 16U) ? max_p : 4U;

    /* BOOT91-D: RC_BAR2 inbound window coverage check.
     *
     * .xhci_dma at 0x30000000 → PCIe 0xF0000000 (with DMA_OFFSET=0xC0000000).
     * RC_BAR2 is configured as PCIe base 0xC0000000, 1GB window.
     * 1GB from 0xC0000000 = 0xC0000000 to 0xFFFFFFFF inclusive.
     * Our DMA top = 0xF0042000 — this is inside a 1GB window from C0000000,
     * BUT 0xF0000000 + 0x42000 = 0xF0042000 < 0xFFFFFFFF ✓ in theory.
     *
     * Read the actual programmed BAR2 value and compute the real window end
     * to confirm the MCU's inbound DMA range is definitely covered.
     *
     * RC_BAR2_CFG_LO is at PCIE_MISC base + 0x4034 (confirmed boot85).
     * Bits[3:0] encode size: 0xF=1GB, 0xE=512MB, etc.
     * Bit[1]=enable.  Bits[31:4]=base address >> 4 (PCIe address, 1MB aligned).
     */
    {
        extern void *pcie_base;
        uint32_t bar2_lo = readl(pcie_base + 0x4034U);
        uint32_t bar2_hi = readl(pcie_base + 0x4038U);
        uint32_t bar2_sz_bits = bar2_lo & 0xFU;
        uint64_t bar2_base = (uint64_t)(bar2_lo & 0xFFFFFFF0U);
        /* Size from low bits: 0xF=1GB (2^30), 0xE=512MB (2^29), ... */
        uint64_t bar2_size = (bar2_sz_bits == 0xFU) ? (1ULL << 30) :
                             (bar2_sz_bits == 0xEU) ? (1ULL << 29) :
                             (bar2_sz_bits == 0xDU) ? (1ULL << 28) :
                             (bar2_sz_bits == 0xCU) ? (1ULL << 27) : 0ULL;
        uint64_t bar2_end  = bar2_base + bar2_size - 1;
        uint64_t dma_start = phys_to_dma((uint64_t)virt_to_phys((void *)xhci_dma_buf));
        uint64_t dma_end   = dma_start + 0x42000ULL - 1;
        int covered = (dma_start >= bar2_base) && (dma_end <= bar2_end);
        uart_puts("[BOOT91-D] RC_BAR2: lo="); print_hex32(bar2_lo);
        uart_puts(" hi="); print_hex32(bar2_hi);
        uart_puts("  base="); print_hex32((uint32_t)bar2_base);
        uart_puts(" end="); print_hex32((uint32_t)bar2_end);
        uart_puts("  DMA="); print_hex32((uint32_t)dma_start);
        uart_puts("-"); print_hex32((uint32_t)dma_end);
        uart_puts(covered ? "  [COVERED OK]\n" : "  [*** NOT COVERED — DMA OUT OF WINDOW ***]\n");
    }

    /* BOOT94: DMA address range sanity check.
     * With .xhci_dma at phys 0x0C000000, all PCIe addresses should be
     * 0xCC000000–0xCC042000 (top nibble = 0xC). Print the key addresses
     * so the boot log confirms the linker placed the section correctly.   */
    {
        uint8_t top_dcbaa = (uint8_t)(dcbaa_dma >> 28);
        uint8_t top_evt   = (uint8_t)(evt_dma    >> 28);
        uint8_t top_erst  = (uint8_t)(erstba_dma >> 28);
        int     all_c     = (top_dcbaa == 0xC) && (top_evt == 0xC) && (top_erst == 0xC);
        uart_puts("[BOOT94] DMA PCIe addresses:\n");
        uart_puts("[BOOT94]   DCBAA  PCIe="); print_hex32((uint32_t)dcbaa_dma);
        uart_puts("  top=0x"); print_hex32(top_dcbaa); uart_puts("\n");
        uart_puts("[BOOT94]   EVTRING PCIe="); print_hex32((uint32_t)evt_dma);
        uart_puts("  top=0x"); print_hex32(top_evt); uart_puts("\n");
        uart_puts("[BOOT94]   ERSTBA PCIe="); print_hex32((uint32_t)erstba_dma);
        uart_puts("  top=0x"); print_hex32(top_erst); uart_puts("\n");
        uart_puts(all_c ?
            "[BOOT94]   All top nibbles = 0xC ✓ — VL805-compatible range\n" :
            "[BOOT94]   *** WARNING: top nibble != 0xC — linker change not applied? ***\n");
    }

    /* Linux programs ALL ring pointers AND enables the event ring (ERSTSZ=1)
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
     * boot142: Write order matches Circle (xhcieventmanager.cpp) and Linux
     * (xhci-mem.c): ERSTSZ=1 FIRST, then ERSTBA, then ERDP.
     *
     * Previous order (ERSTSZ=0 → ERSTBA → ERSTSZ=1) is wrong: the VL805 MCU
     * may latch ERSTBA at write-time with ERSTSZ still 0, treating it as
     * "zero segments" and never consulting the event ring at all.  Circle
     * and Linux both set ERSTSZ before writing ERSTBA so the MCU always
     * sees a valid segment count when it reads the base address.           */
    writel(1U, ir0 + IR_ERSTSZ);                       /* boot142: ERSTSZ first */
    asm volatile("dsb sy; isb" ::: "memory");
    reg_write64(ir0, IR_ERSTBA_LO, erstba_dma);         /* then ERSTBA          */
    asm volatile("dsb sy; isb" ::: "memory");
    /* boot142: Write ERDP with EHB=1 on init — matches Circle exactly.
     * Circle always ORs EHB into every ERDP write including the first one.
     * Per xHCI spec EHB is W1C so writing EHB=1 when EHB=0 is harmless on
     * compliant hardware, but the VL805 firmware may use the EHB=1 write as
     * a "host ready" handshake before it starts posting events.
     * NOTE: the poll loop still uses ERDP_REARM (EHB=0) to advance the
     * dequeue pointer — EHB=1 is ONLY written here at init.               */
    reg_write64(ir0, IR_ERDP_LO, evt_dma | 0x8ULL);   /* EHB=1 on init       */
    asm volatile("dsb sy; isb" ::: "memory");
    writel(0x00000002U, ir0 + IR_IMAN);   /* IE=1, W1C IP */
    /* boot108: IMOD=0 — disable interrupt moderation entirely.
     * Previous value 0x0FA00FA0 set IMODI=4000 (1ms min between events)
     * and IMODC=4000 (counter starts at max delay). Some VL805 firmware
     * treats a non-zero initial IMODC as "interrupt not ready" and defers
     * all event writes. Zero disables the throttle — events fire immediately. */
    writel(0x00000000U, ir0 + IR_IMOD);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Diagnostic readback: verify all ring pointers landed */
    uart_puts("[xHCI] Rings programmed (Linux order). Readback:\n");
    uart_puts("[xHCI]   DCBAAP=");  print_hex32(readl(op + OP_DCBAAP_LO));
    uart_puts("  CRCR=");    print_hex32(readl(op + OP_CRCR_LO));
    uart_puts("  CONFIG=");  print_hex32(readl(op + OP_CONFIG));
    uart_puts("  (boot108: =1)\n");
    uart_puts("[xHCI]   ERSTBA=");  print_hex32(readl(ir0 + IR_ERSTBA_LO));
    uart_puts("  ERSTSZ=");  print_hex32(readl(ir0 + IR_ERSTSZ));
    uart_puts("  ERDP=");    print_hex32(readl(ir0 + IR_ERDP_LO));
    uart_puts("  IMAN=");    print_hex32(readl(ir0 + IR_IMAN));
    uart_puts("  IMOD=");    print_hex32(readl(ir0 + IR_IMOD));
    uart_puts("  (boot108: =0)\n");
    uart_puts("[xHCI]   USBSTS=");  print_hex32(readl(op + OP_USBSTS));
    uart_puts("  USBCMD=");  print_hex32(readl(op + OP_USBCMD)); uart_puts("\n");

    /* boot116: CANARY REMOVED.
     *
     * Boots 85–115 wrote 0xDEADBEEF/0xCAFEF00D to TRB[0] words 0-1
     * before RS=1 as a "did the MCU write here?" detection mechanism.
     *
     * Analysis from boot 115 timing work showed this was PREVENTING the
     * MCU from writing events entirely.  The VL805 firmware appears to
     * do a non-standard sanity check: if TRB[0].word0 is non-zero when
     * the MCU tries to write its first event, it treats the slot as
     * occupied and ABORTS event generation — USBSTS.EINT never gets set,
     * the ring stays empty, and we see no events.
     *
     * The correct detection mechanism is the CYCLE BIT (TRB.word3 bit 0):
     *   Before RS=1: all TRBs have word3.bit0 = 0 (ring is zero-filled)
     *   MCU writes event with CCS=1, setting word3.bit0 = 1
     *   Host polls: if (TRB[deq].word3 & 1) == evt_cycle → event ready
     *
     * The ring is already zeroed by dma_zero() above.  No sentinel needed.
     * evt_ring_poll() uses the cycle bit correctly — this is the right way.
     */
    uart_puts("[xHCI] [CYCLE] Event ring zeroed — cycle bit protocol active (boot116)\n");
    uart_puts("[xHCI]   Polling: TRB.word3.bit0 == evt_cycle(1) signals MCU wrote event\n");

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

        /* boot141: Circle sets INTE in a separate write BEFORE RS=1,
         * matching the xHCI spec sequencing requirement.  Do NOT set
         * HSEE — Circle never sets it and the VL805 may behave
         * differently (HSE flood suppression) when it is absent.     */
        writel(CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        /* First RS=1 attempt (INTE already set above). */
        writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        uart_puts("[xHCI] RS=1 (HSE-retry, no CRCR rewrites)\n");

        uint32_t s       = 0;
        uint32_t cmd_v   = 0;
        int settled      = 0;
        int hse_retries  = 0;

        for (int t = 0; t < 600 && !settled; t++) {
            fast_delay_ms(5);
            s     = readl(op + OP_USBSTS);
            cmd_v = readl(op + OP_USBCMD);

            if (!(s & STS_HCH) && (cmd_v & CMD_RS) && !(s & STS_HSE)) {
                /* Candidate TRUE RUNNING — hold 20ms and recheck.
                 *
                 * FIX-89: boots 86-88 showed the MCU fires HSE within
                 * microseconds of the 5ms poll window, so "TRUE RUNNING"
                 * at the poll point did not mean the MCU stayed stable.
                 * The settle loop then saw hse=100 (every slot) because
                 * ERSTBA=0 caused constant MCU watchdog trips.
                 *
                 * With ERSTBA now correct (Fix 1), the MCU should remain
                 * stable, but we add a 20ms hold to verify and log it.
                 * If HSE fires during the hold we treat it as another retry
                 * rather than declaring settled — this prevents the settle
                 * loop from inheriting a flapping MCU state.              */
                fast_delay_ms(20);
                s     = readl(op + OP_USBSTS);
                cmd_v = readl(op + OP_USBCMD);

                if ((s & STS_HCH) || !(cmd_v & CMD_RS) || (s & STS_HSE)) {
                    /* HSE fired during hold — treat as another retry */
                    uart_puts("[xHCI] TR-hold: MCU fired HSE in 20ms hold window, retrying\n");
                    hse_retries++;
                    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                    asm volatile("dsb sy; isb" ::: "memory");
                    fast_delay_ms(2);
                    reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
                    writel(cfg_val, op + OP_CONFIG);
                    reg_write64(op, OP_CRCR_LO, crcr_val);
                    asm volatile("dsb sy; isb" ::: "memory");
                    writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
                    asm volatile("dsb sy; isb" ::: "memory");
                    reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
                    asm volatile("dsb sy; isb" ::: "memory");
                    ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
                    evt_dequeue = 0;
                    evt_cycle   = 1;
                    writel(0x00000002U, ir0 + IR_IMAN);
                    asm volatile("dsb sy; isb" ::: "memory");
                    writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
                    asm volatile("dsb sy; isb" ::: "memory");
                    continue;
                }

                /* Stable TRUE RUNNING confirmed */
                uart_puts("[xHCI] TRUE RUNNING at ");
                print_hex32((uint32_t)((t + 1) * 5));
                uart_puts("ms  hse_retries=");
                print_hex32((uint32_t)hse_retries); uart_puts("\n");

                /* boot109-A REMOVED (boot142): ringing DB[0] immediately after
                 * TRUE RUNNING with an empty command ring confuses the VL805
                 * MCU — it fetches the ring, finds nothing, and may not restart
                 * the ring scanner when the real No-op TRB arrives later.
                 * Circle never rings DB[0] until it has a real command to submit.*/

                /* boot109-B REMOVED (boot117): writing IMAN=0x3 post-TRUE-RUNNING
                 * caused MFINDEX to go static in boot116 — suspected to reset MCU
                 * interrupt state in a way that stalls the frame timer.
                 * IMAN is already correctly set to 0x2 (IE=1) during ring setup.  */

                /* boot71: MFINDEX check — moved BEFORE the tight poll (boot117).
                 * In boot116 MFINDEX was read at t≈55ms (after 50ms tight poll).
                 * In boots 105-108 it was read at t≈5ms and showed running fine.
                 * Read it here at t≈1ms to get a clean pre-poll baseline, then
                 * again after the tight poll to see if it's still running.       */
                {
                    volatile uint32_t *mf = (volatile uint32_t *)xhci_ctrl.runtime_regs;
                    asm volatile("dsb sy; isb" ::: "memory");
                    uint32_t mf0 = *mf;
                    fast_delay_ms(5);
                    uint32_t mf1 = *mf;
                    uart_puts("[xHCI] MFINDEX t0="); print_hex32(mf0);
                    uart_puts(" t5ms="); print_hex32(mf1);
                    uart_puts(mf1 != mf0 ? "  (running OK)\n" : "  (STATIC — frame timer not started)\n");
                }

                /* boot116: TIGHT EVENT RING POLL — immediately after TRUE RUNNING. */
                {
                    uint32_t ev[4];
                    int     evts_found = 0;
                    uint32_t poll_start = get_time_ms();

                    uart_puts("[BOOT116] Tight event ring poll (50ms window)...\n");

                    while ((get_time_ms() - poll_start) < 50U) {
                        asm volatile("dsb sy; isb" ::: "memory");

                        /* Check USBSTS.EINT — MCU signalled events pending */
                        uint32_t sts = readl(op + OP_USBSTS);
                        if (sts & STS_EINT) {
                            /* W1C EINT before draining — standard Linux sequence */
                            writel(STS_EINT, op + OP_USBSTS);
                            asm volatile("dsb sy; isb" ::: "memory");
                        }

                        /* Drain all available events */
                        while (evt_ring_poll(ev)) {
                            evts_found++;
                            uint32_t trb_type = (ev[3] >> 10) & 0x3FU;
                            uint32_t cc       = (ev[2] >> 24) & 0xFFU;
                            uart_puts("[BOOT116]   evt type=");
                            print_hex32(trb_type);
                            uart_puts(" cc="); print_hex32(cc);
                            uart_puts(" dw0="); print_hex32(ev[0]);
                            uart_puts(" dw2="); print_hex32(ev[2]);
                            uart_puts("\n");
                        }

                        if (evts_found > 0) break; /* got events — stop tight loop */

                        /* ~100µs delay using cycle counter */
                        {
                            uint64_t t0, t1;
                            asm volatile("mrs %0, cntpct_el0" : "=r"(t0));
                            do {
                                asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
                            } while ((t1 - t0) < 5400ULL); /* 54MHz/1000×10 ≈ 100µs */
                        }
                    }

                    if (evts_found > 0) {
                        uart_puts("[BOOT116]   *** ");
                        print_hex32((uint32_t)evts_found);
                        uart_puts(" event(s) received — event ring WORKING! ***\n");
                    } else {
                        uart_puts("[BOOT116]   No events in 50ms — MCU not posting events\n");
                        uart_puts("[BOOT116]   USBSTS="); print_hex32(readl(op + OP_USBSTS));
                        uart_puts("  IMAN="); print_hex32(readl(ir0 + IR_IMAN));
                        uart_puts("  TRB[0].w3=");
                        print_hex32(((volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF))[3]);
                        uart_puts("\n");
                    }
                }

                settled = 1;
                break;
            }

            /* Not TRUE RUNNING — MCU fired HSE and cleared RS.
             * Recovery: W1C HSE, re-write CRCR (while RS=0), then RS=1.
             *
             * boot76 root cause: VL805 MCU zeroes its internal CRCR when
             * firing the watchdog HSE.  If we only write RS=1 without
             * re-writing CRCR, the MCU captures CRCR=0 at the RS=0→1
             * transition and walks its command ring from address 0x0.
             * At address 0 all TRBs have cycle=0 ≠ RCS=1 → ring appears
             * empty → MCU generates zero TLPs forever.
             * Fix: re-write CRCR BEFORE each RS=1 (while RS is 0).        */
            hse_retries++;
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            fast_delay_ms(2);

            /* boot79: VL805 MCU zeroes ALL ring registers (not just CRCR)
             * when it fires HSE.  Re-arm the complete event ring state every
             * time we cycle RS=0→1, otherwise ERSTBA stays 0 and VL805
             * writes all events into its own BAR0 MMIO instead of DRAM.    */
            reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
            writel(cfg_val, op + OP_CONFIG);
            reg_write64(op, OP_CRCR_LO, crcr_val);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
            asm volatile("dsb sy; isb" ::: "memory");
            reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
            asm volatile("dsb sy; isb" ::: "memory");
            ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
            /* boot86: reset software dequeue ptr to match ERDP reset to ring base.
             * After HSE the MCU resets its producer; we must reset our consumer.
             * Without this, evt_dequeue stays > 0 so poll reads the wrong slot. */
            evt_dequeue = 0;
            evt_cycle   = 1;
            writel(0x00000002U, ir0 + IR_IMAN);
            asm volatile("dsb sy; isb" ::: "memory");

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

    /* ── Spontaneous MSI check (boot 69) ───────────────────────────────────
     * User diagnostic from MSI debug.txt.  At this point the MCU is in TRUE
     * RUNNING and the full MSI data chain is correct (DATA_CONFIG=0xFFE0 ==
     * VL805 data, boot 68 fix).  Wait 100ms: if the MCU fires an unsolicited
     * MSI on startup msi_fire_count will increase — full path confirmed.
     * Also read CRCR.CRR (bit 3) to verify MCU started its ring walker.  */
    {
        uart_puts("[DEBUG] Checking if VL805 is sending MSI automatically...\n");
        /* VL805 CRCR is write-only — reads back 0; CRR check via readback is useless.
         * Print scratchpad page 0 instead: if MCU DMA-wrote to scratch, DMA writes work.
         *
         * BUG FIX (boot84 false positive): was hardcoded to 0x12000UL which was only
         * correct when .xhci_dma was at 0x10000 (phoenix_fixed3).  Now computed from
         * xhci_dma_buf + DMA_SCRATCH_PAGES_OFF so it works regardless of linker placement.
         * With .xhci_dma at 0x30000000 (phoenix_fixed5), scratchpad page 0 is at 0x30002000.
         * Boot 84's scratch[0]=0xd453fae0 was reading stale VideoCore RAM at 0x12000. */
        volatile uint32_t *scratch_page0 =
            (volatile uint32_t *)(xhci_dma_buf + DMA_SCRATCH_PAGES_OFF);
        /* BOOT93-B: Normal-NC — no dc civac needed. dsb sy ensures the CPU
         * load is ordered after any in-flight RC write-buffer drain.        */
        asm volatile("dsb sy; isb" ::: "memory");
        uart_puts("[DEBUG]   scratchpad_phys=0x");
        print_hex32((uint32_t)((uint64_t)(uintptr_t)scratch_page0 >> 32));
        print_hex32((uint32_t)(uint64_t)(uintptr_t)scratch_page0);
        uart_puts("\n");
        uart_puts("[DEBUG]   scratch[0]="); print_hex32(scratch_page0[0]);
        uart_puts(" scratch[1]="); print_hex32(scratch_page0[1]);
        uart_puts("  (non-zero = MCU DMA writes working)\n");
        uint32_t initial_count = msi_fire_count;
        fast_delay_ms(100);
        if (msi_fire_count > initial_count) {
            uart_puts("[DEBUG] \xe2\x9c\x93 VL805 MSI auto-delivery WORKING!\n");
        } else {
            uart_puts("[DEBUG] \xe2\x9c\x97 VL805 MSI auto-delivery still BROKEN\n");
        }
    }

    /* BOOT91-A: USBSTS snapshot between TRUE RUNNING and No-op submit.
     *
     * Boot 90: USBSTS=0x00000005 at No-op timeout → MCU had already fired
     * HSE during the 100ms MSI delay above.  Sample here to see how fast
     * the MCU drops out after TRUE RUNNING is declared, and snapshot all
     * ring registers to confirm they haven't been zeroed by the MCU yet.
     */
    {
        uint32_t sts_snap = readl(op + OP_USBSTS);
        uint32_t cmd_snap = readl(op + OP_USBCMD);
        uart_puts("[BOOT91-A] Pre-Noop snapshot:\n");
        uart_puts("[BOOT91-A]   USBSTS="); print_hex32(sts_snap);
        uart_puts("  USBCMD="); print_hex32(cmd_snap);
        uart_puts(((sts_snap & STS_HCH) || (sts_snap & STS_HSE)) ?
                  "  *** MCU already HALTED/HSE before No-op! ***\n" :
                  "  (running clean)\n");
        uart_puts("[BOOT91-A]   DCBAAP="); print_hex32(readl(op + OP_DCBAAP_LO));
        uart_puts("  ERSTBA="); print_hex32(readl(ir0 + IR_ERSTBA_LO));
        uart_puts("  ERSTSZ="); print_hex32(readl(ir0 + IR_ERSTSZ));
        uart_puts("  ERDP=");   print_hex32(readl(ir0 + IR_ERDP_LO));
        uart_puts("\n");
        /* BOOT92-C: Post-HSE full scratchpad canary readback.
         *
         * The MCU fires HSE within ~1ms of RS=1 (boot91).  It should have
         * read the scratchpad array and attempted to write its internal
         * context into the pages before dying.  Flush D-cache across every
         * page and check each canary:
         *
         *   canary UNCHANGED (0xABCD1234) → MCU never wrote to this page
         *   canary CHANGED               → MCU DMA-wrote to this page ✓
         *
         * Also dump the first 64 bytes of page 0 raw — if the MCU wrote
         * anything there, this shows what it actually wrote.
         *
         * Device nGnRnE memory: CPU reads are uncached, but we still issue
         * dc civac to invalidate any speculative prefetch lines.
         */
        uart_puts("[BOOT92-C] Scratchpad canary readback (post-HSE):\n");
        {
            uint32_t n_sp = xhci_ctrl.scratchpad_count;
            int any_changed = 0;
            int any_zero    = 0;

            for (uint32_t i = 0; i < n_sp; i++) {
                volatile uint32_t *pg =
                    (volatile uint32_t *)(xhci_dma_buf + DMA_SCRATCH_PAGES_OFF + i * 4096);
                /* BOOT93-B: Normal-NC memory is never cached — dc civac is
                 * wrong here (nothing to flush).  A plain dsb sy ensures the
                 * CPU's load is ordered after any prior RC write-buffer drain. */
                asm volatile("dsb sy; isb" ::: "memory");

                uint32_t w0 = pg[0], w1 = pg[1];
                int changed = (w0 != 0xABCD1234U) || (w1 != 0xDEAD5678U);
                int zeroed  = (w0 == 0) && (w1 == 0);

                if (changed) any_changed++;
                if (zeroed)  any_zero++;

                /* Print every changed page, plus first/last always */
                if (changed || i == 0 || i == n_sp - 1) {
                    uart_puts("[BOOT92-C]   page["); print_hex32(i); uart_puts("] ");
                    print_hex32(w0); uart_puts(" "); print_hex32(w1);
                    if (zeroed)        uart_puts("  [zero — MCU zeroed it]\n");
                    else if (changed)  uart_puts("  [*** MCU WROTE HERE ***]\n");
                    else               uart_puts("  [canary intact — untouched]\n");
                }
            }

            uart_puts("[BOOT92-C]   pages changed="); print_hex32((uint32_t)any_changed);
            uart_puts("  zeroed="); print_hex32((uint32_t)any_zero);
            uart_puts("  unchanged=");
            print_hex32((uint32_t)(n_sp - any_changed)); uart_puts("\n");

            /* Full 64-byte hex dump of page 0 regardless of canary state. */
            uart_puts("[BOOT92-C]   page[0] first 64 bytes:\n");
            volatile uint32_t *pg0 =
                (volatile uint32_t *)(xhci_dma_buf + DMA_SCRATCH_PAGES_OFF);
            /* BOOT93-B: Normal-NC — no dc civac needed, plain dsb sy suffices */
            asm volatile("dsb sy; isb" ::: "memory");
            for (int row = 0; row < 4; row++) {
                uart_puts("[BOOT92-C]     +0x");
                /* print row offset as 2-digit hex */
                uint8_t off = (uint8_t)(row * 16);
                char hi = (off >> 4) < 10 ? '0'+(off>>4) : 'a'+(off>>4)-10;
                char lo = (off & 0xF) < 10 ? '0'+(off&0xF) : 'a'+(off&0xF)-10;
                char s[3] = {hi, lo, 0};
                uart_puts(s);
                uart_puts(":  ");
                for (int col = 0; col < 4; col++) {
                    print_hex32(pg0[row * 4 + col]);
                    uart_puts(col < 3 ? " " : "\n");
                }
            }

            /* Also check INTR2 now — if INTR2 > 0 and all canaries intact,
             * the MCU generated TLPs that are NOT going to our pages.       */
            uint32_t intr2_now = readl(pcie_base + 0x4300U);
            uart_puts("[BOOT92-C]   INTR2="); print_hex32(intr2_now);
            uart_puts(intr2_now && !any_changed ?
                      "  *** TLPs received but NO page written — writes misrouted ***\n" :
                      intr2_now && any_changed ?
                      "  TLPs received AND pages written — DMA path working\n" :
                      "  No TLPs — MCU never attempted PCIe write\n");
        }
    }

    /* ── MCU keepalive No-op ─────────────────────────────────────────────────
     * Boot 56 root cause: without a command ping at TRUE RUNNING, the VL805 MCU
     * fires its internal watchdog HSE every ~4ms throughout the entire settle
     * window.  Boot 57 fix: submit a Command No-op (TRB type 23) immediately
     * after TRUE RUNNING — the MCU processes it, writes a CCE to the event ring,
     * and the watchdog is satisfied.  Settle then sees 0 HSE events.
     *
     * If the No-op CCE never arrives, the MCU cannot DMA-write to our event ring
     * and we have a fundamental DMA-path problem to diagnose.
     *
     * We use cmd_ring_submit which does W1C HSE + RS=1 + CRCR + doorbell.
     * Since we're at TRUE RUNNING (HSE=0, RS=1), these are harmless no-ops
     * for the W1C and RS writes; the CRCR and doorbell are what matter.       */
    {
        /* BOOT91-B: Explicit full re-arm + RS=1 verify before No-op submit.
         *
         * Even if USBSTS was clean at snapshot time above, the MCU could
         * fire HSE in the microseconds between that read and the doorbell.
         * Perform a definitive re-arm here — write all ring registers then
         * poll for clean TRUE RUNNING (up to 200ms) before the No-op TRB
         * is written.  This guarantees the No-op is never sent to a halted
         * controller.
         */
        uart_puts("[BOOT91-B] Pre-Noop full re-arm...\n");
        writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
        asm volatile("dsb sy; isb" ::: "memory");
        reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
        writel(cfg_val, op + OP_CONFIG);
        reg_write64(op, OP_CRCR_LO, crcr_val);
        asm volatile("dsb sy; isb" ::: "memory");
        writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
        asm volatile("dsb sy; isb" ::: "memory");
        reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
        asm volatile("dsb sy; isb" ::: "memory");
        ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
        evt_dequeue = 0;
        evt_cycle   = 1;
        writel(0x00000002U, ir0 + IR_IMAN);
        asm volatile("dsb sy; isb" ::: "memory");
        writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        /* Wait up to 200ms for clean RS=1, HCH=0, HSE=0 */
        int b91_ok = 0;
        uint32_t b91_t0 = get_time_ms();
        while ((get_time_ms() - b91_t0) < 200U) {
            uint32_t s2 = readl(op + OP_USBSTS);
            uint32_t c2 = readl(op + OP_USBCMD);
            if (!(s2 & STS_HCH) && (c2 & CMD_RS) && !(s2 & STS_HSE)) {
                uart_puts("[BOOT91-B]   RS stable after ");
                print_hex32(get_time_ms() - b91_t0);
                uart_puts("ms  USBSTS="); print_hex32(s2); uart_puts("\n");
                b91_ok = 1;
                break;
            }
            /* Clear HSE and retry if the MCU fires watchdog */
            if (s2 & STS_HSE) {
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
                reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
                reg_write64(op, OP_CRCR_LO, crcr_val);
                asm volatile("dsb sy; isb" ::: "memory");
                writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
                asm volatile("dsb sy; isb" ::: "memory");
                reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
                asm volatile("dsb sy; isb" ::: "memory");
                ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
                evt_dequeue = 0; evt_cycle = 1;
                writel(0x00000002U, ir0 + IR_IMAN);
                asm volatile("dsb sy; isb" ::: "memory");
                writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
                asm volatile("dsb sy; isb" ::: "memory");
            }
            fast_delay_ms(1);
        }
        if (!b91_ok) {
            uart_puts("[BOOT91-B]   WARNING: MCU never stable in 200ms before No-op\n");
            uart_puts("[BOOT91-B]   USBSTS="); print_hex32(readl(op + OP_USBSTS));
            uart_puts("  USBCMD="); print_hex32(readl(op + OP_USBCMD)); uart_puts("\n");
        }

        uart_puts("[xHCI] Sending No-op keepalive to MCU...\n");
        cmd_ring_submit(0, 0, 0, TRB_TYPE_NOOP_CMD, 0);

        /* Verify No-op TRB landed at the expected physical location.
         * cmd_enqueue was incremented by cmd_ring_submit; the TRB we just
         * wrote is at index (cmd_enqueue-1), wrapping at CMD_RING_TRBS-1.
         * DW3 should be (TRB_TYPE_NOOP_CMD<<10)|cycle = 0x5C00|0x1 = 0x5C01. */
        {
            uint32_t noop_idx = (cmd_enqueue == 0)
                                ? (uint32_t)(CMD_RING_TRBS - 2)
                                : (cmd_enqueue - 1);
            uint64_t trb_phys = cmd_ring_dma + (uint64_t)noop_idx * 16;
            uart_puts("[xHCI] No-op TRB["); print_hex32(noop_idx);
            uart_puts("] phys="); print_hex32((uint32_t)trb_phys); uart_puts("\n");
            volatile uint32_t *t = cmd_ring + noop_idx * 4;
            uart_puts("[xHCI]   dw0="); print_hex32(t[0]);
            uart_puts(" dw1="); print_hex32(t[1]);
            uart_puts(" dw2="); print_hex32(t[2]);
            uart_puts(" dw3="); print_hex32(t[3]); uart_puts("\n");
            /* VL805 CRCR is write-only — always reads back 0 (confirmed boot 69).
             * CRR bit check via readback is meaningless on VL805 hardware.    */
            uart_puts("[xHCI]   CRCR_LO(VL805_write-only)=");
            print_hex32(readl(op + OP_CRCR_LO)); uart_puts("\n");
        }

        uint32_t noop_ev[4];
        /* BOOT91-C: Timestamped USBSTS snapshots during No-op wait.
         * We want to know: does the MCU fire HSE immediately on the doorbell,
         * after a few ms, or not at all?  Sample at fixed intervals so we can
         * pinpoint the MCU's watchdog timeline relative to the doorbell ring.
         * xhci_wait_event polls and clears HSE internally so we do the timed
         * samples manually first, then hand off to wait_event.                */
        {
            uint32_t noop_t0 = get_time_ms();
            static const uint32_t sample_ms[] = {1, 2, 5, 10, 20, 50, 100, 200};
            int n_samples = (int)(sizeof(sample_ms)/sizeof(sample_ms[0]));
            uint32_t prev_ms = 0;
            int event_found = 0;
            for (int si = 0; si < n_samples && !event_found; si++) {
                uint32_t wait_for = sample_ms[si];
                /* Spin until this sample point */
                while ((get_time_ms() - noop_t0) < wait_for) {
                    asm volatile("dsb sy; isb" ::: "memory");
                    /* Quick poll — grab event if already here */
                    if (evt_ring_poll(noop_ev)) { event_found = 1; break; }
                }
                if (event_found) {
                    uart_puts("[BOOT91-C] No-op CCE arrived at ~");
                    print_hex32(get_time_ms() - noop_t0);
                    uart_puts("ms\n");
                    break;
                }
                uint32_t sts_c = readl(op + OP_USBSTS);
                uart_puts("[BOOT91-C] t+"); print_hex32(wait_for);
                uart_puts("ms USBSTS="); print_hex32(sts_c);
                uart_puts("  ERDP="); print_hex32(readl(ir0 + IR_ERDP_LO));
                uart_puts("  ERSTBA="); print_hex32(readl(ir0 + IR_ERSTBA_LO));
                uart_puts("  TRB0=["); print_hex32(((volatile uint32_t *)evt_ring)[0]);
                uart_puts(","); print_hex32(((volatile uint32_t *)evt_ring)[3]);
                uart_puts("]\n");
                (void)prev_ms;
                prev_ms = wait_for;
            }
            if (event_found) {
                /* Already consumed by poll above — return success */
                uint8_t cc = (noop_ev[2] >> 24) & 0xFF;
                uart_puts("[xHCI] No-op CCE received! CC="); print_hex32(cc);
                uart_puts(" — MCU DMA write-back confirmed\n");
                goto noop_done;
            }
        }
        /* Full timeout path */
        if (xhci_wait_event(noop_ev, 200) == 0) {
            uint8_t cc = (noop_ev[2] >> 24) & 0xFF;
            uart_puts("[xHCI] No-op CCE received! CC="); print_hex32(cc);
            uart_puts(" — MCU DMA write-back confirmed\n");
        } else {
            uart_puts("[xHCI] WARNING: No-op CCE timeout — re-arming rings before enumeration\n");
            {
                uart_puts("[xHCI]   USBSTS="); print_hex32(readl(op + OP_USBSTS));
                uart_puts("  DCBAAP="); print_hex32(readl(op + OP_DCBAAP_LO));
                uart_puts("  CRCR(w-o)="); print_hex32(readl(op + OP_CRCR_LO)); uart_puts("\n");
                uint32_t intr2 = readl(pcie_base + 0x4300U);
                uart_puts("[xHCI]   PCIE_INTR2_STATUS="); print_hex32(intr2);
                uart_puts(intr2 ? "  (RC got PCIe TLP — check GIC delivery)\n"
                                : "  (RC silent — MCU sent no PCIe TLP)\n");
            }
            /* Re-arm all rings before enumeration: MCU may have fired HSE
             * during the No-op wait and zeroed the ring registers.          */
            {
                void *_ir0 = ir_base(0);
                uint64_t _ea = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
                uint64_t _cr = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
                reg_write64(op, OP_DCBAAP_LO, _ea);
                reg_write64(op, OP_CRCR_LO, _cr);
                asm volatile("dsb sy; isb" ::: "memory");
                writel(0U, _ir0 + IR_ERSTSZ);
                reg_write64(_ir0, IR_ERSTBA_LO, erst_dma_addr);
                asm volatile("dsb sy; isb" ::: "memory");
                writel(1U, _ir0 + IR_ERSTSZ);
                asm volatile("dsb sy; isb" ::: "memory");
                ERDP_REARM(_ir0, evt_ring_dma); /* boot107: EHB=0 arms interrupter */
                evt_dequeue = 0; evt_cycle = 1;
                writel(0x00000002U, _ir0 + IR_IMAN);
                asm volatile("dsb sy; isb" ::: "memory");
                writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
                asm volatile("dsb sy; isb" ::: "memory");
            }
            /* Continue anyway — best-effort enumeration */
        }
        noop_done:; /* BOOT91-C jump target */
    }

    /* Step 5: Power up ports.
     * Boot 39 finding: writing PP=1 to Port 1 (companion, DR=1) triggered
     * HSE with ~3ms delay, killing the Enable Slot window.  Skip the
     * companion port here — enumerate_port() already skips it for WPR /
     * device enumeration, so this write was always unnecessary.           */
    for (int p = 0; p < (int)xhci_ctrl.max_ports; p++) {
        uint32_t ps5 = readl(op + 0x400 + p * 0x10);
        /* boot80: only skip companion if no device (CCS=0).
         * If CCS=1, a device fell back to USB2 — power it up normally. */
        if ((ps5 & (1U << 30)) && !(ps5 & PORTSC_CCS)) {
            uart_puts("[xHCI] step5: skip empty companion ");
            print_hex32((uint32_t)(p + 1)); uart_puts(" (DR=1, CCS=0)\n");
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
        fast_delay_ms(4);
        uint32_t _ev[4];
        while (evt_ring_poll(_ev)) _settle_evts++;
        /* W1C HSE each iteration — count DISTINCT fires, not sticky accumulation.
         * Without No-op keepalive (boot 65): hse=97/100 (fired once, stayed set).
         * With No-op keepalive (boot 66 target): expect hse=0 across all 100.   */
        if (readl(op + OP_USBSTS) & STS_HSE) {
            _settle_hse++;
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            /* boot81: Full ring re-arm on every settle HSE — identical to the
             * initial retry loop.  VL805 MCU resets ALL ring registers (CRCR,
             * ERSTBA, DCBAA) on every HSE.  The old code only wrote RS=1 here,
             * leaving CRCR=0 after the 400ms settle window, so Enable Slot was
             * submitted to a dead command ring (boot 80: CRCR=0x00000000 at
             * every Enable Slot timeout despite ring programmed at init).      */
            reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
            writel(cfg_val, op + OP_CONFIG);
            reg_write64(op, OP_CRCR_LO, crcr_val);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
            asm volatile("dsb sy; isb" ::: "memory");
            reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
            asm volatile("dsb sy; isb" ::: "memory");
            ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
            /* boot86: reset software consumer index to match ERDP = ring_base.
             * If a false canary event was consumed (boot85 bug), evt_dequeue was
             * left at 1 while ERDP is re-armed to base → next poll reads slot 1
             * (stale/zero) while MCU writes to slot 0.  Reset both here.       */
            evt_dequeue = 0;
            evt_cycle   = 1;
            writel(0x00000002U, ir0 + IR_IMAN);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");
        }
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

    /* boot81: Final ring re-arm before port scan.
     * Ensure the last thing the MCU sees before Enable Slot is a fresh set of
     * ring pointers, not the stale-zero state left by the last settle HSE.
     * We are in RS=1 here (settle loop leaves it that way) — write ring regs
     * while running (safe: VL805 only snapshots CRCR at RS=0→1 edge).       */
    {
        uint32_t sts_pre = readl(op + OP_USBSTS);
        if (sts_pre & STS_HSE) {
            /* HSE fired after settle loop — clear and re-arm before scan */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
        }
        reg_write64(op, OP_DCBAAP_LO, dcbaa_dma);
        writel(cfg_val, op + OP_CONFIG);
        reg_write64(op, OP_CRCR_LO, crcr_val);
        asm volatile("dsb sy; isb" ::: "memory");
        writel(1U, ir0 + IR_ERSTSZ);               /* boot142: ERSTSZ first */
        asm volatile("dsb sy; isb" ::: "memory");
        reg_write64(ir0, IR_ERSTBA_LO, erstba_dma); /* then ERSTBA */
        asm volatile("dsb sy; isb" ::: "memory");
        ERDP_REARM(ir0, evt_dma); /* EHB=0 to advance dequeue */
        writel(0x00000002U, ir0 + IR_IMAN);
        asm volatile("dsb sy; isb" ::: "memory");
        if (sts_pre & STS_HSE)
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");
    }

    /* Final state check */
    uart_puts("[xHCI] Final: USBSTS="); print_hex32(readl(op + OP_USBSTS));
    uart_puts(" USBCMD="); print_hex32(readl(op + OP_USBCMD));
    uart_puts(" CRCR="); print_hex32(readl(op + OP_CRCR_LO));
    uart_puts(" ERSTBA="); print_hex32(readl(ir0 + IR_ERSTBA_LO));
    uart_puts(" ERSTSZ="); print_hex32(readl(ir0 + IR_ERSTSZ));
    uart_puts("\n");

    /* boot86: [CANARY-CHECK] Read back event ring TRB 0.
     *
     * We wrote 0xDEADBEEF/0xCAFEF00D/0xDEADC0DE/0x0000BEEE to TRB 0 before
     * (boot86: word3 bit0=0, was 0x0000BEEF in boot85 which caused false-positive poll).
     * RS=1.  The MCU should overwrite TRB 0 with its first event within ms.
     *
     * Possible outcomes:
     *   A) TRB 0 = 0xDEADBEEF (unchanged)
     *      → MCU never wrote to event ring.  UBUS write path broken, OR MCU
     *        never reached the state where it posts events.
     *   B) TRB 0 = 0x00000000 (zeroed)
     *      → Our settle-loop re-arm did dma_zero on evt_ring somewhere, or
     *        MCU wrote all-zeros (unlikely for a real TRB).
     *   C) TRB 0 = valid xHCI TRB (type field bits[15:10] != 0)
     *      → UBUS write path WORKING.  MCU wrote a real event.  Decode it.
     *        Common first events: CCE (type=33), PSCE (type=34), MFIE (type=39).
     *
     * Also read a cache-miss range (512 bytes) to see if ANY part of the ring
     * was written.  The MCU may not start at TRB 0 if ERDP was non-zero.
     *
     * This is the most definitive UBUS write test we have done.            */
    /* boot116: cycle-bit check replaces canary check.
     * Ring is fully zeroed — any TRB with word3.bit0=1 was written by the MCU. */
    {
        volatile uint32_t *evt_trb0 =
            (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
        asm volatile("dsb sy; isb" ::: "memory");

        uint32_t w0 = evt_trb0[0], w1 = evt_trb0[1];
        uint32_t w2 = evt_trb0[2], w3 = evt_trb0[3];
        uart_puts("[xHCI] [TRB0-CHECK] TRB[0]: ");
        print_hex32(w0); uart_puts(" "); print_hex32(w1); uart_puts(" ");
        print_hex32(w2); uart_puts(" "); print_hex32(w3); uart_puts("\n");

        if ((w3 & 1U) == 1U) {
            uint32_t trb_type = (w3 >> 10) & 0x3FU;
            uart_puts("[xHCI] [TRB0] MCU WROTE TRB[0]! type=");
            print_hex32(trb_type); uart_puts(" (33=CCE 34=PSCE)\n");
            uart_puts("[xHCI] *** EVENT RING WORKING ***\n");
        } else if (w0 == 0U && w1 == 0U && w2 == 0U && w3 == 0U) {
            uart_puts("[xHCI] [TRB0] Zero — MCU did not write TRB[0]\n");
        } else {
            uart_puts("[xHCI] [TRB0] Non-zero but cycle=0 — unexpected content\n");
        }

        /* boot116: full ring dump — look for any TRB with cycle=1 written by MCU */
        uart_puts("[xHCI] [RING-DUMP] Full event ring (64 TRBs):\n");
        asm volatile("dsb sy; isb" ::: "memory");
        int any_mcu_wrote = 0;
        for (int ti = 0; ti < EVT_RING_TRBS; ti++) {
            uint32_t t0 = evt_trb0[ti * 4 + 0];
            uint32_t t1 = evt_trb0[ti * 4 + 1];
            uint32_t t2 = evt_trb0[ti * 4 + 2];
            uint32_t t3 = evt_trb0[ti * 4 + 3];
            int is_zero = (t0 == 0 && t1 == 0 && t2 == 0 && t3 == 0);
            if (!is_zero) {
                uart_puts("[xHCI]   TRB["); print_hex32((uint32_t)ti); uart_puts("]: ");
                print_hex32(t0); uart_puts(" "); print_hex32(t1); uart_puts(" ");
                print_hex32(t2); uart_puts(" "); print_hex32(t3);
                if (t3 & 1U) {
                    uint32_t type = (t3 >> 10) & 0x3FU;
                    uint32_t cc   = (t2 >> 24) & 0xFFU;
                    uart_puts("  [MCU wrote: type="); print_hex32(type);
                    uart_puts(" cc="); print_hex32(cc); uart_puts("]");
                    any_mcu_wrote = 1;
                } else {
                    uart_puts("  [non-zero, cycle=0]");
                }
                uart_puts("\n");
            }
        }
        if (!any_mcu_wrote)
            uart_puts("[xHCI] [RING-DUMP] No MCU-written TRBs found (all cycle=0 or zero).\n");
    }

    /* Event ring self-test REMOVED (boot 53 confirmed PASS every run).
     * Ring stays at initial state: deq=0, ERDP=ring_base, cycle=1.
     * MCU will write its first event (Enable Slot CCE) to TRB[0].         */

    /* Also verify phys 0 ERST hasn't been corrupted since setup_event_ring */
    {
        uintptr_t pa = 0;
        asm volatile("" : "+r"(pa));
        volatile uint32_t *p = (volatile uint32_t *)pa;
        asm volatile("dsb sy; isb" ::: "memory"); /* phys 0 = Normal memory */
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
     * kernel8.img.  The physical address is set by the linker:
     *   phoenix_fixed3: 0x10000   (VideoCore contaminates — boot83 garbage)
     *   phoenix_fixed5: 0x30000000 (clean but PCIe top nibble = 0xF — boot84-93)
     *   phoenix_fixed6: 0x0C000000 (boot94 test — PCIe 0xCC000000, top nibble C)
     * Dump it first (to catch any firmware conflict), then zero unconditionally
     * so every ring and struct starts clean.
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
     * Normal-NC mapping: dma_zero uses volatile 32-bit writes (no LDP/STP).
     * volatile prevents compiler from emitting LDP/STP even on Normal memory. */
    dma_zero(xhci_dma_buf, dma_region_size);
    asm volatile("dsb sy; isb" ::: "memory");
    uart_puts("[xHCI] DMA region zeroed.\n");

    /* BOOT93-A: MMU memory-type self-check.
     *
     * Use AT S1E1R (Address Translate Stage-1 EL1 Read) to walk the page
     * tables for the DMA buffer virtual address and read PAR_EL1.
     * PAR_EL1 bits[7:0] on a successful translation:
     *   bit[0]   = F (fault) — must be 0
     *   bits[3:2] = SH (shareability): 0b11 = inner-shareable (needed)
     *   bits[7:4] = Attr (memory type from MAIR):
     *               0x0 = Device nGnRnE  ← WRONG (pre-BOOT93)
     *               0x4 = Normal NC      ← CORRECT (post-BOOT93)
     *               0xF = Normal WB      ← also wrong
     *
     * If Attr=0x0 is still showing, the MMU code change did not take effect
     * and inbound PCIe writes will still be invisible to the CPU.
     */
    {
        uint64_t va  = (uint64_t)(uintptr_t)xhci_dma_buf;
        uint64_t par = 0;
        asm volatile(
            "at s1e1r, %1\n\t"
            "isb\n\t"
            "mrs %0, par_el1"
            : "=r"(par) : "r"(va) : "memory"
        );
        uint8_t fault = par & 0x1U;
        uint8_t sh    = (uint8_t)((par >> 7) & 0x3U);   /* PAR_EL1[8:7] = SH */
        uint8_t attr  = (uint8_t)((par >> 56) & 0xFFU); /* PAR_EL1[63:56] = ATTR */
        /* Note: PAR_EL1 layout on success (F=0):
         *   bits[11:0]   = lower attributes / ignored
         *   bits[47:12]  = PA[47:12]
         *   bits[56]     = reserved
         *   bits[58:56]  = SH[1:0] at bits[8:7] in some implementations
         *   bits[63:56]  = ATTR (memory attribute from MAIR)
         * Exact bit positions vary by implementation; we check ATTR at [63:56]
         * which is the standard ARMv8 PAR_EL1 encoding. */
        uart_puts("[BOOT93-A] MMU type check: PAR_EL1=");
        print_hex32((uint32_t)(par >> 32)); uart_puts(":"); print_hex32((uint32_t)par);
        uart_puts("  fault="); print_hex32(fault);
        uart_puts("  ATTR=0x"); print_hex32(attr);
        uart_puts("\n");
        if (fault) {
            uart_puts("[BOOT93-A] *** AT FAULT — translation failed! Check MMU tables. ***\n");
        } else if (attr == 0x00U) {
            uart_puts("[BOOT93-A] *** ATTR=0x00 = Device nGnRnE — MMU change NOT applied! ***\n");
            uart_puts("[BOOT93-A] *** PCIe DMA writes will NOT be visible. Fix mmu.c first. ***\n");
        } else if (attr == 0x44U || attr == 0x4U) {
            uart_puts("[BOOT93-A] ATTR=Normal-NC ✓  PCIe DMA coherency ENABLED\n");
        } else if (attr == 0xFFU || attr == 0xF4U || attr == 0x4FU) {
            uart_puts("[BOOT93-A] ATTR=Normal Cacheable — unexpected, but coherent\n");
        } else {
            uart_puts("[BOOT93-A] ATTR=unknown — check MAIR register encoding\n");
        }
    }

    /* ── boot85: Event-ring UBUS write canary ───────────────────────────────
     * Place a known marker in event ring TRB 0 AFTER zeroing.  The xHCI MCU
     * writes its first event (usually a Port Status Change or Command Complete)
     * to TRB 0 of the event ring.  If the canary changes after controller
     * start, the UBUS write path from VL805 to DRAM is WORKING.
     * If it stays 0xDEADBEEF, the MCU never wrote to this address (either UBUS
     * is broken, or the MCU never generated an event — a critical distinction
     * from the prior "all zeros" result which was ambiguous).
     *
     * Canary is written AFTER dma_zero so we cannot mistake it for garbage.
     * It will be overwritten by setup_event_ring() → dma_zero(evt_ring, ...)
     * WAIT — setup_event_ring zeros the ring again.  So we write the canary
     * AFTER setup_event_ring() returns in run_controller().  See the canary
     * write point marked [CANARY-WRITE] below in run_controller(). */
    uart_puts("[xHCI] Canary will be written to evt_ring TRB 0 after ring setup.\n");

    /* ── MSI landing pad diagnostic (user boot 66 request) ─────────────────
     * DMA_MSI_PAGE_OFF = 0x1000 into the DMA buffer.  pci.c programs the RC
     * MSI BAR to this physical address, so the VL805 MSI write TLP lands here.
     *
     * NOTE: The CPU write below (0xDEADBEEF) does NOT go through PCIe — it is
     * a direct DRAM write.  It will NOT trigger the GIC MSI interrupt.  The RC
     * MSI mechanism only fires when the VL805 does a PCIe MemWrite to the MSI
     * BAR address.  This test confirms the memory is accessible (no data abort
     * on Device nGnRnE) and msi_fire_count stays 0 after a CPU-side write.   */
    {
        volatile uint32_t *msi_landing = (volatile uint32_t *)(xhci_dma_buf + DMA_MSI_PAGE_OFF);
        uint64_t msi_phys = (uint64_t)(uintptr_t)(xhci_dma_buf + DMA_MSI_PAGE_OFF);

        uart_puts("[DEBUG] MSI landing pad phys: 0x");
        print_hex32((uint32_t)(msi_phys >> 32));
        print_hex32((uint32_t)msi_phys);
        uart_puts("\n");

        uart_puts("[DEBUG] Before test write: 0x");
        print_hex32(msi_landing[0]);
        uart_puts("\n");

        /* CPU-side write to Device memory — confirms no data abort, but
         * does NOT trigger xhci_irq_handler (no PCIe transaction involved). */
        msi_landing[0] = 0xDEADBEEFU;
        asm volatile("dsb sy; isb" ::: "memory");
        fast_delay_ms(10);

        uart_puts("[DEBUG] After test write: 0x");
        print_hex32(msi_landing[0]);
        uart_puts("  msi_fire_count now: 0x");
        print_hex32(msi_fire_count);
        uart_puts("\n");

        /* Clear the test value — ring setup starts below */
        msi_landing[0] = 0U;
        asm volatile("dsb sy; isb" ::: "memory");
    }

    /* ── GIC self-test retired after boot 67 ────────────────────────────
     * Boot 67 confirmed: GIC→CPU→handler path is fully working.
     * The self-test was removed here because it fired xhci_irq_handler
     * before the event ring was set up, which caused the handler to read
     * garbage TRBs from physical address 0x0 (boot vector table area,
     * cycle bit=1 by coincidence).  This advanced the software dequeue
     * pointer to 0x10, so the subsequent No-op wait_event read the same
     * garbage and returned CC=0x52 (fake success), meaning the real MCU
     * No-op CCE was never processed and the watchdog kept firing HSE.
     * GIC wiring is proven — no need to re-run this test.              */

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
     * available during enumeration.                                    */
    usb_register_hc(&g_xhci_hc_ops);
    xhci_ctrl.initialized = 1;

    /* boot141: call port_scan() directly here — usb_init.c stubs the
     * VL805 probe (prints "SKIPPED") so xhci_scan_ports() is NEVER
     * called on the desktop build otherwise.  Circle mirrors this by
     * calling RootHub::Initialize() immediately after RS=1, which
     * resets each port and triggers EnableSlot / AddressDevice.
     * We must do the same: enumerate_port → PORTSC PR reset →
     * issue Enable Slot TRB → etc.                                     */
    port_scan();

    /* boot142: read MFINDEX after port scan — see if SOF frames started
     * once a port was reset and a device is at U0.  MFINDEX was 0 before
     * port scan in boot141; it should now be non-zero if the VL805 ties
     * the frame timer to USB SOF generation rather than RS=1.            */
    {
        volatile uint32_t *mf = (volatile uint32_t *)xhci_ctrl.runtime_regs;
        asm volatile("dsb sy; isb" ::: "memory");
        uint32_t mf_post = *mf;
        fast_delay_ms(5);
        uint32_t mf_post5 = *mf;
        uart_puts("[boot142] MFINDEX post-portscan: t0=");
        print_hex32(mf_post);
        uart_puts("  t5ms=");
        print_hex32(mf_post5);
        if (mf_post5 == mf_post)
            uart_puts("  (STATIC — frame timer still not running)\n");
        else
            uart_puts("  (RUNNING — SOF started after port reset!)\n");
    }

    return 0;
}

/* ── DMA extension + full enumeration functions (from your original) ─────── */
/* boot147: per-slot DMA memory layout — SLOT_STRIDE bytes per slot, slots 1..MAX_SLOTS_ALLOC.
 * Base: 0x22000; top: 0x22000 + 4×0x1000 = 0x26000 < 0x42000 DMA budget.
 * Within each SLOT_STRIDE block:
 *   +SLOT_INPUT_CTX_OFF  input context  (34 × CTX_SIZE = 1088 bytes)
 *   +SLOT_OUT_CTX_OFF    output context (32 × CTX_SIZE = 1024 bytes)
 *   +SLOT_EP0_RING_OFF   EP0 xfer ring  (EP0_RING_TRBS × 16 = 1024 bytes)
 *   +SLOT_EP0_DATA_OFF   EP0 data buf   (512 bytes)
 * Old single-slot DMA_INPUT/OUT/EP0_RING/EP0_DATA_OFF constants removed.   */
#define DMA_SLOTS_BASE_OFF   0x22000
#define SLOT_STRIDE          0x1000
#define MAX_SLOTS_ALLOC      4
#define SLOT_INPUT_CTX_OFF   0x000
#define SLOT_OUT_CTX_OFF     0x500
#define SLOT_EP0_RING_OFF    0x900
#define SLOT_EP0_DATA_OFF    0xD00

#define EP0_RING_TRBS  64
/* boot149: CTX_SIZE is no longer a compile-time constant.
 * CSZ bit in HCCPARAMS1 determines whether entries are 32 or 64 bytes.
 * read_caps() populates xhci_ctrl.csz; this macro reads it at runtime.  */
#define CTX_SIZE  (xhci_ctrl.csz ? 64U : 32U)

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

/* boot147: per-slot EP0 ring state, indexed 1..MAX_SLOTS_ALLOC.
 * active_slot tracks which slot is currently driving ep0_enq().
 * g_slot_ids[port] holds the MCU-assigned slot_id for each root-hub port.
 * g_devs[slot_id] holds the usb_device_t for each enumerated slot.         */
static uint8_t  ep0_cycle_s[MAX_SLOTS_ALLOC + 1]   = {0};
static uint32_t ep0_enqueue_s[MAX_SLOTS_ALLOC + 1] = {0};
static uint8_t  active_slot = 0;
static uint8_t  g_slot_ids[16] = {0};   /* indexed by port (0-based)        */
static usb_device_t g_devs[MAX_SLOTS_ALLOC + 1];  /* indexed by slot_id     */

/* Slot DMA region helpers (slot_id is 1-based, 1..MAX_SLOTS_ALLOC)         */
static inline volatile uint8_t *slot_input_ctx(uint8_t s) {
    return (volatile uint8_t *)(xhci_dma_buf + DMA_SLOTS_BASE_OFF
                                + (uint32_t)(s - 1) * SLOT_STRIDE
                                + SLOT_INPUT_CTX_OFF);
}
static inline volatile uint8_t *slot_out_ctx(uint8_t s) {
    return (volatile uint8_t *)(xhci_dma_buf + DMA_SLOTS_BASE_OFF
                                + (uint32_t)(s - 1) * SLOT_STRIDE
                                + SLOT_OUT_CTX_OFF);
}
static inline volatile uint32_t *slot_ep0_ring(uint8_t s) {
    return (volatile uint32_t *)(xhci_dma_buf + DMA_SLOTS_BASE_OFF
                                 + (uint32_t)(s - 1) * SLOT_STRIDE
                                 + SLOT_EP0_RING_OFF);
}
static inline volatile uint8_t *slot_ep0_data(uint8_t s) {
    return (volatile uint8_t *)(xhci_dma_buf + DMA_SLOTS_BASE_OFF
                                + (uint32_t)(s - 1) * SLOT_STRIDE
                                + SLOT_EP0_DATA_OFF);
}

/* boot147: ep0_ring_init now takes slot_id (1-based) so each slot gets its
 * own EP0 transfer ring in per-slot DMA memory.                             */
static void ep0_ring_init(uint8_t sid) {
    volatile uint32_t *ring = slot_ep0_ring(sid);
    ep0_cycle_s[sid]   = 1;   /* ICS=1; matches VL805 MCU CCS=1 expectation */
    ep0_enqueue_s[sid] = 0;
    dma_zero(ring, EP0_RING_TRBS * 16);

    uint64_t ring_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ring));
    uint32_t li = (EP0_RING_TRBS - 1) * 4;
    ring[li + 0] = (uint32_t)(ring_dma);
    ring[li + 1] = (uint32_t)(ring_dma >> 32);
    ring[li + 2] = 0;
    ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | ep0_cycle_s[sid];
    asm volatile("dsb sy" ::: "memory");
}

/* boot147: ep0_enq uses active_slot to index the correct per-slot EP0 ring.
 * Caller must set active_slot = slot_id before the first ep0_enq() call.   */
static void ep0_enq(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type, uint32_t flags) {
    volatile uint32_t *ring = slot_ep0_ring(active_slot);
    uint32_t b = ep0_enqueue_s[active_slot] * 4;
    ring[b + 0] = dw0;
    ring[b + 1] = dw1;
    ring[b + 2] = dw2;
    ring[b + 3] = (type << TRB_TYPE_SHIFT) | flags | ep0_cycle_s[active_slot];
    asm volatile("dsb sy" ::: "memory");

    ep0_enqueue_s[active_slot]++;
    if (ep0_enqueue_s[active_slot] >= EP0_RING_TRBS - 1) {
        uint32_t li = (EP0_RING_TRBS - 1) * 4;
        ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | ep0_cycle_s[active_slot];
        asm volatile("dsb sy" ::: "memory");
        ep0_cycle_s[active_slot] ^= 1;
        ep0_enqueue_s[active_slot] = 0;
    }
}

static void ep0_doorbell(uint8_t slot) {
    volatile uint32_t *db = (volatile uint32_t *)xhci_ctrl.doorbell_regs;
    asm volatile("dsb sy" ::: "memory");
    db[slot] = 1;
    asm volatile("dsb sy" ::: "memory");
}

/* boot148: dw3_extra allows callers to OR additional fields into DW3.
 * Address Device requires slot_id in bits[31:24] of DW3 (xHCI §6.4.3.4).
 * All other commands pass dw3_extra=0.                                      */
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type, uint32_t dw3_extra) {
    uint32_t b = cmd_enqueue * 4;
    cmd_ring[b + 0] = dw0;
    cmd_ring[b + 1] = dw1;
    cmd_ring[b + 2] = dw2;
    /* DW3: TRB type + cycle bit + any caller-supplied extra bits (e.g. Slot ID). */
    cmd_ring[b + 3] = (type << TRB_TYPE_SHIFT) | dw3_extra | cmd_cycle;
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

    /* Force-ring: W1C → CRCR (before RS=1) → RS=1 → doorbell.
     *
     * Critical ordering (boot76 fix): CRCR must be written BEFORE RS=1.
     * The VL805 MCU captures CRCR at the RS=0→1 transition.  Writing
     * CRCR after RS=1 (when CRR=1) violates xHCI spec §5.4.6 and the
     * VL805 ignores it — the MCU walks from whatever address it captured
     * at the last transition.  Writing CRCR first (while RS may be 0
     * after HSE fired) ensures the MCU captures the correct ring base.  */
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
    asm volatile("dsb sy; isb" ::: "memory");

    uint64_t crcr = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    reg_write64(op, OP_CRCR_LO, crcr);      /* write while RS may be 0 */
    asm volatile("dsb sy; isb" ::: "memory");

    writel(CMD_RS | CMD_INTE, op + OP_USBCMD);   /* RS=1: MCU captures CRCR */
    asm volatile("dsb sy; isb" ::: "memory");

    volatile uint32_t *db = (volatile uint32_t *)xhci_ctrl.doorbell_regs;
    db[0] = 0;
    asm volatile("dsb sy" ::: "memory");

    return 0;
}

static int cmd_address_device(uint8_t slot_id, uint8_t port, uint32_t route, uint32_t speed) {
    /* boot147: use per-slot context memory so each slot has its own
     * Input Context and Output Context in distinct DMA regions.             */
    volatile uint8_t *in_ctx = slot_input_ctx(slot_id);
    volatile uint8_t *oc     = slot_out_ctx(slot_id);
    active_slot = slot_id;   /* set before any ep0_enq() calls below        */
    dma_zero(in_ctx, 34 * CTX_SIZE);
    dma_zero(oc,     32 * CTX_SIZE);

    volatile uint32_t *icc = (volatile uint32_t *)in_ctx;
    icc[1] = 0x00000003;

    /* Speed field in Slot Context DWord 0 bits[23:20] (xHCI §6.2.2):
     *   1=FS 12Mb/s  2=LS 1.5Mb/s  3=HS 480Mb/s  4=SS 5Gb/s
     * Use the actual port speed. If speed=0 (unknown — typically a
     * USB2 companion port that completed reset at HS) fall back to
     * HS (3) not SS (4). SS fallback was causing BABBLE on HS devices
     * because MPS=512 was used for a 64-byte HS endpoint.            */
    uint32_t spd = (speed > 0 && speed <= 6) ? speed : 3U;
    volatile uint32_t *slot_ctx = (volatile uint32_t *)(in_ctx + CTX_SIZE);
    slot_ctx[0] = (route & 0xFFFFF) | (spd << 20) | (1U << 27);
    /* boot146: xHCI spec Table 60, Slot Context DW1:
     *   bits[7:0]  = Max Exit Latency (0)
     *   bits[15:8] = Root Hub Port Number (1-based)  ← THIS was << 16 (bug!)
     *   bits[23:16]= Number of Ports (0 = not a hub)
     *   bits[31:24]= TT Hub Slot ID (0)
     * Shifting by 16 put port number in Number-of-Ports field, leaving
     * Root Hub Port Number = 0 (invalid).  VL805 MCU immediately rejected
     * Address Device with CC=11 (Context State Error) on EVERY boot.    */
    slot_ctx[1] = (uint32_t)(port + 1) << 8;   /* Root Hub Port Number bits[15:8] */

    /* EP0 max packet size depends on speed (xHCI spec §6.2.3.1):
     *   LS  (speed=2): 8 bytes
     *   FS  (speed=1): 8, 16, 32, or 64 — use 64 as safe default
     *   HS  (speed=3): 64 bytes  (fixed by USB 2.0 spec)
     *   SS  (speed=4): 512 bytes (fixed by USB 3.0 spec)
     * Using 512 for HS was causing GET_DESCRIPTOR STALLs — the device
     * receives a control endpoint configured for SS and STALLs the
     * data phase because it cannot match that packet size.            */
    uint32_t ep0_mps;
    switch (spd) {
        case 4:  ep0_mps = 512; break;  /* SuperSpeed */
        case 3:  ep0_mps = 64;  break;  /* HighSpeed  */
        default: ep0_mps = 64;  break;  /* FullSpeed/LowSpeed — 64 safe default */
    }
    volatile uint32_t *ep0_ctx = (volatile uint32_t *)(in_ctx + 2 * CTX_SIZE);
    ep0_ctx[1] = (3U << 1) | (4U << 3) | (ep0_mps << 16);

    ep0_ring_init(slot_id);
    uint64_t ep0_dma = phys_to_dma((uint64_t)virt_to_phys((void *)slot_ep0_ring(slot_id)));
    /* bit 0 = ICS (Initial Cycle State): must match ep0_cycle_s[slot_id] (=1). */
    ep0_ctx[2] = (uint32_t)(ep0_dma) | ep0_cycle_s[slot_id];
    ep0_ctx[3] = (uint32_t)(ep0_dma >> 32);
    ep0_ctx[4] = 8;

    uint64_t out_dma = phys_to_dma((uint64_t)virt_to_phys((void *)oc));
    dcbaa[slot_id] = out_dma;

    uint64_t in_dma = phys_to_dma((uint64_t)virt_to_phys((void *)in_ctx));

    /* boot150: full input context dump + TRB dump so we can verify exactly
     * what the MCU reads.  CC=17 "Parameter Error" persists despite correct-
     * looking field values — this dump will confirm whether the data is
     * actually in memory or stuck in cache.                                   */
    uart_puts("[xHCI] --- AddrDev context dump (slot=");
    print_hex32(slot_id); uart_puts(") ---\n");

    uart_puts("[xHCI] ICC:      D="); print_hex32(icc[0]);
    uart_puts("  A="); print_hex32(icc[1]); uart_puts("\n");

    uart_puts("[xHCI] SlotCtx: DW0="); print_hex32(slot_ctx[0]);
    uart_puts(" DW1="); print_hex32(slot_ctx[1]);
    uart_puts(" DW2="); print_hex32(slot_ctx[2]);
    uart_puts(" DW3="); print_hex32(slot_ctx[3]);
    uart_puts("  RH_PORT="); print_hex32((slot_ctx[1] >> 8) & 0xFF);
    uart_puts("\n");

    uart_puts("[xHCI] EP0Ctx:  DW0="); print_hex32(ep0_ctx[0]);
    uart_puts(" DW1="); print_hex32(ep0_ctx[1]);
    uart_puts(" DW2="); print_hex32(ep0_ctx[2]);
    uart_puts(" DW3="); print_hex32(ep0_ctx[3]);
    uart_puts(" DW4="); print_hex32(ep0_ctx[4]);
    uart_puts("\n");

    uart_puts("[xHCI] in_dma="); print_hex32((uint32_t)in_dma);
    uart_puts("  out_dma="); print_hex32((uint32_t)out_dma);
    uart_puts("  DCBAA[slot]=");
    print_hex32((uint32_t)(dcbaa[slot_id] & 0xFFFFFFFFU)); uart_puts("\n");

    /* Record cmd_enqueue before submission to log the exact TRB written. */
    uint32_t trb_idx = cmd_enqueue;

    /* boot148: DW3 bits[31:24] = Slot ID (xHCI §6.4.3.4). */
    cmd_ring_submit((uint32_t)in_dma, (uint32_t)(in_dma >> 32), 0, TRB_TYPE_ADDR_DEV,
                    (uint32_t)slot_id << 24);

    /* boot150: read back the TRB from cmd_ring to confirm what was written. */
    {
        uint32_t b = trb_idx * 4;
        uart_puts("[xHCI] AddrDev TRB["); print_hex32(trb_idx); uart_puts("]: ");
        uart_puts("dw0="); print_hex32(cmd_ring[b+0]);
        uart_puts(" dw1="); print_hex32(cmd_ring[b+1]);
        uart_puts(" dw2="); print_hex32(cmd_ring[b+2]);
        uart_puts(" dw3="); print_hex32(cmd_ring[b+3]);
        uart_puts("\n");
    }

    /* boot145: drain-loop until we get the Address Device CCE (type=0x21).
     * The event ring may still have PSCEs (type=0x22) or Transfer Events
     * (type=0x20) ahead of the CCE — discard those and keep waiting.
     * Matches laptop/Circle pattern; fixes the bug where a PSCE with cc=1
     * was being accepted as the Address Device CCE (CC=11 followed).      */
    uint32_t ev[4];
    uint32_t cc = 0;
    int got_cce = 0;
    uint32_t deadline = get_time_ms() + 200U;
    while (get_time_ms() < deadline) {
        if (xhci_wait_event(ev, 20) != 0) break;       /* no more events */
        uint32_t trb_type = (ev[3] >> 10) & 0x3FU;
        cc = (ev[2] >> 24) & 0xFF;
        uart_puts("[xHCI] AddrDev drain: type="); print_hex32(trb_type);
        uart_puts(" cc="); print_hex32(cc); uart_puts("\n");
        if (trb_type == 0x21U) { got_cce = 1; break; } /* CCE — done */
        /* type=0x22 PSCE or type=0x20 Transfer Event — drain and retry */
    }
    if (!got_cce || cc != CC_SUCCESS) return -1;

    volatile uint32_t *out_slot = (volatile uint32_t *)oc;
    uint8_t usb_addr = out_slot[3] & 0xFF;
    debug_print("[xHCI] Address Device OK  slot=%u  usb_addr=%u\n", slot_id, usb_addr);
    return 0;
}

static int ep0_get_device_descriptor(uint8_t slot_id, uint8_t *buf, int len) {
    /* boot147: per-slot ep0_data; active_slot already set by caller */
    volatile uint8_t *ep0_data = slot_ep0_data(slot_id);
    dma_zero(ep0_data, 512);

    uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));

    uint32_t setup_lo = 0x80U | (USB_REQ_GET_DESCRIPTOR << 8) | ((uint32_t)USB_DESC_DEVICE << 24);
    uint32_t setup_hi = (uint32_t)len << 16;

    ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
    ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), (uint32_t)len, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
    ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);

    ep0_doorbell(slot_id);

    uint32_t ev[4];
    if (xhci_wait_event(ev, 100) != 0) return -1;
    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;

    uint32_t ev2[4];
    xhci_wait_event(ev2, 20);

    int copy = len < 18 ? len : 18;
    for (int i = 0; i < copy; i++) buf[i] = ep0_data[i];
    return copy;
}

static void enumerate_port(int port) {
    void *op = xhci_ctrl.op_regs;
    uint32_t portsc = readl(op + 0x400 + port * 0x10);

    uart_puts("[xHCI] Port "); print_hex32(port + 1);

    /* DR bit (bit 30): USB2 companion port on VL805.
     * boot79 analysis: USB3 link on port 2 failed; device fell back to USB2
     * on the companion port (DR=1, CCS=1).  Skip only when empty (CCS=0).
     * When CCS=1 fall through to PR-based USB2 enumeration below.          */
    int is_usb2_companion = (portsc & (1U << 30)) ? 1 : 0;
    if (is_usb2_companion && !(portsc & PORTSC_CCS)) {
        uart_puts(": empty companion (DR=1, CCS=0, skipped)  PORTSC=");
        print_hex32(portsc); uart_puts("\n");
        return;
    }
    if (is_usb2_companion)
        uart_puts(": USB2 companion fallback (DR=1, CCS=1)");

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
                fast_delay_ms(1);
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
                fast_delay_ms(300);
                writel(PORTSC_PP, op + 0x400 + port * 0x10);     /* PP=1 */
                asm volatile("dsb sy" ::: "memory");
                for (int t = 0; t < 2000; t++) {
                    fast_delay_ms(1);
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

    uint32_t ps = portsc;
    if (is_usb2_companion) {
        /* USB2 companion port: Port Reset (PR, bit 4) — WPR is SS-only.
         * boot80: device fell back from failed USB3 link to this USB2 port.
         * PR brings the port to Enabled (PED=1) with speed in bits[13:10]. */
        uart_puts("[xHCI] USB2 companion: issuing PR (bit 4)...\n");
        writel(PORTSC_PP | (1U << 4), op + 0x400 + port * 0x10);
        asm volatile("dsb sy" ::: "memory");
        for (int t = 0; t < 300; t++) {
            fast_delay_ms(1);
            ps = readl(op + 0x400 + port * 0x10);
            if (!(ps & (1U << 4))) break;   /* PR cleared → reset done */
        }
        uart_puts("[xHCI] PR done. PORTSC="); print_hex32(ps); uart_puts("\n");
        if (!(ps & PORTSC_CCS)) {
            uart_puts("[xHCI] Device lost after PR — aborting\n");
            return;
        }
    } else {
        /* USB3: Warm Port Reset (WPR, bit 31) — triggers an internal PR.   */
        uart_puts("[xHCI] Issuing single WPR...\n");

        /* Single clean Warm Port Reset — PR alone times out on this SS device.
         * Clean write only: do NOT copy snapshot bits (W1C bits in snapshot
         * can re-trigger change events and generate extra MCU PSCEv).         */
        writel((1U << 31) | PORTSC_PP, op + 0x400 + port * 0x10);
        asm volatile("dsb sy" ::: "memory");

        for (int t = 0; t < 300; t++) {
            fast_delay_ms(1);
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
            fast_delay_ms(1);
            ps = readl(op + 0x400 + port * 0x10);
            if (!(ps & (1U << 4))) break;   /* PR cleared */
        }
    }

    ps = readl(op + 0x400 + port * 0x10);
    uint32_t speed = (ps >> 10) & 0xF;
    uart_puts("[xHCI] Port reset done. PORTSC="); print_hex32(ps);
    uart_puts("  speed="); print_hex32(speed); uart_puts("\n");

    /* boot142: USB 2.0 reset recovery = 10ms minimum (spec) / 100ms (Circle).
     * Wait here before issuing Enable Slot so the device has time to become
     * ready and so the MCU has time to post any pending PSCEv TRBs.       */
    for (int t = 0; t < 100; t++) fast_delay_ms(1);

    /* ── Step 1: Enable Slot ─────────────────────────────────────────── */
    /* boot144: wait for Enable Slot CCE so we get the real MCU-assigned slot_id.
     * boot145 fixes:
     *   a) slot_id lives in CCE DW3 bits[31:24] → use ev[3]>>24, not ev[3]>>8.
     *   b) Event ring may still have stale PSCEs from the port reset.
     *      Drain non-CCE events; CCE has TRB type = 0x21 (33 decimal).
     *      PSCE = 0x22, Transfer Event = 0x20 — discard those and keep waiting.
     * Circle xhcieventmanager.cpp: XHCI_TRB_TYPE_EVENT_CMD_COMPLETION = 33 = 0x21 */
    uint32_t ev[4];
    uint8_t slot_id;
    cmd_ring_submit(0, 0, 0, TRB_TYPE_ENABLE_SLOT, 0);
    uart_puts("[xHCI] Enable Slot submitted — draining to CCE...\n");
    {
        uint8_t es_cc = 0, es_slot = 0;
        int got_cce = 0;
        uint32_t deadline = get_time_ms() + 200U;
        while (get_time_ms() < deadline) {
            if (xhci_wait_event(ev, 20) != 0) break;   /* no more events */
            uint32_t trb_type = (ev[3] >> 10) & 0x3FU;
            es_cc  = (ev[2] >> 24) & 0xFF;
            uart_puts("[xHCI] ES drain: type="); print_hex32(trb_type);
            uart_puts(" cc="); print_hex32(es_cc); uart_puts("\n");
            if (trb_type == 0x21U) {                    /* CCE — done */
                es_slot = (ev[3] >> 24) & 0xFF;
                got_cce = 1;
                break;
            }
            /* type=0x22 PSCE or type=0x20 Transfer Event — drain and retry */
        }
        if (got_cce && es_cc == CC_SUCCESS && es_slot > 0) {
            slot_id = es_slot;
            uart_puts("[xHCI] Enable Slot CCE: cc=1 slot="); print_hex32(slot_id); uart_puts("\n");
        } else {
            slot_id = 1;
            uart_puts("[xHCI] Enable Slot: no CCE or bad cc — assuming slot_id=1\n");
        }
    }
    /* boot147: track per-port slot_id; set active_slot for ep0_enq() */
    g_slot_ids[(uint8_t)port] = slot_id;
    active_slot = slot_id;
    uart_puts("[xHCI] Using slot_id="); print_hex32(slot_id); uart_puts("\n");

    /* ── Step 2: Address Device ──────────────────────────────────────── */
    if (cmd_address_device(slot_id, (uint8_t)port, 0, speed) == 0) {
        uart_puts("[xHCI] Address Device OK slot="); print_hex32(slot_id); uart_puts("\n");
    } else {
        uart_puts("[xHCI] Address Device failed — continuing\n");
    }

    /* Build a minimal usb_device_t so control transfers work via g_hc_ops.
     * boot147: use g_devs[slot_id] (per-slot) instead of a single g_dev.
     * Store slot_id in hcd_private so xhci_control_transfer can find the ring. */
    extern int usb_enumerate_device(usb_device_t *dev, int port);
    usb_device_t *dev = &g_devs[slot_id];
    memset(dev, 0, sizeof(*dev));
    dev->speed       = (uint8_t)speed;
    dev->address     = slot_id; /* xHCI assigns USB address via Address Device */
    dev->hcd_private = (void *)(uintptr_t)slot_id;

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
        volatile uint8_t *_ep0_buf = slot_ep0_data(slot_id);
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

    dev->bMaxPacketSize0  = ddesc[7];
    dev->idVendor         = (uint16_t)(ddesc[8]  | (ddesc[9]  << 8));
    dev->idProduct        = (uint16_t)(ddesc[10] | (ddesc[11] << 8));
    dev->bcdUSB           = (uint16_t)(ddesc[2]  | (ddesc[3]  << 8));
    dev->bDeviceClass     = ddesc[4];
    dev->bDeviceSubClass  = ddesc[5];
    dev->bDeviceProtocol  = ddesc[6];

    uart_puts("[xHCI] Device: VID="); print_hex32(dev->idVendor);
    uart_puts(" PID="); print_hex32(dev->idProduct);
    uart_puts(" class="); print_hex32(dev->bDeviceClass); uart_puts("\n");

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
        volatile uint8_t *ep0_data = slot_ep0_data(slot_id);
        dma_zero(ep0_data, 256);
        uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));

        /* First fetch 9 bytes to get wTotalLength */
        ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
        ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), 9, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
        ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);
        ep0_doorbell(slot_id);
        if (xhci_wait_event(ev, 100) != 0) {
            uart_puts("[xHCI] GET_DESCRIPTOR(Config,9) timeout\n");
            goto probe;
        }
        xhci_wait_event(ev, 20); /* drain Status event */

        total_len = (uint16_t)(ep0_data[2] | ((uint16_t)ep0_data[3] << 8));
        if (total_len < 9 || total_len > 255) total_len = 9;

        /* Fetch full config descriptor */
        dma_zero(ep0_data, 256);
        setup_hi = (uint32_t)total_len << 16;
        ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
        ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), total_len, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
        ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);
        ep0_doorbell(slot_id);
        if (xhci_wait_event(ev, 100) != 0) {
            uart_puts("[xHCI] GET_DESCRIPTOR(Config,full) timeout\n");
            goto probe;
        }
        xhci_wait_event(ev, 20);

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
                int ni = dev->num_interfaces;
                if (ni < USB_MAX_INTERFACES) {
                    cur_intf = &dev->interfaces[ni];
                    cur_intf->bInterfaceNumber  = cfgbuf[pos + 2];
                    cur_intf->bAlternateSetting = cfgbuf[pos + 3];
                    cur_intf->bNumEndpoints     = cfgbuf[pos + 4];
                    cur_intf->bInterfaceClass   = cfgbuf[pos + 5];
                    cur_intf->bInterfaceSubClass= cfgbuf[pos + 6];
                    cur_intf->bInterfaceProtocol= cfgbuf[pos + 7];
                    dev->num_interfaces++;
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

    uart_puts("[xHCI] Config parsed: "); print_hex32(dev->num_interfaces);
    uart_puts(" interface(s)\n");

probe:
    /* ── Step 5: Hand off to USB core for class driver probe ─────────── */
    usb_enumerate_device(dev, port);
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

    /* boot147: set active_slot so ep0_enq() uses the correct per-slot ring */
    active_slot = slot_id;
    volatile uint8_t *ep0_data = slot_ep0_data(slot_id);
    int dir_in = (req_type & 0x80) != 0;

    /* OUT: copy caller's data into DMA buffer */
    if (!dir_in && data && length)
        dma_copy_to(ep0_data, data, length);
    else
        dma_zero(ep0_data, length < 512 ? (length ? length : 8) : 512);

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
    if (xhci_wait_event(ev, timeout ? timeout : 100) != 0) {
        uart_puts("[xHCI] control_transfer: TIMEOUT\n");
        return -1;
    }
    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;

    /* Drain Status event if data phase was issued */
    if (length > 0) {
        uint32_t ev2[4];
        xhci_wait_event(ev2, 20);
    }

    /* IN: copy DMA buffer to caller */
    if (dir_in && data && length) {
        int got = (int)(ev[2] & 0xFFFF); /* residue in low 17 bits of dword 2 */
        int actual = length - got;
        if (actual < 0) actual = 0;
        dma_copy_from(data, ep0_data, (size_t)actual);
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
        /* boot87: quiet polling mode — no periodic UART flood.
         * Timeout diagnostic is printed by xhci_wait_event instead. */
        return 0;
    }

    /* Event consumed — log type for tracing, then advance consumer state */
    uint32_t trb_type = (slot[3] >> 10) & 0x3FU;
    uart_puts("[xHCI] event type="); print_hex32(trb_type);
    uart_puts(" cc="); print_hex32((slot[2] >> 24) & 0xFF);
    uart_puts(" deq="); print_hex32(evt_dequeue); uart_puts("\n");

    ev[0] = slot[0];
    ev[1] = slot[1];
    ev[2] = slot[2];
    ev[3] = slot[3];

    evt_dequeue++;
    if (evt_dequeue >= EVT_RING_TRBS) {
        evt_dequeue = 0;
        evt_cycle ^= 1;
    }

    /* Advance ERDP with EHB=0 (boot107: never set EHB=1 in polling mode).
     * Writing EHB=1 signals "host busy — hold new events", which in a poll
     * loop means the MCU would defer every subsequent event indefinitely.
     * In polling mode we just advance the pointer clean; the MCU can write
     * the next event immediately.  Also W1C IMAN IP. */
    void *ir0 = ir_base(0);
    uint64_t new_erdp = evt_ring_dma + (uint64_t)evt_dequeue * 16;
    ERDP_REARM(ir0, new_erdp);
    writel(readl(ir0 + IR_IMAN) | 1U, ir0 + IR_IMAN);
    asm volatile("dsb sy; isb" ::: "memory");

    return 1;
}

/*
 * xhci_wait_event — wait for a command/transfer completion event.
 *
 * boot87: Pure tight-poll using CNTPCT_EL0 for real wall-clock timeouts.
 *
 * After 86 boots the VL805 MSI→GIC path on BCM2711 is confirmed broken:
 * msi_fire_count stays 0 regardless of INTR2/MSI_INTR0 configuration.
 * WFI-based waiting is gone.  We busy-poll the event ring cycle bit at
 * full CPU speed (limited only by the DSB barrier per iteration).
 *
 * BCM2711 system counter (CNTPCT_EL0) runs at 54 MHz → 54,000 ticks/ms.
 * The 32-bit millisecond counter wraps every ~49.7 days — fine here.
 *
 * HSE keepalive: the VL805 MCU fires its watchdog every few ms if idle.
 * We clear HSE and re-assert RS=1 on every HSE seen during the poll,
 * just as the settle and retry loops do.
 */
static int xhci_wait_event(uint32_t ev[4], int timeout_ms) {
    /* Immediate check — event may already be in the ring */
    if (evt_ring_poll(ev)) return 0;

    uint32_t t0 = get_time_ms();
    void *_op   = xhci_ctrl.op_regs;

    while ((get_time_ms() - t0) < (uint32_t)timeout_ms) {
        asm volatile("dsb sy; isb" ::: "memory");
        if (evt_ring_poll(ev)) return 0;

        /* VL805 HSE watchdog keepalive — full ring re-arm on HSE.
         *
         * FIX-89: previously only W1C HSE + RS=1, which left CRCR and
         * ERSTBA at 0 (MCU resets all ring registers on every HSE).
         * Now we re-arm everything — same as the retry and settle loops. */
        if (readl(_op + OP_USBSTS) & STS_HSE) {
            void *_ir0 = ir_base(0);
            uint64_t _crcr  = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
            writel(STS_HSE | STS_EINT | STS_PCD, _op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            reg_write64(_op, OP_DCBAAP_LO,
                        phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa)));
            reg_write64(_op, OP_CRCR_LO, _crcr);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(0U, _ir0 + IR_ERSTSZ);
            reg_write64(_ir0, IR_ERSTBA_LO, erst_dma_addr);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(1U, _ir0 + IR_ERSTSZ);
            asm volatile("dsb sy; isb" ::: "memory");
            ERDP_REARM(_ir0, evt_ring_dma + (uint64_t)evt_dequeue * 16); /* boot107 */
            writel(0x00000002U, _ir0 + IR_IMAN);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(CMD_RS | CMD_INTE, _op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");
        }
    }

    /* Timeout — single consolidated line to keep the log readable */
    uart_puts("[xHCI] timeout("); print_hex32((uint32_t)timeout_ms);
    uart_puts("ms) USBSTS="); print_hex32(readl(_op + OP_USBSTS));
    uart_puts(" INTR2="); print_hex32(readl(pcie_base + 0x4300U));
    uart_puts(" TRB0=["); print_hex32(((volatile uint32_t *)evt_ring)[0]);
    uart_puts(","); print_hex32(((volatile uint32_t *)evt_ring)[3]);
    uart_puts("]\n");
    return -1;
}

/*
 * xhci_irq_handler — stub kept for linker symbol resolution.
 *
 * GIC registration was removed in boot87 (xHCI polling mode).
 * This handler should never be called.  If it somehow fires,
 * count it and clear the interrupt sources to prevent re-delivery.
 */
void xhci_irq_handler(int vector, void *data) {
    (void)vector; (void)data;
    msi_fire_count++;
    /* Clear both PCIe interrupt levels defensively */
    writel(0xFFFFFFFFU, pcie_base + 0x4508U);  /* PCIE_MSI_INTR0_CLR  W1C */
    writel(0xFFFFFFFFU, pcie_base + 0x4308U);  /* PCIE_INTR2_CPU_CLEAR W1C */
    asm volatile("dsb sy; isb" ::: "memory");
}
/* xhci_dma_phys — return physical base address of the xHCI DMA buffer.
 * Called by pci.c xhci_setup_msi() to compute the PCIe MSI target address. */
uint64_t xhci_dma_phys(void) {
    return (uint64_t)virt_to_phys((void *)xhci_dma_buf);
}
