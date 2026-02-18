#include <stdint.h>

extern uint64_t get_gpio_base(void);

void led_blink_pattern(int count, int reps) {
    uint64_t gpio_base = get_gpio_base();
    volatile uint32_t *gpfsel4 = (volatile uint32_t *)(gpio_base + 0x10);
    volatile uint32_t *gpset1  = (volatile uint32_t *)(gpio_base + 0x20);
    volatile uint32_t *gpclr1  = (volatile uint32_t *)(gpio_base + 0x2C);
    
    /* Set GPIO42 as output */
    uint32_t val = *gpfsel4;
    val &= ~(0x7 << 6);
    val |= (0x1 << 6);
    *gpfsel4 = val;
    
    /* Blink */
    for (int r = 0; r < reps; r++) {
        *gpclr1 = (1 << 10);  /* ON */
        for (volatile int i = 0; i < count * 200000; i++);
        *gpset1 = (1 << 10);  /* OFF */
        for (volatile int i = 0; i < count * 200000; i++);
    }
}

void led_signal_kernel_main(void)  { led_blink_pattern(1, 2); for(volatile int i=0;i<400000;i++); led_blink_pattern(3, 1); }
void led_signal_gpu_start(void)    { led_blink_pattern(1, 4); for(volatile int i=0;i<600000;i++); }
void led_signal_gpu_ok(void)       { led_blink_pattern(1, 5); for(volatile int i=0;i<600000;i++); }
void led_signal_hang(void)         { while(1) { led_blink_pattern(5, 1); } }
void led_signal_boot_ok(void)      { led_blink_pattern(1, 3); for(volatile int i=0;i<600000;i++); }
