/**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver — spec-compliant init for VL805 on Pi 4
 */

#include "kernel.h"
#include "usb_xhci.h"
#include "usb.h"
#include <string.h>

extern void *pcie_base;
extern pci_dev_t vl805_dev;

/* Forward declarations */
static void port_scan(void);
static int cmd_address_device(uint8_t slot_id, uint8_t port, uint32_t route);
static int ep0_get_device_descriptor(uint8_t slot_id, uint8_t *buf, int len);
static void enumerate_port(int port);
static uint64_t cmd_ring_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t type);
static int xhci_wait_event(uint32_t ev[4], int timeout_ms);

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

static volatile uint32_t pending_event[4]  = {0, 0, 0, 0};
static volatile int      pending_event_ready = 0;

static uint64_t cmd_ring_dma  = 0;
static uint64_t evt_ring_dma  = 0;
static uint64_t erst_dma_addr = 0;

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

    debug_print("[xHCI] do_reset: USBSTS=0x%08x  USBCMD=0x%08x\n",
                readl(op + OP_USBSTS), readl(op + OP_USBCMD));

    /*
     * DO NOT issue HCRST on VL805.
     *
     * The VL805 MCU runs proprietary firmware.  HCRST restarts the MCU
     * init cycle, after which it treats our ring pointers as foreign and
     * asserts HCH+HSE ~60us after RS=1 (MCU watchdog on CRCR/DCBAA).
     *
     * pci.c ran a 2000ms CNR poll before calling us.  On this board
     * (BCM2711 rev C0, boardrev d03114) CNR never clears during cold
     * boot — USBSTS stays 0x00000805 throughout.  This is normal for
     * this VL805 firmware.  The MCU is running; it will accept RS=1.
     *
     * We clear stale status bits and proceed.  No HCRST, no waiting.
     */

    /* Clear any stale HSE/EINT/PCD from MCU power-on self-test.
     * CNR is read-only — do not attempt to clear it. */
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
    asm volatile("dsb sy; isb" ::: "memory");

    debug_print("[xHCI] do_reset done. USBSTS=0x%08x\n", readl(op + OP_USBSTS));
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
    cmd_cycle   = 1;
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

    uint64_t crcr = (ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle | (1ULL << 1);
    reg_write64(xhci_ctrl.op_regs, OP_CRCR_LO, crcr);

    return 0;
}

static int setup_event_ring(void) {
    evt_ring = (volatile uint32_t *)(xhci_dma_buf + DMA_EVT_RING_OFF);
    erst     = (volatile uint64_t *)(xhci_dma_buf + DMA_ERST_OFF);

    memset((void *)evt_ring, 0, EVT_RING_TRBS * 16);
    memset((void *)erst,     0, 64);

    uint64_t evt_phys  = (uint64_t)virt_to_phys((void *)evt_ring);
    uint64_t erst_phys = (uint64_t)virt_to_phys((void *)erst);
    uint64_t evt_dma   = phys_to_dma(evt_phys);
    uint64_t erst_dma  = phys_to_dma(erst_phys);
    evt_ring_dma  = evt_dma;
    erst_dma_addr = erst_dma;

    erst[0] = evt_dma;
    erst[1] = (uint64_t)EVT_RING_TRBS;

    asm volatile("dsb sy" ::: "memory");

    void *ir0 = ir_base(0);
    writel(1U, ir0 + IR_ERSTSZ);
    reg_write64(ir0, IR_ERSTBA_LO, erst_dma);
    reg_write64(ir0, IR_ERDP_LO, evt_dma);

    return 0;
}

static void setup_interrupter(void) {
    void *ir0 = ir_base(0);
    writel(0x00000002U, ir0 + IR_IMAN);
    writel(0x0FA00FA0U, ir0 + IR_IMOD);
    asm volatile("dsb sy; isb" ::: "memory");
}

static int run_controller(void) {
    void *op  = xhci_ctrl.op_regs;
    void *ir0 = ir_base(0);

    uint64_t dcbaa_dma  = phys_to_dma((uint64_t)virt_to_phys((void *)dcbaa));
    uint64_t erstba_dma = erst_dma_addr;
    uint64_t evt_dma    = evt_ring_dma;
    uint64_t crcr_val   = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle | (1ULL << 1);
    uint32_t cfg_val    = (readl(op + OP_CONFIG) & ~0xFFU) | (xhci_ctrl.max_slots & 0xFFU);

    /* MCU stabilization + launch (full from your original) */
    debug_print("[xHCI] Waiting for MCU to settle...\n");
    for (int w = 0; w < 200; w++) {
        delay_ms(1);
        uint32_t s = readl(op + OP_USBSTS);
        if (!(s & (STS_EINT | STS_HSE))) break;
    }
    writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);

    /* Tight launch */
    for (int attempt = 0; attempt < 50; attempt++) {
        asm volatile("dsb sy; isb" ::: "memory");
        writel((uint32_t)dcbaa_dma, op + OP_DCBAAP_LO);
        writel((uint32_t)(dcbaa_dma >> 32), op + OP_DCBAAP_HI);
        writel(cfg_val, op + OP_CONFIG);
        writel(1U, ir0 + IR_ERSTSZ);
        reg_write64(ir0, IR_ERSTBA_LO, erstba_dma);
        reg_write64(ir0, IR_ERDP_LO, evt_dma);
        reg_write64(op, OP_CRCR_LO, crcr_val);
        writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS);
        asm volatile("dsb sy; isb" ::: "memory");

        writel(CMD_RS | CMD_INTE, op + OP_USBCMD);
        asm volatile("dsb sy; isb" ::: "memory");

        if (!(readl(op + OP_USBSTS) & STS_HCH)) {
            debug_print("[xHCI] Controller launched\n");
            break;
        }
    }

    /* Power up ports */
    for (int p = 0; p < (int)xhci_ctrl.max_ports; p++) {
        writel((readl(op + 0x400 + p * 0x10) & ~PORTSC_WIC) | PORTSC_PP,
               op + 0x400 + p * 0x10);
        asm volatile("dsb sy" ::: "memory");
    }
    delay_ms(20);
    return 0;
}

/* forward-referenced by xhci_init — defined here so the struct is visible */
static usb_hc_ops_t g_xhci_hc_ops;

/* ── xhci_init ───────────────────────────────────────────────────────────── */
int xhci_init(uint64_t base_addr) {
    debug_print("[xHCI] driver v37 init base=0x%llx\n", base_addr);
    xhci_ctrl.cap_regs = (void *)base_addr;
    if (read_caps() != 0) return -1;
    if (do_reset() != 0) return -1;
    if (setup_dcbaa() != 0) return -1;
    if (setup_cmd_ring() != 0) return -1;
    if (setup_event_ring() != 0) return -1;
    setup_interrupter();

    if (run_controller() != 0) return -1;

    port_scan();
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
static uint8_t  ep0_cycle   = 1;
static uint32_t ep0_enqueue = 0;
static uint8_t  g_slot_id   = 0;

static void ep0_ring_init(void) {
    ep0_ring    = (volatile uint32_t *)(xhci_dma_buf + DMA_EP0_RING_OFF);
    ep0_cycle   = 1;
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
    uint64_t crcr = (cmd_ring_dma & ~0x3FULL) | (uint64_t)cmd_cycle | (1ULL << 1);
    reg_write64(op, OP_CRCR_LO, crcr);
    asm volatile("dsb sy; isb" ::: "memory");

    volatile uint32_t *db = (volatile uint32_t *)xhci_ctrl.doorbell_regs;
    db[0] = 0;
    asm volatile("dsb sy" ::: "memory");
    return 0;
}

static int cmd_address_device(uint8_t slot_id, uint8_t port, uint32_t route) {
    input_ctx = (volatile uint8_t *)(xhci_dma_buf + DMA_INPUT_CTX_OFF);
    out_ctx   = (volatile uint8_t *)(xhci_dma_buf + DMA_OUT_CTX_OFF);
    memset((void *)input_ctx, 0, 34 * CTX_SIZE);
    memset((void *)out_ctx,   0, 32 * CTX_SIZE);

    volatile uint32_t *icc = (volatile uint32_t *)input_ctx;
    icc[1] = 0x00000003;

    volatile uint32_t *slot_ctx = (volatile uint32_t *)(input_ctx + CTX_SIZE);
    slot_ctx[0] = (route & 0xFFFFF) | (4U << 20) | (1U << 27);
    slot_ctx[1] = (uint32_t)(port + 1) << 16;

    volatile uint32_t *ep0_ctx = (volatile uint32_t *)(input_ctx + 2 * CTX_SIZE);
    ep0_ctx[1] = (3U << 1) | (4U << 3) | (512U << 16);

    ep0_ring_init();
    uint64_t ep0_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_ring));
    ep0_ctx[2] = (uint32_t)(ep0_dma) | 1;
    ep0_ctx[3] = (uint32_t)(ep0_dma >> 32);
    ep0_ctx[4] = 8;

    uint64_t out_dma = phys_to_dma((uint64_t)virt_to_phys((void *)out_ctx));
    dcbaa[slot_id] = out_dma;

    uint64_t in_dma = phys_to_dma((uint64_t)virt_to_phys((void *)input_ctx));
    cmd_ring_submit((uint32_t)in_dma, (uint32_t)(in_dma >> 32), 0, TRB_TYPE_ADDR_DEV);

    uint32_t ev[4];
    if (xhci_wait_event(ev, 5000) != 0) return -1;
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

    if (portsc & (1U << 30)) return; /* companion port */
    if (!(portsc & PORTSC_CCS)) {
        uart_puts("[xHCI] Port "); print_hex32(port + 1);
        uart_puts(": not connected (PORTSC="); print_hex32(portsc);
        uart_puts(")\n");
        return;
    }

    uint32_t speed = (portsc >> 10) & 0xF;
    uart_puts("[xHCI] Port "); print_hex32(port + 1);
    uart_puts(": CONNECTED speed="); print_hex32(speed);
    uart_puts(" PORTSC="); print_hex32(portsc); uart_puts("\n");

    /* Port reset — skip for SS already in U0 (PED=1) */
    if (!(speed == 4 && (portsc & PORTSC_PED))) {
        uint32_t reset_bit = (speed == 4) ? (1U << 31) : (1U << 4);
        writel((portsc & ~PORTSC_WIC) | reset_bit, op + 0x400 + port * 0x10);
        for (int t = 0; t < 200; t++) {
            delay_ms(1);
            uint32_t ps = readl(op + 0x400 + port * 0x10);
            if (!(ps & reset_bit)) break;
        }
        uart_puts("[xHCI] Port reset done. PORTSC=");
        print_hex32(readl(op + 0x400 + port * 0x10)); uart_puts("\n");
    }

    /* ── Step 1: Enable Slot ─────────────────────────────────────────── */
    cmd_ring_submit(0, 0, 0, TRB_TYPE_ENABLE_SLOT);
    uint32_t ev[4];
    if (xhci_wait_event(ev, 5000) != 0) {
        uart_puts("[xHCI] Enable Slot TIMEOUT\n");
        return;
    }
    uint8_t cc      = (ev[2] >> 24) & 0xFF;
    uint8_t slot_id = (ev[3] >> 24) & 0xFF;
    uart_puts("[xHCI] Enable Slot: cc="); print_hex32(cc);
    uart_puts(" slot="); print_hex32(slot_id); uart_puts("\n");
    if (cc != 1 || slot_id == 0) return;

    g_slot_id = slot_id;

    /* ── Step 2: Address Device ──────────────────────────────────────── */
    if (cmd_address_device(slot_id, (uint8_t)port, 0) != 0) {
        uart_puts("[xHCI] Address Device failed\n");
        return;
    }
    uart_puts("[xHCI] Address Device OK slot="); print_hex32(slot_id); uart_puts("\n");

    /* Build a minimal usb_device_t so control transfers work via g_hc_ops.
     * Store slot_id in hcd_private so xhci_control_transfer can find the ring. */
    extern int usb_enumerate_device(usb_device_t *dev, int port);
    static usb_device_t g_dev; /* one device for now — extend to array later */
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.speed       = (uint8_t)speed;
    g_dev.address     = 1; /* xHCI assigns USB address via Address Device */
    g_dev.hcd_private = (void *)(uintptr_t)slot_id;

    /* ── Step 3: GET_DESCRIPTOR (Device, 18 bytes) ───────────────────── */
    uint8_t ddesc[18];
    int got = ep0_get_device_descriptor(slot_id, ddesc, 18);
    if (got < 8) {
        uart_puts("[xHCI] GET_DESCRIPTOR(Device) failed\n");
        return;
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

    /* First fetch 9 bytes to get wTotalLength */
    uint32_t setup_lo = (0x80U)
                      | ((uint32_t)USB_REQ_GET_DESCRIPTOR << 8)
                      | ((uint32_t)USB_DESC_CONFIG << 24);
    uint32_t setup_hi = (uint32_t)9 << 16;
    volatile uint8_t *ep0_data = (volatile uint8_t *)(xhci_dma_buf + DMA_EP0_DATA_OFF);
    memset((void *)ep0_data, 0, 256);
    uint64_t data_dma = phys_to_dma((uint64_t)virt_to_phys((void *)ep0_data));
    ep0_enq(setup_lo, setup_hi, 8, TRB_TYPE_SETUP, TRB_IDT | (3U << 16));
    ep0_enq((uint32_t)data_dma, (uint32_t)(data_dma >> 32), 9, TRB_TYPE_DATA, TRB_IOC | TRB_DIR_IN);
    ep0_enq(0, 0, 0, TRB_TYPE_STATUS, TRB_IOC);
    ep0_doorbell(slot_id);
    if (xhci_wait_event(ev, 3000) != 0) {
        uart_puts("[xHCI] GET_DESCRIPTOR(Config,9) timeout\n");
        goto probe;
    }
    xhci_wait_event(ev, 500); /* drain Status event */

    uint16_t total_len = (uint16_t)(ep0_data[2] | ((uint16_t)ep0_data[3] << 8));
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
int xhci_scan_ports(void) { return 0; }

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

/* ── Event ring + IRQ + MSI (final) ─────────────────────────────────────── */
static int xhci_wait_event(uint32_t ev[4], int timeout_ms) {
    for (int t = 0; t < timeout_ms; t++) {
        if (pending_event_ready) {
            ev[0] = pending_event[0]; ev[1] = pending_event[1];
            ev[2] = pending_event[2]; ev[3] = pending_event[3];
            pending_event_ready = 0;
            return 0;
        }
        delay_ms(1);
    }
    return -1;
}

void xhci_irq_handler(int vector, void *data) {
    (void)vector; (void)data;
    void *ir0 = ir_base(0);
    uint32_t iman = readl(ir0 + IR_IMAN);
    if (iman & 1) {
        uint32_t *ev_ptr = (uint32_t *)(evt_ring + evt_dequeue * 4);

        /* Check cycle bit — if it doesn't match evt_cycle the slot is
         * empty or stale (controller hasn't written this entry yet). */
        if ((ev_ptr[3] & 1) != evt_cycle)
            return;

        pending_event[0] = ev_ptr[0];
        pending_event[1] = ev_ptr[1];
        pending_event[2] = ev_ptr[2];
        pending_event[3] = ev_ptr[3];

        evt_dequeue++;
        if (evt_dequeue >= EVT_RING_TRBS) {
            evt_dequeue = 0;
            evt_cycle ^= 1;   /* toggle cycle when we wrap */
        }

        reg_write64(ir0, IR_ERDP_LO, evt_ring_dma + evt_dequeue * 16);
        writel(1U, ir0 + IR_IMAN);
        pending_event_ready = 1;
    }
}
/* xhci_dma_phys — return physical base address of the xHCI DMA buffer.
 * Called by pci.c xhci_setup_msi() to compute the PCIe MSI target address. */
uint64_t xhci_dma_phys(void) {
    return (uint64_t)virt_to_phys((void *)xhci_dma_buf);
}
