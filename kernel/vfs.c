/*
 * vfs.c – Virtual File System for RISC OS Phoenix
 * Handles inodes, open/close/read/write/seek, pipes, file types
 * Integrates with block devices (NVMe, USB, SATA)
 * Author: R Andrews Grok 4 – 05 Feb 2026
 * Updated: 15 Feb 2026 - Added error handling
 */

#include "kernel.h"
#include "vfs.h"
#include "pipe.h"
#include "errno.h"
#include "error.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define MAX_INODES      1024
#define MAX_FILES       1024

typedef struct inode inode_t;
typedef struct file file_t;

struct inode {
    uint64_t i_mode;        // S_IFREG, S_IFDIR, etc.
    uint64_t i_size;
    uint64_t i_blocks;
    uint16_t file_type;     // RISC OS file type (0xFFF for Text, etc.)
    // ... other fields (timestamps, permissions)
    void *private;          // FS-specific data
};

struct file {
    inode_t *f_inode;
    uint64_t f_pos;
    int f_flags;
    file_ops_t *f_ops;
};

static inode_t inodes[MAX_INODES];
static int num_inodes = 0;
static spinlock_t inode_lock = SPINLOCK_INIT;

static file_t files[MAX_FILES];
static int num_files = 0;
static spinlock_t file_lock = SPINLOCK_INIT;

/* Allocate new inode */
inode_t *vfs_alloc_inode(void) {
    unsigned long flags;
    spin_lock_irqsave(&inode_lock, &flags);

    if (num_inodes >= MAX_INODES) {
        spin_unlock_irqrestore(&inode_lock, flags);
        errno = ENFILE;
        debug_print("ERROR: vfs_alloc_inode - inode table full\n");
        return NULL;
    }

    inode_t *inode = &inodes[num_inodes++];
    memset(inode, 0, sizeof(*inode));
    inode->file_type = 0xFFF;  // Default Text

    spin_unlock_irqrestore(&inode_lock, flags);
    return inode;
}

/* Set RISC OS file type */
void vfs_set_file_type(inode_t *inode, uint16_t type) {
    inode->file_type = type & 0xFFF;  // 12-bit code
}

/* Open file */
file_t *vfs_open(const char *path, int flags) {
    unsigned long fl;
    
    if (!path) {
        errno = EINVAL;
        debug_print("ERROR: vfs_open - NULL path\n");
        return NULL;
    }
    
    spin_lock_irqsave(&file_lock, &fl);

    if (num_files >= MAX_FILES) {
        spin_unlock_irqrestore(&file_lock, fl);
        errno = EMFILE;
        debug_print("ERROR: vfs_open - file table full\n");
        return NULL;
    }

    // Resolve path to inode (stub – implement path resolution)
    inode_t *inode = resolve_path(path);
    if (!inode) {
        spin_unlock_irqrestore(&file_lock, fl);
        errno = ENOENT;
        debug_print("ERROR: vfs_open - file not found: %s\n", path);
        return NULL;
    }

    file_t *file = &files