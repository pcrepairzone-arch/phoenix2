/*
 * mmio.c - MMIO Helpers for Phoenix RISC OS
 * Memory-mapped I/O access functions
 */

#include "kernel.h"
#include <stdint.h>

/* Identity mapping for now - works on Pi 4 */
void *ioremap(uint64_t phys_addr, size_t size)
{
    /* Pi 4 has identity mapped I/O region */
    (void)size;
    return (void *)phys_addr;
}

void iounmap(void *virt_addr)
{
    /* Identity mapped - nothing to do */
    (void)virt_addr;
}

/* MMIO read helpers */
uint8_t readb(const void *addr)
{
    return *(volatile uint8_t *)addr;
}

uint16_t readw(const void *addr)
{
    return *(volatile uint16_t *)addr;
}

uint32_t readl(const void *addr)
{
    return *(volatile uint32_t *)addr;
}

uint64_t readq(const void *addr)
{
    return *(volatile uint64_t *)addr;
}

/* MMIO write helpers */
void writeb(uint8_t val, void *addr)
{
    *(volatile uint8_t *)addr = val;
}

void writew(uint16_t val, void *addr)
{
    *(volatile uint16_t *)addr = val;
}

void writel(uint32_t val, void *addr)
{
    *(volatile uint32_t *)addr = val;
}

void writeq(uint64_t val, void *addr)
{
    *(volatile uint64_t *)addr = val;
}

/* DMA helpers */
uint64_t virt_to_phys(void *virt)
{
    /* Identity mapped — virtual == physical for all kernel memory.
     * Used for hardware DMA addresses (xHCI DCBAA, command/event rings).
     * NOTE: do NOT use this for VideoCore mailbox calls — use gpu_bus_addr()
     * instead, which adds the 0xC0000000 ARM→GPU bus alias.               */
    return (uint64_t)virt;
}

uint32_t gpu_bus_addr(void *virt)
{
    /* VideoCore/GPU mailbox buffers need the ARM→GPU bus alias:
     * GPU address = (ARM_phys & 0x3FFFFFFF) | 0xC0000000
     * The bottom 30 bits are the ARM physical address (first 1 GB only).  */
    return ((uint32_t)((uintptr_t)virt & 0x3FFFFFFFU)) | 0xC0000000U;
}

void *phys_to_virt(uint64_t phys)
{
    /* Identity mapped for now */
    return (void *)phys;
}
