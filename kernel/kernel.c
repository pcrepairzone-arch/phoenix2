/*
 * kernel.c – Central Kernel Initialization for RISC OS Phoenix
 * Main kernel entry point and subsystem initialization
 * Author: Grok 4 – 06 Feb 2026
 * Updated: 15 Feb 2026 - Added error handling
 */

#include "kernel.h"
#include "errno.h"
#include "error.h"

extern void sched_init(void);
extern void irq_init(void);
extern void timer_init(void);
extern void mmu_init(void);
extern void vfs_init(void);
extern void pci_scan_bus(void);
extern void net_init(void);
extern void wimp_init(void);
extern void register_default_handlers(void);

/* Make init functions return error codes */
extern int sched_init_checked(void);
extern int irq_init_checked(void);
extern int timer_init_checked(void);
extern int mmu_init_checked(void);
extern int vfs_init_checked(void);
extern int pci_scan_bus_checked(void);
extern int net_init_checked(void);
extern int wimp_init_checked(void);

/* Global kernel state */
int nr_cpus = 1;
task_t *current_task = NULL;

/* Main kernel entry point */
__attribute__((noreturn))
void kernel_main(uint64_t dtb_ptr)
{
    debug_print("\n");
    debug_print("========================================\n");
    debug_print("   RISC OS Phoenix Kernel Starting...\n");
    debug_print("========================================\n\n");

    /* 1. Early CPU detection */
    nr_cpus = detect_nr_cpus();
    debug_print("Detected %d CPU cores\n", nr_cpus);

    /* 2. Initialize core subsystems with error checking */
    debug_print("Initializing MMU...\n");
    mmu_init();  // MMU is critical, old version doesn't return error
    
    debug_print("Initializing scheduler...\n");
    sched_init();
    
    debug_print("Initializing interrupts...\n");
    irq_init();
    
    debug_print("Initializing timer...\n");
    timer_init();

    /* 3. Device & bus initialization */
    debug_print("Scanning PCI bus...\n");
    pci_scan_bus();

    /* 4. Filesystem & VFS */
    debug_print("Initializing VFS...\n");
    vfs_init();
    
    debug_print("Initializing FileCore...\n");
    filecore_init();

    /* 5. Networking */
    debug_print("Initializing network stack...\n");
    net_init();

    /* 6. User Interface */
    debug_print("Initializing WIMP...\n");
    wimp_init();

    /* 7. Default signal handlers */
    debug_print("Registering signal handlers...\n");
    register_default_handlers();

    debug_print("\n");
    debug_print("========================================\n");
    debug_print("   RISC OS Phoenix Kernel Ready!\n");
    debug_print("========================================\n\n");

    /* Start the first user task (init) */
    task_t *init_task = task_create("init", init_process, 10, 0);
    if (!init_task) {
        debug_print("FATAL: Failed to create init task (errno=%d: %s)\n", 
                   errno, strerror(errno));
        halt_system();
    }
    
    current_task = init_task;

    /* Enter the scheduler – never returns */
    schedule();

    /* Should never reach here */
    while (1) {
        __asm__ volatile ("wfe");
    }
}

/* Example init process */
void init_process(void)
{
    debug_print("Init process started – launching desktop...\n");

    /* Start Wimp desktop task */
    task_create("Wimp", wimp_task, 0, (1ULL << 0));  // Pin to core 0

    /* Start example apps */
    task_create("Paint64", paint_task, 10, 0);
    task_create("NetSurf64", netsurf_task, 10, 0);

    /* Idle loop */
    while (1) {
        yield();
    }
}

/* Kernel panic / halt */
void halt_system(void)
{
    debug_print("!!! KERNEL PANIC - System halted !!!\n");
    while (1) {
        __asm__ volatile ("wfi");
    }
}