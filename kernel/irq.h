/*
 * irq.h – IRQ Headers for RISC OS Phoenix
 * Defines IRQ constants, handler types, and functions
 * Author: R Andrews  – 10 Dec 2025
 */

#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

#define TIMER_IRQ_VECTOR  0x1F  // Example vector for timer
#define MMC_IRQ_VECTOR    0x20  // MMC/SD interrupt
#define NVME_IRQ_BASE     0x30  // Base for NVMe MSIX vectors

/* BCM2711 PCIe / VL805 xHCI interrupt
 * PCIe MSI → GIC SPI 148 → INTID 180 (0xB4)
 * Use with irq_set_handler(PCIE_MSI_IRQ_VECTOR, handler, data)
 * and irq_unmask(PCIE_MSI_IRQ_VECTOR) */
#define PCIE_MSI_IRQ_VECTOR   180   /* GIC INTID for PCIe MSI (SPI 148) */
#define PCIE_INTX_IRQ_VECTOR  175   /* GIC INTID for PCIe INTx Pin A (SPI 143) — fallback */

/* GIC-400 distributor base — needed for the pending check below */
#define GICD_BASE_ADDR    0xFF841000ULL
#define GICD_ISPENDR(n)   (0x200 + (n)*4)

/*
 * xhci_irq_pending — non-destructive check if xHCI MSI is pending at the GIC.
 *
 * Reads GICD_ISPENDR[5] bit 20 (INTID 180 = SPI 148).
 * Returns 1 if the VL805 has fired an MSI (CCE waiting in evt_ring).
 * Returns 0 if not.
 *
 * Safe to call in a tight polling loop — one register read, no side effects.
 * Works even when CPU IRQ delivery is masked (DAIF.I=1).
 */
static inline int xhci_irq_pending(void) {
    uint32_t val = *(volatile uint32_t *)(GICD_BASE_ADDR + GICD_ISPENDR(5));
    return (val >> 20) & 1;   /* INTID 180 = register [5] bit 20 */
}

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