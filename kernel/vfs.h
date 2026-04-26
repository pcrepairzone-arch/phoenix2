/*
 * vfs.h – Virtual File System Headers for Phoenix OS
 * Defines inode_t, file_t, file_ops_t, filesystem registration, and VFS functions.
 * Supports RISC OS file types, pipes, block devices, and FileCore integration.
 * Author:  R Andrews – 05 Feb 2026
 * Updated: boot247, April 2026 – added vfs_filesystem_t, vfs_dirent_t, readdir
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#ifndef MAX_FD
#define MAX_FD          64
#endif

/* ── Open flags ─────────────────────────────────────────────────────────── */
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_NONBLOCK      0x0004
#define O_CREAT         0x0008

/* ── Inode mode bits ────────────────────────────────────────────────────── */
#define S_IFIFO         (1ULL << 12)    /* Pipe              */
#define S_IFREG         (1ULL << 13)    /* Regular file      */
#define S_IFDIR         (1ULL << 14)    /* Directory         */
#define S_IFBLK         (1ULL << 15)    /* Block device      */

/* ── Filesystem flags ───────────────────────────────────────────────────── */
#define VFS_FS_READONLY             0x01    /* Read-only filesystem        */
#define VFS_FS_CASE_INSENSITIVE     0x02    /* Case-insensitive names      */

/* ── Directory entry types ──────────────────────────────────────────────── */
#define VFS_DIRENT_FILE     0       /* Regular file                         */
#define VFS_DIRENT_DIR      1       /* Directory                            */
#define VFS_DIRENT_SPECIAL  2       /* Special object (e.g. FAT32 partition)*/

/* ── Directory entry ────────────────────────────────────────────────────── */
#define VFS_NAME_MAX    256
typedef struct {
    int      type;                  /* VFS_DIRENT_FILE / DIR / SPECIAL      */
    uint64_t size;                  /* File size in bytes                   */
    uint16_t riscos_type;           /* RISC OS file type (e.g. &FEB, &FFF)  */
    uint32_t load_addr;             /* RISC OS load address                 */
    uint32_t exec_addr;             /* RISC OS exec address (timestamp)     */
    uint32_t sin;                   /* FileCore SIN / IDA                   */
    char     name[VFS_NAME_MAX];    /* Null-terminated name                 */
} vfs_dirent_t;

typedef struct inode inode_t;
typedef struct file  file_t;
typedef struct file_ops file_ops_t;

/* ── Inode ──────────────────────────────────────────────────────────────── */
struct inode {
    uint64_t i_mode;        /* File type/mode (S_IFREG, S_IFDIR etc.)   */
    uint64_t i_size;        /* File size                                 */
    uint64_t i_blocks;      /* Number of blocks                         */
    uint16_t file_type;     /* RISC OS file type (0xFFF=Text, 0xFEB=Obey etc.) */
    uint32_t load_addr;     /* RISC OS load address                     */
    uint32_t exec_addr;     /* RISC OS exec address                     */
    uint32_t sin;           /* FileCore SIN                             */
    void    *private;       /* FS-specific data (e.g., blockdev)        */
};

/* ── File ───────────────────────────────────────────────────────────────── */
struct file {
    inode_t    *f_inode;    /* Inode pointer                            */
    uint64_t    f_pos;      /* File position                            */
    int         f_flags;    /* Open flags (O_RDONLY etc.)               */
    file_ops_t *f_ops;      /* File operations                          */
    void       *private;    /* FS-specific private data                 */
};

/* ── File operations ────────────────────────────────────────────────────── */
struct file_ops {
    ssize_t (*read)   (file_t *file, void *buf, size_t count);
    ssize_t (*write)  (file_t *file, const void *buf, size_t count);
    off_t   (*seek)   (file_t *file, off_t offset, int whence);
    int     (*poll)   (file_t *file);
    int     (*readdir)(file_t *file, vfs_dirent_t *dirent);
    void    (*close)  (file_t *file);
};

/* ── Filesystem driver ──────────────────────────────────────────────────── */
typedef struct {
    const char *name;                   /* Filesystem name e.g. "FileCore" */
    uint32_t    flags;                  /* VFS_FS_READONLY etc.            */
    int  (*mount)  (const char *dev, const char *mnt,
                    uint32_t flags, void **fsdata);
    int  (*umount) (void *fsdata);
    int  (*readdir)(void *fsdata, const char *path,
                    vfs_dirent_t *dirent, uint32_t *cookie);
} vfs_filesystem_t;

/* ── VFS API ────────────────────────────────────────────────────────────── */
inode_t    *vfs_alloc_inode(void);
void        vfs_set_file_type(inode_t *inode, uint16_t type);

file_t     *vfs_open (const char *path, int flags);
void        vfs_close(file_t *file);
ssize_t     vfs_read (file_t *file, void *buf, size_t count);
ssize_t     vfs_write(file_t *file, const void *buf, size_t count);
off_t       vfs_seek (file_t *file, off_t offset, int whence);
int         vfs_poll (file_t *file);

int         vfs_register_filesystem(const vfs_filesystem_t *fs);
int         vfs_mount  (const char *fsname, const char *mountpoint,
                         uint32_t flags, void *opts);

file_ops_t *get_fs_ops(inode_t *inode);

/* FileCore VFS entry point — called from kernel_main */
void        vfs_register_filecore(void);

#endif /* VFS_H */
