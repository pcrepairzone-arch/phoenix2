/*
 * sched.c – Multi-core Preemptive Scheduler for RISC OS Phoenix
 * Round-robin with priorities, per-CPU runqueues
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "spinlock.h"

/* Forward declarations */
static void idle_task_fn(void);
static void dequeue_task(cpu_sched_t *sched, task_t *task);

/* Per-CPU scheduler state */
cpu_sched_t cpu_sched[MAX_CPUS];

/* Initialize scheduler on CPU 0 */
void sched_init(void) {
    for (int i = 0; i < nr_cpus; i++) {
        cpu_sched[i].cpu_id = i;
        cpu_sched[i].current = NULL;
        cpu_sched[i].idle_task = NULL;
        cpu_sched[i].runqueue_head = NULL;
        cpu_sched[i].runqueue_tail = NULL;
        cpu_sched[i].schedule_count = 0;
        spinlock_init(&cpu_sched[i].lock);
    }
    debug_print("Scheduler initialized for %d CPUs\n", nr_cpus);
}

/* Initialize scheduler on a specific CPU */
void sched_init_cpu(int cpu_id) {
    cpu_sched_t *sched = &cpu_sched[cpu_id];
    
    // Create idle task for this CPU
    sched->idle_task = task_create("idle", idle_task_fn, TASK_MIN_PRIORITY, (1ULL << cpu_id));
    
    debug_print("CPU %d scheduler ready\n", cpu_id);
}

/* Idle task function */
static void idle_task_fn(void) {
    while (1) {
        __asm__ volatile ("wfe");  // Wait for event
    }
}

/* Add task to runqueue */
void enqueue_task(cpu_sched_t *sched, task_t *task) {
    task->next = NULL;
    task->prev = sched->runqueue_tail;
    
    if (sched->runqueue_tail) {
        sched->runqueue_tail->next = task;
    } else {
        sched->runqueue_head = task;
    }
    sched->runqueue_tail = task;
    task->state = TASK_READY;
}

/* Remove task from runqueue */
static void dequeue_task(cpu_sched_t *sched, task_t *task) {
    if (task->prev) {
        task->prev->next = task->next;
    } else {
        sched->runqueue_head = task->next;
    }
    
    if (task->next) {
        task->next->prev = task->prev;
    } else {
        sched->runqueue_tail = task->prev;
    }
    
    task->next = NULL;
    task->prev = NULL;
}

/* Pick next task to run */
static task_t *pick_next_task(cpu_sched_t *sched) {
    task_t *best = NULL;
    int best_priority = -1;
    
    for (task_t *task = sched->runqueue_head; task; task = task->next) {
        if (task->state == TASK_READY && task->priority > best_priority) {
            best = task;
            best_priority = task->priority;
        }
    }
    
    return best ? best : sched->idle_task;
}

/* Context switch implementation */
static void context_switch(task_t *prev, task_t *next) {
    if (prev == next) return;
    
    // Save previous task's context
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
        "stp x22, x23, [sp, #-16]!\n"
        "stp x24, x25, [sp, #-16]!\n"
        "stp x26, x27, [sp, #-16]!\n"
        "stp x28, x29, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mrs %0, sp_el0\n"
        "mrs %1, elr_el1\n"
        "mrs %2, spsr_el1\n"
        "mov %3, sp\n"
        : "=r"(prev->sp_el0), "=r"(prev->elr_el1), 
          "=r"(prev->spsr_el1), "=r"(prev->stack_top)
    );
    
    // Restore next task's context
    __asm__ volatile (
        "mov sp, %3\n"
        "msr sp_el0, %0\n"
        "msr elr_el1, %1\n"
        "msr spsr_el1, %2\n"
        "ldr x30, [sp], #16\n"
        "ldp x28, x29, [sp], #16\n"
        "ldp x26, x27, [sp], #16\n"
        "ldp x24, x25, [sp], #16\n"
        "ldp x22, x23, [sp], #16\n"
        "ldp x20, x21, [sp], #16\n"
        "ldp x18, x19, [sp], #16\n"
        "ldp x16, x17, [sp], #16\n"
        "ldp x14, x15, [sp], #16\n"
        "ldp x12, x13, [sp], #16\n"
        "ldp x10, x11, [sp], #16\n"
        "ldp x8, x9, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        :: "r"(next->sp_el0), "r"(next->elr_el1), 
           "r"(next->spsr_el1), "r"(next->stack_top)
    );
}

/* Main scheduler function */
void schedule(void) {
    int cpu_id = get_cpu_id();
    cpu_sched_t *sched = &cpu_sched[cpu_id];
    
    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    
    task_t *prev = sched->current;
    task_t *next = pick_next_task(sched);
    
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    
    if (next) {
        next->state = TASK_RUNNING;
        sched->current = next;
        current_task = next;
    }
    
    sched->schedule_count++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    if (prev != next) {
        context_switch(prev, next);
    }
}

/* Yield CPU voluntarily */
void yield(void) {
    schedule();
}

/* Block current task */
void task_block(task_state_t state) {
    task_t *task = current_task;
    if (task) {
        task->state = state;
        schedule();
    }
}

/* Wake up a blocked task */
void task_wakeup(task_t *task) {
    if (task && task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        // Send reschedule IPI if on different CPU
        int task_cpu = __builtin_ctzll(task->cpu_affinity);
        if (task_cpu != get_cpu_id()) {
            send_ipi(1ULL << task_cpu, IPI_RESCHEDULE, 0);
        }
    }
}
