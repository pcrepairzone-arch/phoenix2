/*
 * spinlock.c – Spinlock Implementation for RISC OS Phoenix
 * Simple test-and-set spinlock with interrupt save/restore for SMP safety
 * Author: Grok 4 – 06 Feb 2026
 * Fixed: 27 Feb 2026 - separate registers for ldaxr value and stxr status
 */

#include "kernel.h"
#include "spinlock.h"

/* Initialize spinlock */
void spinlock_init(spinlock_t *lock)
{
    lock->value = 0;
}

/* Acquire spinlock and save current interrupt state (DAIF) */
void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    /* Save current interrupt state then disable IRQ+FIQ */
    __asm__ volatile ("mrs %0, daif" : "=r"(*flags));
    __asm__ volatile ("msr daifset, #3" ::: "memory");

    /* Acquire spinlock: %w0=loaded value, %w1=stxr status (separate regs) */
    uint32_t loaded, status;
    do {
        __asm__ volatile (
            "ldaxr  %w0, [%2]\n"   /* exclusive load                     */
            "cbnz   %w0, 1f\n"     /* if non-zero: lock held, retry       */
            "mov    %w0, #1\n"     /* prepare value to store              */
            "stxr   %w1, %w0, [%2]\n" /* attempt store; status→%w1       */
            "b      2f\n"
            "1: mov %w1, #1\n"     /* lock held: force status=1 to retry  */
            "2:"
            : "=&r"(loaded), "=&r"(status)
            : "r"(&lock->value)
            : "memory"
        );
    } while (status != 0);
}

/* Release spinlock and restore previous interrupt state */
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    __asm__ volatile ("stlr wzr, [%0]" :: "r"(&lock->value) : "memory");
    __asm__ volatile ("msr daif, %0"   :: "r"(flags)        : "memory");
}

/* Simple spinlock acquire (no interrupt save) */
void spin_lock(spinlock_t *lock)
{
    uint32_t loaded, status;
    do {
        __asm__ volatile (
            "ldaxr  %w0, [%2]\n"
            "cbnz   %w0, 1f\n"
            "mov    %w0, #1\n"
            "stxr   %w1, %w0, [%2]\n"
            "b      2f\n"
            "1: mov %w1, #1\n"     /* lock held: status=1 → retry */
            "2:"
            : "=&r"(loaded), "=&r"(status)
            : "r"(&lock->value)
            : "memory"
        );
    } while (status != 0);
}

/* Simple spinlock release */
void spin_unlock(spinlock_t *lock)
{
    __asm__ volatile ("stlr wzr, [%0]" :: "r"(&lock->value) : "memory");
}