/*
 * signal.c – POSIX signals for RISC OS Phoenix
 * Supports signal, sigaction, kill, sigreturn
 * Author: R Andrews Grok 4 – 26 Nov 2025
 * Updated: 15 Feb 2026 - Fixed conflicts and missing constants
 */

#include "kernel.h"
#include "spinlock.h"
#include <stdint.h>

#define NSIG            32
#define SIG_BLOCK       1
#define SIG_UNBLOCK     2
#define SIG_SETMASK     3

#define SIG_DFL         ((void(*)(int))0)
#define SIG_IGN         ((void(*)(int))1)
#define SIG_ERR         ((void(*)(int))-1)

/* Signal numbers */
#define SIGKILL         9
#define SIGSTOP         19

/* Signal action flags */
#define SA_SIGINFO      4

/* Signal action structure */
typedef struct {
    void (*sa_handler)(int);
    uint64_t sa_mask;
    int      sa_flags;
} sigaction_t;

/* signal() – simple interface */
void (*signal(int sig, void (*handler)(int)))(int)
{
    if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return SIG_ERR;

    task_t *task = current_task;
    void (*old)(int) = task->signal_state.handlers[sig];
    task->signal_state.handlers[sig] = handler;

    return old;
}

/* sigaction() – full control */
int sigaction(int sig, const sigaction_t *act, sigaction_t *oldact)
{
    if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -1;

    task_t *task = current_task;

    if (oldact) {
        oldact->sa_handler = task->signal_state.handlers[sig];
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }

    if (act) {
        if (act->sa_flags & SA_SIGINFO)
            task->signal_state.handlers[sig] = act->sa_handler;
        else
            task->signal_state.handlers[sig] = act->sa_handler;
    }

    return 0;
}

/* Stub for finding task by pid */
static task_t *find_task_by_pid(int pid) {
    // TODO: Implement proper task lookup
    (void)pid;
    return current_task;  // Stub
}

/* kill() – send signal to task */
int kill(int pid, int sig)
{
    task_t *target = find_task_by_pid(pid);
    if (!target) return -1;

    unsigned long flags;
    spin_lock_irqsave(&target->children_lock, &flags);
    target->signal_state.pending |= (1ULL << sig);
    spin_unlock_irqrestore(&target->children_lock, flags);

    task_wakeup(target);
    return 0;
}

/* Deliver pending signals */
static void deliver_signals(void)
{
    task_t *task = current_task;
    if (!task) return;

    uint64_t pending = task->signal_state.pending & ~task->signal_state.blocked;
    if (!pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1ULL << sig))) continue;

        task->signal_state.pending &= ~(1ULL << sig);

        void (*handler)(int) = task->signal_state.handlers[sig];
        if (handler == SIG_IGN) continue;
        if (handler == SIG_DFL) {
            // Default action - terminate for now
            debug_print("Task %s received signal %d - terminating\n", task->name, sig);
            // Exit stub - just block the task
            task->state = TASK_ZOMBIE;
            schedule();
            continue;
        }

        // Save context for sigreturn
        task->signal_state.old_mask = task->signal_state.blocked;
        task->signal_state.sigreturn_sp = task->sp_el0;

        // Execute handler
        handler(sig);

        // Block signal during handler
        task->signal_state.blocked |= (1ULL << sig);
    }
}

/* sigreturn() – restore context after handler */
int sigreturn(void)
{
    task_t *task = current_task;
    task->signal_state.blocked = task->signal_state.old_mask;
    task->sp_el0 = task->signal_state.sigreturn_sp;
    return 0;
}

/* Handle synchronous exceptions as signals */
void handle_exception(uint64_t esr, uint64_t far)
{
    task_t *task = current_task;
    if (!task) {
        debug_print("Exception with no current task! ESR=0x%llx FAR=0x%llx\n", esr, far);
        halt_system();
        return;
    }

    // Determine signal type from ESR
    int sig = 11; // SIGSEGV by default

    // Send signal to task
    task->signal_state.pending |= (1ULL << sig);
    deliver_signals();
}

/* Register default signal handlers */
void register_default_handlers(void)
{
    // Default handlers are already SIG_DFL (NULL)
    debug_print("Signal handling initialized\n");
}
