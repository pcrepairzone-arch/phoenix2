/*
 * error.h – Error handling macros and utilities for RISC OS Phoenix
 * Author: Error Handling Framework – 15 Feb 2026
 */

#ifndef _ERROR_H
#define _ERROR_H

#include "errno.h"

/* Debug print function - should be defined in kernel.h */
#ifndef debug_print
extern void debug_print(const char *fmt, ...);
#endif

/* Check for NULL pointer and return with errno set */
#define CHECK_NULL(ptr, err) \
    do { \
        if (!(ptr)) { \
            errno = (err); \
            debug_print("ERROR: %s:%d - %s is NULL (errno=%d: %s)\n", \
                       __FILE__, __LINE__, #ptr, err, strerror(err)); \
            return NULL; \
        } \
    } while (0)

/* Check condition and return error code with errno set */
#define CHECK_ERRNO(cond, err, retval) \
    do { \
        if (!(cond)) { \
            errno = (err); \
            debug_print("ERROR: %s:%d - Check failed: %s (errno=%d: %s)\n", \
                       __FILE__, __LINE__, #cond, err, strerror(err)); \
            return (retval); \
        } \
    } while (0)

/* Jump to cleanup label with errno set */
#define CLEANUP_GOTO(label, err) \
    do { \
        errno = (err); \
        debug_print("ERROR: %s:%d - goto %s (errno=%d: %s)\n", \
                   __FILE__, __LINE__, #label, err, strerror(err)); \
        goto label; \
    } while (0)

/* Set errno and return error value */
#define SET_ERRNO_RETURN(err, retval) \
    do { \
        errno = (err); \
        debug_print("ERROR: %s:%d - (errno=%d: %s)\n", \
                   __FILE__, __LINE__, err, strerror(err)); \
        return (retval); \
    } while (0)

/* Assert-like check that panics on failure */
#define KERNEL_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            debug_print("KERNEL PANIC: %s:%d - %s\n", __FILE__, __LINE__, msg); \
            halt_system(); \
        } \
    } while (0)

/* Safe memory allocation with NULL check and errno */
static inline void *kmalloc_safe(size_t size, const char *file, int line)
{
    extern void *kmalloc(size_t size);
    void *ptr = kmalloc(size);
    if (!ptr) {
        errno = ENOMEM;
        debug_print("ERROR: %s:%d - kmalloc(%zu) failed (errno=%d: %s)\n",
                   file, line, size, ENOMEM, strerror(ENOMEM));
    }
    return ptr;
}

#define KMALLOC(size) kmalloc_safe(size, __FILE__, __LINE__)

/* Safe string copy with bounds checking */
static inline int strncpy_safe(char *dst, const char *src, size_t size)
{
    if (!dst || !src || size == 0) {
        errno = EINVAL;
        return -1;
    }
    
    size_t i;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    
    /* Return -1 if truncation occurred */
    if (i == size - 1 && src[i] != '\0') {
        errno = ENAMETOOLONG;
        return -1;
    }
    
    return 0;
}

/* Safe string concatenation with bounds checking */
static inline int strncat_safe(char *dst, const char *src, size_t dst_size)
{
    if (!dst || !src || dst_size == 0) {
        errno = EINVAL;
        return -1;
    }
    
    size_t dst_len = 0;
    while (dst_len < dst_size && dst[dst_len] != '\0') {
        dst_len++;
    }
    
    if (dst_len >= dst_size) {
        errno = EINVAL;
        return -1;
    }
    
    size_t i;
    for (i = 0; i < dst_size - dst_len - 1 && src[i] != '\0'; i++) {
        dst[dst_len + i] = src[i];
    }
    dst[dst_len + i] = '\0';
    
    if (i == dst_size - dst_len - 1 && src[i] != '\0') {
        errno = ENAMETOOLONG;
        return -1;
    }
    
    return 0;
}

#endif /* _ERROR_H */
