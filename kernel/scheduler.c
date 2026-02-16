/*
 * scheduler.c – 64-bit multi-core scheduler for RISC OS Phoenix
 * Features:
 *   • Per-core runqueues (SMP-ready)
 *   • Preemptive scheduling via timer tick
 *   • Context switch in pure AArch64 assembly
 *   • Priority-based enqueue with round-robin
 * Author: R Andrews Grok 4 – 05 Feb 2026
 */

#include "kernel.h"
#include "spinlock.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define MAX_CPUS            8
#define TASK_NAME_LEN       32
#define TASK_MIN_PRIORITY   0
#define TASK_MAX_PRIORITY   255

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef struct task task_t;

struct task {
    uint64_t    regs[31];
    uint64_t    sp_el0;
    uint64_t    elr_el1;
    uint64_t    spsr_el1;
    uint64_t    stack_top;
    task_t     *next;
    task_t     *prev;
    char        name[TASK_NAME_LEN];
    int         pid;
    int         priority;
    task_state_t state;
    uint64_t    cpu_affinity;
};

typedef struct {
    task_t     *current;
    task_t     *idle_task;
    task_t     *runqueue_head;
    task_t     *runqueue_tail;
    spinlock_t  lock;
    int         cpu_id;
    uint64_t    schedule_count;
} cpu_sched_t;

static cpu_sched_t cpu_sched[MAX_CPUS];
static int nr_cpus = 1;

extern task_t *current_task;  // Per-CPU current task pointer

void sched_init_cpu(int cpu_id) {
    cpu_sched_t *sched = &cpu_sched[cpu_id];
    sched->cpu_id = cpu_id;
    sched->current = NULL;
    sched->runqueue_head = sched->runqueue_tail = NULL;
    sched->schedule_count = 0;
    spinlock_init(&sched->lock);

    task_t *idle = kmalloc(sizeof(task_t));
    memset(idle, 0, sizeof(task_t));
    strcpy(idle->name, "idle");
    idle->pid = -1;
    idle->state = TASK_RUNNING;
    idle->priority = TASK_MAX_PRIORITY;
    sched->idle_task = sched->current = idle;
    current_task = idle;
}

void sched_init(void) {
    nr_cpus = detect_nr_cpus();  // From device tree or CPU ID
    for (int i = 0; i < nr_cpus; i++) {
        sched_init_cpu(i);
    }
    debug_print("Scheduler initialized for %d CPUs\n", nr_cpus);
}

static inline void enqueue_task(cpu_sched_t *sched, task_t *task) {
    task->state = TASK_READY;
    task->next = NULL;

    if (!sched->runqueue_head) {
        sched->runqueue_head = sched->runqueue_tail = task;
        task->prev = NULL;
        return;
    }

    task_t *pos = sched->runqueue_head;
    task_t *prev = NULL;
    while (pos && pos->priority <= task->priority) {
        prev = pos;
        pos = pos->next;
    }

    if (!prev) {
        task->next = sched->runqueue_head;
        sched->runqueue_head->prev = task;
        sched->runqueue_head = task;
    } else {
        task->next = prev->next;
        task->prev = prev;
        if (prev->next) prev->next->prev = task;
        else sched->runqueue_tail = task;
        prev->next = task;
    }
}

static inline void dequeue_task(cpu_sched_t *sched, task_t *task) {
    if (task->prev) task->prev->next = task->next;
    else sched->runqueue_head = task->next;
    if (task->next) task->next->prev = task->prev;
    else sched->runqueue_tail = task->prev;
}

static inline task_t *pick_next_task(cpu_sched_t *sched) {
    if (!sched->runqueue_head) {
        return sched->idle_task;
    }
    task_t *next = sched->runqueue_head;
    dequeue_task(sched, next);
    enqueue_task(sched, next);  // Round-robin requeue
    return next;
}

void context_switch(task_t *prev, task_t *next) {
    current_task = next;
    __asm__ volatile (
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "stp x8, x9, [sp, #-16]!\n"
        "stp x10, x11, [sp, #-16]!\n"
        "stp x12, x13, [sp, #-16]!\n"
        "stp x14, x15, [sp, #-16]!\n"
        "stp x16, x17, [sp, #-16]!\n"
        "stp x18, x19, [sp, #-16]!\n"
        "stp x20, x21, [sp, #-16]!\n"
        "stp x22, x23, [sp, #-16]!\