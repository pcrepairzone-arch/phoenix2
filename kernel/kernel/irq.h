/*
 * irq.h – IRQ Headers for RISC OS Phoenix
 * Defines IRQ constants, handler types, and functions
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

#define TIMER_IRQ_VECTOR  0x1F  // Example vector for timer
#define MMC_IRQ_VECTOR    0x20  // MMC/SD interrupt
#define NVME_IRQ_BASE     0x30  // Base for NVMe MSIX vectors

typedef void (*irq_handler_t)(int vector, void *private);

void irq_init(void);
void irq_set_handler(int vector, irq_handler_t handler, void *private);
void irq_unmask(int vector);
void irq_eoi(int vector);

void send_ipi(uint64_t target_cpus, int ipi_id, uint64_t arg);
void ipi_handler(int ipi_id, uint64_t arg);

#define IPI_TLB_SHOOTDOWN 1
#define IPI_RESCHEDULE    2

#endif /* IRQ_H */