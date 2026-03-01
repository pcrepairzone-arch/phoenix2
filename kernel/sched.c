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
/* Static idle task for CPU 0 — avoids kmalloc during early boot.
 * Using a static struct sidesteps any heap/spinlock issues while we
 * diagnose what's killing task_create.                               */
static task_t idle_task_storage[MAX_CPUS];
static uint8_t idle_stacks[MAX_CPUS][4096] __attribute__((aligned(16)));

void sched_init_cpu(int cpu_id) {
    cpu_sched_t *sched = &cpu_sched[cpu_id];

    debug_print("[SCHED] sched_init_cpu(%d) enter\n", cpu_id);

    task_t *task = &idle_task_storage[cpu_id];
    /* task_t is in BSS — already zeroed by boot.S */
    task->pid          = 0;
    task->priority     = TASK_MIN_PRIORITY;
    task->state        = TASK_READY;
    task->cpu_affinity = (1ULL << cpu_id);
    task->started      = 0;
    task->stack_top    = (uint64_t)(idle_stacks[cpu_id] + sizeof(idle_stacks[0]));
    task->elr_el1      = (uint64_t)idle_task_fn;
    task->spsr_el1     = 0;

    /* Copy name safely without strncpy_safe (avoids errno dependency) */
    task->name[0] = 'i'; task->name[1] = 'd'; task->name[2] = 'l';
    task->name[3] = 'e'; task->name[4] = '\0';

    sched->idle_task = task;
    enqueue_task(sched, task);

    debug_print("[SCHED] CPU %d idle task ready at stack_top=0x%llx\n",
                cpu_id, task->stack_top);
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

    /* ── Save previous task ─────────────────────────────────────────
     * Push callee-saved registers onto the current (prev) kernel stack
     * and record the SP.  schedule() is a normal C function so the
     * compiler already saved caller-saved regs; we only need x19-x30. */
    if (prev) {
        __asm__ volatile (
            "stp x19, x20, [sp, #-16]!\n"
            "stp x21, x22, [sp, #-16]!\n"
            "stp x23, x24, [sp, #-16]!\n"
            "stp x25, x26, [sp, #-16]!\n"
            "stp x27, x28, [sp, #-16]!\n"
            "stp x29, x30, [sp, #-16]!\n"
            "mov %0, sp\n"
            : "=r"(prev->stack_top)
            :: "memory"
        );
        prev->started = 1;
    }

    /* ── Restore / launch next task ─────────────────────────────────
     * First run: launch via eret at entry point (elr_el1).
     * Subsequent runs: restore callee-saved regs and ret into schedule(). */
    if (!next->started) {
        /* First run — jump to entry via eret (stays at EL1 since spsr=0) */
        __asm__ volatile (
            "mov  sp,      %0\n"   /* switch to next task's kernel stack  */
            "msr  sp_el0,  %1\n"   /* EL0 SP (0 for pure kernel tasks)    */
            "msr  elr_el1, %2\n"   /* entry point                         */
            "msr  spsr_el1,%3\n"   /* PSTATE: EL1h, all interrupts masked */
            "eret\n"
            :: "r"(next->stack_top), "r"(next->sp_el0),
               "r"(next->elr_el1),   "r"((uint64_t)0x3C5)
               /* spsr 0x3C5 = EL1h, DAIF all masked — task unmasks when ready */
            : "memory"
        );
        __builtin_unreachable();
    } else {
        /* Resume — restore callee-saved regs and ret back into schedule() */
        __asm__ volatile (
            "mov  sp,  %0\n"
            "ldp  x29, x30, [sp], #16\n"
            "ldp  x27, x28, [sp], #16\n"
            "ldp  x25, x26, [sp], #16\n"
            "ldp  x23, x24, [sp], #16\n"
            "ldp  x21, x22, [sp], #16\n"
            "ldp  x19, x20, [sp], #16\n"
            "ret\n"
            :: "r"(next->stack_top)
            : "memory"
        );
        __builtin_unreachable();
    }
}

/* Main scheduler function */
void schedule(void) {
    int cpu_id = get_cpu_id();
    cpu_sched_t *sched = &cpu_sched[cpu_id];
    
    spin_lock(&sched->lock);
    
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
    
    spin_unlock(&sched->lock);
    
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
