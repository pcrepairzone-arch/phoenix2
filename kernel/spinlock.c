/*
 * spinlock.c – Spinlock Implementation for RISC OS Phoenix
 * Simple ticket spinlock with interrupt save/restore for SMP safety
 * Author: Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "spinlock.h"

/* Acquire spinlock and save current interrupt state (DAIF) */
void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    unsigned long temp;

    /* Save current interrupt state */
    __asm__ volatile ("mrs %0, daif" : "=r"(*flags));

    /* Disable interrupts (IRQ and FIQ) */
    __asm__ volatile ("msr daifset, #3" ::: "memory");

    /* Acquire the spinlock using test-and-set */
    do {
        __asm__ volatile (
            "ldaxr %w0, [%1]\n"
            "cmp %w0, #0\n"
            "b.ne 1f\n"
            "mov %w0, #1\n"
            "stxr %w2, %w0, [%1]\n"
            "1:"
            : "=&r"(temp), "+r"(lock->value), "=&r"(temp)
            :
            : "memory"
        );
    } while (temp != 0);
}

/* Release spinlock and restore previous interrupt state */
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    /* Release the lock */
    __asm__ volatile (
        "stlr wzr, [%0]"
        : : "r"(&lock->value) : "memory"
    );

    /* Restore previous interrupt state */
    __asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
}

/* Simple spinlock acquire (no interrupt save) */
void spin_lock(spinlock_t *lock)
{
    unsigned long temp;

    do {
        __asm__ volatile (
            "ldaxr %w0, [%1]\n"
            "cmp %w0, #0\n"
            "b.ne 1f\n"
            "mov %w0, #1\n"
            "stxr %w2, %w0, [%1]\n"
            "1:"
            : "=&r"(temp), "+r"(lock->value), "=&r"(temp)
            :
            : "memory"
        );
    } while (temp != 0);
}

/* Simple spinlock release */
void spin_unlock(spinlock_t *lock)
{
    __asm__ volatile (
        "stlr wzr, [%0]"
        : : "r"(&lock->value) : "memory"
    );
}