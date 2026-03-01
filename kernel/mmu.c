/*
 * mmu.c – AArch64 MMU for RISC OS Phoenix
 *
 * Design: flat identity map, MMU + caches on, no per-task page tables yet.
 *
 * ARM architecture note (DDI0487, Table D5-11):
 *   With 4 KB granule and T0SZ = 25 (39-bit VA), the translation walk
 *   starts at LEVEL 1.  TTBR0 must point directly to a 512-entry L1
 *   table where each entry is a 1 GB block descriptor.
 *   Adding an extra L0→L1 indirection causes an immediate translation
 *   fault when the MMU is enabled — that was the previous hang.
 *
 * Pi 4 memory map (physical):
 *   0x000000000 –  0x0FFFFFFFF  4 GB RAM          Normal WB
 *   0x0C0000000 –  0x0EFFFFFFF  (within 4th GB)   Normal WB
 *   0x0F0000000 –  0x0FFFFFFFF  peripherals zone   Device nGnRnE
 *     └ Pi 4 UART/GPIO/mailbox base: 0xFE000000
 *   0x600000000                 Pi 4 VL805 BAR0    Device nGnRnE  (L1 index 24)
 *
 * Pi 5 additions:
 *   0x107C000000               Pi 5 peripherals    Device nGnRnE  (L1 index 65)
 *   0x1F00000000               Pi 5 PCIe RC        Device nGnRnE  (L1 index 124)
 *
 * MAIR_EL1:
 *   index 0 = 0xFF  Normal memory (WB/WA, inner+outer)
 *   index 1 = 0x00  Device nGnRnE
 *
 * TCR_EL1:
 *   T0SZ = 25  → 39-bit VA, initial walk at L1
 *   IPS  = 010 → 40-bit physical address (covers 0x600000000)
 *   4 KB granules, WB+WA cacheable walks, inner-shareable
 */

#include "kernel.h"
#include "spinlock.h"
#include "errno.h"
#include "error.h"
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define PAGE_SHIFT   12
#ifndef PAGE_SIZE
#define PAGE_SIZE    (1ULL << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK    (~(PAGE_SIZE - 1ULL))
#endif
#define PT_ENTRIES   512
#define GB           (1ULL << 30)

/* AArch64 block/page descriptor bits */
#define PTE_VALID       (1ULL << 0)
#define PTE_BLOCK       (0ULL << 1)   /* L1/L2 block entry          */
#define PTE_TABLE       (1ULL << 1)   /* table pointer              */
#define PTE_PAGE        (1ULL << 1)   /* L3 page entry              */
#define PTE_ATTRINDX(n) ((uint64_t)(n) << 2)
#define PTE_AP_RW       (0ULL << 6)
#define PTE_SH_INNER    (3ULL << 8)
#define PTE_SH_OUTER    (2ULL << 8)
#define PTE_AF          (1ULL << 10)
#define PTE_PXN         (1ULL << 53)
#define PTE_UXN         (1ULL << 54)

/* MAIR indices */
#define MAIR_NORMAL  0   /* Normal WB cacheable */
#define MAIR_DEVICE  1   /* Device nGnRnE       */

#define MAIR_VALUE   ((0xFFULL << (MAIR_NORMAL * 8)) | \
                      (0x00ULL << (MAIR_DEVICE * 8)))

/* TCR_EL1 */
#define TCR_T0SZ    (25ULL << 0)    /* 39-bit VA, walk starts at L1 */
#define TCR_IRGN0   (1ULL  << 8)    /* inner WB+WA                 */
#define TCR_ORGN0   (1ULL  << 10)   /* outer WB+WA                 */
#define TCR_SH0     (3ULL  << 12)   /* inner-shareable             */
#define TCR_TG0     (0ULL  << 14)   /* 4 KB granule                */
#define TCR_T1SZ    (25ULL << 16)   /* 39-bit kernel VA (unused)   */
#define TCR_IRGN1   (1ULL  << 24)
#define TCR_ORGN1   (1ULL  << 26)
#define TCR_SH1     (3ULL  << 28)
#define TCR_TG1     (2ULL  << 30)   /* 4 KB granule                */
#define TCR_IPS     (2ULL  << 32)   /* 40-bit PA (1 TB)            */
#define TCR_VALUE   (TCR_T0SZ | TCR_IRGN0 | TCR_ORGN0 | TCR_SH0 | TCR_TG0 | \
                     TCR_T1SZ | TCR_IRGN1 | TCR_ORGN1 | TCR_SH1 | TCR_TG1 | \
                     TCR_IPS)

/* Protection flags (public API) */
#define PROT_NONE    0
#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_EXEC    4

/* ── Static page tables ──────────────────────────────────────────── */

/*
 * l1_table: the SINGLE table TTBR0 points to.
 *   With T0SZ=25, each entry covers 1 GB.  512 entries × 1 GB = 512 GB total.
 *   All blocks are identity-mapped (VA == PA).
 */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

/*
 * l2_periph4: splits the 3rd GB (0xC0000000–0xFFFFFFFF) into
 *   2 MB blocks so we can mark 0xF0000000+ as Device.
 *   Pi 4 peripherals sit at 0xFE000000 — inside this region.
 */
static uint64_t l2_periph4[512] __attribute__((aligned(4096)));

/*
 * l2_first_gb: splits the FIRST 1 GB (l1_table[0]) into 512 × 2 MB blocks.
 *   Required so we can carve out a single 2 MB region containing the
 *   .xhci_dma section and further split it into 4 KB pages.
 *   All entries default to Normal WB; the entry covering .xhci_dma is
 *   replaced with a TABLE pointer to l3_dma_2mb below.
 */
static uint64_t l2_first_gb[512] __attribute__((aligned(4096)));

/*
 * l3_dma_2mb: splits the 2 MB block that contains .xhci_dma into
 *   512 × 4 KB pages.  Pages inside [__xhci_dma_start, __xhci_dma_end)
 *   are marked Device nGnRnE (non-cacheable); all others are Normal WB.
 *
 *   This ensures the xHCI DMA ring buffers (DCBAA, command ring, event
 *   ring, ERST) require NO explicit cache maintenance — the hardware
 *   observes writes immediately without D-cache involvement.
 */
static uint64_t l3_dma_2mb[512] __attribute__((aligned(4096)));

/*
 * Linker-exported symbols bracketing the .xhci_dma section.
 * Declared as zero-length arrays so taking their address gives the PA/VA.
 */
extern char __xhci_dma_start[];
extern char __xhci_dma_end[];

/* ── mmu_init ────────────────────────────────────────────────────── */

void mmu_init(void)
{
    debug_print("[MMU] Building identity page tables...\n");

    /* Zero all tables */
    for (int i = 0; i < 512; i++) {
        l1_table[i]   = 0;
        l2_periph4[i] = 0;
        l2_first_gb[i] = 0;
        l3_dma_2mb[i]  = 0;
    }

    /* ── Descriptor helpers ──────────────────────────────────────── */

    /* 1 GB L1 block descriptor (Normal WB) */
#define L1_NORMAL(pa)  ((pa) | PTE_VALID | PTE_BLOCK | PTE_AF | \
                         PTE_SH_INNER | PTE_AP_RW | PTE_ATTRINDX(MAIR_NORMAL))
    /* 1 GB L1 block descriptor (Device nGnRnE) */
#define L1_DEVICE(pa)  ((pa) | PTE_VALID | PTE_BLOCK | PTE_AF | \
                         PTE_SH_OUTER | PTE_AP_RW | PTE_ATTRINDX(MAIR_DEVICE) | \
                         PTE_PXN | PTE_UXN)
    /* 2 MB L2 block descriptor (Normal WB) */
#define L2_NORMAL(pa)  ((pa) | PTE_VALID | PTE_BLOCK | PTE_AF | \
                         PTE_SH_INNER | PTE_AP_RW | PTE_ATTRINDX(MAIR_NORMAL))
    /* 2 MB L2 block descriptor (Device nGnRnE) */
#define L2_DEVICE(pa)  ((pa) | PTE_VALID | PTE_BLOCK | PTE_AF | \
                         PTE_SH_OUTER | PTE_AP_RW | PTE_ATTRINDX(MAIR_DEVICE) | \
                         PTE_PXN | PTE_UXN)
    /* 4 KB L3 page descriptor (Normal WB) */
#define L3_NORMAL(pa)  ((pa) | PTE_VALID | PTE_PAGE | PTE_AF | \
                         PTE_SH_INNER | PTE_AP_RW | PTE_ATTRINDX(MAIR_NORMAL))
    /* 4 KB L3 page descriptor (Device nGnRnE) */
#define L3_DEVICE(pa)  ((pa) | PTE_VALID | PTE_PAGE | PTE_AF | \
                         PTE_SH_OUTER | PTE_AP_RW | PTE_ATTRINDX(MAIR_DEVICE) | \
                         PTE_PXN | PTE_UXN)
    /* L1/L2 table (pointer-to-next-level) descriptor */
#define TABLE_DESC(pa) ((pa) | PTE_VALID | PTE_TABLE)

    /* ── .xhci_dma location ──────────────────────────────────────── */

    /*
     * Identity map means VA == PA.  Get the physical extent of the
     * .xhci_dma section from the linker-exported symbols.
     */
    uint64_t dma_start = (uint64_t)__xhci_dma_start;
    uint64_t dma_end   = (uint64_t)__xhci_dma_end;

    /*
     * All DMA memory must be within the first 1 GB and within a single
     * 2 MB L2 block, so we only need one L3 table.  Panic if violated.
     */
    if ((dma_start >> 30) != 0 || (dma_end >> 30) != 0) {
        debug_print("[MMU] PANIC: .xhci_dma spans beyond first 1 GB "
                    "(start=0x%llx end=0x%llx)\n",
                    (unsigned long long)dma_start,
                    (unsigned long long)dma_end);
        for (;;) {}
    }
    uint64_t dma_l2_idx = (dma_start >> 21) & 0x1FF;
    if (((dma_end - 1) >> 21) != dma_l2_idx) {
        debug_print("[MMU] PANIC: .xhci_dma crosses 2 MB boundary "
                    "(start=0x%llx end=0x%llx)\n",
                    (unsigned long long)dma_start,
                    (unsigned long long)dma_end);
        for (;;) {}
    }

    debug_print("[MMU] .xhci_dma: 0x%llx – 0x%llx  (L2[%llu], Device pages)\n",
                (unsigned long long)dma_start,
                (unsigned long long)dma_end,
                (unsigned long long)dma_l2_idx);

    /* ── Build l3_dma_2mb: 4 KB pages for the 2 MB block holding DMA buf ── */

    uint64_t l2_block_base = dma_l2_idx << 21;   /* PA of first byte in this 2MB */
    uint64_t dma_l3_first  = (dma_start >> 12) & 0x1FF;
    /* Round dma_end up to next page, then compute index within this 2MB block.
     * Cap at 512 in case dma_end is exactly 2MB-aligned (& 0x1FF would wrap to 0). */
    uint64_t dma_end_page  = (dma_end + 0xFFFULL) & ~0xFFFULL;  /* ceil to 4KB */
    uint64_t dma_l3_last   = (dma_end_page - l2_block_base) >> 12;
    if (dma_l3_last > 512) dma_l3_last = 512;

    for (int i = 0; i < 512; i++) {
        uint64_t pa = l2_block_base + ((uint64_t)i << 12);
        if ((uint64_t)i >= dma_l3_first && (uint64_t)i < dma_l3_last)
            l3_dma_2mb[i] = L3_DEVICE(pa);   /* DMA pages — non-cacheable */
        else
            l3_dma_2mb[i] = L3_NORMAL(pa);   /* Everything else — Normal WB */
    }
    debug_print("[MMU] L3 DMA pages: entries %llu–%llu marked Device\n",
                (unsigned long long)dma_l3_first,
                (unsigned long long)dma_l3_last - 1);

    /* ── Build l2_first_gb: 2 MB blocks for the first 1 GB ─────── */

    for (int i = 0; i < 512; i++) {
        uint64_t pa = (uint64_t)i << 21;   /* 2 MB each, starting at 0 */
        if ((uint64_t)i == dma_l2_idx)
            /* Replace with table pointer to the L3 table we just built */
            l2_first_gb[i] = TABLE_DESC((uint64_t)l3_dma_2mb);
        else
            l2_first_gb[i] = L2_NORMAL(pa);
    }

    /* ── L1 table ────────────────────────────────────────────────── */

    /*
     * Index 0: first 1 GB — now a TABLE pointer (so we can mark .xhci_dma
     *           as Device at 4 KB granularity via l2_first_gb).
     */
    l1_table[0] = TABLE_DESC((uint64_t)l2_first_gb);

    /* 1 GB: 0x040000000 – 0x07FFFFFFF  Normal WB (RAM) */
    l1_table[1] = L1_NORMAL(0x040000000ULL);

    /* 2 GB: 0x080000000 – 0x0BFFFFFFF  Normal WB (RAM) */
    l1_table[2] = L1_NORMAL(0x080000000ULL);

    /*
     * 3 GB: 0x0C0000000 – 0x0FFFFFFFF
     * Split via l2_periph4 so the Pi 4 peripheral window (0xFE000000+)
     * gets Device attributes.  Everything below 0xF0000000 stays Normal.
     */
    l1_table[3] = TABLE_DESC((uint64_t)l2_periph4);

    for (int i = 0; i < 512; i++) {
        uint64_t pa = 0xC0000000ULL + ((uint64_t)i << 21);
        if (pa >= 0xF0000000ULL)
            l2_periph4[i] = L2_DEVICE(pa);
        else
            l2_periph4[i] = L2_NORMAL(pa);
    }

    /* ── Pi 4: VL805 PCIe BAR0 @ 0x600000000  (index 24) ────────── */
    l1_table[24] = L1_DEVICE(0x600000000ULL);

    /* ── Pi 5: peripherals @ 0x107C000000  (index 65) ───────────── */
    l1_table[65] = L1_DEVICE(0x107C000000ULL);

    /* ── Pi 5: PCIe RC @ 0x1F00000000  (index 124) ──────────────── */
    l1_table[124] = L1_DEVICE(0x1F00000000ULL);

#undef L1_NORMAL
#undef L1_DEVICE
#undef L2_NORMAL
#undef L2_DEVICE
#undef L3_NORMAL
#undef L3_DEVICE
#undef TABLE_DESC

    debug_print("[MMU] Tables ready, enabling MMU + caches...\n");

    /* Drain all data writes before loading page tables */
    asm volatile("dsb ishst; isb" ::: "memory");

    uint64_t ttbr  = (uint64_t)l1_table;
    uint64_t tcr   = TCR_VALUE;
    uint64_t mair  = MAIR_VALUE;
    uint64_t sctlr;

    asm volatile(
        /*
         * 1. Program MAIR and TCR first so the hardware knows the
         *    memory type encodings before we load TTBR.
         */
        "msr mair_el1, %[mair]\n"
        "msr tcr_el1,  %[tcr]\n"
        "isb\n"

        /*
         * 2. Load TTBR0 (and TTBR1 with the same table — they share
         *    the same identity map so kernel addresses work too).
         */
        "msr ttbr0_el1, %[ttbr]\n"
        "msr ttbr1_el1, %[ttbr]\n"
        "isb\n"

        /*
         * 3. Invalidate all TLB entries before the MMU is switched on
         *    so there are no stale entries from the firmware.
         */
        "tlbi vmalle1is\n"
        "dsb ish\n"
        "isb\n"

        /*
         * 4. Enable MMU (M), D-cache (C), and I-cache (I) in one write.
         *    Read-modify-write so we don't disturb other SCTLR bits set
         *    by the firmware (e.g. alignment check, SP alignment).
         */
        "mrs  %[sctlr], sctlr_el1\n"
        "orr  %[sctlr], %[sctlr], #(1 << 0)\n"   /* M – MMU on    */
        "orr  %[sctlr], %[sctlr], #(1 << 2)\n"   /* C – D-cache   */
        "orr  %[sctlr], %[sctlr], #(1 << 12)\n"  /* I – I-cache   */
        "msr  sctlr_el1, %[sctlr]\n"
        "isb\n"                                    /* pipeline flush */

        : [sctlr] "=&r" (sctlr)
        : [ttbr]  "r"   (ttbr),
          [tcr]   "r"   (tcr),
          [mair]  "r"   (mair)
        : "memory"
    );

    debug_print("[MMU] Enabled (identity map, caches on)\n");
}

/* ── Stubs — future per-task page table support ──────────────────── */

void mmu_init_task(task_t *task)         { (void)task; }
int  mmu_map(task_t *task, uint64_t virt, uint64_t size, int prot, int guard)
     { (void)task; (void)virt; (void)size; (void)prot; (void)guard; return 0; }
int  mmu_duplicate_pagetable(task_t *p, task_t *c) { (void)p; (void)c; return 0; }
void mmu_map_kernel(uint64_t v, uint64_t s, int p)  { (void)v; (void)s; (void)p; }
void page_ref_inc(uint64_t phys)                     { (void)phys; }

void mmu_tlb_invalidate_all(void) {
    asm volatile("tlbi vmalle1is\ndsb ish\nisb" ::: "memory");
}

void mmu_tlb_invalidate_addr(uint64_t virt, uint64_t size) {
    (void)size;
    asm volatile("tlbi vae1is, %0\ndsb ish\nisb" :: "r"(virt >> 12) : "memory");
}
