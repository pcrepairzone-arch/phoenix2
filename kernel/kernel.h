/*
 * kernel.h – Complete Self-Contained Kernel Headers for RISC OS Phoenix
 * Single extern for cpu_sched, all prototypes included
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

#define KERNEL_VIRT_BASE    0xFFFF000000000000ULL
#define USER_STACK_SIZE     (8 * 1024 * 1024)

#define TASK_MIN_PRIORITY   0
#define TASK_MAX_PRIORITY   255

#define IPI_RESCHEDULE      2

/* ELF constants */
#define EI_CLASS            4
#define EI_DATA             5
#define ELFCLASS64          2
#define ELFDATA2LSB         1
#define ET_EXEC             2
#define EM_AARCH64          183
#define SELFMAG             4
#define ELFMAG              "\177ELF"

/* Protection flags */
#define PF_R                4
#define PF_W                2
#define PF_X                1

#define PROT_READ           1
#define PROT_WRITE          2
#define PROT_EXEC           4

#define SEEK_SET            0

/* Spinlock */
typedef struct {
    uint32_t value;
} spinlock_t;

#define SPINLOCK_INIT {0}

/* Signal State */
typedef struct signal_state {
    void (*handlers[32])(int);
    uint64_t pending;
    uint64_t blocked;
    uint64_t old_mask;
    uint64_t sigreturn_sp;
} signal_state_t;

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
    void           *files[MAX_FD];
    void           *cwd;
    signal_state_t  signal_state;
};

/* Scheduler structure */
typedef struct {
    task_t     *current;
    task_t     *idle_task;
    task_t     *runqueue_head;
    task_t     *runqueue_tail;
    spinlock_t  lock;
    int         cpu_id;
    uint64_t    schedule_count;
} cpu_sched_t;

extern cpu_sched_t cpu_sched[MAX_CPUS];   // SINGLE extern
extern int vl805_init(void);		// second extern

/* Function Prototypes */
void kernel_main(uint64_t dtb_ptr);
void debug_print(const char *fmt, ...);
void halt_system(void);

void sched_init(void);
void sched_init_cpu(int cpu_id);
void schedule(void);
void yield(void);
void task_block(task_state_t state);
void task_wakeup(task_t *task);
void enqueue_task(cpu_sched_t *sched, task_t *task);

/* Spinlock functions */
void spinlock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags);
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);

task_t *task_create(const char *name, void (*entry)(void), int priority, uint64_t cpu_affinity);
int fork(void);
int execve(const char *pathname, char *const argv[], char *const envp[]);
pid_t wait(int *wstatus);
pid_t waitpid(pid_t pid, int *wstatus, int options);

void mmu_init(void);
void mmu_init_task(task_t *task);
int mmu_map(task_t *task, uint64_t virt, uint64_t size, int prot, int guard);
int mmu_duplicate_pagetable(task_t *parent, task_t *child);
void mmu_free_usermemory(task_t *task);
void mmu_free_pagetable(task_t *task);

void timer_init(void);
void timer_init_cpu(void);
void timer_tick(void);
void irq_init(void);

void device_tree_parse(uint64_t dtb_ptr);
int detect_nr_cpus(void);
int get_cpu_id(void);
void filecore_init(void);

void *kcalloc(size_t nmemb, size_t size);
void kfree(void *ptr);
void *kmalloc(size_t size);
void heap_stats(void);

/* String functions (minimal implementations) */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

void send_ipi(uint64_t target_cpus, int ipi_id, uint64_t arg);

int Wimp_Poll(int mask, void *event);
void *wimp_create_window(void *def);
void wimp_redraw_request(void *win, void *clip);

void init_process(void);
void wimp_task(void);
void paint_task(void);
void netsurf_task(void);

extern task_t *current_task;
extern int nr_cpus;

#endif /* KERNEL_H */
/* Memory management */
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void kfree(void *ptr);

void heap_stats(void);

/* MMIO helpers */
void *ioremap(uint64_t phys_addr, size_t size);
void iounmap(void *virt_addr);
uint8_t readb(const void *addr);
uint16_t readw(const void *addr);
uint32_t readl(const void *addr);
uint64_t readq(const void *addr);
void writeb(uint8_t val, void *addr);
void writew(uint16_t val, void *addr);
void writel(uint32_t val, void *addr);
void writeq(uint64_t val, void *addr);
uint64_t virt_to_phys(void *virt);
void *phys_to_virt(uint64_t phys);

/* IRQ system */
#include "irq.h"

/* PCI system */
#include "pci.h"
