/*
 * malloc.c - Simple Memory Allocator for Phoenix RISC OS
 * Bump allocator - simple and fast, no free for now
 */

#include "kernel.h"
#include <stdint.h>

/* 16MB heap - simple and effective */
static uint8_t heap[16 * 1024 * 1024] __attribute__((aligned(16)));
static size_t heap_used = 0;

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;
    
    /* 16-byte alignment */
    size = (size + 15) & ~15;
    
    if (heap_used + size > sizeof(heap)) {
        debug_print("kmalloc: Out of memory! (requested %zu bytes)\n", size);
        return NULL;
    }
    
    void *ptr = &heap[heap_used];
    heap_used += size;
    
    return ptr;
}

void kfree(void *ptr)
{
    /* Bump allocator - no free for now */
    /* TODO: Implement proper free list later */
    (void)ptr;
}

void *kcalloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr = kmalloc(total);
    
    if (ptr) {
        uint8_t *p = ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    
    return ptr;
}

/* Get heap statistics */
void heap_stats(void)
{
    debug_print("Heap: %zu / %zu bytes used (%.1f%%)\n",
                heap_used, sizeof(heap),
                (heap_used * 100.0) / sizeof(heap));
}
