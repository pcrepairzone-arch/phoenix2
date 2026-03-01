/*
 * exceptions.S - AArch64 Exception Vectors
 *
 * On any exception: print ESR_EL1, ELR_EL1, FAR_EL1 and SP to UART,
 * blink the LED, then halt.  This makes crashes visible on the serial
 * terminal instead of silently rebooting.
 */

.section ".text"

.global exception_vectors

.align 11   /* VBAR must be 2 KB aligned */
exception_vectors:

/* ── Current EL with SP0 ─────────────────────────────── */
.align 7
curr_el_sp0_sync:    b exc_handler
.align 7
curr_el_sp0_irq:     b exc_irq_handler
.align 7
curr_el_sp0_fiq:     b exc_handler
.align 7
curr_el_sp0_serror:  b exc_handler

/* ── Current EL with SPx ─────────────────────────────── */
.align 7
curr_el_spx_sync:    b exc_handler
.align 7
curr_el_spx_irq:     b exc_irq_handler
.align 7
curr_el_spx_fiq:     b exc_handler
.align 7
curr_el_spx_serror:  b exc_handler

/* ── Lower EL AArch64 ────────────────────────────────── */
.align 7
lower_el_aarch64_sync:   b exc_handler
.align 7
lower_el_aarch64_irq:    b exc_irq_handler
.align 7
lower_el_aarch64_fiq:    b exc_handler
.align 7
lower_el_aarch64_serror: b exc_handler

/* ── Lower EL AArch32 ────────────────────────────────── */
.align 7
lower_el_aarch32_sync:   b exc_handler
.align 7
lower_el_aarch32_irq:    b exc_irq_handler
.align 7
lower_el_aarch32_fiq:    b exc_handler
.align 7
lower_el_aarch32_serror: b exc_handler

/* ────────────────────────────────────────────────────── */
/* Unexpected exception: print registers to UART, halt   */
/* ────────────────────────────────────────────────────── */
exc_handler:
    /* Save x0-x3 on the current stack */
    stp  x0, x1, [sp, #-16]!
    stp  x2, x3, [sp, #-16]!

    /* Read exception info */
    mrs  x0, esr_el1
    mrs  x1, elr_el1
    mrs  x2, far_el1
    mov  x3, sp

    /* Save to fixed scratchpad for JTAG inspection */
    ldr  x10, =0x8F200
    str  x0, [x10, #0]
    str  x1, [x10, #8]
    str  x2, [x10, #16]
    str  x3, [x10, #24]

    /* Print banner */
    ldr  x10, =exc_msg_crash
    bl   exc_uart_str

    /* Print ESR */
    ldr  x10, =exc_msg_esr
    bl   exc_uart_str
    bl   exc_uart_hex64

    /* Print ELR */
    stp  x0, x1, [sp, #-16]!
    mov  x0, x1
    ldr  x10, =exc_msg_elr
    bl   exc_uart_str
    bl   exc_uart_hex64
    ldp  x0, x1, [sp], #16

    /* Print FAR */
    stp  x0, x2, [sp, #-16]!
    mov  x0, x2
    ldr  x10, =exc_msg_far
    bl   exc_uart_str
    bl   exc_uart_hex64
    ldp  x0, x2, [sp], #16

    /* Print SP */
    stp  x0, x3, [sp, #-16]!
    mov  x0, x3
    ldr  x10, =exc_msg_sp
    bl   exc_uart_str
    bl   exc_uart_hex64
    ldp  x0, x3, [sp], #16

    /* Newline */
    mov  x0, #'\n'
    bl   exc_uart_putc
    mov  x0, #'\r'
    bl   exc_uart_putc

    /* Blink LED and halt */
    b    exc_halt_blink

/* IRQ handler: for now just print and halt (GIC not set up yet) */
exc_irq_handler:
    stp  x0, x1, [sp, #-16]!
    mrs  x0, esr_el1
    mrs  x1, elr_el1
    ldr  x10, =exc_msg_irq
    bl   exc_uart_str
    ldr  x10, =exc_msg_elr
    bl   exc_uart_str
    mov  x0, x1
    bl   exc_uart_hex64
    mov  x0, #'\n'
    bl   exc_uart_putc
    mov  x0, #'\r'
    bl   exc_uart_putc
    ldp  x0, x1, [sp], #16
    b    exc_halt_blink

/* ── UART helpers ────────────────────────────────────── */

/*
 * exc_uart_putc: send character in w0.
 * Uses x10=UART_BASE (caller sets it), preserves x0.
 * Pi 4 UART base = 0xFE201000 (identity mapped after mmu_init).
 * Before mmu_init the UART still works since identity map is 1:1.
 */
exc_uart_putc:
    /* Load UART base into x10 if not already set */
    ldr  x10, =0xFE201000
    /* Wait for TX FIFO not full (FR bit 5) */
1:  ldr  w11, [x10, #0x18]
    tst  w11, #(1 << 5)
    b.ne 1b
    str  w0, [x10, #0x00]
    ret

/* exc_uart_str: print null-terminated string pointed to by x10 */
exc_uart_str:
    stp  x0, x30, [sp, #-16]!
    stp  x10, xzr, [sp, #-16]!
1:  ldrb w0, [x10], #1
    cbz  w0, 2f
    bl   exc_uart_putc
    b    1b
2:  ldp  x10, xzr, [sp], #16
    ldp  x0, x30, [sp], #16
    ret

/* exc_uart_hex64: print x0 as 16 hex digits */
exc_uart_hex64:
    stp  x0, x30, [sp, #-16]!
    stp  x1, x2,  [sp, #-16]!
    mov  x1, x0
    ldr  x10, =exc_hex_chars
    mov  x2, #60
1:  lsr  x0, x1, x2
    and  x0, x0, #0xF
    ldrb w0, [x10, x0]
    bl   exc_uart_putc
    subs x2, x2, #4
    b.ge 1b
    ldp  x1, x2,  [sp], #16
    ldp  x0, x30, [sp], #16
    ret

/* ── LED blink halt (infinite) ──────────────────────── */
exc_halt_blink:
    ldr  x10, =0xFE200000      /* GPIO base, Pi 4 */
    /* GPIO42 output */
    ldr  w11, [x10, #0x10]
    bic  w11, w11, #(7 << 6)
    orr  w11, w11, #(1 << 6)
    str  w11, [x10, #0x10]
1:
    /* LED on */
    mov  w11, #(1 << 10)
    str  w11, [x10, #0x2C]
    mov  x12, #0x200000
2:  subs x12, x12, #1
    b.ne 2b
    /* LED off */
    str  w11, [x10, #0x20]
    mov  x12, #0x200000
3:  subs x12, x12, #1
    b.ne 3b
    b    1b

/* ── String literals ─────────────────────────────────── */
.section ".rodata"
exc_msg_crash: .asciz "\r\n*** EXCEPTION ***\r\n"
exc_msg_esr:   .asciz "  ESR_EL1 = "
exc_msg_elr:   .asciz "\r\n  ELR_EL1 = "
exc_msg_far:   .asciz "\r\n  FAR_EL1 = "
exc_msg_sp:    .asciz "\r\n  SP      = "
exc_msg_irq:   .asciz "\r\n*** SPURIOUS IRQ ***\r\n"
exc_hex_chars: .ascii "0123456789abcdef"
