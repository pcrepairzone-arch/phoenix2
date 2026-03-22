/*
 * irq.c - BCM2711 GIC-400 Interrupt Controller Driver for Phoenix RISC OS
 *
 * BCM2711 uses an ARM GIC-400 (GICv2) interrupt controller.
 *
 * ── GIC-400 MEMORY MAP (BCM2711) ────────────────────────────────────────────
 *   GIC base:        0xFF840000
 *   Distributor:     0xFF841000  (GICD — global, configures all SPIs)
 *   CPU Interface:   0xFF842000  (GICC — per-CPU, handles pending IRQs)
 *
 * ── HOW GICv2 WORKS ─────────────────────────────────────────────────────────
 *   The GIC has three interrupt types:
 *     SGI  (Software Generated Interrupts): IRQ 0–15   — IPI between CPUs
 *     PPI  (Private Peripheral Interrupts): IRQ 16–31  — per-CPU (timers etc)
 *     SPI  (Shared Peripheral Interrupts):  IRQ 32–255 — peripheral devices
 *
 *   Initialisation sequence:
 *     1. GICD_CTLR  = 1   Enable distributor (forwards SPIs to CPU interfaces)
 *     2. GICC_PMR   = 0xFF  Priority mask: accept all priorities (0=highest,255=lowest)
 *     3. GICC_CTLR  = 1   Enable CPU interface (forwards pending IRQs to CPU)
 *     4. Per IRQ: GICD_ISENABLER[n] bit = 1  (enable that SPI)
 *     5. CPU daifclr to unmask IRQ at processor level
 *
 * ── BCM2711 USB / VL805 IRQ ─────────────────────────────────────────────────
 *   VL805 xHCI asserts PCIe interrupt → GIC SPI 75 (IRQ 75+32=107 in GIC)
 *   Actually on BCM2711 the PCIe MSI interrupt lines map to:
 *     PCIe L2 interrupt controller → GIC SPI 48 for pcie0_aer
 *   The VL805 uses MSI-X; on BCM2711 MSI is routed to GIC SPI 48+x.
 *   For polling-based operation (current xHCI implementation), the GIC
 *   only needs basic initialisation — the USB driver polls the event ring.
 *   Interrupt-driven USB transfers require registering handler on SPI 48.
 *
 * ── CURRENT BOOT STATUS ─────────────────────────────────────────────────────
 *   The xHCI driver currently operates in pure polling mode (evt_ring_poll).
 *   GIC initialisation here establishes the hardware baseline. Class drivers
 *   (HID, mass storage) will register handlers via irq_set_handler() once
 *   interrupt-driven transfers are implemented.
 */

#include "kernel.h"
#include "irq.h"

/* ── GIC-400 base addresses ─────────────────────────────────────────────── */
#define GIC_BASE        0xFF840000ULL
#define GICD_BASE       (GIC_BASE + 0x1000ULL)   /* Distributor */
#define GICC_BASE       (GIC_BASE + 0x2000ULL)   /* CPU Interface */

/* ── GIC Distributor registers (GICD) — offsets from GICD_BASE ─────────── */
#define GICD_CTLR           0x000   /* Distributor Control Register         */
#define GICD_TYPER          0x004   /* Interrupt Controller Type Register   */
#define GICD_IGROUPR(n)     (0x080 + (n)*4)  /* Interrupt Group (n=IRQ/32)  */
#define GICD_ISENABLER(n)   (0x100 + (n)*4)  /* Interrupt Set-Enable        */
#define GICD_ICENABLER(n)   (0x180 + (n)*4)  /* Interrupt Clear-Enable      */
#define GICD_ICPENDR(n)     (0x280 + (n)*4)  /* Interrupt Clear-Pending     */
#define GICD_IPRIORITYR(n)  (0x400 + (n)*4)  /* Interrupt Priority          */
#define GICD_ITARGETSR(n)   (0x800 + (n)*4)  /* Interrupt Processor Targets */
#define GICD_ICFGR(n)       (0xC00 + (n)*4)  /* Interrupt Configuration     */

/* ── GIC CPU Interface registers (GICC) — offsets from GICC_BASE ────────── */
#define GICC_CTLR           0x000   /* CPU Interface Control Register       */
#define GICC_PMR            0x004   /* Interrupt Priority Mask Register     */
#define GICC_BPR            0x008   /* Binary Point Register                */
#define GICC_IAR            0x00C   /* Interrupt Acknowledge Register       */
#define GICC_EOIR           0x010   /* End of Interrupt Register            */
#define GICC_RPR            0x014   /* Running Priority Register            */
#define GICC_HPPIR          0x018   /* Highest Priority Pending IRQ         */

/* ── IRQ table ──────────────────────────────────────────────────────────── */
#define MAX_IRQ_VECTORS     256

typedef struct {
    irq_handler_t handler;
    void         *private;
    int           enabled;
} irq_entry_t;

static irq_entry_t irq_table[MAX_IRQ_VECTORS];

/* ── GIC register accessors ─────────────────────────────────────────────── */

static inline void gicd_write(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(GICD_BASE + offset) = val;
    asm volatile("dsb sy" ::: "memory");
}

static inline uint32_t gicd_read(uint32_t offset) {
    return *(volatile uint32_t *)(GICD_BASE + offset);
}

static inline void gicc_write(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(GICC_BASE + offset) = val;
    asm volatile("dsb sy" ::: "memory");
}

static inline uint32_t gicc_read(uint32_t offset) {
    return *(volatile uint32_t *)(GICC_BASE + offset);
}

/* ── GIC Distributor initialisation ─────────────────────────────────────── */

/*
 * gicd_init — initialise the GIC-400 distributor.
 *
 * Disables the distributor first, clears all pending/enabled SPIs,
 * sets all priorities to 0xA0 (mid-priority), targets all IRQs to CPU 0,
 * then re-enables.
 *
 * Must be called once, on CPU 0, before any CPU interface init.
 */
static void gicd_init(void) {
    /* Read the number of supported interrupt lines */
    uint32_t typer    = gicd_read(GICD_TYPER);
    uint32_t it_lines = (typer & 0x1F) + 1;   /* ITLinesNumber: (n+1)*32 IRQs */
    uint32_t num_irqs = it_lines * 32;
    if (num_irqs > 1020) num_irqs = 1020;

    debug_print("[GIC] GICD: %u interrupt lines (%u IRQs)\n", it_lines, num_irqs);

    /* Step 1: Disable distributor during configuration */
    gicd_write(GICD_CTLR, 0);

    /* Step 2: Clear all enable bits (disable every SPI) */
    for (uint32_t i = 0; i < it_lines; i++)
        gicd_write(GICD_ICENABLER(i), 0xFFFFFFFF);

    /* Step 3: Clear all pending bits */
    for (uint32_t i = 0; i < it_lines; i++)
        gicd_write(GICD_ICPENDR(i), 0xFFFFFFFF);

    /* Step 4: Set all interrupts to Group 0 (IRQ, not FIQ) */
    for (uint32_t i = 0; i < it_lines; i++)
        gicd_write(GICD_IGROUPR(i), 0x00000000);

    /* Step 5: Set all interrupt priorities to 0xA0 (mid-level).
     * GICv2 uses 8-bit priority fields packed 4-per-register.
     * Lower value = higher priority. 0xA0 leaves room for
     * high-priority (0x00–0x7F) and low-priority (0xC0–0xFF). */
    for (uint32_t i = 0; i < num_irqs / 4; i++)
        gicd_write(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* Step 6: Route all SPIs (IRQ 32+) to CPU 0 (target bit 0 = CPU 0) */
    for (uint32_t i = 8; i < num_irqs / 4; i++)
        gicd_write(GICD_ITARGETSR(i), 0x01010101);

    /* Step 7: Set all SPIs as level-sensitive (0=level, 1=edge) */
    for (uint32_t i = 2; i < it_lines * 2; i++)
        gicd_write(GICD_ICFGR(i), 0x00000000);

    /* Step 8: Re-enable distributor */
    gicd_write(GICD_CTLR, 1);

    debug_print("[GIC] GICD enabled\n");
}

/* ── GIC CPU Interface initialisation ───────────────────────────────────── */

/*
 * gicc_init — initialise the GIC-400 CPU interface for the current CPU.
 *
 * Must be called on each CPU that will receive interrupts.
 * For Phoenix, called on CPU 0 during boot.
 */
static void gicc_init(void) {
    /* Disable CPU interface during setup */
    gicc_write(GICC_CTLR, 0);

    /* Priority mask: 0xFF accepts all priorities (anything below 0xFF runs).
     * GIC compares RPR (running priority) with PMR; IRQ fires if pending
     * priority is lower numerically (= higher urgency) than PMR. */
    gicc_write(GICC_PMR, 0xFF);

    /* Binary Point: 0 = all priority bits used for preemption */
    gicc_write(GICC_BPR, 0x00);

    /* Enable CPU interface */
    gicc_write(GICC_CTLR, 1);

    debug_print("[GIC] GICC enabled on CPU 0, PMR=0xFF\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * irq_init — initialise the complete interrupt subsystem.
 *
 * Call order: irq_init() → irq_set_handler(n, fn, data) → irq_unmask(n)
 * Then enable IRQs at CPU level with: asm volatile("msr daifclr, #2")
 */
void irq_init(void) {
    debug_print("[IRQ] Initialising interrupt system (BCM2711 GIC-400)\n");

    /* Clear software handler table */
    for (int i = 0; i < MAX_IRQ_VECTORS; i++) {
        irq_table[i].handler = NULL;
        irq_table[i].private = NULL;
        irq_table[i].enabled = 0;
    }

    /* Initialise GIC hardware */
    gicd_init();
    gicc_init();

    debug_print("[IRQ] Interrupt system ready (GIC-400 @ 0xFF840000)\n");
    debug_print("[IRQ] NOTE: CPU IRQ mask still set (DAIF.I=1).\n");
    debug_print("[IRQ]       Call: asm volatile(\"msr daifclr, #2\") to unmask.\n");
    debug_print("[IRQ] NOTE: xHCI currently uses polling — no GIC IRQ needed yet.\n");
    debug_print("[IRQ]       When interrupt-driven USB is implemented, register\n");
    debug_print("[IRQ]       irq_set_handler(SPI_PCIE_MSI, xhci_irq_handler, ctrl)\n");
    debug_print("[IRQ]       then irq_unmask(SPI_PCIE_MSI) to activate.\n");
}

/*
 * irq_set_handler — register a C handler for a GIC IRQ number.
 *
 * @vector  GIC IRQ number (0–255). SPI IRQs start at 32.
 * @handler Called from irq_dispatch() when this IRQ fires.
 * @private Passed unchanged to handler as second argument.
 *
 * Does not enable/unmask the IRQ — call irq_unmask() separately.
 */
void irq_set_handler(int vector, irq_handler_t handler, void *private) {
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) {
        debug_print("[IRQ] irq_set_handler: invalid vector %d\n", vector);
        return;
    }

    irq_table[vector].handler = handler;
    irq_table[vector].private = private;

    debug_print("[IRQ] Handler registered for vector %d\n", vector);
}

/*
 * irq_unmask — enable delivery of a specific GIC IRQ to this CPU.
 *
 * @vector  GIC IRQ number. Writes the enable bit in GICD_ISENABLER.
 *
 * The handler must be registered via irq_set_handler() first.
 * IRQs also require the CPU-level mask to be cleared (DAIF.I=0).
 */
void irq_unmask(int vector) {
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) return;

    irq_table[vector].enabled = 1;

    /* Set the enable bit in the distributor for this IRQ.
     * GICD_ISENABLER is a set-enable register: write 1 to bit N enables IRQ N.
     * Register index = vector / 32; bit = vector % 32. */
    uint32_t reg = vector / 32;
    uint32_t bit = 1U << (vector % 32);
    gicd_write(GICD_ISENABLER(reg), bit);

    debug_print("[IRQ] Unmasked IRQ %d (GICD_ISENABLER[%u] bit %u)\n",
                vector, reg, vector % 32);
}

/*
 * irq_mask — disable delivery of a specific GIC IRQ.
 *
 * @vector  GIC IRQ number.
 *
 * Uses GICD_ICENABLER (clear-enable) — write 1 to bit N disables IRQ N.
 */
void irq_mask(int vector) {
    if (vector < 0 || vector >= MAX_IRQ_VECTORS) return;

    irq_table[vector].enabled = 0;

    uint32_t reg = vector / 32;
    uint32_t bit = 1U << (vector % 32);
    gicd_write(GICD_ICENABLER(reg), bit);
}

/*
 * irq_eoi — signal End-of-Interrupt to the GIC CPU interface.
 *
 * Must be called at the END of every IRQ handler (after the handler
 * function returns but before re-enabling interrupts at DAIF level).
 * Writes the IRQ number to GICC_EOIR so the GIC knows the CPU has
 * finished handling it and can deliver the next pending IRQ.
 *
 * @vector  The IAR value from irq_dispatch (includes CPU ID in bits[12:10]).
 *          Pass the raw IAR value, not just the IRQ number.
 */
void irq_eoi(int vector) {
    gicc_write(GICC_EOIR, (uint32_t)vector);
}

/*
 * irq_dispatch — called from the AArch64 EL1 IRQ exception vector.
 *
 * Reads the GIC CPU Interface Acknowledge Register (GICC_IAR) to find
 * out which IRQ fired, calls the registered handler, then EOIs the GIC.
 *
 * The caller (exceptions.S EL1_IRQ handler) must:
 *   1. Save all registers
 *   2. Call irq_dispatch(0)  (argument is ignored — we read GICC_IAR)
 *   3. Restore registers and ERET
 */
void irq_dispatch(int vector) {
    /* Read IAR: this also acknowledges the interrupt in the GIC,
     * allowing the GIC to forward the next pending IRQ. */
    uint32_t iar = gicc_read(GICC_IAR);
    int irq = (int)(iar & 0x3FF);   /* bits[9:0] = interrupt ID */

    /* Spurious interrupt: GIC returns 1023 (0x3FF) if no interrupt pending */
    if (irq == 1023) {
        return;
    }

    (void)vector;   /* parameter unused — we use IAR directly */

    if (irq < 0 || irq >= MAX_IRQ_VECTORS) {
        debug_print("[IRQ] Out-of-range IRQ %d (IAR=0x%08x)\n", irq, iar);
        irq_eoi((int)iar);
        return;
    }

    irq_entry_t *entry = &irq_table[irq];
    if (entry->handler && entry->enabled) {
        entry->handler(irq, entry->private);
    } else {
        debug_print("[IRQ] Spurious IRQ %d (no handler registered)\n", irq);
    }

    /* End of interrupt — must use original IAR value */
    irq_eoi((int)iar);
}

/* ── IPI support (SMP) ──────────────────────────────────────────────────── */

/*
 * send_ipi — send a Software Generated Interrupt to target CPUs.
 *
 * @target_cpus  Bitmask of CPUs to target (bit N = CPU N)
 * @ipi_id       SGI IRQ number (0–15)
 * @arg          Currently unused (SGIs carry no payload in GICv2)
 *
 * Writes GICD_SGIR to trigger the SGI on the target CPUs.
 * Format: [25:24]=TargetListFilter(0=target list), [23:16]=CPUTargetList,
 *         [3:0]=SGIINTID
 */
void send_ipi(uint64_t target_cpus, int ipi_id, uint64_t arg) {
    if (ipi_id < 0 || ipi_id > 15) return;
    (void)arg;

    /* GICD_SGIR: offset 0xF00 */
    uint32_t sgir = (uint32_t)(target_cpus & 0xFF) << 16  /* CPUTargetList */
                  | (uint32_t)(ipi_id & 0xF);              /* SGIINTID      */
    gicd_write(0xF00, sgir);
}

void ipi_handler(int ipi_id, uint64_t arg) {
    /* TODO: Handle IPI — scheduler reschedule, TLB shootdown, etc. */
    debug_print("[IPI] Received IPI %d\n", ipi_id);
    (void)arg;
}
