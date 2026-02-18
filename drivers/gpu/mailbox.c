#include "kernel.h"
#include "mailbox.h"

extern uint64_t get_mailbox_base(void);
extern void led_signal_hang(void);

static uint64_t mbox_base = 0;

static inline void mb(void) {
    __asm__ volatile ("dsb sy\nisb" ::: "memory");
}

int mbox_call(volatile uint32_t *buf)
{
    if (!mbox_base) mbox_base = get_mailbox_base();
    
    volatile uint32_t *mb_read   = (volatile uint32_t *)(mbox_base + 0x00);
    volatile uint32_t *mb_status = (volatile uint32_t *)(mbox_base + 0x18);
    volatile uint32_t *mb_write  = (volatile uint32_t *)(mbox_base + 0x20);
    
    uint32_t phys = (uint32_t)(uint64_t)buf;
    uint32_t msg  = (phys & ~0xFU) | 0xC000000EU;

    debug_print("mbox: base=0x%llx phys=0x%x msg=0x%x\n", mbox_base, phys, msg);
    debug_print("mbox: initial status=0x%x\n", *mb_status);

    int timeout = 10000000;
    while (*mb_status & 0x80000000U) {
        if (--timeout <= 0) {
            debug_print("mbox: TX timeout\n");
            led_signal_hang();
            return -1;
        }
        mb();
    }

    mb();
    *mb_write = msg;
    mb();

    timeout = 10000000;
    while (1) {
        int inner = 1000000;
        while (*mb_status & 0x40000000U) {
            if (--inner <= 0 || --timeout <= 0) {
                debug_print("mbox: RX timeout, status=0x%x\n", *mb_status);
                led_signal_hang();
                return -1;
            }
            mb();
        }
        mb();
        uint32_t r = *mb_read;
        mb();
        if ((r & 0xF) == 8) {
            debug_print("mbox: code=0x%x\n", buf[1]);
            return (buf[1] == 0x80000000U) ? 0 : -1;
        }
    }
}
