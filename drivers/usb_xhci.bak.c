/**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver — spec-compliant init for VL805 on Pi 4
 *
 * Implements the full xHCI §4.2 initialization sequence:
 *   2.1 Read capabilities
 *   2.2 Controller reset (HCRST + CNR poll)
 *   2.3 Scratchpad buffers (if required by HCSPARAMS2)
 *   2.4 Device Context Base Address Array (DCBAA)
 *   2.5 Command Ring
 *   2.6 Event Ring + ERST
 *   2.7 Interrupter (IMAN, IMOD)
 *   2.8 Run (RS=1, INTE=1) + HCH poll
 *   2.9 Port power-on (PP=1 per port)
 *
 * MEMORY MODEL
 * ============
 * All DMA structures live in a single static 64-byte-aligned buffer placed
 * in the .xhci_dma linker section.  mmu.c maps this section as Device
 * nGnRnE memory (non-cacheable, strongly ordered) at 4 KB granularity via
 * an L3 page table split within the first 2 MB of RAM.
 *
 * Consequences:
 *   - CPU writes to xhci_dma_buf are immediately visible to the hardware;
 *     no dc civac cache flush is needed for those structures.
 *   - A DSB barrier before any register write that causes the controller
 *     to read a DMA structure ensures ordering.
 *   - Scratchpad pages (if any) are kcalloc'd from the Normal WB heap and
 *     must still be flushed with dc civac before their addresses are given
 *     to the controller.
 *
 * virt_to_phys() returns the physical address (identity-mapped in Phoenix).
 *
 * PSEUDOCODE CORRECTIONS vs Intel xHCI §4.2 sample:
 *   - PORTSC.PP  = bit 9  (not bit 0; bit 0 = CCS)
 *   - USBSTS.CNR = bit 11 (not bit 0; bit 0 = HCH)
 *   - Scratchpad needed: HCSPARAMS2[31:27|25:21] > 0
 *   - RTSOFF at cap+0x18, DBOFF at cap+0x14
 *   - ERST entry size field is in TRBs, not bytes
 *
 * VL805 (from boot logs):
 *   CAPLENGTH=0x20, HCIVERSION=0x0100, MaxPorts=5, MaxSlots=32
 *   AC64=1 (64-bit addresses required), CSZ=0, Scratchpad=31
 */

#include "kernel.h"
#include "usb_xhci.h"
#include <string.h>

/* ── Capability register offsets (from xhci_base) ─────────────────────── */
#define CAP_CAPLENGTH   0x00  /* [7:0]=length  [31:16]=HCIVERSION        */
#define CAP_HCSPARAMS1  0x04  /* MaxSlots[7:0] MaxIntrs[18:8] MaxPorts[31:24] */
#define CAP_HCSPARAMS2  0x08  /* IST[3:0] ERST_Max[7:4] SPB_hi[25:21] SPB_lo[31:27] */
#define CAP_HCCPARAMS1  0x10  /* AC64[0] BNC[1] CSZ[2] PPC[3] ...       */
#define CAP_DBOFF       0x14  /* Doorbell array offset [31:2]            */
#define CAP_RTSOFF      0x18  /* Runtime register space offset [31:5]    */

/* ── Operational register offsets (from op_base = xhci_base + CAPLENGTH) ─ */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR_LO      0x18  /* Command Ring Control low 32             */
#define OP_CRCR_HI      0x1C  /* Command Ring Control high 32            */
#define OP_DCBAAP_LO    0x30  /* DCBAAP low 32                           */
#define OP_DCBAAP_HI    0x34  /* DCBAAP high 32                          */
#define OP_CONFIG       0x38  /* MaxSlotsEn[7:0]                         */

/* USBCMD bits */
#define CMD_RS          (1U << 0)   /* Run/Stop                          */
#define CMD_HCRST       (1U << 1)   /* Host Controller Reset             */
#define CMD_INTE        (1U << 2)   /* Interrupter Enable                */
#define CMD_HSEE        (1U << 3)   /* Host System Error Enable          */

/* USBSTS bits */
#define STS_HCH         (1U << 0)   /* HC Halted                         */
#define STS_HSE         (1U << 2)   /* Host System Error                 */
#define STS_EINT        (1U << 3)   /* Event Interrupt                   */
#define STS_PCD         (1U << 4)   /* Port Change Detect                */
#define STS_CNR         (1U << 11)  /* Controller Not Ready              */

/* ── Runtime interrupter offsets (from runtime_base + 0x20 + n*0x20) ───── */
#define IR_IMAN         0x00  /* IP[0] IE[1]                             */
#define IR_IMOD         0x04  /* IMODI[15:0] IMODC[31:16]               */
#define IR_ERSTSZ       0x08  /* Segment table size [15:0]               */
/* 0x0C reserved */
#define IR_ERSTBA_LO    0x10
#define IR_ERSTBA_HI    0x14
#define IR_ERDP_LO      0x18  /* DESI[2:0] EHB[3] ptr[63:4]            */
#define IR_ERDP_HI      0x1C

/* ── PORTSC bits (port regs at op_base + 0x400 + port*0x10) ────────────── */
#define PORTSC_CCS      (1U <<  0)  /* Current Connect Status            */
#define PORTSC_PED      (1U <<  1)  /* Port Enabled/Disabled             */
#define PORTSC_PP       (1U <<  9)  /* Port Power                        */
#define PORTSC_SPEED_SHIFT  10
#define PORTSC_SPEED_MASK   (0xFU << 10)
/* Change-status bits — write-1-to-clear; mask them out on RMW writes   */
#define PORTSC_WIC      0x00FE0000U /* CSC|PEC|WRC|OCC|PRC|PLC|CEC     */

/* ── TRB control dword bits ─────────────────────────────────────────────── */
#define TRB_CYCLE           (1U <<  0)
#define TRB_TC              (1U <<  1)  /* Toggle Cycle (Link TRB)       */
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE_LINK        6   /* xHCI spec Table 6-91: Link TRB = type 6   */
#define TRB_TYPE_NOOP_CMD   23   /* xHCI spec Table 6-91: No-Op Command = 23  */

/* Command TRB types */
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_ADDRESS_DEV   11
#define TRB_TYPE_EVAL_CTX      13
/* Transfer TRB types */
#define TRB_TYPE_SETUP_STAGE    2
#define TRB_TYPE_DATA_STAGE     3
#define TRB_TYPE_STATUS_STAGE   4
/* Event TRB types */
#define TRB_TYPE_TRANSFER_EVT  32
#define TRB_TYPE_CMD_CMPL_EVT  33
#define TRB_TYPE_PORT_CHNG_EVT 34

/* Setup stage TRB flags */
#define TRB_IDT             (1U << 6)   /* Immediate Data               */
#define TRB_TRT_IN          (3U << 16)  /* Transfer Type: IN data stage */
/* Data stage direction */
#define TRB_DIR_IN          (1U << 16)

/* Event completion codes (TRB status [31:24]) */
#define CC_SUCCESS          1
#define CC_SHORT_PACKET     13

/* Context size: VL805 CSZ=0 → 32 bytes per context entry */
#define CTX_ENTRY_SIZE  32

/* ── DMA buffer layout ───────────────────────────────────────────────────── */
/*
 * Single statically-allocated, 64-byte-aligned buffer.  Must be placed
 * in non-cacheable memory by the linker/MMU.
 *
 * [0x0000 ] DCBAA        2048 B  (256 × 8-byte slot pointers)
 * [0x0800 ] cmd_ring     1024 B  (64 × 16-byte TRBs)
 * [0x0C00 ] evt_ring     1024 B  (64 × 16-byte TRBs)
 * [0x1000 ] erst           64 B  (1 × 16-byte ERST entry, 64-byte padded)
 * [0x1040 ] scratch_arr   512 B  (up to 64 × 8-byte scratchpad pointers)
 * [0x1240 ] input_ctx    1088 B  (Input Context: ctrl + 32 slot/ep @ 32 B each)
 * [0x16C0 ] out_ctx      1024 B  (Output Device Context: 32 × 32 B)
 * [0x1AC0 ] ep0_ring     1024 B  (64 × 16-byte TRBs for EP0 transfers)
 * [0x1EC0 ] ep0_data      512 B  (descriptor receive buffer)
 * [0x2000 ] scratch_pages MAX_SCRATCH_PAGES × 4096 B  (static, non-cacheable)
 *           VL805 needs 31 pages = 0x1F000 B
 * Total    0x21000 = 135168 B
 * This fits within one 2 MB block (the MMU remaps it entirely as Device).
 */
#define DMA_DCBAA_OFF      0x0000
#define DMA_CMD_RING_OFF   0x0800
#define DMA_EVT_RING_OFF   0x0C00
#define DMA_ERST_OFF       0x1000
#define DMA_SCRATCH_OFF    0x1040
#define DMA_INPUT_CTX_OFF  0x1240
#define DMA_OUT_CTX_OFF    0x16C0
#define DMA_EP0_RING_OFF   0x1AC0
#define DMA_EP0_DATA_OFF   0x1EC0
#define DMA_SCRATCH_PAGES_OFF 0x2000   /* scratchpad pages start here (4KB aligned) */
#define MAX_SCRATCH_PAGES  64          /* supports up to 64 pages; VL805 needs 31  */
#define DMA_BUF_SIZE       (DMA_SCRATCH_PAGES_OFF + MAX_SCRATCH_PAGES * 4096)
                           /* = 0x2000 + 0x40000 = 0x42000 = 270336 B           */

#define CMD_RING_TRBS    64
#define EVT_RING_TRBS    64

/*
 * xhci_dma_buf — points to the .xhci_dma section reserved by the linker.
 *
 * We do NOT use a C static array here.  A zero-initialised static array
 * always ends up in .bss regardless of __attribute__((section())) on many
 * GCC versions, making __xhci_dma_start == __xhci_dma_end and breaking the
 * MMU Device-memory remapping in mmu.c.
 *
 * Instead the linker script reserves exactly DMA_BUF_SIZE bytes in .xhci_dma
 * via '. += 0x1400' and exports __xhci_dma_start/__xhci_dma_end.
 * mmu.c reads those symbols to find and remap the pages as Device nGnRnE.
 * usb_xhci.c treats __xhci_dma_start as the base of the buffer.
 */
extern char __xhci_dma_start[];          /* linker symbol — start of .xhci_dma  */
#define xhci_dma_buf ((uint8_t *)__xhci_dma_start)

static volatile uint64_t *dcbaa;
static volatile uint32_t *cmd_ring;
static volatile uint32_t *evt_ring;
static volatile uint64_t *erst;
static volatile uint64_t *scratch_arr;

/* Per-device enumeration buffers (one device at a time in bare-metal) */
static volatile uint32_t *input_ctx;   /* Input Context (ctrl + 32 entries)  */
static volatile uint32_t *out_ctx;     /* Output Device Context              */
static volatile uint32_t *ep0_ring;    /* EP0 transfer ring                  */
static volatile uint8_t  *ep0_data;    /* descriptor receive buffer          */

/* EP0 transfer ring producer state */
static uint8_t  ep0_cycle   = 1;
static uint32_t ep0_enqueue = 0;

/* Event ring consumer state */
static uint32_t evt_dequeue = 0;   /* next TRB index to consume             */
static uint8_t  evt_cycle   = 1;   /* expected cycle bit                    */

/* ── Controller state ────────────────────────────────────────────────────── */
static xhci_controller_t xhci_ctrl;

static uint8_t  cmd_cycle   = 1;   /* producer cycle bit */
static uint32_t cmd_enqueue = 0;   /* next TRB slot index */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * phys_to_dma — convert a CPU physical address to a PCIe DMA bus address.
 *
 * BCM2711 PCIe inbound window (RC_BAR2): PCIe 0xC0000000..0xFFFFFFFF → CPU 0x0.
 * This mapping is silicon-fixed and INDEPENDENT of WIN0_LO (the outbound ATU
 * that routes CPU MMIO writes to the VL805 BAR at PCI address 0x00000000).
 *
 * To reach CPU physical address P via DMA, the VL805 must use PCIe address
 * P + 0xC0000000.  Outbound (CPU→PCIe) and inbound (PCIe→CPU) are separate
 * paths — WIN0_LO=0 does not imply DMA_OFFSET=0.
 */
#define DMA_OFFSET  0xC0000000ULL
static inline uint64_t phys_to_dma(uint64_t phys) {
    return phys + DMA_OFFSET;
}

/*
 * reg_write64 — write a 64-bit xHCI register.
 *
 * The xHCI spec §4.1 says 64-bit registers "shall be written as a Qword".
 * On AArch64 a single STR Xn instruction is a naturally-aligned 64-bit store
 * which the PCIe bus presents as a single 8-byte memory write transaction.
 *
 * This matters critically for CRCR: the VL805 latches the command ring
 * pointer only when it sees the full 64-bit value in one transaction.
 * Two sequential 32-bit writes (with or without a DSB between them) cause
 * the VL805 to silently discard CRCR_LO, leaving CRCR=0 and triggering HSE
 * immediately after RS=1.
 *
 * DCBAAP tolerated two 32-bit writes because it has no latch/trigger logic —
 * it just stores the address.  CRCR, ERSTBA, and ERDP must be written as
 * a single 64-bit transaction.
 */
static void reg_write64(void *base, uint32_t lo_off, uint64_t val) {
    volatile uint64_t *reg = (volatile uint64_t *)((uint8_t *)base + lo_off);
    *reg = val;
    asm volatile("dsb sy" ::: "memory");
}

static void delay_us(int us) {
    for (volatile int i = 0; i < us * 150; i++) {}
}

static void delay_ms(int ms) { delay_us(ms * 1000); }

/*
 * flush_normal_page — clean + invalidate one Normal-WB cacheable page.
 *
 * Used ONLY for scratchpad pages allocated via kcalloc() from the Normal WB
 * kernel heap.  The xHCI DMA ring buffers in xhci_dma_buf do NOT need this
 * because mmu.c maps .xhci_dma as Device nGnRnE (non-cacheable).
 */
static void flush_normal_page(void *start, size_t len) {
    uintptr_t a = (uintptr_t)start & ~63UL;
    uintptr_t e = (uintptr_t)start + len;
    for (; a < e; a += 64)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy; isb" ::: "memory");
}

/* Interrupter n register base inside runtime space */
static void *ir_base(int n) {
    return xhci_ctrl.runtime_regs + 0x20 + n * 0x20;
}

/* ── 2.1 Read capabilities ───────────────────────────────────────────────── */
static int read_caps(void) {
    void    *base = xhci_ctrl.cap_regs;
    uint32_t cap0 = readl(base + CAP_CAPLENGTH);
    uint8_t  clen = cap0 & 0xFF;
    uint16_t hver = cap0 >> 16;

    debug_print("[xHCI] CAPLENGTH=0x%02x  HCIVERSION=0x%04x\n", clen, hver);

    if (cap0 == 0xdeaddead || cap0 == 0xFFFFFFFF || clen < 0x10) {
        debug_print("[xHCI] ERROR: bad CAPLENGTH 0x%02x "
                    "— Memory Space not enabled or ATU/BAR mismatch\n", clen);
        return -1;
    }

    uint32_t hcs1 = readl(base + CAP_HCSPARAMS1);
    uint32_t hcs2 = readl(base + CAP_HCSPARAMS2);
    uint32_t hcc  = readl(base + CAP_HCCPARAMS1);

    xhci_ctrl.max_slots = hcs1 & 0xFF;
    xhci_ctrl.num_intrs = (hcs1 >>  8) & 0x7FF;
    xhci_ctrl.max_ports = (hcs1 >> 24) & 0xFF;

    /* Scratchpad count: HCSPARAMS2 bits [31:27] (lo) and [25:21] (hi) */
    uint32_t spb_lo = (hcs2 >> 27) & 0x1F;
    uint32_t spb_hi = (hcs2 >> 21) & 0x1F;
    xhci_ctrl.scratchpad_count = (spb_hi << 5) | spb_lo;

    xhci_ctrl.ac64 = hcc & 1U;          /* 64-bit addressing capable   */
    xhci_ctrl.csz  = (hcc >> 2) & 1U;   /* context size: 0=32B, 1=64B  */

    uint32_t rtsoff = readl(base + CAP_RTSOFF) & ~0x1FU;
    uint32_t dboff  = readl(base + CAP_DBOFF)  & ~0x03U;

    xhci_ctrl.cap_len       = clen;
    xhci_ctrl.op_regs       = xhci_ctrl.cap_regs + clen;
    xhci_ctrl.runtime_regs  = xhci_ctrl.cap_regs + rtsoff;
    xhci_ctrl.doorbell_regs = xhci_ctrl.cap_regs + dboff;

    debug_print("[xHCI] MaxSlots=%u  MaxPorts=%u  MaxIntrs=%u  Scratchpads=%u\n",
                xhci_ctrl.max_slots, xhci_ctrl.max_ports,
                xhci_ctrl.num_intrs, xhci_ctrl.scratchpad_count);
    debug_print("[xHCI] AC64=%u  CSZ=%u  RTSOFF=0x%x  DBOFF=0x%x  "
                "HCSPARAMS1=0x%08x  HCCPARAMS1=0x%08x\n",
                xhci_ctrl.ac64, xhci_ctrl.csz, rtsoff, dboff, hcs1, hcc);
    return 0;
}

/* ── 2.2 Reset ───────────────────────────────────────────────────────────── */
static int do_reset(void) {
    void *op = xhci_ctrl.op_regs;

    /*
     * Issue HCRST (xHCI Host Controller Reset).
     *
     * This is the standard xHCI reset sequence per §4.2.  We previously
     * skipped it thinking FLR was sufficient, but the two produce different
     * USBSTS outcomes after RS=1:
     *
     *   with HCRST:    USBSTS=0x1d (HCH+HSE+EINT+PCD) — controller writes an
     *                  event to the ring before halting.  EINT=1 means there is
     *                  diagnostic information available.
     *   without HCRST: USBSTS=0x05 (HCH+HSE only) — controller fails so fast
     *                  it cannot write any event at all.
     *
     * HCRST gives us more to work with.  Restore it here and read the full
     * event ring on failure to find the completion code.
     */
    if (!(readl(op + OP_USBSTS) & STS_HCH)) {
        writel(readl(op + OP_USBCMD) & ~CMD_RS, op + OP_USBCMD);
        for (int t = 400; t > 0; t--) {
            delay_us(500);
            if (readl(op + OP_USBSTS) & STS_HCH) break;
        }
        if (!(readl(op + OP_USBSTS) & STS_HCH)) {
            debug_print("[xHCI] ERROR: failed to halt before HCRST\n");
            return -1;
        }
    }

    /*
     * xHCI spec §4.2: "Software shall not set HCRST to '1' while CNR = '1'."
     * On boards where PCIe trains quickly (< 200ms from power-on), the VL805
     * MCU may still be in its internal xHCI init (CNR=1) even after the SPI
     * firmware-ready poll passes.  Wait up to 2s for CNR to clear.
     */
    {
        int cnr_ready = 0;
        uint32_t last_sts = 0xDEADBEEF;
        for (int t = 0; t < 2000; t++) {    /* 2000 * 1ms = 2s */
            uint32_t s = readl(op + OP_USBSTS);
            if (s == 0xFFFFFFFF) {
                debug_print("[xHCI] ERROR: device vanished waiting for CNR\n");
                return -1;
            }
            if (!(s & STS_CNR)) {
                debug_print("[xHCI] CNR cleared after %dms  USBSTS=0x%08x\n",
                            t, s);
                cnr_ready = 1;
                break;
            }
            /* Log progress on first read or on status change */
            if (t == 0 || s != last_sts) {
                debug_print("[xHCI]   CNR poll t=%dms  USBSTS=0x%08x\n", t, s);
                last_sts = s;
            }
            delay_ms(1);
        }
        if (!cnr_ready) {
            debug_print("[xHCI] ERROR: CNR never cleared after 2s"
                        " (USBSTS=0x%08x)\n", readl(op + OP_USBSTS));
            return -1;
        }
    }

    debug_print("[xHCI] Issuing HCRST\n");
    writel(readl(op + OP_USBCMD) | CMD_HCRST, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");

    for (int t = 5000; t > 0; t--) {   /* 5000 * 100µs = 500ms */
        delay_us(100);
        uint32_t cmd = readl(op + OP_USBCMD);
        uint32_t sts = readl(op + OP_USBSTS);
        if (cmd == 0xFFFFFFFF || sts == 0xFFFFFFFF) {
            debug_print("[xHCI] ERROR: device vanished during HCRST\n");
            return -1;
        }
        if (!(cmd & CMD_HCRST) && !(sts & STS_CNR)) {
            debug_print("[xHCI] HCRST complete (CNR clear)  USBSTS=0x%08x\n", sts);

            /*
             * Wait for EINT to clear after HCRST.
             *
             * USBSTS.EINT (bit 3) is set when the VL805 firmware posts events
             * to its own internal ring during SPI-reload init.  If EINT is
             * still set when we write RS=1, the controller tries to deliver
             * that pending event using its internal (now stale) ring pointer
             * rather than our ERSTBA → DMA write to unmapped address → HSE
             * at ~10µs with empty event ring.
             *
             * Poll until EINT clears (typically <5ms), then proceed.
             * Force-clear after 100ms if firmware never drains its ring.
             */
            if (sts & STS_EINT) {
                debug_print("[xHCI] EINT set after HCRST — waiting for "
                            "firmware to settle (USBSTS=0x%08x)\n", sts);
                int eint_cleared = 0;
                for (int w = 0; w < 100; w++) {
                    delay_ms(1);
                    uint32_t s2 = readl(op + OP_USBSTS);
                    if (!(s2 & STS_EINT)) {
                        debug_print("[xHCI] EINT cleared after %dms  "
                                    "USBSTS=0x%08x\n", w + 1, s2);
                        eint_cleared = 1;
                        break;
                    }
                }
                if (!eint_cleared) {
                    debug_print("[xHCI] EINT still set after 100ms — "
                                "force-clearing via RW1C\n");
                    writel(STS_EINT, op + OP_USBSTS);
                    asm volatile("dsb sy" ::: "memory");
                    debug_print("[xHCI] USBSTS after force-clear: 0x%08x\n",
                                readl(op + OP_USBSTS));
                }
            }
            return 0;
        }
    }
    debug_print("[xHCI] ERROR: HCRST timed out\n");
    return -1;
}

/* ── 2.3 + 2.4 DCBAA (and optional scratchpad) ───────────────────────────── */
static int setup_dcbaa(void) {
    dcbaa = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    memset((void *)dcbaa, 0, 2048);

    uint32_t n = xhci_ctrl.scratchpad_count;

    debug_print("[xHCI] Scratchpad count = %u\n", n);

    if (n > 0) {
        if (n > MAX_SCRATCH_PAGES) {
            debug_print("[xHCI] ERROR: scratchpad count %u > max %u\n",
                        n, MAX_SCRATCH_PAGES);
            return -1;
        }
        debug_print("[xHCI] Allocating %u scratchpad page(s) from DMA buffer\n", n);

        scratch_arr = (volatile uint64_t *)(xhci_dma_buf + DMA_SCRATCH_OFF);
        memset((void *)scratch_arr, 0, MAX_SCRATCH_PAGES * 8);

        /*
         * Scratchpad pages come from the static DMA buffer at DMA_SCRATCH_PAGES_OFF.
         * They are already in non-cacheable Device memory (same .xhci_dma section
         * remapped by mmu.c) — no kcalloc, no flush needed.
         */
        uint8_t *pages_base = xhci_dma_buf + DMA_SCRATCH_PAGES_OFF;
        memset(pages_base, 0, n * 4096);
        asm volatile("dsb sy" ::: "memory");

        for (uint32_t i = 0; i < n; i++) {
            void *pg = pages_base + i * 4096;
            scratch_arr[i] = phys_to_dma((uint64_t)virt_to_phys(pg));
        }
        asm volatile("dsb sy" ::: "memory");

        /* DCBAA slot 0 = DMA address of scratchpad pointer array */
        dcbaa[0] = phys_to_dma((uint64_t)virt_to_phys((void *)scratch_arr));
        debug_print("[xHCI] DCBAA[0] = scratchpad array dma=0x%llx\n",
                    (unsigned long long)dcbaa[0]);
    } else {
        debug_print("[xHCI] No scratchpad required\n");
    }

    /* dcbaa is in Device memory — no D-cache flush needed; DSB ensures ordering */
    asm volatile("dsb sy" ::: "memory");

    uint64_t dcbaa_phys = (uint64_t)virt_to_phys((void *)dcbaa);
    reg_write64(xhci_ctrl.op_regs, OP_DCBAAP_LO, phys_to_dma(dcbaa_phys));
    debug_print("[xHCI] DCBAA dma=0x%llx\n",
                (unsigned long long)phys_to_dma(dcbaa_phys));

    /* CONFIG.MaxSlotsEn — must be ≤ MaxSlots from HCSPARAMS1 */
    uint32_t cfg = readl(xhci_ctrl.op_regs + OP_CONFIG);
    cfg = (cfg & ~0xFFU) | (xhci_ctrl.max_slots & 0xFF);
    writel(cfg, xhci_ctrl.op_regs + OP_CONFIG);
    debug_print("[xHCI] CONFIG.MaxSlotsEn = %u\n", xhci_ctrl.max_slots);

    return 0;
}

/* ── 2.5 Command Ring ────────────────────────────────────────────────────── */
static int setup_cmd_ring(void) {
    cmd_ring    = (volatile uint32_t *)(xhci_dma_buf + DMA_CMD_RING_OFF);
    cmd_cycle   = 1;
    cmd_enqueue = 0;
    memset((void *)cmd_ring, 0, CMD_RING_TRBS * 16);

    /*
     * Link TRB at slot [CMD_RING_TRBS - 1]: points back to slot 0.
     * TC=1 (Toggle Cycle) — when the controller crosses this link
     * it inverts the Expected Cycle State, keeping ring sync.
     */
    uint64_t ring_phys = (uint64_t)virt_to_phys((void *)cmd_ring);
    uint64_t ring_dma  = phys_to_dma(ring_phys);
    uint32_t li = (CMD_RING_TRBS - 1) * 4;   /* dword index of Link TRB */
    cmd_ring[li + 0] = (uint32_t)(ring_dma);
    cmd_ring[li + 1] = (uint32_t)(ring_dma >> 32);
    cmd_ring[li + 2] = 0;
    cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;

    /* cmd_ring is in Device memory — no flush; DSB before CRCR write */
    asm volatile("dsb sy" ::: "memory");

    /*
     * Check CRR (Command Ring Running, CRCR bit 3) before writing.
     *
     * The VL805 firmware (loaded from SPI on every PERST#) may restart
     * the command ring after HCRST to do internal init.  If CRR=1 when we
     * write CRCR, the xHCI spec says the pointer bits[63:6] are silently
     * ignored — leaving CRCR=0 and causing HSE immediately after RS=1.
     *
     * Fix: read CRCR, if CRR=1 issue Command Stop (CS=1) and wait.
     */
    uint32_t crcr_cur = readl(xhci_ctrl.op_regs + OP_CRCR_LO);
    debug_print("[xHCI] CRCR before write: 0x%08x  CRR=%u\n",
                crcr_cur, (crcr_cur >> 3) & 1);

    if (crcr_cur & (1U << 3)) {   /* CRR=1 — command ring running */
        debug_print("[xHCI] CRR=1 — issuing Command Stop (CS) to drain ring\n");
        /* Write CS=1 to stop the ring.  Keep existing pointer bits. */
        volatile uint64_t *crcr_reg = (volatile uint64_t *)
                                      (xhci_ctrl.op_regs + OP_CRCR_LO);
        *crcr_reg = (uint64_t)(crcr_cur | (1U << 1));   /* CS bit */
        asm volatile("dsb sy" ::: "memory");

        /* Wait up to 500ms for CRR to clear */
        for (int t = 500; t > 0; t--) {
            delay_ms(1);
            crcr_cur = readl(xhci_ctrl.op_regs + OP_CRCR_LO);
            if (!(crcr_cur & (1U << 3))) {
                debug_print("[xHCI] CRR cleared after CS\n");
                break;
            }
        }
        if (crcr_cur & (1U << 3)) {
            debug_print("[xHCI] ERROR: CRR still set after CS — cannot write CRCR\n");
            return -1;
        }
    }

    /*
     * CRCR write.
     *
     * The VL805 implements CRCR bits[63:6] (ring pointer) as WRITE-ONLY.
     * Reads always return 0 for those bits regardless of what was written —
     * the controller stores the value internally but does not expose it via
     * readback. This is a VL805-specific deviation from the xHCI spec which
     * says CRCR is readable when CRR=0.
     *
     * Confirmed by register dump: three write methods (writeq, writel-LO,
     * HI-first) all produce identical before/after dumps with 0x18=0.
     * No value appeared anywhere else in the space. DCBAAP at 0x30 written
     * with identical code reads back correctly. CRCR is simply write-only.
     *
     * Write the value and proceed — do NOT readback-check it.
     */
    uint64_t crcr = (ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    reg_write64(xhci_ctrl.op_regs, OP_CRCR_LO, crcr);

    debug_print("[xHCI] Command ring dma=0x%llx  RCS=%u  (%d TRBs)  "
                "[CRCR write-only, no readback]\n",
                (unsigned long long)ring_dma, cmd_cycle, CMD_RING_TRBS);
    return 0;
}

/* ── 2.6 Event Ring + ERST ───────────────────────────────────────────────── */
static int setup_event_ring(void) {
    evt_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
    erst     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);

    memset((void *)evt_ring, 0, EVT_RING_TRBS * 16);
    memset((void *)erst,     0, 64);

    uint64_t evt_phys  = (uint64_t)virt_to_phys((void *)evt_ring);
    uint64_t erst_phys = (uint64_t)virt_to_phys((void *)erst);
    uint64_t evt_dma   = phys_to_dma(evt_phys);
    uint64_t erst_dma  = phys_to_dma(erst_phys);

    /*
     * ERST entry 0 (spec §6.5):
     *   [63:0]  = ring segment base address (DMA address, 64-byte aligned)
     *   [79:64] = ring segment size in TRBs  (stored in erst[1] lo 16 bits)
     *   [127:80]= reserved
     */
    erst[0] = evt_dma;
    erst[1] = (uint64_t)EVT_RING_TRBS;

    /* evt_ring and erst are in Device memory — no flush; DSB before runtime reg writes */
    asm volatile("dsb sy" ::: "memory");

    /*
     * Program interrupter 0 runtime registers (spec §4.2 step 6):
     *   ERSTSZ  = 1  (one ERST entry)
     *   ERSTBA  = erst_dma  (DMA address of ERST table)
     *   ERDP    = evt_dma | EHB  (DMA address of event ring dequeue ptr)
     */
    void *ir0 = ir_base(0);
    writel(1U, ir0 + IR_ERSTSZ);
    asm volatile("dsb sy" ::: "memory");
    reg_write64(ir0, IR_ERSTBA_LO, erst_dma);
    reg_write64(ir0, IR_ERDP_LO,  evt_dma | 0x8ULL);

    debug_print("[xHCI] Event ring dma=0x%llx  ERST dma=0x%llx  (%d TRBs)\n",
                (unsigned long long)evt_dma,
                (unsigned long long)erst_dma, EVT_RING_TRBS);
    return 0;
}

/* ── 2.7 Interrupter ─────────────────────────────────────────────────────── */
static void setup_interrupter(void) {
    void *ir0 = ir_base(0);

    /*
     * IMAN: IP=1 (write 1 to clear any pending interrupt), IE=1 (enable).
     * Spec says to clear IP before setting IE, but a single write of 0x3
     * achieves both atomically.
     */
    writel(0x00000003U, ir0 + IR_IMAN);

    /*
     * IMOD: interrupt moderation.
     * IMODI[15:0] = throttle interval in units of 250 ns.
     *   0x0FA0 = 4000 × 250 ns = 1 ms  — reasonable for bare metal.
     * IMODC[31:16] = countdown (set equal to IMODI initially).
     */
    writel(0x0FA00FA0U, ir0 + IR_IMOD);

    /* Re-write ERSTSZ in case the register reset between steps */
    writel(1U, ir0 + IR_ERSTSZ);

    asm volatile("dsb sy; isb" ::: "memory");
    debug_print("[xHCI] Interrupter 0: IMAN=0x%08x  IMOD=0x%08x\n",
                readl(ir0 + IR_IMAN), readl(ir0 + IR_IMOD));
}

/* ── 2.8 Run ─────────────────────────────────────────────────────────────── */
static int run_controller(void) {
    void    *op  = xhci_ctrl.op_regs;
    void    *ir0 = ir_base(0);

    /*
     * ── Phase 1: Diagnostics (prints allowed) ─────────────────────────────
     * Dump DMA register state and CPU readback.  This is purely informational
     * and may take hundreds of µs.  Firmware is free to interfere here.
     */
    uint32_t dcbaap_lo = readl(op + OP_DCBAAP_LO);
    uint32_t dcbaap_hi = readl(op + OP_DCBAAP_HI);
    uint32_t crcr_lo   = readl(op + OP_CRCR_LO);
    uint32_t crcr_hi   = readl(op + OP_CRCR_HI);
    uint32_t config    = readl(op + OP_CONFIG);
    uint32_t erstba_lo = readl(ir0 + IR_ERSTBA_LO);
    uint32_t erstba_hi = readl(ir0 + IR_ERSTBA_HI);
    uint32_t erdp_lo   = readl(ir0 + IR_ERDP_LO);
    uint32_t erdp_hi   = readl(ir0 + IR_ERDP_HI);
    uint32_t erstsz    = readl(ir0 + IR_ERSTSZ);
    uint32_t iman      = readl(ir0 + IR_IMAN);
    debug_print("[xHCI] Pre-run register dump:\n");
    debug_print("[xHCI]   DCBAAP  = 0x%08x_%08x\n", dcbaap_hi, dcbaap_lo);
    debug_print("[xHCI]   CRCR    = 0x%08x_%08x\n", crcr_hi,   crcr_lo);
    debug_print("[xHCI]   CONFIG  = 0x%08x\n", config);
    debug_print("[xHCI]   ERSTSZ  = 0x%08x\n", erstsz);
    debug_print("[xHCI]   ERSTBA  = 0x%08x_%08x\n", erstba_hi, erstba_lo);
    debug_print("[xHCI]   ERDP    = 0x%08x_%08x\n", erdp_hi,   erdp_lo);
    debug_print("[xHCI]   IMAN    = 0x%08x\n", iman);

    volatile uint64_t *dcbaa_v    = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    volatile uint64_t *scratch_v  = (volatile uint64_t *)(xhci_dma_buf + DMA_SCRATCH_OFF);
    volatile uint64_t *erst_v     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);
    volatile uint32_t *cmd_link_v = (volatile uint32_t *)(xhci_dma_buf + DMA_CMD_RING_OFF)
                                    + (CMD_RING_TRBS - 1) * 4;
    asm volatile("dsb sy" ::: "memory");
    debug_print("[xHCI] DMA buffer CPU readback (data-in-DRAM check):\n");
    debug_print("[xHCI]   dcbaa[0]      = 0x%016llx  (expect scratchpad arr phys)\n",
                (unsigned long long)dcbaa_v[0]);
    debug_print("[xHCI]   dcbaa[1]      = 0x%016llx  (expect 0)\n",
                (unsigned long long)dcbaa_v[1]);
    debug_print("[xHCI]   scratch_arr[0]= 0x%016llx  (expect first page phys)\n",
                (unsigned long long)scratch_v[0]);
    debug_print("[xHCI]   scratch_arr[1]= 0x%016llx\n",
                (unsigned long long)scratch_v[1]);
    debug_print("[xHCI]   erst[0]       = 0x%016llx  (expect evt_ring phys)\n",
                (unsigned long long)erst_v[0]);
    debug_print("[xHCI]   erst[1]       = 0x%016llx  (expect 64)\n",
                (unsigned long long)erst_v[1]);
    debug_print("[xHCI]   cmd_link_trb  = %08x %08x %08x %08x\n",
                cmd_link_v[0], cmd_link_v[1], cmd_link_v[2], cmd_link_v[3]);

    /*
     * ── Phase 2: Pre-compute all register values ───────────────────────────
     * Derive all values we'll write in the tight launch below so Phase 3
     * contains zero arithmetic and zero branching — just stores.
     */
    uint64_t dcbaa_phys = (uint64_t)virt_to_phys((void *)dcbaa);
    uint64_t dcbaa_dma  = phys_to_dma(dcbaa_phys);
    uint32_t launch_dcbaa_lo = (uint32_t)(dcbaa_dma & 0xFFFFFFFF);
    uint32_t launch_dcbaa_hi = (uint32_t)(dcbaa_dma >> 32);

    uint32_t launch_config   = (readl(op + OP_CONFIG) & ~0xFFU) |
                               (xhci_ctrl.max_slots & 0xFFU);

    uint64_t erst_phys   = (uint64_t)virt_to_phys((void *)(xhci_dma_buf + DMA_ERST_OFF));
    uint64_t erstba_dma  = phys_to_dma(erst_phys);
    uint32_t launch_erstba_lo = (uint32_t)(erstba_dma & 0xFFFFFFFF);
    uint32_t launch_erstba_hi = (uint32_t)(erstba_dma >> 32);

    uint64_t evt_phys    = (uint64_t)virt_to_phys((void *)(xhci_dma_buf + DMA_EVT_RING_OFF));
    uint64_t erdp_dma    = phys_to_dma(evt_phys);
    /* EHB (bit 3) must be set in the initial ERDP write.  Without it the
     * interrupter considers its previous event un-acknowledged and will not
     * post the Command Completion Event after Enable Slot.
     * erdp_dma is 4 KB-aligned so bit 3 is always clear in the address. */
    /* EHB (bit 3) must NOT be set in the launch ERDP.
     * The VL805 fires HSE immediately after RS=1 if EHB=1 is present at
     * startup — it does not follow the xHCI spec rule that HC ignores EHB
     * on RS 0->1.  EHB is written by evt_ring_consume() after each event. */
    uint32_t launch_erdp_lo  = (uint32_t)(erdp_dma & 0xFFFFFFFF);
    uint32_t launch_erdp_hi  = (uint32_t)(erdp_dma >> 32);

    uint64_t ring_dma    = phys_to_dma((uint64_t)virt_to_phys(
                               (void *)(xhci_dma_buf + DMA_CMD_RING_OFF)));
    uint64_t crcr_val    = (ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle;
    uint32_t launch_crcr_lo  = (uint32_t)(crcr_val & 0xFFFFFFFF);
    uint32_t launch_crcr_hi  = (uint32_t)(crcr_val >> 32);

    debug_print("[xHCI] Launch values precomputed:\n");
    debug_print("[xHCI]   DCBAAP  = 0x%08x_%08x\n", launch_dcbaa_hi, launch_dcbaa_lo);
    debug_print("[xHCI]   CRCR    = 0x%08x_%08x  RCS=%u\n",
                launch_crcr_hi, launch_crcr_lo, cmd_cycle);
    debug_print("[xHCI]   ERSTBA  = 0x%08x_%08x\n", launch_erstba_hi, launch_erstba_lo);
    debug_print("[xHCI]   ERDP    = 0x%08x_%08x\n", launch_erdp_hi, launch_erdp_lo);
    debug_print("[xHCI]   CONFIG  = 0x%08x\n", launch_config);

    /*
     * ── Phase 3: Patient launch — wait for MCU, steal controller ────────────
     *
     * OBSERVATION (boot log): After our first HCRST, the VL805 MCU asserts HSE
     * and holds it for several ms while it re-initialises its own internal state.
     * Issuing a second HCRST while the MCU is mid-init only restarts the HSE
     * cycle — we cannot win by racing then.
     *
     * UPDATED APPROACH (boot 30 finding): Even after waiting for HSE to clear
     * and halting the controller, the MCU is still active and re-fires HSE within
     * ~60µs of our RS=1.  It monitors for RS=1 and immediately re-runs its own
     * init on top of ours.  The fix is a second HCRST issued AFTER the MCU has
     * fully settled (HSE cleared, controller halted).  This resets ALL MCU
     * internal state.  We then burst our registers within ~100µs of CNR=0,
     * before the MCU completes its next re-init cycle (~500µs).
     *
     * CORRECT APPROACH:
     *   1. Wait up to 500ms for HSE to clear naturally (MCU finishes re-init).
     *   2. Halt the MCU-started controller (clear RS if HCH=0).
     *   3. Issue second HCRST; busy-wait for CNR=0 (no delay — every µs counts).
     *   4. Write our registers in a tight burst immediately after CNR=0.
     *   5. Clear all status bits (RW1C), then set RS=1.
     */
    debug_print("[xHCI] Waiting for MCU post-HCRST init (HSE poll, up to 500ms)...\n");

    /* Steps 1 & 2: all debug_prints are DEFERRED until after RS=1.
     * Every UART byte at 115200 baud takes ~86µs; the two prints in step 1+2
     * cost ~8ms total — enough for the MCU to complete a full re-init after
     * the second HCRST (step 3a) and re-assert HSE before our RS=1 lands. */
    int      defer_hse_ok   = 0;
    int      defer_hse_ms   = 0;
    uint32_t defer_hse_sts  = 0;
    int      defer_halted   = 0;   /* 1=was already halted, 0=we halted it */
    uint32_t defer_halt_sts = 0;

    /* Step 1: wait up to 500ms for HSE to clear */
    {
        int hse_clear = 0;
        for (int w = 0; w < 500; w++) {
            delay_ms(1);
            uint32_t s2 = readl(op + OP_USBSTS);
            if (!(s2 & STS_HSE)) {
                defer_hse_ok  = 1;
                defer_hse_ms  = w + 1;
                defer_hse_sts = s2;
                hse_clear = 1;
                break;
            }
        }
        if (!hse_clear) {
            defer_hse_ok  = 0;
            defer_hse_sts = readl(op + OP_USBSTS);
        }
    }

    /* Step 2: halt controller if MCU started it (HCH=0 means running) */
    {
        uint32_t s2 = readl(op + OP_USBSTS);
        if (!(s2 & STS_HCH)) {
            defer_halted = 0;
            writel(readl(op + OP_USBCMD) & ~CMD_RS, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");
            for (int t = 200; t > 0; t--) {
                delay_us(100);
                if (readl(op + OP_USBSTS) & STS_HCH) break;
            }
            defer_halt_sts = readl(op + OP_USBSTS);
        } else {
            defer_halted   = 1;
            defer_halt_sts = s2;
        }
    }

    /* Step 2b removed.
     *
     * Port power-cycling (PP=0→PP=1) added 4×40 ms = ~160 ms between the
     * controller halt and our register burst.  The VL805 MCU uses that gap
     * to re-run its own xHCI init and set RS=1 with its SRAM-based rings,
     * so HSE is already asserted before our RS=1 write lands.
     *
     * Port state (WPR, PLS) is managed correctly by the xHCI spec once
     * RS=1.  Connected devices re-enumerate via Port Status Change events.
     * No pre-RS=1 port manipulation is required.
     */

    /* Step 3a: second HCRST — resets MCU internal state from a clean baseline.
     *
     * Even after step 1 (HSE cleared) + step 2 (controller halted), the MCU
     * remains active and re-fires HSE within ~60µs of our RS=1 by monitoring
     * for RS=1 and running its own init on top (observed boot 30: USBSTS=0x18
     * at RS=1 latch, 0x1d only 60µs later).
     *
     * A second HCRST at this point resets ALL MCU internal state.  We
     * busy-wait for CNR=0 with no prints or delays — every µs counts.
     * Typical CNR clear: <500µs.  MCU re-init time: ~500µs.
     * Our register burst (step 3b) completes in ~100µs — we win the race.
     */
    writel(readl(op + OP_USBCMD) | CMD_HCRST, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");
    for (volatile int t = 0; t < 5000; t++) {
        uint32_t cmd = readl(op + OP_USBCMD);
        uint32_t sts = readl(op + OP_USBSTS);
        if (!(cmd & CMD_HCRST) && !(sts & STS_CNR))
            break;
    }
    asm volatile("dsb sy; isb" ::: "memory");

    /* Step 3b: write our rings in a burst — no prints or reads between writes */
    asm volatile("dsb sy; isb" ::: "memory");
    writel(launch_dcbaa_lo,  op  + OP_DCBAAP_LO);
    writel(launch_dcbaa_hi,  op  + OP_DCBAAP_HI);
    writel(launch_config,    op  + OP_CONFIG);
    writel(1U,               ir0 + IR_ERSTSZ);
    writel(launch_erstba_lo, ir0 + IR_ERSTBA_LO);
    writel(launch_erstba_hi, ir0 + IR_ERSTBA_HI);
    writel(launch_erdp_lo,   ir0 + IR_ERDP_LO);
    writel(launch_erdp_hi,   ir0 + IR_ERDP_HI);
    writel(launch_crcr_lo,   op  + OP_CRCR_LO);
    writel(launch_crcr_hi,   op  + OP_CRCR_HI);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Step 4: retry loop — clear HSE, wait for clean window, write RS=1.
     *
     * After the second HCRST, the VL805 MCU re-asserts HSE as part of its own
     * re-init sequence.  A single RW1C+RS=1 write lands while HSE is still set,
     * causing the controller to immediately self-halt (observed: USBSTS=0x05 at
     * RS=1 latch).  Our rings are intact (ERDP proof), so we just need RS=1 to
     * land in a HSE-free window.
     *
     * Strategy: clear HSE (RW1C), wait up to 5ms for HSE=0, write RS=1, then
     * poll 2ms for the controller to stay running (HCH=0, HSE=0).  If HSE fires
     * again, loop and retry.  The MCU's HSE phase is transient; after it passes
     * our RS=1 will stick.
     */
    {
        int launched = 0;
        for (int attempt = 0; attempt < 20 && !launched; attempt++) {
            /*
             * Re-write all ring registers before every attempt.
             *
             * Each failed RS=1 (controller halts on HSE) triggers another
             * MCU re-init cycle.  The MCU overwrites CRCR, DCBAAP, ERSTBA
             * and ERDP to point at its own internal SRAM rings.  If RS=1
             * then lands with those stale values the controller starts,
             * tries to DMA-read from SRAM addresses, gets a bus error and
             * fires HSE immediately.  Restoring our values here is cheap
             * (10 posted writes) and guarantees the controller always starts
             * with our DMA rings regardless of how many MCU re-init cycles
             * have occurred.
             */
            asm volatile("dsb sy; isb" ::: "memory");
            writel(launch_dcbaa_lo,  op  + OP_DCBAAP_LO);
            writel(launch_dcbaa_hi,  op  + OP_DCBAAP_HI);
            writel(launch_config,    op  + OP_CONFIG);
            writel(1U,               ir0 + IR_ERSTSZ);
            writel(launch_erstba_lo, ir0 + IR_ERSTBA_LO);
            writel(launch_erstba_hi, ir0 + IR_ERSTBA_HI);
            writel(launch_erdp_lo,   ir0 + IR_ERDP_LO);
            writel(launch_erdp_hi,   ir0 + IR_ERDP_HI);
            writel(launch_crcr_lo,   op  + OP_CRCR_LO);
            writel(launch_crcr_hi,   op  + OP_CRCR_HI);
            asm volatile("dsb sy; isb" ::: "memory");

            /* Clear all pending status bits */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");

            /* Wait up to 5ms for HSE to clear */
            int hse_gone = 0;
            for (int w = 0; w < 50; w++) {
                delay_us(100);
                if (!(readl(op + OP_USBSTS) & STS_HSE)) { hse_gone = 1; break; }
            }
            if (!hse_gone) continue;   /* HSE stuck — retry clear */

            /* Write RS=1 into confirmed-clean USBSTS */
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");

            /* Check latch */
            uint32_t cmd_chk = readl(op + OP_USBCMD);
            uint32_t sts_chk = readl(op + OP_USBSTS);
            if (cmd_chk & CMD_RS)
                debug_print("[xHCI] RS=1 latched (attempt %d)  USBCMD=0x%08x  USBSTS=0x%08x\n",
                            attempt + 1, cmd_chk, sts_chk);
            else {
                debug_print("[xHCI] RS=1 did not latch (attempt %d)  USBCMD=0x%08x  USBSTS=0x%08x\n",
                            attempt + 1, cmd_chk, sts_chk);
                continue;
            }

            /* Poll up to 2ms: controller must stay running (HCH=0, HSE=0) */
            int still_running = 0;
            for (int p = 0; p < 200; p++) {
                delay_us(10);
                uint32_t s = readl(op + OP_USBSTS);
                if (s & STS_HSE) break;          /* HSE fired — retry */
                if (!(s & STS_HCH)) { still_running = 1; break; }
            }
            if (still_running) {
                launched = 1;
            } else {
                debug_print("[xHCI] Controller halted after RS=1 (HSE) — retrying\n");
            }
        }
        if (!launched)
            debug_print("[xHCI] ERROR: failed to start controller after 20 attempts\n");
    }

launch_done:

    /* Deferred diagnostics from steps 1 & 2 (suppressed until after RS=1) */
    if (defer_hse_ok)
        debug_print("[xHCI] [deferred] HSE cleared after %dms  USBSTS=0x%08x\n",
                    defer_hse_ms, defer_hse_sts);
    else
        debug_print("[xHCI] [deferred] HSE not cleared in 500ms  USBSTS=0x%08x\n",
                    defer_hse_sts);
    if (defer_halted)
        debug_print("[xHCI] [deferred] Controller was halted: USBSTS=0x%08x\n", defer_halt_sts);
    else
        debug_print("[xHCI] [deferred] MCU was running — halted it: USBSTS=0x%08x\n", defer_halt_sts);

    /* Read USBSTS and USBCMD immediately after RS=1 (no delay) */
    uint32_t sts_imm = readl(op + OP_USBSTS);
    uint32_t cmd_imm = readl(op + OP_USBCMD);
    debug_print("[xHCI] USBSTS immediately after RS=1: 0x%08x  USBCMD=0x%08x\n",
                sts_imm, cmd_imm);
    if (!(cmd_imm & CMD_RS)) {
        if (sts_imm & STS_HSE)
            debug_print("[xHCI] RS=0: controller started then self-halted on HSE\n");
        else
            debug_print("[xHCI] WARNING: RS bit did not take effect in USBCMD!\n");
    }

    /* Tight poll for first 2ms (10us intervals) to catch exact HSE timing */
    for (int t = 200; t > 0; t--) {
        delay_us(10);
        uint32_t sts = readl(op + OP_USBSTS);
        if (sts & STS_HSE) {
            debug_print("[xHCI] HSE within first 2ms (at %d0us): USBSTS=0x%08x\n",
                        200-t+1, sts);
            break;
        }
        if (!(sts & STS_HCH)) {
            debug_print("[xHCI] Controller RUNNING at %d0us  USBSTS=0x%08x\n",
                        200-t+1, sts);
            goto running;
        }
    }

    /* Poll until HCH clears (controller left halted state) */
    for (int t = 200; t > 0; t--) {
        delay_us(500);
        uint32_t sts = readl(op + OP_USBSTS);
        if (sts == 0xFFFFFFFF) {
            debug_print("[xHCI] ERROR: device vanished after RS=1\n");
            return -1;
        }
        if (sts & STS_HSE) {
            debug_print("[xHCI] HSE set (USBSTS=0x%08x) — scanning event ring\n", sts);
            /* DSB: ensure inbound PCIe posted writes have drained to DRAM */
            asm volatile("dsb sy; isb" ::: "memory");

            uint32_t erdp_rb = readl(ir_base(0) + IR_ERDP_LO);
            debug_print("[xHCI]   ERDP readback = 0x%08x\n", erdp_rb);

            /* Always dump first 4 TRBs unconditionally */
            debug_print("[xHCI]   EVT ring first 4 TRBs (raw):\n");
            for (int i = 0; i < 4; i++)
                debug_print("[xHCI]   [%d] %08x %08x %08x %08x\n", i,
                            evt_ring[i*4+0], evt_ring[i*4+1],
                            evt_ring[i*4+2], evt_ring[i*4+3]);

            /* Dump all ports PORTSC */
            for (int p = 0; p < xhci_ctrl.max_ports && p < 5; p++) {
                uint32_t psc = readl(xhci_ctrl.op_regs + 0x400 + p * 0x10);
                debug_print("[xHCI]   PORT%d PORTSC=0x%08x  CCS=%u PP=%u speed=%u\n",
                            p+1, psc, psc & 1, (psc >> 9) & 1, (psc >> 10) & 0xF);
            }

            int found = 0;
            for (int i = 0; i < EVT_RING_TRBS; i++) {
                uint32_t d0 = evt_ring[i*4+0];
                uint32_t d1 = evt_ring[i*4+1];
                uint32_t d2 = evt_ring[i*4+2];
                uint32_t d3 = evt_ring[i*4+3];
                if (d0 || d1 || d2 || d3) {
                    debug_print("[xHCI]   EVT[%d]: %08x %08x %08x %08x"
                                "  type=%u cc=%u cycle=%u\n",
                                i, d0, d1, d2, d3,
                                (d3 >> 10) & 0x3F,
                                (d2 >> 24) & 0xFF,
                                d3 & 1);
                    found++;
                }
            }
            if (!found)
                debug_print("[xHCI]   event ring empty — HSE fired before"
                            " first DMA access completed\n");
            return -1;
        }
        if (!(sts & STS_HCH)) {
            debug_print("[xHCI] Controller RUNNING  USBSTS=0x%08x\n", sts);
            if (sts & STS_HSE) {
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
                uint32_t sts_after = readl(op + OP_USBSTS);
                debug_print("[xHCI] Post-RS=1 MCU HSE cleared  USBSTS=0x%08x\n", sts_after);
            }
            return 0;
        }
    }
    debug_print("[xHCI] ERROR: HCH still set after RS=1 (USBSTS=0x%08x)\n",
                readl(op + OP_USBSTS));
    return -1;
running:
    {
        uint32_t fast_sts = readl(op + OP_USBSTS);
        debug_print("[xHCI] Controller RUNNING (fast)  USBSTS=0x%08x\n", fast_sts);
        if (fast_sts & STS_HSE) {
            /* The VL805 MCU fires HSE shortly after RS=1 as part of its own
             * init sequence — this is cosmetic and transient.  Boot 34 proved
             * the controller keeps running through it.  RW1C clear and proceed. */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            uint32_t sts_after = readl(op + OP_USBSTS);
            debug_print("[xHCI] Post-RS=1 MCU HSE cleared  USBSTS=0x%08x\n", sts_after);
        }
        return 0;
    }
}

/* ── 2.9 Port power-on ───────────────────────────────────────────────────── */
static void power_ports(void) {
    void *op = xhci_ctrl.op_regs;
    int   n  = xhci_ctrl.max_ports;

    debug_print("[xHCI] Powering %d port(s)\n", n);
    for (int p = 0; p < n; p++) {
        void    *reg    = op + 0x400 + p * 0x10;
        uint32_t portsc = readl(reg);

        if (portsc & PORTSC_PP) continue;   /* already powered */

        /* Set PP, preserve RW bits, clear WIC bits (write-1-to-clear) */
        portsc = (portsc & ~PORTSC_WIC) | PORTSC_PP;
        writel(portsc, reg);
        asm volatile("dsb sy" ::: "memory");
    }
    /* Spec minimum 20 ms stabilisation after PP=1 */
    delay_ms(20);
}

/* ── Command ring submission ─────────────────────────────────────────────── */

/*
 * Push one TRB onto the command ring and ring doorbell 0.
 * dw0–dw2: command-specific payload.
 * type:     TRB type field (goes into dw3 bits[15:10]).
 * Returns the physical address of the submitted TRB (used as command tag).
 */
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1,
                                uint32_t dw2, uint32_t type)
{
    uint32_t slot = cmd_enqueue;
    uint32_t base = slot * 4;   /* dword index */

    cmd_ring[base + 0] = dw0;
    cmd_ring[base + 1] = dw1;
    cmd_ring[base + 2] = dw2;
    cmd_ring[base + 3] = (type << TRB_TYPE_SHIFT) | cmd_cycle;
    asm volatile("dsb sy" ::: "memory");

    uint64_t trb_phys = virt_to_phys((void *)&cmd_ring[base]);

    cmd_enqueue++;
    if (cmd_enqueue >= CMD_RING_TRBS - 1) {
        /* Reached Link TRB — toggle cycle and wrap */
        uint32_t li = (CMD_RING_TRBS - 1) * 4;
        cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;
        asm volatile("dsb sy" ::: "memory");
        cmd_cycle ^= 1;
        cmd_enqueue = 0;
    }

    /* Ring host controller doorbell 0 (command ring) */
    writel(0, xhci_ctrl.doorbell_regs);
    asm volatile("dsb sy" ::: "memory");

    return trb_phys;
}

/* ── Event ring polling ──────────────────────────────────────────────────── */

/*
 * Consume one entry from the event ring (internal helper).
 * Advances evt_dequeue, toggles evt_cycle at wraparound, updates ERDP.
 */
static void evt_ring_consume(void *ir0, uint32_t *out)
{
    out[0] = evt_ring[evt_dequeue * 4 + 0];
    out[1] = evt_ring[evt_dequeue * 4 + 1];
    out[2] = evt_ring[evt_dequeue * 4 + 2];
    out[3] = evt_ring[evt_dequeue * 4 + 3];
    asm volatile("dsb sy" ::: "memory");

    evt_dequeue++;
    if (evt_dequeue >= EVT_RING_TRBS) {
        evt_dequeue = 0;
        evt_cycle ^= 1;
    }

    uint64_t new_erdp = phys_to_dma(
        virt_to_phys((void *)&evt_ring[evt_dequeue * 4]));
    reg_write64(ir0, IR_ERDP_LO, new_erdp | 0x8ULL);
}

/*
 * Poll event ring for a Command Completion or Transfer Event, timeout in ms.
 * Fills out[0..3] with the matching TRB dwords.
 * Port Status Change Events are silently consumed (logged) while waiting —
 * they arrive after port reset and must not block command completion polling.
 * Returns 0 on success, -1 on timeout.
 */
static int evt_ring_poll(uint32_t *out, int timeout_ms)
{
    void *ir0 = ir_base(0);

    for (int ms = 0; ms < timeout_ms * 10; ms++) {
        uint32_t dw3 = evt_ring[evt_dequeue * 4 + 3];
        if ((dw3 & TRB_CYCLE) == evt_cycle) {
            /* Event is valid — peek at type */
            uint32_t type = (dw3 >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TYPE_PORT_CHNG_EVT) {
                /* Port Status Change Event — consume, log, keep waiting */
                uint32_t tmp[4];
                evt_ring_consume(ir0, tmp);
                uint32_t port_id = (tmp[0] >> 24) & 0xFF;
                debug_print("[xHCI]   (drained Port Change Event port=%u)\n",
                            port_id);
                continue;   /* don't count this against timeout */
            }
            /* Command Completion or Transfer Event — consume and return */
            evt_ring_consume(ir0, out);
            return 0;
        }
        delay_us(100);
    }
    return -1;   /* timeout */
}

/* ── Port reset and wait for enabled ────────────────────────────────────── */

static int port_reset_and_enable(int port)
{
    void    *reg    = xhci_ctrl.op_regs + 0x400 + port * 0x10;
    uint32_t portsc = readl(reg);

    /* Clear WIC bits (write-1-to-clear), set PR */
    portsc = (portsc & ~PORTSC_WIC) | (1U << 4);   /* PR = bit 4 */
    writel(portsc, reg);
    asm volatile("dsb sy" ::: "memory");

    /* Poll for PRC (Port Reset Change, bit 21) — up to 500ms */
    for (int t = 500; t > 0; t--) {
        delay_ms(1);
        portsc = readl(reg);
        if (portsc & (1U << 21)) {   /* PRC set */
            /* Clear PRC by writing 1 to it */
            writel((portsc & ~PORTSC_WIC) | (1U << 21), reg);
            asm volatile("dsb sy" ::: "memory");
            debug_print("[xHCI] Port %d reset complete  PORTSC=0x%08x\n",
                        port + 1, readl(reg));
            return 0;
        }
    }
    debug_print("[xHCI] Port %d reset TIMEOUT  PORTSC=0x%08x\n",
                port + 1, portsc);
    return -1;
}

/* ── Enable Slot command ─────────────────────────────────────────────────── */

static int cmd_enable_slot(uint8_t *slot_id_out)
{
    uint64_t trb_phys = cmd_ring_submit(0, 0, 0, TRB_TYPE_ENABLE_SLOT);
    uint32_t ev[4];

    if (evt_ring_poll(ev, 2000) != 0) {
        debug_print("[xHCI] Enable Slot: event timeout\n");
        return -1;
    }

    uint8_t  cc  = (ev[2] >> 24) & 0xFF;
    uint8_t  sid = (ev[3] >> 24) & 0xFF;
    (void)trb_phys;

    if (cc != CC_SUCCESS) {
        debug_print("[xHCI] Enable Slot: CC=%u (expected 1=SUCCESS)\n", cc);
        return -1;
    }

    *slot_id_out = sid;
    debug_print("[xHCI] Enable Slot: slot_id=%u\n", sid);
    return 0;
}

/* ── Address Device command ──────────────────────────────────────────────── */
/*
 * Builds a minimal Input Context for slot 0 and EP0, then issues
 * Address Device.  VL805 CSZ=0 → 32-byte context entries.
 *
 *  Input Context layout (32-byte entries):
 *    [0]     Input Control Context  (A2=1 for slot, A1=1 for EP0)
 *    [1]     Slot Context
 *    [2]     EP0 Context  (Endpoint Descriptor Info)
 *
 *  Slot Context dword 0:
 *    [27:20] Context Entries = 1  (only EP0)
 *    [19:10] Root Hub Port Number = port+1
 *    [9:0]   Route String = 0 (root hub)
 *
 *  EP0 Context dword 1:
 *    [31:16] Max Packet Size  (8 for LS/FS, 64 for HS, 512 for SS)
 *    [5:3]   EP Type = 4 (Control Bidirectional)
 *    [2:1]   Error Count = 3
 *
 *  EP0 Context dword 2: TR Dequeue Pointer lo | DCS=1
 *  EP0 Context dword 3: TR Dequeue Pointer hi
 */
static int cmd_address_device(uint8_t slot_id, int port, uint32_t speed)
{
    /* Max packet size based on speed */
    uint16_t mps;
    switch (speed) {
        case 1:  mps =   8; break;   /* Full speed */
        case 2:  mps =   8; break;   /* Low speed  */
        case 3:  mps =  64; break;   /* High speed */
        case 4:  mps = 512; break;   /* SuperSpeed */
        default: mps =   8; break;
    }

    /* Zero the input and output context areas */
    memset((void *)input_ctx, 0, 1088);
    memset((void *)out_ctx,   0, 1024);

    /* Install output context pointer in DCBAA at slot_id */
    uint64_t out_phys = virt_to_phys((void *)out_ctx);
    dcbaa[slot_id] = phys_to_dma(out_phys);
    asm volatile("dsb sy" ::: "memory");

    /* Setup EP0 transfer ring */
    ep0_cycle   = 1;
    ep0_enqueue = 0;
    memset((void *)ep0_ring, 0, 1024);
    /* Link TRB at end */
    uint64_t ep0_ring_phys = virt_to_phys((void *)ep0_ring);
    uint64_t ep0_ring_dma  = phys_to_dma(ep0_ring_phys);
    uint32_t li = (EVT_RING_TRBS - 1) * 4;
    ep0_ring[li + 0] = (uint32_t)(ep0_ring_dma);
    ep0_ring[li + 1] = (uint32_t)(ep0_ring_dma >> 32);
    ep0_ring[li + 2] = 0;
    ep0_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | ep0_cycle;
    asm volatile("dsb sy" ::: "memory");

    /*
     * Input Control Context (entry 0, offset 0×CTX_ENTRY_SIZE):
     *   Add Context flags: A1=EP0 (bit 1), A2=Slot (bit 2) — wait, spec:
     *   A0=bit 0 = reserved in Address Device. A1=bit 1 = EP0 add.
     *   D-flags: 0. Drop all, add slot(A0? no): per spec §6.2.2.2:
     *   For Address Device: A0 (slot) = bit 0, A1 (EP0) = bit 1.
     *   Actually: add_flags bit N = add context entry N.
     *   Entry 0 = Slot Context → A0 = bit 0.
     *   Entry 1 = EP0 → A1 = bit 1.
     */
    volatile uint32_t *icc = &input_ctx[0 * (CTX_ENTRY_SIZE / 4)];
    icc[0] = 0;         /* Drop flags  */
    icc[1] = (1U << 0) | (1U << 1);   /* Add slot (A0) + EP0 (A1) */

    /* Slot Context (entry 1) */
    volatile uint32_t *sc = &input_ctx[1 * (CTX_ENTRY_SIZE / 4)];
    sc[0] = (1U << 27)            /* Context Entries = 1         */
          | ((uint32_t)(port + 1) << 16) /* Root Hub Port Number        */
          | (speed << 10)         /* Speed                       */
          ;
    sc[1] = 0;
    sc[2] = 0;
    sc[3] = 0;

    /* EP0 Context (entry 2) */
    volatile uint32_t *ep0c = &input_ctx[2 * (CTX_ENTRY_SIZE / 4)];
    ep0c[0] = 0;
    ep0c[1] = ((uint32_t)mps << 16)   /* Max Packet Size             */
            | (4U << 3)               /* EP Type = Control Bidir     */
            | (3U << 1)               /* Error Count = 3             */
            ;
    ep0c[2] = (uint32_t)(ep0_ring_dma & ~1ULL) | 1U;  /* TR Dequeue lo | DCS */
    ep0c[3] = (uint32_t)(ep0_ring_dma >> 32);          /* TR Dequeue hi       */

    asm volatile("dsb sy" ::: "memory");

    /* Issue Address Device command — slot_id in dw3[31:24] */
    uint64_t ictx_dma = phys_to_dma(virt_to_phys((void *)input_ctx));

    /* Manually write TRB to avoid slot_id being lost in cmd_ring_submit */
    {
        uint32_t s = cmd_enqueue;
        uint32_t b = s * 4;
        cmd_ring[b+0] = (uint32_t)(ictx_dma & 0xFFFFFFF0U);
        cmd_ring[b+1] = (uint32_t)(ictx_dma >> 32);
        cmd_ring[b+2] = 0;
        cmd_ring[b+3] = ((uint32_t)slot_id << 24)
                      | (TRB_TYPE_ADDRESS_DEV << TRB_TYPE_SHIFT)
                      | cmd_cycle;
        asm volatile("dsb sy" ::: "memory");
        cmd_enqueue++;
        if (cmd_enqueue >= CMD_RING_TRBS - 1) {
            uint32_t li = (CMD_RING_TRBS - 1) * 4;
            cmd_ring[li+3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;
            asm volatile("dsb sy" ::: "memory");
            cmd_cycle ^= 1;
            cmd_enqueue = 0;
        }
        writel(0, xhci_ctrl.doorbell_regs);
        asm volatile("dsb sy" ::: "memory");
    }

    uint32_t ev[4];
    if (evt_ring_poll(ev, 2000) != 0) {
        debug_print("[xHCI] Address Device: event timeout\n");
        return -1;
    }

    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS) {
        debug_print("[xHCI] Address Device: CC=%u slot=%u\n", cc, slot_id);
        return -1;
    }

    /* Read back assigned USB address from output Slot Context dword 3 */
    uint8_t usb_addr = out_ctx[3 * (CTX_ENTRY_SIZE / 4) + 3] & 0xFF;
    debug_print("[xHCI] Address Device: success  slot=%u  usb_addr=%u  MPS=%u\n",
                slot_id, usb_addr, mps);
    return 0;
}

/* ── EP0 control transfer: Get Descriptor ───────────────────────────────── */
/*
 * Issues a standard GET_DESCRIPTOR control transfer on EP0.
 * Setup stage → Data stage (IN) → Status stage (OUT).
 *
 * desc_type:  1=Device, 2=Config, 3=String
 * desc_idx:   descriptor index (0 for Device/Config, language/string for String)
 * lang_id:    0x0409 for English (only used for String descriptors)
 * length:     number of bytes to request
 *
 * Returns bytes received (≥0) or -1 on error.
 */
static int ep0_get_descriptor(uint8_t slot_id, uint8_t desc_type,
                               uint8_t desc_idx, uint16_t lang_id,
                               uint16_t length)
{
    memset((void *)ep0_data, 0, 512);

    uint64_t data_phys = virt_to_phys((void *)ep0_data);

    /* Helper: submit one TRB to EP0 ring */
#define EP0_SUBMIT(d0, d1, d2, type_flags) do {                         \
        uint32_t _s = ep0_enqueue;                                       \
        ep0_ring[_s*4+0] = (d0);                                         \
        ep0_ring[_s*4+1] = (d1);                                         \
        ep0_ring[_s*4+2] = (d2);                                         \
        ep0_ring[_s*4+3] = (type_flags) | ep0_cycle;                     \
        asm volatile("dsb sy" ::: "memory");                             \
        ep0_enqueue++;                                                    \
        if (ep0_enqueue >= EVT_RING_TRBS - 1) {                         \
            uint32_t _li = (EVT_RING_TRBS-1)*4;                         \
            ep0_ring[_li+3] = (TRB_TYPE_LINK<<TRB_TYPE_SHIFT)|TRB_TC|ep0_cycle; \
            asm volatile("dsb sy" ::: "memory");                         \
            ep0_cycle ^= 1; ep0_enqueue = 0;                             \
        }                                                                \
    } while (0)

    /*
     * Setup Stage TRB (§6.4.1.2.1):
     *   dw0–dw1: bmRequestType + bRequest + wValue + wIndex + wLength
     *     bmRequestType=0x80 (Device→Host, Standard, Device)
     *     bRequest=6 (GET_DESCRIPTOR)
     *     wValue = (desc_type<<8)|desc_idx
     *     wIndex = lang_id
     *     wLength = length
     *   dw2: TRB Transfer Length = 8 (setup packet is always 8 bytes)
     *   dw3: TRT=3 (IN data stage) | IDT | type
     */
    uint32_t setup_dw0 = 0x80U                      /* bmRequestType  */
                       | (6U << 8)                   /* bRequest       */
                       | ((uint32_t)desc_type << 24) /* wValue hi      */
                       | ((uint32_t)desc_idx  << 16) /* wValue lo      */
                       ;
    uint32_t setup_dw1 = (uint32_t)lang_id           /* wIndex         */
                       | ((uint32_t)length << 16)     /* wLength        */
                       ;
    EP0_SUBMIT(setup_dw0, setup_dw1, 8,
               (TRB_TYPE_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | TRB_TRT_IN);

    /*
     * Data Stage TRB (§6.4.1.2.2):
     *   dw0–dw1: data buffer address
     *   dw2: transfer length
     *   dw3: DIR=IN | type
     */
    EP0_SUBMIT((uint32_t)(data_phys),
               (uint32_t)(data_phys >> 32),
               length,
               (TRB_TYPE_DATA_STAGE << TRB_TYPE_SHIFT) | TRB_DIR_IN);

    /*
     * Status Stage TRB (§6.4.1.2.3):
     *   DIR=OUT (0), IOC=1 (interrupt on completion)
     */
    EP0_SUBMIT(0, 0, 0,
               (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT) | (1U << 5));

#undef EP0_SUBMIT

    /* Ring EP0 doorbell: target = 1 (Endpoint ID 1 = EP0) */
    writel(1U, xhci_ctrl.doorbell_regs + slot_id * 4);
    asm volatile("dsb sy" ::: "memory");

    /* Collect two transfer events: Data stage + Status stage */
    int received = -1;
    for (int stage = 0; stage < 2; stage++) {
        uint32_t ev[4];
        if (evt_ring_poll(ev, 2000) != 0) {
            debug_print("[xHCI] EP0 GetDesc: event timeout (stage %d)\n", stage);
            return -1;
        }
        uint8_t cc  = (ev[2] >> 24) & 0xFF;
        uint8_t typ = (ev[3] >> 10) & 0x3F;
        if (typ == TRB_TYPE_TRANSFER_EVT) {
            if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
                uint32_t residual = ev[2] & 0xFFFFFF;
                received = (int)length - (int)residual;
            } else {
                debug_print("[xHCI] EP0 GetDesc: transfer CC=%u\n", cc);
                return -1;
            }
        }
    }
    return received;
}

/* ── USB String descriptor decode (UTF-16LE → ASCII) ─────────────────────── */
static void print_string_descriptor(const uint8_t *buf, int len)
{
    /* String descriptor: buf[0]=bLength, buf[1]=3, buf[2..] = UTF-16LE chars */
    if (len < 4 || buf[1] != 3) return;
    debug_print("\"");
    for (int i = 2; i + 1 < len && i + 1 < buf[0]; i += 2) {
        uint16_t c = buf[i] | ((uint16_t)buf[i+1] << 8);
        if (c >= 0x20 && c < 0x7F)
            debug_print("%c", (char)c);
        else
            debug_print("?");
    }
    debug_print("\"");
}

/* ── Full device enumeration for one port ────────────────────────────────── */
static void enumerate_device(int port, uint32_t speed)
{
    const char *speed_str = "?";
    switch (speed) {
        case 1: speed_str = "Full 12Mbps";  break;
        case 2: speed_str = "Low 1.5Mbps";  break;
        case 3: speed_str = "High 480Mbps"; break;
        case 4: speed_str = "Super 5Gbps";  break;
    }
    debug_print("[USB] Port %d: %s device — enumerating\n", port + 1, speed_str);

    /* 1. Reset the port */
    if (port_reset_and_enable(port) != 0) {
        debug_print("[USB] Port %d: reset failed\n", port + 1);
        return;
    }
    delay_ms(10);   /* Spec: ≥10ms after reset before first transaction */

    /* 2. Enable a device slot */
    uint8_t slot_id = 0;
    if (cmd_enable_slot(&slot_id) != 0) {
        debug_print("[USB] Port %d: Enable Slot failed\n", port + 1);
        return;
    }

    /* 3. Address the device (sets up EP0 context and assigns USB address) */
    if (cmd_address_device(slot_id, port, speed) != 0) {
        debug_print("[USB] Port %d: Address Device failed\n", port + 1);
        return;
    }
    delay_ms(2);

    /* 4. GET_DESCRIPTOR(Device) — first 8 bytes to confirm bMaxPacketSize0 */
    int n = ep0_get_descriptor(slot_id, 1, 0, 0, 8);
    if (n < 8) {
        debug_print("[USB] Port %d: GET_DESCRIPTOR(Device,8) failed n=%d\n",
                    port + 1, n);
        return;
    }

    const uint8_t *dev = (const uint8_t *)ep0_data;
    debug_print("[USB] Device: bLength=%u  bDescriptorType=%u  bcdUSB=0x%04x\n",
                dev[0], dev[1], dev[2] | ((uint16_t)dev[3] << 8));
    debug_print("[USB] Device: bDeviceClass=%u  bDeviceSubClass=%u  bDeviceProtocol=%u\n",
                dev[4], dev[5], dev[6]);
    debug_print("[USB] Device: bMaxPacketSize0=%u\n", dev[7]);

    /* 5. GET_DESCRIPTOR(Device) — full 18 bytes */
    n = ep0_get_descriptor(slot_id, 1, 0, 0, 18);
    if (n < 18) {
        debug_print("[USB] Port %d: GET_DESCRIPTOR(Device,18) short n=%d\n",
                    port + 1, n);
        /* Continue anyway with what we have */
    }

    if (n >= 18) {
        uint16_t vid = dev[8]  | ((uint16_t)dev[9]  << 8);
        uint16_t pid = dev[10] | ((uint16_t)dev[11] << 8);
        uint8_t  iMfr    = dev[14];
        uint8_t  iProd   = dev[15];
        uint8_t  iSerial = dev[16];
        debug_print("[USB] idVendor=0x%04x  idProduct=0x%04x\n", vid, pid);

        /* Fetch string descriptors if indices non-zero */
        if (iMfr) {
            n = ep0_get_descriptor(slot_id, 3, iMfr, 0x0409, 64);
            if (n > 0) {
                debug_print("[USB] Manufacturer: ");
                print_string_descriptor((const uint8_t *)ep0_data, n);
                debug_print("\n");
            }
        }
        if (iProd) {
            n = ep0_get_descriptor(slot_id, 3, iProd, 0x0409, 64);
            if (n > 0) {
                debug_print("[USB] Product:      ");
                print_string_descriptor((const uint8_t *)ep0_data, n);
                debug_print("\n");
            }
        }
        if (iSerial) {
            n = ep0_get_descriptor(slot_id, 3, iSerial, 0x0409, 64);
            if (n > 0) {
                debug_print("[USB] Serial:       ");
                print_string_descriptor((const uint8_t *)ep0_data, n);
                debug_print("\n");
            }
        }
    }

    debug_print("[USB] Port %d: enumeration complete  slot=%u\n",
                port + 1, slot_id);
}

/* ── Port scan ───────────────────────────────────────────────────────────── */
static void port_scan(void) {
    void *op = xhci_ctrl.op_regs;
    int   n  = xhci_ctrl.max_ports;

    debug_print("[xHCI] Port scan (%d port(s)):\n", n);
    for (int p = 0; p < n; p++) {
        uint32_t portsc = readl(op + 0x400 + p * 0x10);
        uint32_t speed  = (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
        const char *s   = "Unknown";
        switch (speed) {
            case 1: s = "Full  12 Mbps";  break;
            case 2: s = "Low   1.5 Mbps"; break;
            case 3: s = "High  480 Mbps"; break;
            case 4: s = "Super 5 Gbps";   break;
        }
        if (portsc & PORTSC_CCS) {
            debug_print("[xHCI]   Port %d: CONNECTED  %s  PORTSC=0x%08x\n",
                        p + 1, s, portsc);
            enumerate_device(p, speed);
        } else {
            debug_print("[xHCI]   Port %d: empty  PORTSC=0x%08x\n",
                        p + 1, portsc);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * xhci_init — full xHCI §4.2 initialisation sequence.
 *
 * @param base_addr  CPU-side virtual address of the xHCI MMIO window.
 *                   For VL805 on Pi 4: pass the value of VL805_BAR0_CPU
 *                   (0x600000000) after the ATU and RC are configured in pci.c.
 *                   The registers must already be accessible (Memory Space
 *                   enabled in both RC and VL805 Command registers).
 *
 * @return 0 on success, -1 on failure.
 */
int xhci_init(uint64_t base_addr) {
    debug_print("\n[xHCI] ══ Init start  base=0x%llx ══\n",
                (unsigned long long)base_addr);

    memset(&xhci_ctrl, 0, sizeof(xhci_ctrl));
    xhci_ctrl.cap_regs      = (void *)(uintptr_t)base_addr;
    xhci_ctrl.cap_regs_phys = base_addr;

    /* Initialise enumeration DMA region pointers */
    input_ctx = (volatile uint32_t *)(xhci_dma_buf + DMA_INPUT_CTX_OFF);
    out_ctx   = (volatile uint32_t *)(xhci_dma_buf + DMA_OUT_CTX_OFF);
    ep0_ring  = (volatile uint32_t *)(xhci_dma_buf + DMA_EP0_RING_OFF);
    ep0_data  = (volatile uint8_t  *)(xhci_dma_buf + DMA_EP0_DATA_OFF);

    /* Initialise event ring consumer state */
    evt_dequeue = 0;
    evt_cycle   = 1;

    /* 2.1 */ if (read_caps()        != 0) return -1;
    /* 2.2 */ if (do_reset()         != 0) return -1;
    /* 2.3+2.4 */ if (setup_dcbaa() != 0) return -1;
    /* 2.5 */ if (setup_cmd_ring()   != 0) return -1;
    /* 2.6 */ if (setup_event_ring() != 0) return -1;
    /* 2.7 */    setup_interrupter();
    /* 2.8 */ if (run_controller()   != 0) return -1;

    /* Settle: give the VL805 MCU time to complete its post-RS=1 init
     * before we start issuing port resets and command ring doorbells.
     * Also dump USBSTS and ERDP so we can see the controller's state
     * at the point we hand off to the port scanner. */
    {
        debug_print("[xHCI] Settling 150ms after RS=1...\n");
        for (int i = 0; i < 150; i++) delay_us(1000);
        uint32_t settle_sts  = readl(xhci_ctrl.op_regs + OP_USBSTS);
        uint32_t settle_erdp = readl(ir_base(0) + IR_ERDP_LO);
        uint32_t settle_iman = readl(ir_base(0) + IR_IMAN);
        debug_print("[xHCI] Post-settle USBSTS=0x%08x  ERDP_LO=0x%08x"
                    "  IMAN=0x%08x\n",
                    settle_sts, settle_erdp, settle_iman);
        if (settle_sts & STS_HSE) {
            writel(STS_HSE | STS_EINT | STS_PCD,
                   xhci_ctrl.op_regs + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            debug_print("[xHCI] HSE still set after settle — RW1C cleared\n");
        }
    }

    /* 2.9 */    power_ports();
               port_scan();

    xhci_ctrl.initialized = 1;
    debug_print("[xHCI] ══ Init complete ══\n\n");
    return 0;
}

int xhci_is_ready(void) {
    return xhci_ctrl.initialized;
}

xhci_controller_t *xhci_get_controller(void) {
    return xhci_ctrl.initialized ? &xhci_ctrl : NULL;
}

int xhci_scan_ports(void) {
    if (!xhci_ctrl.initialized) { debug_print("[xHCI] not initialized\n"); return -1; }
    port_scan();
    return xhci_ctrl.max_ports;
}

