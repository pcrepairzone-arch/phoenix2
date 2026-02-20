/*
 * irq.c - IRQ Handler for Phoenix RISC OS
 * Adapted from GitHub repo for Pi 4
 */

#include "kernel.h"
#include "irq.h"

#define MAX_IRQ_VECTORS 256

typedef struct {
    irq_handler_t handler;
    void *private;
    int enabled;
} irq_entry_t;

static irq_entry_t irq_table[MAX_IRQ_VECTORS];

void irq_init(void)
{
    debug_print("[IRQ] Initializing interrupt system\n");
    
    /* Clear all handlers */
    for (int i = 0; i < MAX_IRQ_VECTORS; i++) {
        irq_table[i].handler = NULL;
        irq_table[i].private = NULL;
        irq_table[i].enabled = 0;
    }
    
    /* TODO: Set up GIC (Generic Interrupt Controller) */
    /* For now, basic setup */
    
    debug_print("[IRQ] Interrupt system ready\n");
}

void irq_set_handler(int vector, irq_handler_t handler, void *private)
{
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) {
        debug_print("[IRQ] Invalid vector %d\n", vector);
        return;
    }
    
    irq_table[vector].handler = handler;
    irq_table[vector].private = private;
    
    debug_print("[IRQ] Handler registered for vector %d\n", vector);
}

void irq_unmask(int vector)
{
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) return;
    
    irq_table[vector].enabled = 1;
    
    /* TODO: Unmask in GIC */
}

void irq_mask(int vector)
{
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) return;
    
    irq_table[vector].enabled = 0;
    
    /* TODO: Mask in GIC */
}

void irq_eoi(int vector)
{
    /* TODO: Send End-of-Interrupt to GIC */
    (void)vector;
}

/* Called from exception handler */
void irq_dispatch(int vector)
{
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) {
        debug_print("[IRQ] Invalid IRQ vector %d\n", vector);
        return;
    }
    
    irq_entry_t *entry = &irq_table[vector];
    
    if (entry->handler && entry->enabled) {
        entry->handler(vector, entry->private);
    } else {
        debug_print("[IRQ] Spurious IRQ %d\n", vector);
    }
    
    irq_eoi(vector);
}

/* IPI support (for SMP) */
void send_ipi(uint64_t target_cpus, int ipi_id, uint64_t arg)
{
    /* TODO: Implement IPI for multi-core */
    (void)target_cpus;
    (void)ipi_id;
    (void)arg;
}

void ipi_handler(int ipi_id, uint64_t arg)
{
    /* TODO: Handle IPI */
    (void)ipi_id;
    (void)arg;
}
