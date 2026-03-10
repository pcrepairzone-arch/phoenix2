/**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver — spec-compliant init for VL805 on Pi 4
 */

#include "kernel.h"
#include "usb_xhci.h"
#include "usb.h"
#include <string.h>

/* ── Capability register offsets ─────────────────────────────────────────── */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Operational registers */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_CRCR_LO      0x18
#define OP_CRCR_HI      0x1C
#define OP_DCBAAP_LO    0x30
#define OP_DCBAAP_HI    0x34
#define OP_CONFIG       0x38

/* USBCMD bits */
#define CMD_RS          (1U << 0)
#define CMD_HCRST       (1U << 1)
#define CMD_INTE        (1U << 2)

/* USBSTS bits */
#define STS_HCH         (1U << 0)
#define STS_HSE         (1U << 2)
#define STS_EINT        (1U << 3)
#define STS_PCD         (1U << 4)
#define STS_CNR         (1U << 11)

/* Runtime interrupter offsets */
#define IR_IMAN         0x00
#define IR_IMOD         0x04
#define IR_ERSTSZ       0x08
#define IR_ERSTBA_LO    0x10
#define IR_ERSTBA_HI    0x14
#define IR_ERDP_LO      0x18
#define IR_ERDP_HI      0x1C

/* PORTSC bits */
#define PORTSC_CCS      (1U <<  0)
#define PORTSC_PED      (1U <<  1)
#define PORTSC_PP       (1U <<  9)
#define PORTSC_WIC      0x00FE0000U

/* TRB bits */
#define TRB_CYCLE           (1U <<  0)
#define TRB_TC              (1U <<  1)
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE_LINK        6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_PORT_CHNG_EVT 34

/* Completion codes */
#define CC_SUCCESS          1

/* DMA buffer layout (adjust to match your linker script) */
#define DMA_DCBAA_OFF      0x0000
#define DMA_CMD_RING_OFF   0x0800
#define DMA_EVT_RING_OFF   0x0C00
#define DMA_ERST_OFF       0x1000
#define DMA_SCRATCH_OFF    0x1040
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
static uint8_t  evt_cycle   = 1;

static xhci_controller_t xhci_ctrl;

static uint8_t  cmd_cycle   = 1;
static uint32_t cmd_enqueue = 0;

#define DMA_OFFSET  0xC0000000ULL

static inline uint64_t phys_to_dma(uint64_t phys) {
    return phys + DMA_OFFSET;
}

static void reg_write64(void *base, uint32_t lo_off, uint64_t val) {
    volatile uint64_t *reg = (volatile uint64_t *)((uint8_t *)base + lo_off);
    *reg = val;
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

/* ── Read capabilities ───────────────────────────────────────────────────── */
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

/* ── Controller reset ────────────────────────────────────────────────────── */
static int do_reset(void) {
    void *op = xhci_ctrl.op_regs;

    if (!(readl(op + OP_USBSTS) & STS_HCH)) {
        writel(readl(op + OP_USBCMD) & ~CMD_RS, op + OP_USBCMD);
        for (int t = 400; t > 0; t--) {
            delay_ms(1);
            if (readl(op + OP_USBSTS) & STS_HCH) break;
        }
    }

    int cnr_ready = 0;
    for (int t = 0; t < 2000; t++) {
        delay_ms(1);
        uint32_t s = readl(op + OP_USBSTS);
        if (!(s & STS_CNR)) {
            cnr_ready = 1;
            break;
        }
    }
    if (!cnr_ready) return -1;

    writel(readl(op + OP_USBCMD) | CMD_HCRST, op + OP_USBCMD);
    asm volatile("dsb sy; isb" ::: "memory");

    for (int t = 5000; t > 0; t--) {
        delay_ms(1);
        uint32_t cmd = readl(op + OP_USBCMD);
        uint32_t sts = readl(op + OP_USBSTS);
        if (!(cmd & CMD_HCRST) && !(sts & STS_CNR)) {
            /* HCRST complete — clear sticky status bits (HSE, EINT, PCD)
             * left by start4.elf's handoff.  These are RW1C: writing 1 clears.
             * If HSE is still set when RS=1 is written the MCU self-clears RS
             * immediately, causing the stabilization loop to reburst forever. */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            delay_ms(1);
            return 0;
        }
    }
    return -1;
}

/* ── Setup DCBAA and scratchpads ─────────────────────────────────────────── */
static int setup_dcbaa(void) {
    dcbaa = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    memset((void *)dcbaa, 0, 2048);

    /* Read and validate PAGESIZE register.
     * Bits[15:0]: each bit set = that page size is supported.
     * Bit 0 = 4KB, bit 1 = 8KB, bit 2 = 16KB, etc.
     * We only support 4KB (bit 0). If not set, scratchpad alloc is wrong. */
    uint32_t pgsz = readl(xhci_ctrl.op_regs + OP_PAGESIZE);
    debug_print("[xHCI] PAGESIZE reg = 0x%08x (%s)\n", pgsz,
                (pgsz & 1) ? "4KB OK" : "4KB NOT supported — scratchpad alloc wrong");
    if (!(pgsz & 1)) {
        debug_print("[xHCI] ERROR: controller does not support 4KB pages\n");
        return -1;
    }

    uint32_t n = xhci_ctrl.scratchpad_count;
    debug_print("[xHCI] Scratchpad count = %u\n", n);

    if (n > 0) {
        if (n > MAX_SCRATCH_PAGES) {
            debug_print("[xHCI] ERROR: scratchpad count %u > max %u\n", n, MAX_SCRATCH_PAGES);
            return -1;
        }

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
        asm volatile("dsb sy" ::: "memory");

        debug_print("[xHCI] Scratch arr DMA=0x%llx  page[0] DMA=0x%llx  page[30] DMA=0x%llx\n",
                    scratch_arr_dma, (uint64_t)scratch_arr[0], (uint64_t)scratch_arr[n - 1]);
    }

    asm volatile("dsb sy" ::: "memory");

    uint64_t dcbaa_phys = (uint64_t)virt_to_phys((void *)dcbaa);
    uint64_t dcbaa_dma  = phys_to_dma(dcbaa_phys);
    debug_print("[xHCI] DCBAA phys=0x%llx  DMA=0x%llx  dcbaa[0]=0x%llx\n",
                dcbaa_phys, dcbaa_dma, (uint64_t)dcbaa[0]);
    reg_write64(xhci_ctrl.op_regs, OP_DCBAAP_LO, dcbaa_dma);

    writel(xhci_ctrl.max_slots & 0xFFU, xhci_ctrl.op_regs + OP_CONFIG);
    return 0;
}

/* ── Setup command ring ──────────────────────────────────────────────────── */
static int setup_cmd_ring(void) {
    cmd_ring    = (volatile uint32_t *)(xhci_dma_buf + DMA_CMD_RING_OFF);
    cmd_cycle   = 1;
    cmd_enqueue = 0;
    memset((void *)cmd_ring, 0, CMD_RING_TRBS * 16);

    uint64_t ring_phys = (uint64_t)virt_to_phys((void *)cmd_ring);
    uint64_t ring_dma  = phys_to_dma(ring_phys);
    uint32_t li = (CMD_RING_TRBS - 1) * 4;
    cmd_ring[li + 0] = (uint32_t)(ring_dma);
    cmd_ring[li + 1] = (uint32_t)(ring_dma >> 32);
    cmd_ring[li + 2] = 0;
    cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;

    asm volatile("dsb sy" ::: "memory");

    /* CS (Command Stop, bit 1) must be set in the CRCR write on VL805.
     * Without it the ring base address is silently ignored — CRCR reads
     * back as 0x00000000 and the MCU has no valid ring pointer, causing
     * an immediate HSE the moment RS=1 is asserted. */
    uint64_t crcr = (ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle | (1ULL << 1);
    reg_write64(xhci_ctrl.op_regs, OP_CRCR_LO, crcr);

    return 0;
}

/* ── Setup event ring ────────────────────────────────────────────────────── */
static int setup_event_ring(void) {
    evt_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
    erst     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);

    memset((void *)evt_ring, 0, EVT_RING_TRBS * 16);
    memset((void *)erst,     0, 64);

    uint64_t evt_phys  = (uint64_t)virt_to_phys((void *)evt_ring);
    uint64_t erst_phys = (uint64_t)virt_to_phys((void *)erst);
    uint64_t evt_dma   = phys_to_dma(evt_phys);
    uint64_t erst_dma  = phys_to_dma(erst_phys);

    erst[0] = evt_dma;
    erst[1] = (uint64_t)EVT_RING_TRBS;

    asm volatile("dsb sy" ::: "memory");

    void *ir0 = ir_base(0);
    writel(1U, ir0 + IR_ERSTSZ);
    reg_write64(ir0, IR_ERSTBA_LO, erst_dma);
    /* Write ERDP pointing at the first TRB.  Do NOT set EHB (bit 3) here —
     * EHB is a RW1C status bit; pre-setting it causes the VL805 to treat
     * the dequeue pointer as misaligned, triggering a DMA bus error (HSE)
     * the moment the controller tries to write its first event, which sets
     * USBSTS.HSE and immediately halts the controller. */
    reg_write64(ir0, IR_ERDP_LO, evt_dma);

    return 0;
}

/* ── Setup interrupter ───────────────────────────────────────────────────── */
static void setup_interrupter(void) {
    void *ir0 = ir_base(0);
    /* IE=1 only.  IP (bit 0) is RW1C — writing 1 would attempt to clear a
     * pending interrupt, but on the VL805 it triggers a spurious event DMA
     * write immediately on RS=1 which races with MCU startup and sets HSE. */
    writel(0x00000002U, ir0 + IR_IMAN);
    writel(0x0FA00FA0U, ir0 + IR_IMOD);
    asm volatile("dsb sy; isb" ::: "memory");
}

/* ── Extended MCU stabilization ──────────────────────────────────────────── */
static int run_controller(void) {
    void *op  = xhci_ctrl.op_regs;
    void *ir0 = ir_base(0);

    /* Pre-compute all register values — no arithmetic in the tight burst below */
    uint64_t dcbaa_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
    uint64_t erstba_dma = phys_to_dma((uint64_t)virt_to_phys((void *)erst));
    uint64_t evt_dma    = phys_to_dma((uint64_t)virt_to_phys((void *)evt_ring));
    uint64_t crcr_val   = (phys_to_dma((uint64_t)virt_to_phys((void *)cmd_ring))
                           & ~0x3FULL) | (uint64_t)cmd_cycle | (1ULL << 1); /* RCS + CS */
    uint32_t cfg_val    = (readl(op + OP_CONFIG) & ~0xFFU) | (xhci_ctrl.max_slots & 0xFFU);

    /* Diagnostic register dump */
    debug_print("[xHCI] pre-run: DCBAAP=%08x_%08x ERSTBA=%08x_%08x ERDP=%08x_%08x\n",
                readl(op + OP_DCBAAP_HI), readl(op + OP_DCBAAP_LO),
                readl(ir0 + IR_ERSTBA_HI), readl(ir0 + IR_ERSTBA_LO),
                readl(ir0 + IR_ERDP_HI),  readl(ir0 + IR_ERDP_LO));
    debug_print("[xHCI] pre-run: CRCR_LO=%08x ERSTSZ=%08x IMAN=%08x\n",
                readl(op + OP_CRCR_LO), readl(ir0 + IR_ERSTSZ), readl(ir0 + IR_IMAN));
    debug_print("[xHCI] mem: dcbaa[0]=0x%016llx erst[0]=0x%016llx erst[1]=0x%016llx\n",
                (unsigned long long)dcbaa[0],
                (unsigned long long)erst[0],
                (unsigned long long)erst[1]);

    /*
     * After HCRST the VL805 MCU re-runs its own firmware init and posts events
     * to its internal ring, setting USBSTS.EINT.  If RS=1 is written while
     * EINT=1, the MCU tries to deliver that pending event using its stale
     * internal ring pointer (not our ERSTBA) → DMA to unmapped address → HSE.
     *
     * Wait up to 200ms for EINT + HSE to clear, then force-clear and proceed.
     */
    debug_print("[xHCI] Waiting for MCU to settle (EINT+HSE poll)...\n");
    {
        int settled = 0;
        for (int w = 0; w < 200; w++) {
            delay_ms(1);
            uint32_t s = readl(op + OP_USBSTS);
            if (!(s & (STS_EINT | STS_HSE))) {
                debug_print("[xHCI] MCU settled after %dms  USBSTS=0x%08x\n", w+1, s);
                settled = 1;
                break;
            }
        }
        if (!settled) {
            debug_print("[xHCI] MCU not settled after 200ms — force-clearing  USBSTS=0x%08x\n",
                        readl(op + OP_USBSTS));
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
        }
    }

    /*
     * Tight launch loop.
     *
     * Burst all ring registers with no prints or reads between writes — every
     * µs counts here as the MCU may race to overwrite them.  Then clear status
     * and write RS=1 immediately.  Poll HCH only (not HSE): the MCU fires a
     * cosmetic HSE pulse within ~60µs of RS=1 but does NOT halt the controller
     * as long as CMD_HSEE=0 (which it is — we write CMD_RS|CMD_INTE only).
     */
    int launched = 0;
    for (int attempt = 0; attempt < 50 && !launched; attempt++) {
        /* Tight register burst — no prints, no reads, no delays */
        asm volatile("dsb sy; isb" ::: "memory");
        writel((uint32_t)(dcbaa_dma),        op  + OP_DCBAAP_LO);
        writel((uint32_t)(dcbaa_dma >> 32),  op  + OP_DCBAAP_HI);
        writel(cfg_val,                      op  + OP_CONFIG);
        writel(1U,                           ir0 + IR_ERSTSZ);
        reg_write64(ir0, IR_ERSTBA_LO, erstba_dma);
        reg_write64(ir0, IR_ERDP_LO,   evt_dma);
        reg_write64(op,  OP_CRCR_LO,   crcr_val);
        /* Clear stale status bits before RS=1 */
        writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
        asm volatile("dsb sy; isb" ::: "memory");

        /* RS=1 — NOTE: CMD_HSEE is NOT set, so HSE does not halt the controller */
        writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        /* Tight poll for HCH=0 (~300ns per iteration) */
        int running = 0;
        for (int p = 0; p < 500; p++) {
            if (!(readl(op + OP_USBSTS) & STS_HCH)) { running = 1; break; }
        }

        uint32_t cmd_now = readl(op + OP_USBCMD);
        uint32_t sts_now = readl(op + OP_USBSTS);
        debug_print("[xHCI] attempt %d: USBCMD=0x%08x USBSTS=0x%08x %s\n",
                    attempt + 1, cmd_now, sts_now, running ? "RUNNING" : "halted");

        if (running) {
            /* Clear cosmetic MCU HSE pulse if present */
            if (sts_now & STS_HSE) {
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
            }
            launched = 1;
        }
    }

    if (!launched) {
        debug_print("[xHCI] ERROR: controller would not start after 50 attempts\n");
        return -1;
    }

    debug_print("[xHCI] Controller running. USBSTS=0x%08x\n", readl(op + OP_USBSTS));
    return 0;
}


/* ── Forward declarations ────────────────────────────────────────────────── */
static int  evt_ring_poll(uint32_t *out, int timeout_ms);
static void evt_ring_consume(void *ir0, uint32_t *out);

/* ── Command ring submission ─────────────────────────────────────────────── */
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type) {
    void *ir0 = ir_base(0);

    /* Pre-drain stale events */
    uint32_t tmp[4];
    while (1) {
        uint32_t dw3 = evt_ring[evt_dequeue * 4 + 3];
        if ((dw3 & TRB_CYCLE) != evt_cycle) break;
        evt_ring_consume(ir0, tmp);
    }

    uint32_t slot = cmd_enqueue;
    uint32_t base = slot * 4;

    cmd_ring[base + 0] = dw0;
    cmd_ring[base + 1] = dw1;
    cmd_ring[base + 2] = dw2;
    cmd_ring[base + 3] = (type << TRB_TYPE_SHIFT) | cmd_cycle;
    asm volatile("dsb sy" ::: "memory");

    cmd_enqueue++;
    if (cmd_enqueue >= CMD_RING_TRBS - 1) {
        uint32_t li = (CMD_RING_TRBS - 1) * 4;
        cmd_ring[li + 3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | cmd_cycle;
        asm volatile("dsb sy" ::: "memory");
        cmd_cycle ^= 1;
        cmd_enqueue = 0;
    }

    /* Ring doorbell, but first check if the VL805 MCU has halted the
     * controller and wiped the operational registers (CRCR in particular
     * reads back as 0 after every internal MCU restart).  If so, restore
     * all writable op-regs before asserting RS=1 again, then wait for the
     * controller to be genuinely running before we ring. */
    {
        void    *op   = xhci_ctrl.op_regs;
        void    *ir0  = ir_base(0);
        uint64_t crcr = (phys_to_dma((uint64_t)virt_to_phys((void *)cmd_ring)) & ~0x3FULL)
                        | (uint64_t)cmd_cycle | (1ULL << 1);
        uint64_t erst_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)erst));
        uint64_t erdp_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)&evt_ring[evt_dequeue * 4]));

        for (int attempt = 0; attempt < 20; attempt++) {
            uint32_t sts = readl(op + OP_USBSTS);
            if (!(sts & STS_HCH))
                break;  /* controller is running — safe to doorbell */

            /* Clear HSE first and confirm before restoring registers */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            for (int c = 0; c < 10; c++) {
                delay_ms(1);
                if (!(readl(op + OP_USBSTS) & STS_HSE)) break;
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
            }

            /* Restore only ERSTSZ/ERSTBA/ERDP — DCBAAP and CONFIG are not
             * wiped by the VL805 MCU and rewriting DCBAAP triggers a
             * scratchpad prefetch which can race and re-set HSE. */
            writel(1U,                                  ir0 + IR_ERSTSZ);
            reg_write64(ir0, IR_ERSTBA_LO, erst_dma);
            reg_write64(ir0, IR_ERDP_LO,   erdp_dma);
            asm volatile("dsb sy; isb" ::: "memory");
            reg_write64(op, OP_CRCR_LO, crcr);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");

            /* Wait up to 50 ms for HCH to clear */
            for (int w = 0; w < 50; w++) {
                delay_ms(1);
                if (!(readl(op + OP_USBSTS) & STS_HCH))
                    break;
            }
        }

        /* 5 ms settle before doorbell so MCU latches register state */
        delay_ms(5);
        writel(0, xhci_ctrl.doorbell_regs);
        asm volatile("dsb sy" ::: "memory");
    }

    return (uint64_t)virt_to_phys((void *)&cmd_ring[base]);
}

/* ── Enable Slot ─────────────────────────────────────────────────────────── */
static int cmd_enable_slot(uint8_t *slot_id_out) {
    cmd_ring_submit(0, 0, 0, TRB_TYPE_ENABLE_SLOT);

    uint32_t ev[4];
    if (evt_ring_poll(ev, 5000) != 0) {
        debug_print("[xHCI] Enable Slot timeout after 5s\n");
        return -1;
    }

    uint8_t cc = (ev[2] >> 24) & 0xFF;
    if (cc != CC_SUCCESS) {
        debug_print("[xHCI] Enable Slot failed CC=%u\n", cc);
        return -1;
    }

    *slot_id_out = (ev[3] >> 24) & 0xFF;
    debug_print("[xHCI] Enable Slot success: slot=%u\n", *slot_id_out);
    return 0;
}

/* ── Event ring poll ─────────────────────────────────────────────────────── */
static int evt_ring_poll(uint32_t *out, int timeout_ms) {
    void    *op  = xhci_ctrl.op_regs;
    void    *ir0 = ir_base(0);

    /* Pre-compute stable DMA addresses for re-burst restores */
    uint64_t dcbaa_dma = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
    uint64_t erst_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)erst));

    /* Iterate in 100 µs steps */
    int steps = timeout_ms * 10;
    for (int i = 0; i < steps; i++) {
        asm volatile("dsb sy" ::: "memory");

        /* If the VL805 MCU has halted the controller again, restore all
         * operational registers (including CRCR) before re-asserting RS=1.
         * Without this, CRCR stays zero and the pending command TRB is
         * never processed, so no completion event ever appears. */
        uint32_t sts = readl(op + OP_USBSTS);
        if (sts & STS_HCH) {
            uint64_t crcr     = (phys_to_dma(virt_to_phys((void *)cmd_ring)) & ~0x3FULL)
                                | (uint64_t)cmd_cycle | (1ULL << 1);
            uint64_t erdp_dma = phys_to_dma(virt_to_phys((void *)&evt_ring[evt_dequeue * 4]));

            /* Clear HSE first and confirm before restoring registers */
            writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
            asm volatile("dsb sy; isb" ::: "memory");
            for (int c = 0; c < 10; c++) {
                delay_ms(1);
                if (!(readl(op + OP_USBSTS) & STS_HSE)) break;
                writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
                asm volatile("dsb sy; isb" ::: "memory");
            }

            /* Restore only ERSTSZ/ERSTBA/ERDP — not DCBAAP/CONFIG */
            writel(1U,                                  ir0 + IR_ERSTSZ);
            reg_write64(ir0, IR_ERSTBA_LO, erst_dma);
            reg_write64(ir0, IR_ERDP_LO,   erdp_dma);
            asm volatile("dsb sy; isb" ::: "memory");
            reg_write64(op, OP_CRCR_LO, crcr);
            asm volatile("dsb sy; isb" ::: "memory");
            writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
            asm volatile("dsb sy; isb" ::: "memory");

            /* Wait up to 10 ms for controller to start running */
            for (int w = 0; w < 10; w++) {
                delay_ms(1);
                if (!(readl(op + OP_USBSTS) & STS_HCH))
                    break;
            }

            /* Re-ring doorbell so the command TRB gets processed */
            delay_ms(2);
            writel(0, xhci_ctrl.doorbell_regs);
            asm volatile("dsb sy" ::: "memory");

            delay_us(100);
            continue;
        }

        /* Clear EHB (Event Handler Busy) if set so the interrupter advances */
        uint32_t erdp_lo = readl(ir0 + IR_ERDP_LO);
        if (erdp_lo & 0x8U) {
            uint64_t erdp = phys_to_dma(virt_to_phys((void *)&evt_ring[evt_dequeue * 4]));
            reg_write64(ir0, IR_ERDP_LO, erdp | 0x8ULL);
        }

        uint32_t dw3 = evt_ring[evt_dequeue * 4 + 3];
        if ((dw3 & TRB_CYCLE) == evt_cycle) {
            uint32_t type = (dw3 >> TRB_TYPE_SHIFT) & 0x3F;
            if (type == TRB_TYPE_PORT_CHNG_EVT) {
                uint32_t tmp[4];
                evt_ring_consume(ir0, tmp);
                continue;
            }
            evt_ring_consume(ir0, out);
            return 0;
        }

        delay_us(100);
    }
    return -1;
}

/* ── Consume event ───────────────────────────────────────────────────────── */
static void evt_ring_consume(void *ir0, uint32_t *out) {
    out[0] = evt_ring[evt_dequeue * 4 + 0];
    out[1] = evt_ring[evt_dequeue * 4 + 1];
    out[2] = evt_ring[evt_dequeue * 4 + 2];
    out[3] = evt_ring[evt_dequeue * 4 + 3];

    evt_dequeue++;
    if (evt_dequeue >= EVT_RING_TRBS) {
        evt_dequeue = 0;
        evt_cycle ^= 1;
    }

    uint64_t new_erdp = phys_to_dma(virt_to_phys((void *)&evt_ring[evt_dequeue * 4]));
    reg_write64(ir0, IR_ERDP_LO, new_erdp);
}

/* ── Main init entry point (called from pci.c or kernel_main) ────────────── */
int xhci_init(uint64_t base_addr) {
    debug_print("[xHCI] driver v17 init base=0x%llx\n", base_addr);

    xhci_ctrl.cap_regs = (void *)(uintptr_t)base_addr;

    dcbaa    = (volatile uint64_t *)(xhci_dma_buf + DMA_DCBAA_OFF);
    cmd_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_CMD_RING_OFF);
    evt_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
    erst     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);

    if (read_caps() != 0) return -1;

    /* Skip do_reset() — the VideoCore already did a clean PERST# cycle
     * via mbox_notify_xhci_reset (tag 0x00030058) before we got here.
     * Issuing HCRST on top of that does NOT fully reset the VL805 MCU
     * firmware — it only resets the xHCI register interface — leaving
     * the MCU in an inconsistent state that causes immediate HSE on RS=1.
     * Instead: just wait for CNR to clear and flush any sticky status bits
     * left by the firmware handoff. */
    {
        void *op = xhci_ctrl.op_regs;
        debug_print("[xHCI] USBSTS at entry: 0x%08x  USBCMD: 0x%08x\n",
                    readl(op + OP_USBSTS), readl(op + OP_USBCMD));

        /* xHCI spec 4.2: software must issue HCRST after power-on before
         * programming operational registers.  VC PERST# reloads VL805 firmware
         * via PCIe but HCRST is still required to initialise the xHCI register
         * block and signal the MCU to prepare its internal data structures.
         * Without HCRST the MCU fires HSE the instant RS=1 is written. */
        debug_print("[xHCI] Issuing HCRST...\n");
        if (do_reset() != 0) {
            debug_print("[xHCI] ERROR: HCRST timed out\n");
            return -1;
        }
        debug_print("[xHCI] HCRST complete. USBSTS=0x%08x\n",
                    readl(op + OP_USBSTS));

        /* Clear all port change bits (RW1C) and ensure PED=0 on all ports.
         * After HCRST some ports retain PED=1 and CSC=1 from connected devices.
         * On RS=1 the VL805 immediately tries to post Port Status Change Events
         * for any port with pending change bits — if anything goes wrong with
         * that first DMA write it fires HSE before any command is processed.
         * Pre-clearing change bits here prevents spurious events on startup. */
        {
            uint32_t nports = xhci_ctrl.max_ports;
            for (uint32_t p = 0; p < nports; p++) {
                uint32_t sc = readl(op + 0x400 + p * 0x10);
                debug_print("[xHCI] pre-clear PORTSC[%u]=0x%08x\n", p, sc);
                /* Write back: preserve PP, clear PED (write 0), clear all RW1C change bits */
                uint32_t clr = (sc & PORTSC_PP) | PORTSC_WIC;
                writel(clr, op + 0x400 + p * 0x10);
            }
            asm volatile("dsb sy; isb" ::: "memory");
            delay_ms(20);
            for (uint32_t p = 0; p < nports; p++) {
                uint32_t sc = readl(op + 0x400 + p * 0x10);
                debug_print("[xHCI] post-clear PORTSC[%u]=0x%08x\n", p, sc);
            }
        }
    }

    if (setup_dcbaa() != 0) return -1;
    if (setup_cmd_ring() != 0) return -1;
    if (setup_event_ring() != 0) return -1;
    setup_interrupter();

    if (run_controller() != 0) return -1;

    debug_print("[xHCI] Init complete\n");
    xhci_ctrl.initialized = 1;
    return 0;
}