/*
 * linker.ld - Phoenix RISC OS kernel linker script
 * Target: AArch64, Raspberry Pi 4/5
 * Link/run address: 0x80000  (Pi 4 firmware relocates here; Pi 5 loads directly)
 *
 * Sections split so .text is RX-only and .data/.bss are RW.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(primary_cpu_entry)

SECTIONS
{
    /*
     * Pi 4 firmware loads kernel8.img to 0x200000 then relocates it to
     * 0x80000 before jumping in.  Pi 5 firmware loads directly to 0x80000.
     * Either way the kernel RUNS at 0x80000 — all symbols must be linked
     * here, otherwise static data (page tables, heap, stack) has wrong
     * addresses from the first instruction.
     */
    . = 0x80000;

    /* ── Read + Execute ───────────────────────────────────────────── */
    .text : {
        KEEP(*(.text.boot))   /* primary_cpu_entry must be first */
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)
    }

    /* ── Read + Write (no execute) ────────────────────────────────── */
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

    /*
     * .xhci_dma (NOLOAD) — xHCI DMA ring buffers + VL805 scratchpad pages.
     *
     * NOLOAD: space is reserved in the address map but NOT present in the
     * ELF/binary image — the section contributes zero bytes to kernel8.img.
     * mmu.c maps [__xhci_dma_start, __xhci_dma_end) as Device nGnRnE
     * (non-cacheable) so CPU writes are immediately visible to VL805 DMA.
     *
     * Size breakdown:
     *   0x0000 – 0x07FF  DCBAA  (64 slots × 8 bytes)
     *   0x0800 – 0x0BFF  cmd_ring  (64 TRBs × 16 bytes)
     *   0x0C00 – 0x0FFF  evt_ring  (64 TRBs × 16 bytes)
     *   0x1000 – 0x103F  ERST      (1 entry × 16 bytes)
     *   0x1040 – 0x117F  scratchpad pointer array (31 × 8 bytes)
     *   0x2000+          scratchpad pages (31 × 4096 bytes)
     *
     *   Total: 0x42000 = DCBAA + rings + ERST + arr + 64 × 4 KB pages
     */
    . = ALIGN(4096);
    .xhci_dma (NOLOAD) : {
        __xhci_dma_start = .;
        . = ALIGN(64);
        *(.xhci_dma)
        . += 0x42000;
        __xhci_dma_end = .;
    }
    . = ALIGN(4096);

    /* Kernel stack: 64 KB × 4 CPUs = 256 KB */
    . = ALIGN(4096);
    . = . + 0x40000;
    __kernel_stack_top = .;
}
