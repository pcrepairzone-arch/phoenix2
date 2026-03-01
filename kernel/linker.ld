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

    /*
     * xHCI DMA structures: DCBAA, command ring, event ring, ERST.
     * MUST be in non-cacheable memory (Device memory in ARMv8 MMU terms).
     *
     * Two options:
     *  (a) Place this at a physical address covered by a Device memory
     *      mapping in mmu.c — simplest if you have a spare Device region.
     *  (b) Keep it in normal RAM but flush D-cache (dc civac) after every
     *      write to a DMA structure before the address is given to the
     *      xHCI controller — usb_xhci.c does this via flush_dcache().
     *
     * For now the section is placed in normal BSS-adjacent RAM and relies
     * on usb_xhci.c's flush_dcache() calls for correctness.  To make it
     * truly non-cacheable, remap this address range as Device memory in
     * mmu.c after __xhci_dma_start is defined.
     */
    . = ALIGN(4096);
    __xhci_dma_start = .;
    .xhci_dma : {
        /*
         * Explicitly reserve space for the xHCI DMA buffer (5120 bytes,
         * 64-byte aligned).  Using '. += size' here rather than a C static
         * array means the reservation is linker-owned and immune to GCC
         * placing zero-initialised data into .bss instead of this section.
         * usb_xhci.c accesses this region via extern __xhci_dma_start[].
         */
        . = ALIGN(64);
        *(.xhci_dma)          /* catch any explicit section()-attributed vars */
        . += 0x1400;          /* 5120 B minimum: DCBAA+cmd_ring+evt_ring+ERST+scratch */
    }
    __xhci_dma_end = .;
    . = ALIGN(4096);

    /* Kernel stack: 64 KB × 4 CPUs = 256 KB */
    . = ALIGN(4096);
    . = . + 0x40000;
    __kernel_stack_top = .;
}
