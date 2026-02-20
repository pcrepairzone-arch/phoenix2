/*
 * mailbox.c - Pi 4 mailbox with CORRECT cache alias
 * Pi 4 uses 0x40000000, NOT 0xC0000000!
 */
#include "kernel.h"
#include "mailbox.h"

extern uint64_t get_mailbox_base(void);
extern void led_signal_hang(void);

static inline void mb(void) {
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

int mbox_call(volatile uint32_t *buf)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *mb_status = (volatile uint32_t *)(mbox_base + 0x18);
    volatile uint32_t *mb_read   = (volatile uint32_t *)(mbox_base + 0x00);
    volatile uint32_t *mb_write  = (volatile uint32_t *)(mbox_base + 0x20);
    
    uint32_t phys = (uint32_t)(uint64_t)buf;
    
    /* CRITICAL: Pi 4 uses 0x40000000, not 0xC0000000! */
    uint32_t msg  = ((phys & 0x0FFFFFFFU) | 0x40000000U) | 8;
    
    debug_print("mbox: phys=0x%x gpu=0x%x\n", phys, msg);
    
    mb();
    
    /* Wait TX ready */
    int timeout = 1000000;
    while (*mb_status & 0x80000000U) {
        if (--timeout <= 0) {
            debug_print("mbox: TX timeout\n");
            return -1;
        }
        mb();
    }
    
    mb();
    *mb_write = msg;
    mb();
    
    /* Wait RX */
    timeout = 2000000;
    while (1) {
        int inner = 1000000;
        while (*mb_status & 0x40000000U) {
            if (--inner <= 0 || --timeout <= 0) {
                debug_print("mbox: RX timeout\n");
                return -1;
            }
            mb();
        }
        if (timeout <= 0) break;
        
        mb();
        uint32_t r = *mb_read;
        mb();
        
        if ((r & 0xF) == 8) {
            mb();
            uint32_t code = buf[1];
            debug_print("mbox: code=0x%x\n", code);
            return (code == 0x80000000U) ? 0 : -1;
        }
    }
    
    debug_print("mbox: no response\n");
    return -1;
}
