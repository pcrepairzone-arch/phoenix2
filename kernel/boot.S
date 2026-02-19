.section ".text.boot"
.global _start
.global primary_cpu_entry

_start:
primary_cpu_entry:
    mov     x19, x0                     /* Save DTB */

    /* Park secondary CPUs */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, 1f
0:  wfe
    b       0b
1:
    /* Check EL, drop to EL1 */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    el1_mode
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1
    isb
    msr     sctlr_el1, xzr
    isb
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    mov     x1, #0x33FF
    msr     cptr_el2, x1
    mov     x1, #0x3C5
    msr     spsr_el2, x1
    adr     x1, el1_mode
    msr     elr_el2, x1
    adrp    x1, __kernel_stack_top
    add     x1, x1, :lo12:__kernel_stack_top
    msr     sp_el1, x1
    eret

el1_mode:
    adrp    x1, __kernel_stack_top
    add     x1, x1, :lo12:__kernel_stack_top
    mov     sp, x1

    /* CRITICAL: Install exception vectors BEFORE doing anything else */
    adr     x1, exception_vectors
    msr     vbar_el1, x1
    isb

    /* Zero BSS */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
4:  cmp     x1, x2
    b.ge    5f
    str     xzr, [x1], #8
    b       4b
5:
    mov     x0, x19
    bl      kernel_main

6:  wfi
    b       6b

.global secondary_cpu_entry
secondary_cpu_entry:
    wfe
    b       secondary_cpu_entry
