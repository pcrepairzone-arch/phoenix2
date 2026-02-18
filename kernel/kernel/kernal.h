/*
 * kernel.h – Minimal Self-Contained Kernel Headers
 * Simplified to get a successful build
 * Author: Grok 4 – 06 Feb 2026
 */

#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>

/* Basic Types */
typedef int64_t ssize_t;
typedef int64_t off_t;
typedef int32_t pid_t;

/* Constants */
#define TASK_NAME_LEN       32
#define MAX_CPUS            8
#define MAX_FD              1024
#define PAGE_SIZE           4096
#define PAGE_MASK           (~(PAGE_SIZE - 1))

#define TASK_MIN_PRIORITY   0
#define TASK_MAX_PRIORITY   255

#define IPI_RESCHEDULE      2

/* Spinlock */
typedef struct {
    uint32_t value;
} spinlock_t;

#define SPINLOCK_INIT {0}

/* Task Structure */
typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef struct task task_t;

struct task {
    uint64_t        regs[31];
    uint64_t        sp_el0;
    uint64_t        elr_el1;
    uint64_t        spsr_el1;
    uint64_t        stack_top;
    task_t         *next;
    task_t         *prev;
    char            name[TASK_NAME_LEN];
    int             pid;
    int             priority;
    task_state_t    state;
    uint64_t        cpu_affinity;
    task_t         *parent;
    task_t        **children;
    int             child_count;
    spinlock_t      children_lock;
    int             exit_status;
    void           *pgtable_l0;
    void           *files[MAX_FD];   // Simplified
    void           *cwd;
    void           *signal_state;    // Simplified
};

/* Function Prototypes (Minimal) */
void kernel_main(uint64_t dtb_ptr);
void halt_system(void);
void debug_print(const char *fmt, ...);

void sched_init(void);
void schedule(void);
void yield(void);

task_t *task_create(const char *name, void (*entry)(void), int priority, uint64_t cpu_affinity);
int execve(const char *pathname, char *const argv[], char *const envp[]);

void mmu_init(void);
void mmu_init_task(task_t *task);

void timer_init(void);

extern task_t *current_task;
extern int nr_cpus;

#endif /* KERNEL_H */