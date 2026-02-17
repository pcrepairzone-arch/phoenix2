/*
 * lib.c - Basic library functions
 */
#include "kernel.h"
#include <stdarg.h>

/* Forward declare UART functions */
extern void uart_putc(char c);
extern void uart_puts(const char *str);
extern void uart_hex(uint64_t value);
extern void uart_dec(int64_t value);

/* Real debug print with UART */
void debug_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    uart_puts(s ? s : "(null)");
                    break;
                }
                case 'd': {
                    int val = va_arg(args, int);
                    uart_dec(val);
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    uart_hex(val);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    uart_hex((uint64_t)ptr);
                    break;
                }
                case 'l': {
                    if (*(fmt + 1) == 'l') {
                        fmt++;
                        if (*(fmt + 1) == 'd') {
                            fmt++;
                            int64_t val = va_arg(args, int64_t);
                            uart_dec(val);
                        } else if (*(fmt + 1) == 'x') {
                            fmt++;
                            uint64_t val = va_arg(args, uint64_t);
                            uart_hex(val);
                        }
                    }
                    break;
                }
                case '%':
                    uart_putc('%');
                    break;
                default:
                    uart_putc('%');
                    uart_putc(*fmt);
                    break;
            }
        } else {
            uart_putc(*fmt);
        }
        fmt++;
    }
    
    va_end(args);
}

/* Real memory allocation with bump allocator */
#define HEAP_SIZE (16 * 1024 * 1024)  // 16MB heap
static char heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_used = 0;
static spinlock_t heap_lock = {0};

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    // Align to 16 bytes
    size = (size + 15) & ~15;
    
    unsigned long flags;
    spin_lock_irqsave(&heap_lock, &flags);
    
    if (heap_used + size > HEAP_SIZE) {
        spin_unlock_irqrestore(&heap_lock, flags);
        debug_print("ERROR: kmalloc() - out of memory! Requested: %lld, Available: %lld\n",
                   (int64_t)size, (int64_t)(HEAP_SIZE - heap_used));
        return NULL;
    }
    
    void *ptr = &heap[heap_used];
    heap_used += size;
    
    spin_unlock_irqrestore(&heap_lock, flags);
    
    return ptr;
}

void kfree(void *ptr) {
    // Simple bump allocator doesn't support free
    // TODO: Implement real allocator with free support
    (void)ptr;
}

/* String functions */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++; s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++; s2++; n--;
    }
    return n ? *(unsigned char*)s1 - *(unsigned char*)s2 : 0;
}

/* Stub functions */
void pci_scan_bus(void) {}
void vfs_init(void) {}
void net_init(void) {}
void wimp_task(void) {}
void paint_task(void) {}
void netsurf_task(void) {}
void send_ipi(uint64_t cpus, int ipi, uint64_t arg) { (void)cpus; (void)ipi; (void)arg; }
void mmu_free_pagetable(task_t *task) { (void)task; }
void mmu_free_usermemory(task_t *task) { (void)task; }

/* Dummy symbols - declared in kernel.c */
extern int nr_cpus;
extern task_t *current_task;
extern char exception_vectors[];

/* Atomic operations stub */
int __aarch64_ldadd4_acq_rel(int val, int *ptr) {
    int old = *ptr;
    *ptr += val;
    return old;
}
