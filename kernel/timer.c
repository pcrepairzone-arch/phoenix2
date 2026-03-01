/*
 * timer.c – ARM Generic Timer for RISC OS Phoenix (Simplified)
 * Author: R Andrews Grok 4 – 26 Nov 2025
 * Updated: 15 Feb 2026 - Simplified for compilation
 */

#include "kernel.h"
#include "spinlock.h"
#include "errno.h"
#include <stdint.h>

#define TICK_INTERVAL 10  // 10ms tick

/* Timer structure */
typedef struct timer {
    void (*callback)(struct timer *);
    void *private;
    uint64_t expires_ns;
    int active;
    struct timer *next;
} timer_t;

/* Per-CPU timer lists */
static timer_t *timer_lists[MAX_CPUS];
static spinlock_t timer_locks[MAX_CPUS];

/* Get timer frequency from system register */
static uint64_t timer_get_freq(void) {
    uint64_t freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

/* Get current time in nanoseconds */
static uint64_t get_time_ns(void) {
    uint64_t ticks;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ticks));
    return (ticks * 1000000000ULL) / timer_get_freq();
}

/* Convert nanoseconds to ticks */
static uint64_t ns_to_ticks(uint64_t ns) {
    return (ns * timer_get_freq()) / 1000000000ULL;
}

/* Initialize timer for a specific CPU */
void timer_init_cpu(void) {
    int cpu = get_cpu_id();
    
    timer_lists[cpu] = NULL;
    spinlock_init(&timer_locks[cpu]);
    
    /* NOTE: We deliberately do NOT enable the physical timer interrupt here.
     * The GIC (Generic Interrupt Controller) has not been configured yet —
     * irq_init() is currently a stub.  Arming the timer before the GIC is
     * ready causes a spurious IRQ exception that halts the kernel.
     *
     * Once irq_init() properly configures the GIC distributor and CPU
     * interface, re-enable these lines:
     *
     *   uint32_t ctl;
     *   __asm__ volatile ("mrs %0, cntp_ctl_el0" : "=r"(ctl));
     *   ctl |= 1;  // ENABLE bit
     *   __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
     *   __asm__ volatile ("msr cntp_tval_el0, %0"
     *       :: "r"(ns_to_ticks(TICK_INTERVAL * 1000000ULL)));
     *
     * For now, make sure the timer is masked (IMASK bit set, ENABLE clear).
     */
    uint32_t ctl = 0x2;  /* IMASK=1, ENABLE=0 — timer masked and disabled */
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
    
    debug_print("Timer initialized for CPU %d\n", cpu);
}

/* Global timer init */
void timer_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        spinlock_init(&timer_locks[i]);
        timer_lists[i] = NULL;
    }
    debug_print("ARM Generic Timer initialized – freq %lld Hz\n", timer_get_freq());
}

/* Timer tick handler */
void timer_tick(void) {
    int cpu = get_cpu_id();
    uint64_t now = get_time_ns();
    unsigned long flags;
    spin_lock_irqsave(&timer_locks[cpu], &flags);
    
    timer_t *t = timer_lists[cpu];
    while (t && t->expires_ns <= now) {
        timer_t *next = t->next;
        t->active = 0;
        spin_unlock_irqrestore(&timer_locks[cpu], flags);
        if (t->callback) {
            t->callback(t);
        }
        spin_lock_irqsave(&timer_locks[cpu], &flags);
        t = next;
    }
    timer_lists[cpu] = t;
    
    spin_unlock_irqrestore(&timer_locks[cpu], flags);
    
    // Reschedule
    schedule();
    
    // Re-arm timer
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(ns_to_ticks(TICK_INTERVAL * 1000000ULL)));
}
