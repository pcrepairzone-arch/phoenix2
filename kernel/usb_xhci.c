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
 *   AC64=1 (64-bit addresses required), CSZ=0, Scratchpad=0
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
#define TRB_TYPE_LINK       23
#define TRB_TYPE_NOOP_CMD   6

/* ── DMA buffer layout ───────────────────────────────────────────────────── */
/*
 * Single statically-allocated, 64-byte-aligned buffer.  Must be placed
 * in non-cacheable memory by the linker/MMU.
 *
 * [0x0000] DCBAA      2048 B  (256 × 8-byte slot pointers)
 * [0x0800] cmd_ring   1024 B  (64 × 16-byte TRBs)
 * [0x0C00] evt_ring   1024 B  (64 × 16-byte TRBs)
 * [0x1000] erst         64 B  (1 × 16-byte ERST entry, 64-byte padded)
 * [0x1040] scratch_arr 512 B  (up to 64 × 8-byte scratchpad pointers)
 * Total    0x1240 → padded to 0x1400 = 5120 B
 */
#define DMA_DCBAA_OFF    0x0000
#define DMA_CMD_RING_OFF 0x0800
#define DMA_EVT_RING_OFF 0x0C00
#define DMA_ERST_OFF     0x1000
#define DMA_SCRATCH_OFF  0x1040
#define DMA_BUF_SIZE     0x1400

#define CMD_RING_TRBS    64
#define EVT_RING_TRBS    64
#define MAX_SCRATCH_PTRS 64

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

/* ── Controller state ────────────────────────────────────────────────────── */
static xhci_controller_t xhci_ctrl;

static uint8_t  cmd_cycle   = 1;   /* producer cycle bit */
static uint32_t cmd_enqueue = 0;   /* next TRB slot index */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Write 64-bit value as two 32-bit register writes, LO before HI (spec req) */
static void reg_write64(void *base, uint32_t lo_off, uint64_t val) {
    writel((uint32_t)(val),        base + lo_off);
    asm volatile("dsb sy" ::: "memory");
    writel((uint32_t)(val >> 32),  base + lo_off + 4);
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

    if (clen < 0x10 || clen > 0x40) {
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

    /* Stop controller if it is running (HCH=0 means running) */
    if (!(readl(op + OP_USBSTS) & STS_HCH)) {
        debug_print("[xHCI] Stopping controller before reset\n");
        writel(readl(op + OP_USBCMD) & ~CMD_RS, op + OP_USBCMD);
        for (int t = 400; t > 0; t--) {
            delay_us(500);
            if (readl(op + OP_USBSTS) & STS_HCH) break;
        }
        if (!(readl(op + OP_USBSTS) & STS_HCH)) {
            debug_print("[xHCI] ERROR: failed to halt before reset\n");
            return -1;
        }
    }

    debug_print("[xHCI] Issuing HCRST\n");
    writel(readl(op + OP_USBCMD) | CMD_HCRST, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");

    /*
     * Spec §4.2 step 2: software must not write any operational register
     * until HCRST self-clears AND CNR clears.
     */
    for (int t = 1000; t > 0; t--) {
        delay_us(100);
        uint32_t cmd = readl(op + OP_USBCMD);
        uint32_t sts = readl(op + OP_USBSTS);
        if (cmd == 0xFFFFFFFF || sts == 0xFFFFFFFF) {
            debug_print("[xHCI] ERROR: device vanished during reset\n");
            return -1;
        }
        if (!(cmd & CMD_HCRST) && !(sts & STS_CNR)) {
            debug_print("[xHCI] Reset complete (CNR clear)\n");
            return 0;
        }
    }
    debug_print("[xHCI] ERROR: reset timed out\n");
    return -1;
}

/* ── 2.3 + 2.4 DCBAA (and optional scratchpad) ───────────────────────────── */
static int setup_dcbaa(void) {
    dcbaa = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    memset((void *)dcbaa, 0, 2048);

    uint32_t n = xhci_ctrl.scratchpad_count;
    if (n > 0) {
        if (n > MAX_SCRATCH_PTRS) {
            debug_print("[xHCI] ERROR: scratchpad count %u > max %u\n",
                        n, MAX_SCRATCH_PTRS);
            return -1;
        }
        debug_print("[xHCI] Allocating %u scratchpad page(s)\n", n);

        scratch_arr = (volatile uint64_t *)(xhci_dma_buf + DMA_SCRATCH_OFF);
        memset((void *)scratch_arr, 0, MAX_SCRATCH_PTRS * 8);

        for (uint32_t i = 0; i < n; i++) {
            void *pg = kcalloc(1, 4096);
            if (!pg) {
                debug_print("[xHCI] ERROR: scratchpad page %u alloc failed\n", i);
                return -1;
            }
        flush_normal_page(pg, 4096);      /* pg is Normal WB heap — must flush */
        scratch_arr[i] = (uint64_t)virt_to_phys(pg);
    }
    /* scratch_arr is in Device memory — no flush needed */
    asm volatile("dsb sy" ::: "memory");

        /* DCBAA slot 0 = physical address of scratchpad pointer array */
        dcbaa[0] = (uint64_t)virt_to_phys((void *)scratch_arr);
        debug_print("[xHCI] DCBAA[0] = scratchpad array phys=0x%llx\n",
                    (unsigned long long)dcbaa[0]);
    } else {
        debug_print("[xHCI] No scratchpad required\n");
    }

    /* dcbaa is in Device memory — no D-cache flush needed; DSB ensures ordering */
    asm volatile("dsb sy" ::: "memory");

    uint64_t dcbaa_phys = (uint64_t)virt_to_phys((void *)dcbaa);
    debug_print("[xHCI] DCBAA phys=0x%llx\n", (unsigned long long)dcbaa_phys);
    reg_write64(xhci_ctrl.op_regs, OP_DCBAAP_LO, dcbaa_phys);

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
    uint32_t li = (CMD_RING_TRBS - 1) * 4;   /* dword index of Link TRB */
    cmd_ring[li + 0] = (uint32_t)(ring_phys);
    cmd_ring[li + 1] = (uint32_t)(ring_phys >> 32);
    cmd_ring[li + 2] = 0;
    cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;

    /* cmd_ring is in Device memory — no flush; DSB before CRCR write */
    asm volatile("dsb sy" ::: "memory");

    /*
     * CRCR: [63:6]=ring base phys, [5:4]=reserved, [3]=CRR(RO),
     *        [2]=CA, [1]=CS, [0]=RCS.  Write base | RCS=cmd_cycle.
     * Bits [5:1] must be 0 on write.
     */
    uint64_t crcr = (ring_phys & ~0x3FULL) | (uint64_t)cmd_cycle;
    reg_write64(xhci_ctrl.op_regs, OP_CRCR_LO, crcr);

    debug_print("[xHCI] Command ring phys=0x%llx  RCS=%u  (%d TRBs)\n",
                (unsigned long long)ring_phys, cmd_cycle, CMD_RING_TRBS);
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

    /*
     * ERST entry 0 (spec §6.5):
     *   [63:0]  = ring segment base address (64-byte aligned phys)
     *   [79:64] = ring segment size in TRBs  (stored in erst[1] lo 16 bits)
     *   [127:80]= reserved
     * Use two 64-bit writes for clarity; hardware reads 16 bytes total.
     */
    erst[0] = evt_phys;
    erst[1] = (uint64_t)EVT_RING_TRBS;

    /* evt_ring and erst are in Device memory — no flush; DSB before runtime reg writes */
    asm volatile("dsb sy" ::: "memory");

    /*
     * Program interrupter 0 runtime registers (spec §4.2 step 6):
     *   ERSTSZ  = 1  (one ERST entry)
     *   ERSTBA  = erst_phys
     *   ERDP    = evt_phys | EHB  (EHB=bit3, write 1 to clear; DESI=0)
     */
    void *ir0 = ir_base(0);
    writel(1U, ir0 + IR_ERSTSZ);
    asm volatile("dsb sy" ::: "memory");
    reg_write64(ir0, IR_ERSTBA_LO, erst_phys);
    reg_write64(ir0, IR_ERDP_LO,  evt_phys | 0x8ULL);

    debug_print("[xHCI] Event ring phys=0x%llx  ERST phys=0x%llx  (%d TRBs)\n",
                (unsigned long long)evt_phys,
                (unsigned long long)erst_phys, EVT_RING_TRBS);
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
    uint32_t cmd = readl(op + OP_USBCMD);

    cmd |= CMD_RS;    /* Run/Stop = 1      */
    cmd |= CMD_INTE;  /* Interrupter enable */
    cmd |= CMD_HSEE;  /* Host System Error enable */
    writel(cmd, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");

    /* Poll until HCH clears (controller left halted state) */
    for (int t = 200; t > 0; t--) {
        delay_us(500);
        uint32_t sts = readl(op + OP_USBSTS);
        if (sts == 0xFFFFFFFF) {
            debug_print("[xHCI] ERROR: device vanished after RS=1\n");
            return -1;
        }
        if (sts & STS_HSE) {
            debug_print("[xHCI] ERROR: HSE set (USBSTS=0x%08x) — "
                        "DMA structure address or alignment problem\n", sts);
            return -1;
        }
        if (!(sts & STS_HCH)) {
            debug_print("[xHCI] Controller RUNNING  USBSTS=0x%08x\n", sts);
            return 0;
        }
    }
    debug_print("[xHCI] ERROR: HCH still set after RS=1 (USBSTS=0x%08x)\n",
                readl(op + OP_USBSTS));
    return -1;
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
        if (portsc & PORTSC_CCS)
            debug_print("[xHCI]   Port %d: CONNECTED  %s  PORTSC=0x%08x\n",
                        p + 1, s, portsc);
        else
            debug_print("[xHCI]   Port %d: empty  PORTSC=0x%08x\n",
                        p + 1, portsc);
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

    /* 2.1 */ if (read_caps()        != 0) return -1;
    /* 2.2 */ if (do_reset()         != 0) return -1;
    /* 2.3+2.4 */ if (setup_dcbaa() != 0) return -1;
    /* 2.5 */ if (setup_cmd_ring()   != 0) return -1;
    /* 2.6 */ if (setup_event_ring() != 0) return -1;
    /* 2.7 */    setup_interrupter();
    /* 2.8 */ if (run_controller()   != 0) return -1;
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
