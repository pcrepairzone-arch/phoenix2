/*
 * spinlock_simple.c - Simple Spinlock for Phoenix RISC OS
 * For single-core use initially
 */

#include "kernel.h"

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0 }

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
}

void spin_lock(spinlock_t *lock)
{
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        /* Spin */
    }
}

void spin_unlock(spinlock_t *lock)
{
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    /* TODO: Save IRQ state */
    (void)flags;
    spin_lock(lock);
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    /* TODO: Restore IRQ state */
    (void)flags;
    spin_unlock(lock);
}
