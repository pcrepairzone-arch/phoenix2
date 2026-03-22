/*
 * mailbox.c - VideoCore mailbox for Phoenix RISC OS
 *
 * GPU cache alias differs between boards:
 *   Pi 4 (BCM2711):  ARM→GPU alias = 0x40000000
 *   Pi 5 (BCM2712):  ARM→GPU alias = 0xC0000000  (VC6 / RPIVID)
 *
 * We read pi_model (set by periph_base.c) to pick the right alias.
 *
 * DMA CACHE COHERENCY NOTE (BCM2711 / Pi 4):
 *   The kernel runs with MMU + D-cache enabled (Normal WB, mmu.c).
 *   The VideoCore reads SDRAM directly and cannot snoop the ARM L2.
 *   Without explicit cache maintenance the VC sees stale zeros (our
 *   dirty buffer never reached DRAM) and the ARM reads the stale
 *   pre-call value of buf[1] (VC's response never reaches cache).
 *   Both failures manifest as mbox: code=0x0.
 *
 *   Fix — two-phase cache maintenance in mbox_call():
 *     Phase 1 (before TX): DC CVAC  — clean dirty lines to DRAM so
 *                           the VC can see our request.  Marks lines
 *                           Clean (no dirty bit) in the ARM cache.
 *     Phase 2 (after RX): DC CIVAC — since lines are Clean (from
 *                           phase 1), the Clean step is a no-op;
 *                           only the Invalidate fires, forcing the
 *                           next read of buf[] to fetch from DRAM
 *                           where the VC wrote its response.
 */
#include "kernel.h"
#include "mailbox.h"

extern uint64_t get_mailbox_base(void);
extern int      get_pi_model(void);
extern void     led_signal_hang(void);

static inline void mb(void) {
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

/*
 * dcache_clean_range — DC CVAC on every cache line touching [start, start+len).
 * Cleans dirty lines to PoC (DRAM) without invalidating.
 * Use BEFORE sending a buffer to a DMA master (VC/GPU) that reads DRAM.
 */
static void dcache_clean_range(const void *start, unsigned int len)
{
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)63;
    uintptr_t e = (uintptr_t)start + len;
    for (; a < e; a += 64)
        asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy\nisb" ::: "memory");
}

/*
 * dcache_inval_range — DC CIVAC on every cache line touching [start, start+len).
 * Cleans (no-op if already clean) and invalidates, forcing a fresh DRAM fetch.
 * Use AFTER a DMA master (VC/GPU) has written its response to DRAM.
 */
static void dcache_inval_range(const void *start, unsigned int len)
{
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)63;
    uintptr_t e = (uintptr_t)start + len;
    for (; a < e; a += 64)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy\nisb" ::: "memory");
}

int mbox_call(volatile uint32_t *buf)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *mb_status = (volatile uint32_t *)(mbox_base + 0x18);
    volatile uint32_t *mb_read   = (volatile uint32_t *)(mbox_base + 0x00);
    volatile uint32_t *mb_write  = (volatile uint32_t *)(mbox_base + 0x20);

    uint32_t phys = (uint32_t)(uint64_t)buf;

    /*
     * GPU cache alias:
     *   Pi 4: 0x40000000  (L2 cache coherent DMA alias)
     *   Pi 5: 0xC0000000  (VC6 coherent alias)
     */
    uint32_t alias = (get_pi_model() == 5) ? 0xC0000000U : 0x40000000U;
    uint32_t msg   = ((phys & 0x0FFFFFFFU) | alias) | 8;

    debug_print("mbox: phys=0x%p gpu=0x%x\n", (void*)(uint64_t)phys, msg);

    /*
     * Phase 1: clean our request buffer to DRAM so the VC can see it.
     * buf[0] holds the total message size in bytes (set by the caller).
     * We flush that many bytes, clamped to the max mbox_simple buffer
     * size of 128 bytes (32 × u32).  Flushing a few extra bytes is
     * harmless; it only touches the same 1–2 cache lines.
     */
    {
        unsigned int sz = buf[0];
        if (sz < 4 || sz > 128) sz = 128;
        dcache_clean_range((const void *)(uintptr_t)buf, sz);
    }

    mb();

    /* Wait TX ready */
    int timeout = 1000000;
    while (*mb_status & 0x80000000U) {
        if (--timeout <= 0) {
            debug_print("mbox: TX timeout\n");
            return -1;
        }
        mb();
    }

    mb();
    *mb_write = msg;
    mb();

    /* Wait RX */
    timeout = 2000000;
    while (1) {
        int inner = 1000000;
        while (*mb_status & 0x40000000U) {
            if (--inner <= 0 || --timeout <= 0) {
                debug_print("mbox: RX timeout\n");
                return -1;
            }
            mb();
        }
        if (timeout <= 0) break;

        mb();
        uint32_t r = *mb_read;
        mb();

        if ((r & 0xF) == 8) {
            mb();
            /*
             * Phase 2: invalidate so the CPU re-fetches the VC's
             * response from DRAM instead of reading our stale cache.
             * After phase 1 the lines are Clean; the DC CIVAC clean
             * step is therefore a no-op, and only the invalidate fires.
             */
            dcache_inval_range((const void *)(uintptr_t)buf, 128);

            uint32_t code = buf[1];
            debug_print("mbox: code=0x%x\n", code);
            return (code == 0x80000000U) ? 0 : -1;
        }
    }

    debug_print("mbox: no response\n");
    return -1;
}
