/*
 * task.c – Task management for RISC OS Phoenix
 * Author:  anrews Grok 4 – 06 Feb 2026
 * Updated: 15 Feb 2026 - Added error handling
 */

#include "kernel.h"
#include "mmu.h"
#include "vfs.h"
#include "elf64.h"
#include "errno.h"
#include "error.h"

#define KERNEL_STACK_SIZE   (32 * 1024)
#define USER_STACK_SIZE     (8 * 1024 * 1024)

static volatile int next_pid = 1;

task_t *task_create(const char *name, void (*entry)(void), int priority, uint64_t cpu_affinity)
{
    extern void uart_puts(const char *s);
    uart_puts("[TC] enter\r\n");

    /* Validate parameters */
    if (!name || !entry) {
        errno = EINVAL;
        debug_print("ERROR: task_create - invalid parameters (name=%p, entry=%p)\n", 
                   name, entry);
        return NULL;
    }
    uart_puts("[TC] kmalloc task\r\n");

    task_t *task = kmalloc(sizeof(task_t));
    if (!task) {
        errno = ENOMEM;
        debug_print("ERROR: task_create - failed to allocate task structure\n");
        return NULL;
    }
    uart_puts("[TC] kmalloc kstack\r\n");

    uint8_t *kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!kernel_stack) {
        errno = ENOMEM;
        debug_print("ERROR: task_create - failed to allocate kernel stack\n");
        kfree(task);
        return NULL;
    }
    uart_puts("[TC] user stack check\r\n");

    uint8_t *user_stack = NULL;
    /* Only allocate a user stack for tasks that will run at EL0.
     * Kernel tasks (idle, etc.) have their entry point inside the kernel
     * image (<1 GB physical), never drop to EL0, and don't need 8 MB of
     * user-mode stack. This saves memory and avoids touching a huge region
     * before the page allocator exists.                                    */
    if ((uint64_t)entry >= 0x40000000ULL) {
        user_stack = kmalloc(USER_STACK_SIZE);
        if (!user_stack) {
            errno = ENOMEM;
            debug_print("ERROR: task_create - failed to allocate user stack\n");
            kfree(kernel_stack);
            kfree(task);
            return NULL;
        }
    }

    uart_puts("[TC] memset task\r\n");
    memset(task, 0, sizeof(task_t));
    uart_puts("[TC] strncpy\r\n");
    strncpy_safe(task->name, name, TASK_NAME_LEN);
    task->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_SEQ_CST);
    task->priority = priority;
    task->state = TASK_READY;
    task->cpu_affinity = cpu_affinity ? cpu_affinity : (1ULL << get_cpu_id());

    task->stack_top = (uint64_t)(kernel_stack + KERNEL_STACK_SIZE);
    task->sp_el0 = user_stack ? (uint64_t)(user_stack + USER_STACK_SIZE) : 0;

    memset(task->regs, 0, sizeof(task->regs));
    task->regs[0] = 0;
    task->elr_el1 = (uint64_t)entry;
    task->spsr_el1 = 0;

    uart_puts("[TC] mmu_init_task\r\n");
    mmu_init_task(task);

    uart_puts("[TC] enqueue\r\n");
    int cpu = (int)__builtin_ctzll(task->cpu_affinity);
    if (cpu >= MAX_CPUS) cpu = 0;   /* guard against affinity=0 or out-of-range */
    cpu_sched_t *sched = &cpu_sched[cpu];
    spin_lock(&sched->lock);
    enqueue_task(sched, task);
    spin_unlock(&sched->lock);

    uart_puts("[TC] done\r\n");

    return task;
}

int fork(void)
{
    task_t *parent = current_task;
    task_t *child = NULL;
    uint8_t *new_stack = NULL;
    int child_pid;

    /* Allocate child task structure */
    child = kmalloc(sizeof(task_t));
    if (!child) {
        errno = ENOMEM;
        debug_print("ERROR: fork - failed to allocate child task\n");
        return -1;
    }

    child_pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_SEQ_CST);

    /* Copy parent task structure */
    memcpy(child, parent, sizeof(task_t));
    child->pid = child_pid;
    child->parent = parent;
    child->state = TASK_READY;
    strncpy_safe(child->name, parent->name, TASK_NAME_LEN);
    strncat_safe(child->name, "+", TASK_NAME_LEN);

    /* Duplicate page table with COW */
    if (mmu_duplicate_pagetable(parent, child) != 0) {
        debug_print("ERROR: fork - failed to duplicate page table\n");
        goto fail_free_child;
    }

    child->regs[0] = 0;

    /* Allocate new kernel stack */
    new_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!new_stack) {
        errno = ENOMEM;
        debug_print("ERROR: fork - failed to allocate kernel stack\n");
        goto fail_free_pagetable;
    }
    
    memcpy(new_stack, (void*)(parent->stack_top - KERNEL_STACK_SIZE), KERNEL_STACK_SIZE);
    child->stack_top = (uint64_t)(new_stack + KERNEL_STACK_SIZE);

    uint64_t offset = parent->sp_el0 - (parent->stack_top - KERNEL_STACK_SIZE);
    child->sp_el0 = child->stack_top - offset;

    /* Add to scheduler queue */
    int cpu = get_cpu_id();
    cpu_sched_t *sched = &cpu_sched[cpu];
    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    enqueue_task(sched, child);
    spin_unlock_irqrestore(&sched->lock, flags);

    debug_print("Fork successful: parent PID=%d, child PID=%d\n", parent->pid, child_pid);
    return child_pid;

fail_free_pagetable:
    mmu_free_pagetable(child);
fail_free_child:
    kfree(child);
    return -1;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    task_t *task = current_task;
    file_t *file = NULL;
    void *page = NULL;
    
    /* Validate parameters */
    if (!pathname || !argv || !envp) {
        errno = EINVAL;
        debug_print("ERROR: execve - invalid parameters\n");
        return -1;
    }
    
    file = vfs_open(pathname, O_RDONLY);
    if (!file) {
        errno = ENOENT;
        debug_print("ERROR: execve - failed to open '%s'\n", pathname);
        return -1;
    }

    Elf64_Ehdr ehdr;
    ssize_t read_size = vfs_read(file, &ehdr, sizeof(ehdr));
    if (read_size != sizeof(ehdr)) {
        errno = EIO;
        debug_print("ERROR: execve - failed to read ELF header\n");
        goto fail;
    }

    /* Validate ELF header */
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr.e_machine != EM_AARCH64 ||
        ehdr.e_type != ET_EXEC) {
        errno = ENOEXEC;
        debug_print("ERROR: execve - invalid ELF format\n");
        goto fail;
    }

    mmu_free_usermemory(task);

    uint64_t entry = ehdr.e_entry;
    uint64_t phoff = ehdr.e_phoff;
    int phnum = ehdr.e_phnum;

    /* Load program segments */
    for (int i = 0; i < phnum; i++) {
        Elf64_Phdr phdr;
        vfs_seek(file, phoff + i * ehdr.e_phentsize, SEEK_SET);
        read_size = vfs_read(file, &phdr, sizeof(phdr));
        if (read_size != sizeof(phdr)) {
            errno = EIO;
            debug_print("ERROR: execve - failed to read program header %d\n", i);
            goto fail;
        }

        if (phdr.p_type == PT_LOAD) {
            size_t memsz = (phdr.p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
            page = kmalloc(memsz);
            if (!page) {
                errno = ENOMEM;
                debug_print("ERROR: execve - failed to allocate memory for segment %d\n", i);
                goto fail;
            }

            vfs_seek(file, phdr.p_offset, SEEK_SET);
            read_size = vfs_read(file, page, phdr.p_filesz);
            if (read_size != phdr.p_filesz) {
                errno = EIO;
                debug_print("ERROR: execve - failed to read segment %d\n", i);
                kfree(page);
                page = NULL;
                goto fail;
            }

            memset((char*)page + phdr.p_filesz, 0, phdr.p_memsz - phdr.p_filesz);

            int prot = 0;
            if (phdr.p_flags & PF_R) prot |= PROT_READ;
            if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
            if (phdr.p_flags & PF_X) prot |= PROT_EXEC;

            if (mmu_map(task, phdr.p_vaddr, phdr.p_memsz, prot, 0) != 0) {
                errno = ENOMEM;
                debug_print("ERROR: execve - failed to map segment %d\n", i);
                kfree(page);
                page = NULL;
                goto fail;
            }
            
            page = NULL; /* Ownership transferred to page table */
        }
    }

    vfs_close(file);
    file = NULL;

    // Stack setup with safe string operations
    uint64_t sp = 0x0000fffffffff000ULL;
    int argc = 0, envc = 0;
    
    /* Count arguments and environment variables safely */
    while (argv && argv[argc] && argc < 1024) argc++;
    while (envp && envp[envc] && envc < 1024) envc++;

    sp -= 8 * (argc + envc + 2);
    uint64_t *arg_env = (uint64_t*)sp;

    char *str_ptr = (char*)(sp - (argc + envc + 2) * 256);
    uint64_t *argv_ptrs = arg_env;
    uint64_t *envp_ptrs = arg_env + argc + 1;

    /* Copy arguments with bounds checking */
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            strncpy_safe(str_ptr, argv[i], 256);
            argv_ptrs[i] = (uint64_t)str_ptr;
            str_ptr += strnlen(argv[i], 256) + 1;
        }
    }
    argv_ptrs[argc] = 0;

    /* Copy environment with bounds checking */
    for (int i = 0; i < envc; i++) {
        if (envp[i]) {
            strncpy_safe(str_ptr, envp[i], 256);
            envp_ptrs[i] = (uint64_t)str_ptr;
            str_ptr += strnlen(envp[i], 256) + 1;
        }
    }
    envp_ptrs[envc] = 0;

    mmu_map(task, sp & PAGE_MASK, PAGE_SIZE * 8, PROT_READ | PROT_WRITE, 0);

    task->sp_el0 = sp;
    task->elr_el1 = entry;
    task->spsr_el1 = 0;

    task->regs[0] = argc;
    task->regs[1] = (uint64_t)argv_ptrs;
    task->regs[2] = (uint64_t)envp_ptrs;

    debug_print("execve: '%s' loaded at 0x%llx (argc=%d)\n", pathname, entry, argc);

    return 0;

fail:
    if (page) kfree(page);
    if (file) vfs_close(file);
    return -1;
}

pid_t wait(int *wstatus)
{
    return waitpid(-1, wstatus, 0);
}

pid_t waitpid(pid_t pid, int *wstatus, int options)
{
    // Stub – basic implementation
    return -1;
}