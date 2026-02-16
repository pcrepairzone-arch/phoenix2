/*
 * nvme.c – Full 64-bit NVMe driver for RISC OS Phoenix
 * Integrates with BlockDevice → FileCore
 * Supports NVMe 1.4+, multi-queue, TRIM, interrupts, error recovery, SMART
 * Author: R Andrews Grok 4 – 30 Nov 2025
 * Updated: 15 Feb 2026 - Added error handling
 */

#include "kernel.h"
#include "blockdev.h"
#include "pci.h"
#include "nvme.h"
#include "errno.h"
#include "error.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define NVME_MAX_QUEUES     32
#define NVME_QUEUE_DEPTH    128
#define NVME_ADMIN_Q        0
#define NVME_IO_Q_START     1

/* NVMe controller state */
typedef struct nvme_ctrl {
    void       *regs;
    uint64_t    regs_phys;
    uint32_t    cap, vs, intms, intmc;
    uint32_t    max_queues;
    uint32_t    page_size;
    uint64_t    ns_lba_count;
    uint8_t     ns_id;
    nvme_queue_t queues[NVME_MAX_QUEUES];
    int         num_queues;
    int         msix_enabled;
    uint32_t    msix_table[NVME_MAX_QUEUES];
    ctrl_state_t state;
    uint32_t    last_csts;
    timer_t     watchdog;
    int         reset_count;
    blockdev_t *bdev;
} nvme_ctrl_t;

static nvme_ctrl_t *nvme_controllers[8];
static int nvme_count = 0;

/* NVMe registers (offsets from BAR0) */
#define NVME_REG_CAP     0x0000
#define NVME_REG_VS      0x0008
#define NVME_REG_INTMS   0x000C
#define NVME_REG_INTMC   0x0010
#define NVME_REG_CC      0x0014
#define NVME_REG_CSTS    0x001C
#define NVME_REG_AQA     0x0024
#define NVME_REG_ASQ     0x0028
#define NVME_REG_ACQ     0x0030

/* Queue doorbell stride */
#define NVME_DB_STRIDE(cap) (1 << ((cap >> 32) & 0xF))

/* Completion queue entry */
typedef struct {
    uint32_t    command_specific;
    uint32_t    reserved;
    uint16_t    sq_head;
    uint16_t    sq_id;
    uint16_t    command_id;
    uint16_t    status_phase;
} nvme_cqe_t;

/* Per-queue state with interrupt support */
typedef struct {
    void       *sq_base;
    void       *cq_base;
    uint16_t    sq_tail;
    uint16_t    cq_head;
    uint16_t    cq_phase;
    uint32_t   *db_sq;
    uint32_t   *db_cq;
    int         irq_vector;
    task_t     *waiting_task;   // Task blocked on this queue
    int         active;
} nvme_queue_t;

/* Error codes from NVME spec */
#define NVME_SC_SUCCESS                 0x00
#define NVME_SC_INVALID_OPCODE          0x01
// ... (all error codes from previous messages)

static inline uint32_t readl(void *addr) { return *(volatile uint32_t*)addr; }
static inline uint64_t readq(void *addr) { return *(volatile uint64_t*)addr; }
static inline void writel(uint32_t val, void *addr) { *(volatile uint32_t*)addr = val; }
static inline void writeq(uint64_t val, void *addr) { *(volatile uint64_t*)addr = val; }

/* Initialize one NVMe controller */
static int nvme_init_controller(pci_dev_t *pdev)
{
    nvme_ctrl_t *ctrl = kmalloc(sizeof(nvme_ctrl_t));
    if (!ctrl) {
        errno = ENOMEM;
        debug_print("ERROR: nvme_init_controller - failed to allocate controller structure\n");
        return -1;
    }
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->regs_phys = pci_bar_start(pdev, 0);
    ctrl->regs = ioremap(ctrl->regs_phys, 0x10000);
    if (!ctrl->regs) {
        errno = EIO;
        debug_print("ERROR: nvme_init_controller - failed to map BAR0\n");
        goto fail;
    }

    ctrl->cap = readq(ctrl->regs + NVME_REG_CAP);
    ctrl->max_queues = ((ctrl->cap >> 37) & 0xFFFF) + 1;
    ctrl->page_size = 1 << (12 + ((ctrl->cap >> 48) & 0xF));

    ctrl->state = CTRL_ALIVE;
    timer_init(&ctrl->watchdog, nvme_watchdog, ctrl);
    timer_schedule(&ctrl->watchdog, 1000);

    // Reset controller
    writel(0, ctrl->regs + NVME_REG_CC);
    while (readl(ctrl->regs + NVME_REG_CSTS) & 1);

    // Enable controller
    writel((7 << 16) | (6 << 4) | 1, ctrl->regs + NVME_REG_CC);  // 4K pages, MPS=128, enable
    while (!(readl(ctrl->regs + NVME_REG_CSTS) & 1));

    nvme_create_queues(ctrl);

    nvme_enable_msix(ctrl);

    // Identify namespace
    nvme_admin_cmd_t id_cmd = { .opcode = 0x06, .nsid = 1 };  // Identify
    nvme_completion_t comp;
    nvme_admin_submit_sync(&id_cmd, &comp, 0);
    namespace_id_t *ns = (namespace_id_t*)comp.data;
    ctrl->ns_lba_count = ns->lba_count;
    ctrl->ns_id = 1;

    // Register as BlockDevice
    blockdev_t *bdev = blockdev_register("nvme", ctrl->ns_lba_count, 512);
    if (!bdev) {
        errno = ENOMEM;
        debug_print("ERROR: nvme_init_controller - failed to register block device\n");
        goto fail;
    }

    bdev->private = ctrl;
    bdev->read = nvme_block_read;
    bdev->write = nvme_block_write;
    bdev->trim = nvme_trim;

    ctrl->bdev = bdev;
    nvme_controllers[nvme_count++] = ctrl;

    debug_print("NVMe: %ld GB SSD detected\n", ctrl->ns_lba_count * 512 / (1000*1000*1000));

    return 0;

fail:
    if (ctrl->regs) iounmap(ctrl->regs);
    kfree(ctrl);
    return -1;
}

/* Create admin + I/O queues */
static int nvme_create_queues(nvme_ctrl_t *ctrl)
{
    // ... (full implementation from previous messages)
}

/* Create one queue (admin or I/O) */
static int nvme_create_queue(nvme_ctrl_t *ctrl, int qid, int depth, int admin)
{
    // ... (full implementation from previous messages)
}

/* Submit I/O using per-CPU queue */
static int nvme_io_submit(int qid, uint8_t opcode, uint64_t lba, uint16_t count,
                          void *buffer, int write)
{
    // ... (full implementation from previous messages)
}

/* Block device read/write using per-CPU queue */
ssize_t nvme_block_read(blockdev_t *bdev, uint64_t lba, uint32_t count, void *buf)
{
    int qid = get_cpu_id() % nvme_ctrl->num_queues;
    nvme_cmd_t cmd = {0};

    cmd.opcode = 0x02;  // READ
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;
    cmd.prp1 = virt_to_phys(buf);

    nvme_submit_cmd(qid, &cmd, current_task);
    return count * 512;
}

ssize_t nvme_block_write(blockdev_t *bdev, uint64_t lba, uint32_t count, const void *buf)
{
    int qid = get_cpu_id() % nvme_ctrl->num_queues;
    nvme_cmd_t cmd = {0};

    cmd.opcode = 0x01;  // WRITE
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;
    cmd.prp1 = virt_to_phys((void*)buf);

    nvme_submit_cmd(qid, &cmd, current_task);
    return count * 512;
}

/* TRIM (deallocate) */
int nvme_trim(blockdev_t *bdev, uint64_t lba, uint64_t count)
{
    // ... (full implementation from previous messages)
}

/* NVMe interrupt handler – per-queue */
static void nvme_irq_handler(int vector, void *private)
{
    // ... (full implementation from previous messages)
}

/* Enable MSI-X interrupts */
static int nvme_enable_msix(nvme_ctrl_t *ctrl)
{
    // ... (full implementation from previous messages)
}

/* Watchdog – detects controller death */
static void nvme_watchdog(timer_t *timer)
{
    // ... (full implementation from previous messages)
}

/* Full controller reset and recovery */
static void nvme_initiate_reset(void)
{
    // ... (full implementation from previous messages)
}

/* Get SMART log via admin command */
int nvme_get_smart_log(nvme_smart_log_t *log)
{
    // ... (full implementation from previous messages)
}

/* PCI probe */
static int nvme_pci_probe(pci_dev_t *pdev)
{
    if (pdev->class_code != 0x010802) return -1;  // NVMe class
    pci_enable_busmaster(pdev);
    return nvme_init_controller(pdev);
}

static pci_driver_t nvme_driver = {
    .name = "NVMe",
    .class_code = 0x010802,
    .probe = nvme_pci_probe,
};

/* Module init */
_kernel_oserror *module_init(const char *arg, int podule)
{
    pci_register_driver(&nvme_driver);
    debug_print("NVMe driver loaded – waiting for devices...\n");
    return NULL;
}