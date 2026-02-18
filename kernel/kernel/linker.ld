/*
 * linker.ld - Phoenix RISC OS kernel linker script
 * Target: AArch64, Raspberry Pi 4
 * Load: 0x80000 (standard Pi ARM64 kernel address)
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(primary_cpu_entry)

SECTIONS
{
    . = 0x80000;

    /* Boot entry must be at exactly 0x80000 */
    .text : {
        KEEP(*(.text.boot))   /* primary_cpu_entry first */
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : { *(.rodata*) }

    . = ALIGN(4096);
    .data : {
        *(.data*)             /* mbox[] buffer lives here */
    }

    . = ALIGN(4096);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* Stacks: 64KB × 4 CPUs, immediately after BSS */
    . = ALIGN(4096);
    . = . + 0x40000;          /* 4 × 64KB */
    __kernel_stack_top = .;   /* top = high address, stack grows down */
}
