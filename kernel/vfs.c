/*
 * vfs.c – Virtual File System (Simplified stub)
 * Author: R Andrews Grok 4 – 05 Feb 2026
 * Updated: 15 Feb 2026 - Stub version for compilation
 */

#include "kernel.h"
#include "vfs.h"
#include "pipe.h"
#include "errno.h"
#include "spinlock.h"

#define MAX_INODES      1024
#define MAX_FILES       1024

static inode_t inodes[MAX_INODES];
static int num_inodes = 0;
static spinlock_t inode_lock = SPINLOCK_INIT;

static file_t files[MAX_FILES];
static int num_files = 0;
static spinlock_t file_lock = SPINLOCK_INIT;

/* Stub: Resolve path to inode */
static inode_t *resolve_path(const char *path) {
    // TODO: Implement path resolution
    (void)path;
    return NULL;
}

/* Allocate new inode */
inode_t *vfs_alloc_inode(void) {
    unsigned long flags;
    spin_lock_irqsave(&inode_lock, &flags);

    if (num_inodes >= MAX_INODES) {
        spin_unlock_irqrestore(&inode_lock, flags);
        errno = ENFILE;
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
    if (inode) {
        inode->file_type = type & 0xFFF;
    }
}

/* Stub: Open file */
file_t *vfs_open(const char *path, int flags) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    
    unsigned long fl;
    spin_lock_irqsave(&file_lock, &fl);

    if (num_files >= MAX_FILES) {
        spin_unlock_irqrestore(&file_lock, fl);
        errno = EMFILE;
        return NULL;
    }

    inode_t *inode = resolve_path(path);
    if (!inode) {
        spin_unlock_irqrestore(&file_lock, fl);
        errno = ENOENT;
        return NULL;
    }

    file_t *file = &files[num_files++];
    file->f_inode = inode;
    file->f_pos = 0;
    file->f_flags = flags;
    file->f_ops = get_fs_ops(inode);

    spin_unlock_irqrestore(&file_lock, fl);
    return file;
}

/* Stub: Close file */
void vfs_close(file_t *file) {
    if (file && file->f_ops && file->f_ops->close) {
        file->f_ops->close(file);
    }
}

/* Stub: Read from file */
ssize_t vfs_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->f_ops || !file->f_ops->read) {
        errno = EBADF;
        return -1;
    }
    return file->f_ops->read(file, buf, count);
}

/* Stub: Write to file */
ssize_t vfs_write(file_t *file, const void *buf, size_t count) {
    if (!file || !file->f_ops || !file->f_ops->write) {
        errno = EBADF;
        return -1;
    }
    return file->f_ops->write(file, buf, count);
}

/* Stub: Seek in file */
off_t vfs_seek(file_t *file, off_t offset, int whence) {
    if (!file || !file->f_ops || !file->f_ops->seek) {
        errno = EBADF;
        return -1;
    }
    return file->f_ops->seek(file, offset, whence);
}

/* Stub: Poll file */
int vfs_poll(file_t *file) {
    if (!file || !file->f_ops || !file->f_ops->poll) {
        return 0;
    }
    return file->f_ops->poll(file);
}

/* Stub: Get filesystem operations */
file_ops_t *get_fs_ops(inode_t *inode) {
    // TODO: Implement FS ops lookup based on inode type
    (void)inode;
    return NULL;
}
