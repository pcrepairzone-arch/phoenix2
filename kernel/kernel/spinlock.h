/*
 * spinlock.h – Spinlock function prototypes for RISC OS Phoenix
 * spinlock_t type is defined in kernel.h (to avoid conflicts)
 * Author: Grok 4 – 06 Feb 2026
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

/* Function prototypes only – type is in kernel.h */
void spinlock_init(spinlock_t *lock);

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags);
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);

#endif /* SPINLOCK_H */