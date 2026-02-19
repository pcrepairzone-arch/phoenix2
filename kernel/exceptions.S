/*
 * exceptions.S - Exception handlers with LED diagnostics
 * When an exception occurs, blink LED in a unique pattern to identify it
 */

.section ".text"

/* Exception types - different LED blink patterns */
.global exception_vectors

.align 11  /* Exception vectors must be 2KB aligned (2^11 = 2048) */
exception_vectors:

/* Current EL with SP0 */
.align 7
curr_el_sp0_sync:
    b exception_sync_blink      /* 6 quick blinks */
.align 7
curr_el_sp0_irq:
    b exception_irq_blink       /* 7 quick blinks */
.align 7
curr_el_sp0_fiq:
    b exception_fiq_blink       /* 8 quick blinks */
.align 7
curr_el_sp0_serror:
    b exception_serror_blink    /* 9 quick blinks */

/* Current EL with SPx */
.align 7
curr_el_spx_sync:
    b exception_sync_blink
.align 7
curr_el_spx_irq:
    b exception_irq_blink
.align 7
curr_el_spx_fiq:
    b exception_fiq_blink
.align 7
curr_el_spx_serror:
    b exception_serror_blink

/* Lower EL using AArch64 */
.align 7
lower_el_aarch64_sync:
    b exception_sync_blink
.align 7
lower_el_aarch64_irq:
    b exception_irq_blink
.align 7
lower_el_aarch64_fiq:
    b exception_fiq_blink
.align 7
lower_el_aarch64_serror:
    b exception_serror_blink

/* Lower EL using AArch32 */
.align 7
lower_el_aarch32_sync:
    b exception_sync_blink
.align 7
lower_el_aarch32_irq:
    b exception_irq_blink
.align 7
lower_el_aarch32_fiq:
    b exception_fiq_blink
.align 7
lower_el_aarch32_serror:
    b exception_serror_blink

/* Exception handlers - blink LED in different patterns */
exception_sync_blink:
    /* 6 quick blinks = Synchronous exception (data abort, instruction abort, etc) */
    mov     x20, #6
    b       exception_blink_loop

exception_irq_blink:
    /* 7 quick blinks = IRQ */
    mov     x20, #7
    b       exception_blink_loop

exception_fiq_blink:
    /* 8 quick blinks = FIQ */
    mov     x20, #8
    b       exception_blink_loop

exception_serror_blink:
    /* 9 quick blinks = SError */
    mov     x20, #9
    b       exception_blink_loop

exception_blink_loop:
    /* Get GPIO base - try both 0xFE200000 and 0x47E200000 */
    movz    x10, #0xFE20, lsl #16
    
    /* Set GPIO42 as output */
    ldr     w11, [x10, #0x10]
    bic     w11, w11, #(0x7 << 6)
    orr     w11, w11, #(0x1 << 6)
    str     w11, [x10, #0x10]
    
    /* Blink x20 times */
exc_blink:
    mov     w11, #(1 << 10)
    str     w11, [x10, #0x2C]       /* LED on */
    mov     x13, #0x300000          /* Slower - 3x longer */
1:  subs    x13, x13, #1
    b.ne    1b
    str     w11, [x10, #0x20]       /* LED off */
    mov     x13, #0x300000          /* Slower - 3x longer */
2:  subs    x13, x13, #1
    b.ne    2b
    subs    x20, x20, #1
    b.ne    exc_blink
    
    /* Longer pause between pattern repeats */
    mov     x13, #0xA00000          /* Much longer pause */
3:  subs    x13, x13, #1
    b.ne    3b
    
    /* Read exception info before looping again */
    mrs     x0, esr_el1             /* Exception Syndrome */
    mrs     x1, elr_el1             /* Return address */
    mrs     x2, far_el1             /* Fault address */
    
    /* Store to fixed memory for later inspection */
    ldr     x10, =0x8F200
    str     x0, [x10, #0]
    str     x1, [x10, #8]
    str     x2, [x10, #16]
    
    /* Reset blink count and loop */
    mrs     x20, esr_el1
    lsr     x20, x20, #26           /* EC field - exception class */
    and     x20, x20, #0x3F
    add     x20, x20, #6            /* Offset so we get 6-15 blinks */
    b       exception_blink_loop
