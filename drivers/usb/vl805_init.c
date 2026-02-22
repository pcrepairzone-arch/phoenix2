/**
 * @file drivers/usb/vl805_init.c
 * @brief VL805 USB Controller Initialization
 * 
 * The Pi 4 firmware normally loads vl805.bin to initialize the USB controller.
 * If that's missing, we can initialize it ourselves by writing to the mailbox.
 * 
 * @author Phoenix RISC OS Team
 * @since v56
 */

#include "kernel.h"

/* Mailbox channels */
#define MAILBOX_CHANNEL_PROPERTY  8

/* Mailbox tags */
#define MAILBOX_TAG_SET_DEVICE_POWER  0x00028001
#define MAILBOX_TAG_NOTIFY_XHCI_RESET 0x00030058

/* VL805 USB controller device ID */
#define DEVICE_ID_USB_HCD  3

/* Power states */
#define POWER_STATE_OFF    (0 << 0)
#define POWER_STATE_ON     (1 << 0)
#define POWER_STATE_WAIT   (1 << 1)

/**
 * @brief Mailbox property buffer (must be 16-byte aligned)
 */
static volatile uint32_t __attribute__((aligned(16))) mailbox_buffer[256];

/**
 * @brief Write to mailbox
 */
static void mailbox_write(uint32_t channel, uint32_t data)
{
    extern uint64_t peripheral_base;
    volatile uint32_t *mailbox = (volatile uint32_t *)(peripheral_base + 0xB880);
    
    /* Wait until we can write */
    while (mailbox[6] & 0x80000000) {
        asm volatile("nop");
    }
    
    /* Write data + channel */
    mailbox[8] = (data & 0xFFFFFFF0) | (channel & 0xF);
}

/**
 * @brief Read from mailbox
 */
static uint32_t mailbox_read(uint32_t channel)
{
    extern uint64_t peripheral_base;
    volatile uint32_t *mailbox = (volatile uint32_t *)(peripheral_base + 0xB880);
    
    while (1) {
        /* Wait for data */
        while (mailbox[6] & 0x40000000) {
            asm volatile("nop");
        }
        
        uint32_t data = mailbox[0];
        
        /* Check if it's for our channel */
        if ((data & 0xF) == channel) {
            return data & 0xFFFFFFF0;
        }
    }
}

/**
 * @brief Send property message to mailbox
 */
static int mailbox_property(void *buf)
{
    uint32_t addr = virt_to_phys(buf);
    
    /* Ensure buffer is written to memory */
    asm volatile("dmb sy" ::: "memory");
    
    /* Write to mailbox */
    mailbox_write(MAILBOX_CHANNEL_PROPERTY, addr);
    
    /* Read response */
    uint32_t response = mailbox_read(MAILBOX_CHANNEL_PROPERTY);
    
    /* Ensure we read the response */
    asm volatile("dmb sy" ::: "memory");
    
    /* Check if request was successful */
    volatile uint32_t *buf32 = (volatile uint32_t *)buf;
    if (buf32[1] != 0x80000000) {
        return -1;
    }
    
    return 0;
}

/**
 * @brief Power on VL805 USB controller
 */
static int vl805_power_on(void)
{
    mailbox_buffer[0] = 8 * 4;  /* Buffer size */
    mailbox_buffer[1] = 0;      /* Request */
    
    /* Tag: Set device power */
    mailbox_buffer[2] = MAILBOX_TAG_SET_DEVICE_POWER;
    mailbox_buffer[3] = 8;      /* Value buffer size */
    mailbox_buffer[4] = 8;      /* Request size */
    mailbox_buffer[5] = DEVICE_ID_USB_HCD;
    mailbox_buffer[6] = POWER_STATE_ON | POWER_STATE_WAIT;
    
    /* End tag */
    mailbox_buffer[7] = 0;
    
    if (mailbox_property((void *)mailbox_buffer) < 0) {
        debug_print("[VL805] Failed to power on USB controller\n");
        return -1;
    }
    
    /* Check power state */
    if ((mailbox_buffer[6] & POWER_STATE_ON) == 0) {
        debug_print("[VL805] USB controller did not power on\n");
        return -1;
    }
    
    debug_print("[VL805] USB controller powered on\n");
    return 0;
}

/**
 * @brief Notify firmware that we're resetting xHCI
 */
static int vl805_notify_xhci_reset(uint32_t pci_addr)
{
    mailbox_buffer[0] = 7 * 4;  /* Buffer size */
    mailbox_buffer[1] = 0;      /* Request */
    
    /* Tag: Notify xHCI reset */
    mailbox_buffer[2] = MAILBOX_TAG_NOTIFY_XHCI_RESET;
    mailbox_buffer[3] = 4;      /* Value buffer size */
    mailbox_buffer[4] = 4;      /* Request size */
    mailbox_buffer[5] = pci_addr;
    
    /* End tag */
    mailbox_buffer[6] = 0;
    
    if (mailbox_property((void *)mailbox_buffer) < 0) {
        debug_print("[VL805] Failed to notify xHCI reset\n");
        return -1;
    }
    
    debug_print("[VL805] Notified firmware of xHCI reset\n");
    return 0;
}

/**
 * @brief Initialize VL805 USB controller
 * 
 * This must be called BEFORE scanning the PCI bus.
 * The firmware normally does this with vl805.bin, but we can do it manually.
 * 
 * @return 0 on success, negative on error
 * @since v56
 */
int vl805_init(void)
{
    debug_print("[VL805] Initializing USB controller via mailbox\n");
    
    /* Power on the USB controller */
    if (vl805_power_on() < 0) {
        return -1;
    }
    
   /* Wait for controller to be ready - needs ~100ms */
debug_print("[VL805] Waiting for PCIe link...\n");
for (volatile int i = 0; i < 10000000; i++) {  // 10x longer!
    asm volatile("nop");
}
debug_print("[VL805] Wait complete\n");
    
    /* Notify that we'll be resetting it */
    /* PCI address: bus=1, device=0, function=0 */
    uint32_t pci_addr = (1 << 20) | (0 << 15) | (0 << 12);
    vl805_notify_xhci_reset(pci_addr);
    
    debug_print("[VL805] Initialization complete\n");
    return 0;
}
