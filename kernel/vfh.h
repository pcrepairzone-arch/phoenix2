/*
 * vfs.h – VFS Headers
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

typedef struct file file_t;
typedef struct inode inode_t;

struct inode {
    uint64_t i_mode;
    uint64_t i_size;
    uint64_t i_blocks;
    uint16_t file_type;
    void *private;
};

struct file {
    inode_t     *f_inode;
    uint64_t    f_pos;
    int         f_flags;
    void       *f_ops;   /* Simplified for now */
    void       *private;
};

#endif /* VFS_H */