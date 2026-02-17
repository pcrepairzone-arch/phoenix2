/* 
 * linker.ld - Linker script for RISC OS Phoenix kernel
 * Target: ARM64 (AArch64) for Raspberry Pi 5
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(primary_cpu_entry)

SECTIONS
{
    /* Kernel loads at 0x80000 on Pi */
    . = 0x80000;
    
    .text : {
        *(.text.boot)
        *(.text*)
    }
    
    .rodata : {
        *(.rodata*)
    }
    
    .data : {
        *(.data*)
    }
    
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }
    
    /* Stack at end */
    . = ALIGN(16);
    . = . + 0x100000; /* 1MB stack */
    __kernel_stack_top = .;
    
    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
