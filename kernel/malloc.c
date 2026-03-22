/**
 * @file kernel/malloc.c
 * @brief Kernel Memory Allocator – Free-List with Coalescing
 *
 * Replaces the original bump allocator (which had a no-op kfree) with a
 * proper first-fit free-list allocator.  Every allocation is preceded by a
 * small header so kfree() can coalesce adjacent free blocks and actually
 * return memory to the pool.
 *
 * Layout inside the heap:
 *   [ block_hdr_t | <user data> ] [ block_hdr_t | <user data> ] …
 */

#include "kernel.h"

#define HEAP_SIZE   (128 * 1024 * 1024)   /* 128 MB */
#define ALIGN       16
#define HDR_MAGIC   0xFEEDC0DEUL

#define ALIGN_UP(n)  (((n) + (ALIGN - 1)) & ~(size_t)(ALIGN - 1))
#define MIN_PAYLOAD  (ALIGN_UP(sizeof(block_hdr_t)) + ALIGN)

typedef struct block_hdr {
    uint32_t           magic;
    uint32_t           free;
    size_t             size;        /* total bytes incl. this header */
    struct block_hdr  *prev_phys;
    struct block_hdr  *next_free;
    struct block_hdr  *prev_free;
} block_hdr_t;

static char heap[HEAP_SIZE] __attribute__((aligned(ALIGN)));
static block_hdr_t *free_list_head = NULL;
static size_t heap_allocated = 0;

static inline block_hdr_t *next_phys(block_hdr_t *b)
{
    return (block_hdr_t *)((char *)b + b->size);
}

static void free_list_insert(block_hdr_t *b)
{
    b->next_free = free_list_head;
    b->prev_free = NULL;
    if (free_list_head)
        free_list_head->prev_free = b;
    free_list_head = b;
}

static void free_list_remove(block_hdr_t *b)
{
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        free_list_head = b->next_free;
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
    b->next_free = NULL;
    b->prev_free = NULL;
}

static block_hdr_t *coalesce_forward(block_hdr_t *b)
{
    char *heap_end = heap + HEAP_SIZE;
    block_hdr_t *nx = next_phys(b);
    if ((char *)nx >= heap_end || nx->magic != HDR_MAGIC || !nx->free)
        return b;
    free_list_remove(nx);
    b->size += nx->size;
    block_hdr_t *after = next_phys(b);
    if ((char *)after < heap_end && after->magic == HDR_MAGIC)
        after->prev_phys = b;
    return b;
}

void malloc_init(void)
{
    block_hdr_t *b = (block_hdr_t *)heap;
    b->magic     = HDR_MAGIC;
    b->free      = 1;
    b->size      = HEAP_SIZE;
    b->prev_phys = NULL;
    b->next_free = NULL;
    b->prev_free = NULL;
    free_list_head = b;
    heap_allocated = 0;
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;
    if (!free_list_head) malloc_init();

    size_t need = ALIGN_UP(sizeof(block_hdr_t)) + ALIGN_UP(size);

    block_hdr_t *b = free_list_head;
    while (b && b->size < need)
        b = b->next_free;

    if (!b) {
        debug_print("[kmalloc] Out of memory! Requested: %zu\n", size);
        return NULL;
    }

    if (b->size >= need + MIN_PAYLOAD) {
        block_hdr_t *split = (block_hdr_t *)((char *)b + need);
        split->magic     = HDR_MAGIC;
        split->free      = 1;
        split->size      = b->size - need;
        split->prev_phys = b;
        split->next_free = NULL;
        split->prev_free = NULL;
        char *heap_end = heap + HEAP_SIZE;
        block_hdr_t *after = next_phys(split);
        if ((char *)after < heap_end && after->magic == HDR_MAGIC)
            after->prev_phys = split;
        free_list_insert(split);
        b->size = need;
    }

    free_list_remove(b);
    b->free = 0;
    heap_allocated += b->size;

    return (void *)((char *)b + ALIGN_UP(sizeof(block_hdr_t)));
}

void kfree(void *ptr)
{
    if (!ptr) return;
    block_hdr_t *b = (block_hdr_t *)((char *)ptr - ALIGN_UP(sizeof(block_hdr_t)));
    if (b->magic != HDR_MAGIC) {
        debug_print("[kfree] Corrupt header at %p\n", ptr);
        return;
    }
    if (b->free) {
        debug_print("[kfree] Double-free at %p\n", ptr);
        return;
    }
    heap_allocated -= b->size;
    b->free = 1;

    b = coalesce_forward(b);

    if (b->prev_phys && b->prev_phys->magic == HDR_MAGIC && b->prev_phys->free) {
        block_hdr_t *prev = b->prev_phys;
        free_list_remove(prev);
        prev->size += b->size;
        char *heap_end = heap + HEAP_SIZE;
        block_hdr_t *after = next_phys(prev);
        if ((char *)after < heap_end && after->magic == HDR_MAGIC)
            after->prev_phys = prev;
        b = prev;
    }

    free_list_insert(b);
}

void *kcalloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) return NULL;
    size_t total = nmemb * size;
    if (total / nmemb != size) {
        debug_print("[kcalloc] Size overflow!\n");
        return NULL;
    }
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void heap_stats(void)
{
    if (!free_list_head) malloc_init();
    size_t free_bytes = 0, free_blocks = 0;
    for (block_hdr_t *b = free_list_head; b; b = b->next_free) {
        free_bytes  += b->size - ALIGN_UP(sizeof(block_hdr_t));
        free_blocks++;
    }
    debug_print("Heap: %zu / %d bytes used, %zu bytes free in %zu blocks\n",
                heap_allocated, HEAP_SIZE, free_bytes, free_blocks);
}
