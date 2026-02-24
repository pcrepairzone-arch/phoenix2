/**
 * @file kernel/malloc.c
 * @brief Kernel Memory Allocator
 * 
 * Simple bump allocator for kernel heap.
 * 
 * @author Phoenix RISC OS Team
 * @since v55
 */

#include "kernel.h"
#include <string.h>

#define HEAP_SIZE (128 * 1024 * 1024)  /* 128MB heap */

static char heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_used = 0;

/**
 * @brief Allocate memory from kernel heap
 * 
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory (16-byte aligned), or NULL on failure
 * @since v55
 */
void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    
    /* Align size to 16 bytes */
    size = (size + 15) & ~15;
    
    if (heap_used + size > HEAP_SIZE) {
        debug_print("[kmalloc] Out of memory! Requested: %zu, Available: %zu\n",
                    size, HEAP_SIZE - heap_used);
        return NULL;
    }
    
    void *ptr = &heap[heap_used];
    heap_used += size;
    
    return ptr;
}

/**
 * @brief Free allocated memory
 * 
 * Currently a no-op (bump allocator doesn't support freeing)
 * 
 * @param ptr Pointer to free (ignored)
 * @since v55
 */
void kfree(void *ptr)
{
    /* No-op for bump allocator */
    (void)ptr;
}

/**
 * @brief Allocate and zero memory
 * 
 * @param nmemb Number of elements
 * @param size Size of each element  
 * @return Pointer to zeroed memory, or NULL on failure
 * @since v56
 */
void *kcalloc(size_t nmemb, size_t size)
{
    /* Check for overflow */
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) {
        debug_print("[kcalloc] Size overflow!\n");
        return NULL;
    }
    
    /* Allocate */
    void *ptr = kmalloc(total);
    if (!ptr) {
        return NULL;
    }
    
    /* Zero the memory */
    memset(ptr, 0, total);
    
    return ptr;
}

/**
 * @brief Print heap statistics
 * 
 * @since v55
 */
void heap_stats(void)
{
    debug_print("Heap: %zu / %d bytes used (%zu%%)\n",
                heap_used, HEAP_SIZE,
                (heap_used * 100) / HEAP_SIZE);
}
