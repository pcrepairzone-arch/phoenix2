#include "kernel.h"
#include <stdint.h>

extern uint64_t get_mailbox_base(void);
extern void led_signal_hang(void);
extern void led_blink_pattern(int count, int reps);

/* Separate buffers for each attempt - avoid any reuse issues */
static volatile uint32_t __attribute__((aligned(16))) mbox_buf1[32];
static volatile uint32_t __attribute__((aligned(16))) mbox_buf2[32];
static volatile uint32_t __attribute__((aligned(16))) mbox_buf3[32];

static inline void mb(void) {
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

static void progress_blink(int n) {
    led_blink_pattern(1, n);
    for (volatile int i = 0; i < 3000000; i++);
}

static void emergency_blink(int count) {
    volatile uint32_t *gpio_base = (volatile uint32_t *)0xFE200000ULL;
    volatile uint32_t *gpfsel4 = gpio_base + 4;
    volatile uint32_t *gpset1  = gpio_base + 8;
    volatile uint32_t *gpclr1  = gpio_base + 11;
    
    uint32_t val = *gpfsel4;
    val &= ~(0x7 << 6);
    val |= (0x1 << 6);
    *gpfsel4 = val;
    
    for (int i = 0; i < count; i++) {
        *gpclr1 = (1 << 10);
        for (volatile int d = 0; d < 500000; d++);
        *gpset1 = (1 << 10);
        for (volatile int d = 0; d < 500000; d++);
    }
    for (volatile int d = 0; d < 3000000; d++);
}

static int try_simple_message(volatile uint32_t *buf, uint32_t cache_alias, int attempt_num)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *mb_status = (volatile uint32_t *)(mbox_base + 0x18);
    volatile uint32_t *mb_read   = (volatile uint32_t *)(mbox_base + 0x00);
    volatile uint32_t *mb_write  = (volatile uint32_t *)(mbox_base + 0x20);
    
    /* Zero buffer */
    for (int i = 0; i < 32; i++) buf[i] = 0;
    
    /* Build message */
    buf[0] = 7 * 4;
    buf[1] = 0x00000000;
    buf[2] = 0x00000001;    /* Get Firmware Revision */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0x00000000;
    
    uint32_t phys = (uint32_t)(uint64_t)buf;
    uint32_t msg  = ((phys & 0x0FFFFFFFU) | cache_alias) | 8;
    
    /* Show we're attempting */
    emergency_blink(30 + attempt_num);  /* 31, 32, 33 for attempts 1, 2, 3 */
    
    debug_print("Try %d: alias=0x%x phys=0x%x msg=0x%x\n", 
                attempt_num, cache_alias, phys, msg);
    
    mb();
    
    /* Wait TX */
    int timeout = 500000;
    while (*mb_status & 0x80000000U) {
        if (--timeout <= 0) {
            emergency_blink(40 + attempt_num);  /* 41, 42, 43 = TX timeout */
            return -1;
        }
        mb();
    }
    
    mb();
    *mb_write = msg;
    mb();
    
    emergency_blink(35 + attempt_num);  /* 36, 37, 38 = write succeeded */
    
    /* Wait RX - shorter timeout */
    timeout = 500000;
    int responses = 0;
    
    while (timeout > 0) {
        if (!(*mb_status & 0x40000000U)) {
            mb();
            uint32_t r = *mb_read;
            mb();
            
            responses++;
            int ch = (int)(r & 0xF);
            
            /* Show channel received */
            emergency_blink(50 + ch);  /* 50-65 = channel number */
            
            if (ch == 8) {
                mb();
                uint32_t code = buf[1];
                
                /* Show response code high nibble */
                emergency_blink(70 + ((code >> 28) & 0xF));  /* 78 = 0x8xxxxxxx */
                
                if (code == 0x80000000U) {
                    return 0;  /* SUCCESS */
                }
            }
        }
        timeout--;
        mb();
    }
    
    emergency_blink(45 + attempt_num);  /* 46, 47, 48 = RX timeout */
    return -1;
}

void mailbox_comprehensive_test(void)
{
    debug_print("\n=== MAILBOX TEST ===\n");
    
    progress_blink(10);
    
    /* Attempt 1: 0x40000000 */
    progress_blink(11);
    if (try_simple_message(mbox_buf1, 0x40000000U, 1) == 0) {
        progress_blink(18);
        debug_print("SUCCESS: 0x40000000\n");
        return;
    }
    
    /* Attempt 2: 0xC0000000 */
    progress_blink(12);
    if (try_simple_message(mbox_buf2, 0xC0000000U, 2) == 0) {
        progress_blink(19);
        debug_print("SUCCESS: 0xC0000000\n");
        return;
    }
    
    /* Attempt 3: 0x00000000 */
    progress_blink(13);
    if (try_simple_message(mbox_buf3, 0x00000000U, 3) == 0) {
        progress_blink(20);
        debug_print("SUCCESS: no alias\n");
        return;
    }
    
    progress_blink(17);
    debug_print("All failed\n");
    led_signal_hang();
}
