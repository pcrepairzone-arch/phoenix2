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
extern int vl805_init(void);		/* drivers/usb/vl805_init.c */
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

extern int usb_init(void);
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
    led_signal_boot_ok();

    uart_init();

    extern void led_signal_kernel_main(void);
    led_signal_kernel_main();

    debug_print("\n");
    debug_print("========================================\n");
    debug_print("  RISC OS Phoenix Kernel Starting\n");
    debug_print("========================================\n\n");

    /* [1/9] GPU / framebuffer */
    debug_print("[1/9] GPU / framebuffer...\n");
    gpu_init();

    /* [2/9] Device tree */
    debug_print("[2/9] Device tree (DTB at 0x%llx)...\n", dtb_ptr);
    device_tree_parse(dtb_ptr);
    nr_cpus = detect_nr_cpus();
    debug_print("      CPUs detected: %d\n", nr_cpus);

    /* [3/9] MMU */
    debug_print("[3/9] MMU...\n");
    mmu_init();
    debug_print("MMU ready\n");

    /* [4/9] Scheduler — data structures only, no tasks yet.
     * task_create() uses spin_lock_irqsave which restores DAIF, potentially
     * unmasking IRQs before the GIC is ready. Defer sched_init_cpu until
     * after irq_init().                                                     */
    debug_print("[4/9] Scheduler...\n");
    sched_init();
    debug_print("Scheduler data structures ready\n");

    /* [5/9] Memory management */
    debug_print("\n[5/9] Memory management...\n");
    heap_stats();
    debug_print("Heap ready\n");

    /* [6/9] Interrupt system — must come before any task_create call */
    debug_print("[6/9] Interrupt system...\n");
    irq_init();
    timer_init();
    timer_init_cpu();
    debug_print("IRQ/Timer ready\n");

    /* Now safe to create the idle task (IRQs are masked/managed) */
    debug_print("Creating idle task...\n");
    sched_init_cpu(0);
    debug_print("sched_init_cpu returned OK\n");
    debug_print("Scheduler initialized for %d CPUs\n\n", nr_cpus);

    /* [7/9] PCI bus */
    debug_print("\n[7/9] PCI bus...\n");
    pci_init();
    debug_print("PCI ready\n");

    /* [8/9] USB subsystem */
    debug_print("\n[8/9] USB subsystem...\n");
    usb_init();
    debug_print("USB ready\n");

    /* [9/9] VFS + Network + Signals */
    debug_print("\n[9/9] VFS / Network / Signals...\n");
    vfs_init();
    filecore_init();
    net_init();
    register_default_handlers();
    debug_print("Subsystems ready\n");

    debug_print("\n========================================\n");
    debug_print("  Boot complete – launching init\n");
    debug_print("========================================\n\n");

    /* Create the init task and start scheduling */
    task_create("init", init_process, TASK_MAX_PRIORITY, (1ULL << 0));
    schedule();

    /* Should never reach here */
    halt_system();
    while (1) { asm volatile("wfi"); }
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
