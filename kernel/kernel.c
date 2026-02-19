/*
 * kernel.c - Central Kernel Initialization for RISC OS Phoenix
 *
 * kernel_main() is the ONE entry point called from boot.c after
 * the CPU is in a safe state and UART is running.
 *
 * Boot order:
 *   UART (boot.c)  →  kernel_main()
 *     GPU / framebuffer   ← video up ASAP so all msgs appear on screen
 *     Device tree parse   ← know how much RAM / how many CPUs
 *     MMU                 ← page tables
 *     Scheduler           ← task structures
 *     IRQ controller
 *     Timer
 *     PCI bus scan
 *     VFS + FileCore
 *     Network stack
 *     WIMP (deferred – needs userspace)
 *     Create init task → schedule()
 */

#include "kernel.h"
#include "errno.h"
#include "error.h"

/* ------------------------------------------------------------------ */
/* Forward declarations for subsystem init functions                  */
/* ------------------------------------------------------------------ */
extern void uart_init(void);           /* drivers/uart/uart.c     */
extern void gpu_init(void);             /* drivers/gpu/gpu.c        */
extern void device_tree_parse(uint64_t);/* kernel/devicetree.c      */
extern int  detect_nr_cpus(void);       /* kernel/devicetree.c      */
extern void mmu_init(void);             /* kernel/mmu.c             */
extern void sched_init(void);           /* kernel/sched.c           */
extern void sched_init_cpu(int cpu);    /* kernel/sched.c           */
extern void irq_init(void);             /* kernel/irq.c             */
extern void timer_init(void);           /* kernel/timer.c           */
extern void timer_init_cpu(void);       /* kernel/timer.c           */
extern void pci_scan_bus(void);         /* kernel/pci.c (stub)      */
extern void vfs_init(void);             /* kernel/vfs.c (stub)      */
extern void filecore_init(void);        /* kernel/filecore.c (stub) */
extern void net_init(void);             /* net/tcpip.c (stub)       */
extern void wimp_init(void);            /* wimp/wimp.c (stub)       */
extern void register_default_handlers(void); /* kernel/signal.c    */

/* ------------------------------------------------------------------ */
/* Task entry points                                                   */
/* ------------------------------------------------------------------ */
extern void wimp_task(void);
extern void paint_task(void);
extern void netsurf_task(void);

/* ------------------------------------------------------------------ */
/* Global kernel state                                                 */
/* ------------------------------------------------------------------ */
int     nr_cpus      = 1;
task_t *current_task = NULL;

/* Exception vectors are defined in exceptions.S */

/* ------------------------------------------------------------------ */
/* kernel_main - called from boot.c after UART is ready              */
/* ------------------------------------------------------------------ */
__attribute__((noreturn))
void kernel_main(uint64_t dtb_ptr)
{
    /* CRITICAL: Detect peripheral base address from DTB/hardware */
    extern void detect_peripheral_base(uint64_t dtb_ptr);
    detect_peripheral_base(dtb_ptr);
    
    /* Now we can use GPIO/UART/Mailbox with correct addresses */
    extern void led_signal_boot_ok(void);
    led_signal_boot_ok();  /* 3 blinks = boot.S → kernel_main OK */

    /* Belt-and-suspenders: ensure UART is initialised even if boot.S
     * forgot to call uart_init() before branching here.           */
    uart_init();

    /* LED diagnostic: prove C code reached (VISUAL confirmation) */
    extern void led_signal_kernel_main(void);
    led_signal_kernel_main();

    /* UART is already up – first messages go to serial immediately  */
    debug_print("\n");
    debug_print("========================================\n");
    debug_print("  RISC OS Phoenix Kernel Starting\n");
    debug_print("========================================\n\n");

    /* -------------------------------------------------------------- */
    /* 1. VIDEO – bring up framebuffer so all subsequent messages     */
    /*    appear BOTH on serial AND on screen.                        */
    /* -------------------------------------------------------------- */
    debug_print("[1/9] GPU / framebuffer...\n");
    gpu_init();
    /* From here on debug_print() mirrors to screen console too      */

    /* -------------------------------------------------------------- */
    /* 2. Device tree – find RAM size, CPU count, peripherals         */
    /* -------------------------------------------------------------- */
    debug_print("[2/9] Device tree (DTB at 0x%llx)...\n", dtb_ptr);
    device_tree_parse(dtb_ptr);
    nr_cpus = detect_nr_cpus();
    debug_print("      CPUs detected: %d\n", nr_cpus);

    /* -------------------------------------------------------------- */
    /* 3. MMU – identity-map kernel, set up page tables               */
    /* -------------------------------------------------------------- */
    debug_print("[3/9] MMU...\n");
    mmu_init();

    /* -------------------------------------------------------------- */
    /* 4. Scheduler                                                    */
    /* -------------------------------------------------------------- */
    debug_print("[4/9] Scheduler...\n");
    sched_init();
    sched_init_cpu(0);

    /* -------------------------------------------------------------- */
    /* 5. Interrupt controller (GIC)                                  */
    /* -------------------------------------------------------------- */
    debug_print("[5/9] IRQ controller...\n");
    irq_init();

    /* -------------------------------------------------------------- */
    /* 6. Timer                                                        */
    /* -------------------------------------------------------------- */
    debug_print("[6/9] Timer...\n");
    timer_init();
    timer_init_cpu();

    /* -------------------------------------------------------------- */
    /* 7. Storage (PCI/NVMe/FileCore)                                 */
    /* -------------------------------------------------------------- */
    debug_print("[7/9] Storage...\n");
    pci_scan_bus();
    vfs_init();
    filecore_init();

    /* -------------------------------------------------------------- */
    /* 8. Network                                                      */
    /* -------------------------------------------------------------- */
    debug_print("[8/9] Network...\n");
    net_init();

    /* -------------------------------------------------------------- */
    /* 9. Signals                                                      */
    /* -------------------------------------------------------------- */
    debug_print("[9/9] Signal handlers...\n");
    register_default_handlers();

    /* -------------------------------------------------------------- */
    /* Done – print banner and launch init task                        */
    /* -------------------------------------------------------------- */
    debug_print("\n");
    debug_print("========================================\n");
    debug_print("  RISC OS Phoenix Kernel Ready!\n");
    debug_print("========================================\n\n");

    task_t *init_task = task_create("init", init_process, 10, 0);
    if (!init_task) {
        debug_print("FATAL: Cannot create init task (errno=%d)\n", errno);
        halt_system();
    }

    current_task = init_task;
    schedule();   /* never returns */

    while (1) __asm__ volatile ("wfi");
}

/* ------------------------------------------------------------------ */
/* init_process - first user-space task                               */
/* ------------------------------------------------------------------ */
void init_process(void)
{
    debug_print("init: launching desktop...\n");

    task_create("Wimp",     wimp_task,    0,  (1ULL << 0));
    task_create("Paint64",  paint_task,   10, 0);
    task_create("NetSurf",  netsurf_task, 10, 0);

    while (1) yield();
}

/* ------------------------------------------------------------------ */
/* halt_system - kernel panic                                         */
/* ------------------------------------------------------------------ */
void halt_system(void)
{
    debug_print("\n!!! KERNEL PANIC – system halted !!!\n");
    while (1) __asm__ volatile ("wfi");
}
