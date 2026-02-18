/*
 * vfs.h – Virtual File System Headers for RISC OS Phoenix
 * Defines inode_t, file_t, file_ops_t, and VFS functions
 * Supports file types, pipes, block devices
 * Author: R Andrews Grok 4 – 05 Feb 2026
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define MAX_FD          1024
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_NONBLOCK      0x0004
#define O_CREAT         0x0008

#define S_IFIFO         (1ULL << 12)  // Pipe
#define S_IFREG         (1ULL << 13)  // Regular file
#define S_IFDIR         (1ULL << 14)  // Directory
#define S_IFBLK         (1ULL << 15)  // Block device

typedef struct inode inode_t;
typedef struct file file_t;
typedef struct file_ops file_ops_t;

struct inode {
    uint64_t i_mode;        // File type/mode (S_IFREG etc.)
    uint64_t i_size;        // File size
    uint64_t i_blocks;      // Number of blocks
    uint16_t file_type;     // RISC OS file type (e.g., 0xFFF for Text)
    // ... other fields (timestamps, permissions, refcount)
    void *private;          // FS-specific data (e.g., blockdev)
};

struct file {
    inode_t *f_inode;       // Inode pointer
    uint64_t f_pos;         // File position
    int f_flags;            // Open flags (O_RDONLY etc.)
    file_ops_t *f_ops;      // File operations
    void *private;          // FS-specific private data
};

struct file_ops {
    ssize_t (*read)(file_t *file, void *buf, size_t count);
    ssize_t (*write)(file_t *file, const void *buf, size_t count);
    off_t (*seek)(file_t *file, off_t offset, int whence);
    int (*poll)(file_t *file);
    void (*close)(file_t *file);
};

inode_t *vfs_alloc_inode(void);
void vfs_set_file_type(inode_t *inode, uint16_t type);

file_t *vfs_open(const char *path, int flags);
void vfs_close(file_t *file);
ssize_t vfs_read(file_t *file, void *buf, size_t count);
ssize_t vfs_write(file_t *file, const void *buf, size_t count);
off_t vfs_seek(file_t *file, off_t offset, int whence);
int vfs_poll(file_t *file);

file_ops_t *get_fs_ops(inode_t *inode);

#endif /* VFS_H */