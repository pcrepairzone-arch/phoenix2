/*
 * linker.ld - Phoenix RISC OS kernel linker script
 * Target: AArch64, Raspberry Pi 4/5
 * Link/run address: 0x80000  (Pi 4 firmware relocates here; Pi 5 loads directly)
 *
 * Sections are split so .text is RX-only and .data/.bss are RW,
 * eliminating the RWX LOAD segment warning.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(primary_cpu_entry)

SECTIONS
{
    /*
     * xHCI DMA structures at a FIXED LOW ADDRESS — 64 KB physical.
     *
     * BOOT 50 FIX: previously landed at 0x080b8000 (128 MB) because the
     * 128 MB static BSS heap in malloc.c pushed __bss_end to ~128 MB and
     * this section followed immediately after BSS.  Despite RC_BAR2=0x13
     * covering [0, 1 GB), the VL805 MCU never wrote its CCE there.
     *
     * Placing the section here (0x00010000–0x00052000) puts all DMA
     * addresses well below 1 MB, definitively inside any inbound PCIe
     * window and free of any BCM2711 RC address-decode edge cases.
     *
     * Layout:
     *   0x00010000  DCBAA           (DMA_DCBAA_OFF    = 0x0000)
     *   0x00010800  cmd_ring        (DMA_CMD_RING_OFF  = 0x0800)
     *   0x00010C00  evt_ring        (DMA_EVT_RING_OFF  = 0x0C00)
     *   0x00011000  MSI landing pad (DMA_MSI_PAGE_OFF  = 0x1000)
     *   0x00011040  ERST            (DMA_ERST_OFF      = 0x1040)
     *   0x00012000  scratchpad pages(DMA_SCRATCH_PAGES_OFF=0x2000)
     *
     * mmu.c reads __xhci_dma_start / __xhci_dma_end and maps the range
     * as Normal Non-Cacheable pages automatically — no manual update needed.
     *
     * Constraints satisfied:
     *   (1) 0x00010000 >> 30 == 0   → within first 1 GB ✓
     *   (2) 0x00010000 >> 21 == 0x00051FFF >> 21 == 0   → single 2 MB block ✓
     *
     * IMPORTANT: this section sits BELOW .text (0x80000).  The Pi 4
     * firmware never touches [0x10000, 0x52000]; the region is free RAM
     * from reset.  BSS zero-clearing in boot.S uses __bss_start/__bss_end
     * which are in the kernel body above 0x80000 — xhci_dma is excluded.
     */
    . = 0x00010000;
    __xhci_dma_start = .;
    .xhci_dma : {
        . = ALIGN(64);
        *(.xhci_dma)          /* catch any explicit section()-attributed vars */
        . += 0x42000;         /* DCBAA+rings+ERST+scratch_arr+ctx+ep0+scratch_pages(64×4KB) */
    }
    __xhci_dma_end = .;

    /*
     * Pi 4 firmware loads kernel8.img to 0x200000 then relocates it to
     * 0x80000 before jumping in.  Pi 5 firmware loads directly to 0x80000.
     * Either way the kernel RUNS at 0x80000, so all symbols must be linked
     * there — otherwise static data (page tables, heap, stack) has wrong
     * addresses from the first instruction.
     */
    . = 0x80000;

    /* ---- Read + Execute ------------------------------------------ */
    .text : {
        KEEP(*(.text.boot))   /* primary_cpu_entry must be first */
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)
    }

    /* ---- Read + Write (no execute) ------------------------------- */
    . = ALIGN(4096);
    .data : {
        *(.data*)
    }

    . = ALIGN(4096);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* Kernel stack: 64 KB × 4 CPUs = 256 KB */
    . = ALIGN(4096);
    . = . + 0x40000;
    __kernel_stack_top = .;
}
