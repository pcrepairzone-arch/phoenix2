/* boot.c – 64-bit ARM (AArch64) Primary Boot Stub for RISC OS Phoenix */
#include "kernel.h"

/* External UART functions */
extern void uart_init(void);

__attribute__((noreturn))
void primary_cpu_entry(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    /* x0 = device tree pointer (optional) */

    /* 1. Set up exception level and vectors */
    uint64_t sctlr;
    __asm__ volatile (
        "mrs %0, sctlr_el1\n"
        "bic %0, %0, #1\n"       // Disable MMU (bit 0)
        "bic %0, %0, #4\n"       // Disable D-cache (bit 2)
        "bic %0, %0, #0x1000\n"  // Disable I-cache (bit 12)
        "msr sctlr_el1, %0\n"
        "isb\n"
        : "=r"(sctlr)
    );

    /* Configure HCR_EL2 if coming from EL2 */
    uint64_t hcr_val = (1ULL << 31);  // RW=1 → EL1 is AArch64
    __asm__ volatile (
        "msr hcr_el2, %0\n"
        "isb\n"
        :: "r"(hcr_val)
    );

    /* Set vector base */
    __asm__ volatile (
        "adr x0, exception_vectors\n"
        "msr vbar_el1, x0\n"
        "isb\n"
    );

    /* 2. Set up stack (per-CPU stack) */
    extern uint8_t __kernel_stack_top;
    uint64_t stack = (uint64_t)&__kernel_stack_top;
    __asm__ volatile ("msr sp_el1, %0" :: "r"(stack));

    /* 3. Zero BSS */
    extern uint8_t __bss_start, __bss_end;
    uint64_t *bss = (uint64_t*)&__bss_start;
    uint64_t *bss_end = (uint64_t*)&__bss_end;
    while (bss < bss_end) *bss++ = 0;

    /* 4. Initialize UART first for debug output */
    uart_init();

    /* 5. Jump to C kernel_main */
    kernel_main(x0);

    __builtin_unreachable();
}

__attribute__((noreturn))
void secondary_cpu_entry(void) {
    uint64_t cpu_id = get_cpu_id();

    debug_print("Secondary CPU %d online\n", cpu_id);

    /* Initialize per-CPU data */
    sched_init_cpu(cpu_id);
    irq_init();
    timer_init_cpu();

    /* Signal ready */
    __asm__ volatile ("sev");

    /* Enter idle loop until scheduler picks a task */
    while (1) {
        __asm__ volatile ("wfe");
        schedule();
    }
}

__attribute__((noreturn))
void kernel_main(uint64_t dtb_ptr) {
    debug_print("RISC OS Phoenix kernel starting...\n");

    /* 1. Early CPU detection */
    nr_cpus = detect_nr_cpus();  // From device tree

    /* 2. Initialize primary CPU */
    sched_init_cpu(0);

    /* 3. Set up page tables (simple 1:1 mapping for now) */
    mmu_init();  // From mmu.c

    /* 4. Full kernel init */
    sched_init();
    irq_init();      // GICv3
    timer_init();    // ARM generic timer
    device_tree_parse(dtb_ptr);  // Optional

    debug_print("RISC OS Phoenix ready – starting init task\n");

    /* 5. Start first user task (init) */
    task_t *init_task = task_create("init", init_process, 10, 0);
    cpu_sched[0].current = init_task;

    /* 6. Enter scheduler – never returns */
    schedule();

    __builtin_unreachable();
}

// Example init process
void init_process(void)
{
    debug_print("Hello from RISC OS Phoenix!\n");
    while (1) {
        debug_print("Init task running...\n");
        yield();
    }
}
/* Get current CPU ID from MPIDR register */
int get_cpu_id(void) {
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 0xFF);  // Aff0 field contains CPU ID
}
