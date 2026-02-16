/*
 * timer.c – ARM Generic Timer for RISC OS Phoenix
 * Provides microsecond precision timers, periodic ticks for scheduler
 * Multi-core support with per-core timers
 * Author: R Andrews Grok 4 – 05 Feb 2026
 */

#include "kernel.h"
#include <stdint.h>

#define TIMER_FREQ      1000000  // 1 MHz tick (1us resolution)
#define TICK_INTERVAL   10000    // 10ms scheduler tick

typedef struct timer {
    struct timer *next;
    uint64_t expires_ns;
    void (*callback)(struct timer *);
    void *private;
    int active;
} timer_t;

static timer_t *timer_lists[MAX_CPUS];
static spinlock_t timer_locks[MAX_CPUS] = { [0 ... MAX_CPUS-1] = SPINLOCK_INIT };

/* Read counter frequency */
static uint64_t timer_get_freq(void) {
    uint64_t freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

/* Read current counter */
static uint64_t timer_get_cnt(void) {
    uint64_t cnt;
    __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

/* Convert ns to counter ticks */
static uint64_t ns_to_ticks(uint64_t ns) {
    return (ns * timer_get_freq()) / 1000000000ULL;
}

/* Convert counter ticks to ns */
static uint64_t ticks_to_ns(uint64_t ticks) {
    return (ticks * 1000000000ULL) / timer_get_freq();
}

/* Get current time in ns */
uint64_t get_time_ns(void) {
    return ticks_to_ns(timer_get_cnt());
}

/* Initialize timer for current CPU */
void timer_init_cpu(void) {
    int cpu = get_cpu_id();

    // Enable timer
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(1ULL));  // Enable

    // Set initial tick for scheduler
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(ns_to_ticks(TICK_INTERVAL * 1000000ULL)));

    debug_print("Timer initialized for CPU %d\n", cpu);
}

/* Global timer init – called from boot */
void timer_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        timer_init_cpu();
    }
    debug_print("ARM Generic Timer initialized – freq %lld Hz\n", timer_get_freq());
}

/* Initialize a timer */
void timer_init(timer_t *timer, void (*callback)(timer_t *), void *private) {
    memset(timer, 0, sizeof(*timer));
    timer->callback = callback;
    timer->private = private;
}

/* Schedule timer */
void timer_schedule(timer_t *timer, uint64_t ms) {
    int cpu = get_cpu_id();
    unsigned long flags;
    spin_lock_irqsave(&timer_locks[cpu], &flags);

    timer->expires_ns = get_time_ns() + ms * 1000000ULL;
    timer->active = 1;

    timer_t **p = &timer_lists[cpu];
    while (*p && (*p)->expires_ns < timer->expires_ns) {
        p = &(*p)->next;
    }
    timer->next = *p;
    *p = timer;

    spin_unlock_irqrestore(&timer_locks[cpu], flags);
}

/* Cancel timer */
void timer_cancel(timer_t *timer) {
    int cpu = get_cpu_id();
    unsigned long flags;
    spin_lock_irqsave(&timer_locks[cpu], &flags);

    timer_t **p = &timer_lists[cpu];
    while (*p) {
        if (*p == timer) {
            *p = timer->next;
            timer->active = 0;
            break;
        }
        p = &(*p)->next;
    }

    spin_unlock_irqrestore(&timer_locks[cpu], flags);
}

/* Timer interrupt handler – per-core */
void timer_irq_handler(int vector, void *private) {
    int cpu = get_cpu_id();
    uint64_t now = get_time_ns();
    unsigned long flags;
    spin_lock_irqsave(&timer_locks[cpu], &flags);

    timer_t *t = timer_lists[cpu];
    while (t && t->expires_ns <= now) {
        timer_t *next = t->next;
        t->active = 0;
        spin_unlock_irqrestore(&timer_locks[cpu], flags);
        t->callback(t);
        spin_lock_irqsave(&timer_locks[cpu], &flags);
        t = next;
    }
    timer_lists[cpu] = t;

    spin_unlock_irqrestore(&timer_locks[cpu], flags);

    // Reschedule
    schedule();

    // Re-arm timer for next tick
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(ns_to_ticks(TICK_INTERVAL * 1000000ULL)));
}

/* Module init – register IRQ handler */
_kernel_oserror *