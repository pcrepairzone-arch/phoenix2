/*
 * vfs.c – Virtual File System (Simplified stub)
 * Author:  R Andrews  – 05 Feb 2026
 * Updated: 15 Feb 2026 - Stub version for compilation
 * Updated: boot247, April 2026 – FileCore VFS driver; vfs_dirent_t readdir
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

/* Forward declarations for FileCore public API (defined in filecore.c) */
extern int      filecore_find_path(const char *path, vfs_dirent_t *out);
extern uint8_t *filecore_read_file(uint32_t sin, uint32_t size);
extern void     kfree(void *);

/* ── FileCore file_ops ───────────────────────────────────────────────────── */

static ssize_t fc_vfs_read(file_t *file, void *buf, size_t count)
{
    if (!file || !file->f_inode) return -1;
    uint32_t sin  = file->f_inode->sin;
    uint32_t size = (uint32_t)file->f_inode->i_size;
    if (size == 0u || count == 0u) return 0;

    uint8_t *fbuf = filecore_read_file(sin, size);
    if (!fbuf) return -1;

    uint64_t pos = file->f_pos;
    if (pos >= (uint64_t)size) { kfree(fbuf); return 0; }

    size_t avail = (size_t)(size - (uint32_t)pos);
    size_t n     = (count < avail) ? count : avail;

    const uint8_t *src = fbuf + pos;
          uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0u; i < n; i++) dst[i] = src[i];

    file->f_pos += (uint64_t)n;
    kfree(fbuf);
    return (ssize_t)n;
}

static off_t fc_vfs_seek(file_t *file, off_t offset, int whence)
{
    if (!file || !file->f_inode) return (off_t)-1;
    uint64_t size = file->f_inode->i_size;
    off_t    np;
    if      (whence == 0) np = offset;                      /* SEEK_SET */
    else if (whence == 1) np = (off_t)file->f_pos + offset; /* SEEK_CUR */
    else                  np = (off_t)size + offset;        /* SEEK_END */
    if (np < 0)              np = 0;
    if ((uint64_t)np > size) np = (off_t)size;
    file->f_pos = (uint64_t)np;
    return np;
}

static file_ops_t filecore_file_ops = {
    .read  = fc_vfs_read,
    .write = NULL,
    .seek  = fc_vfs_seek,
    .poll  = NULL,
    .readdir = NULL,
    .close = NULL,
};

/* ── resolve_path ────────────────────────────────────────────────────────── */
static inode_t *resolve_path(const char *path)
{
    vfs_dirent_t ent;
    if (filecore_find_path(path, &ent) != 0) return NULL;

    inode_t *inode = vfs_alloc_inode();
    if (!inode) return NULL;

    inode->sin       = ent.sin;
    inode->i_size    = (uint64_t)ent.size;
    inode->i_blocks  = (ent.size + 511u) / 512u;
    inode->load_addr = ent.load_addr;
    inode->exec_addr = ent.exec_addr;
    inode->file_type = ent.riscos_type;
    inode->i_mode    = (ent.type == VFS_DIRENT_DIR) ? S_IFDIR : S_IFREG;
    return inode;
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
    inode->file_type = 0xFFF;  /* Default Text */

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

/* Return file_ops for the inode's filesystem type */
file_ops_t *get_fs_ops(inode_t *inode)
{
    if (!inode) return NULL;
    /* FileCore regular files — directories are not directly read */
    if (inode->i_mode & S_IFREG) return &filecore_file_ops;
    return NULL;
}


/* ── vfs_register_filesystem ────────────────────────────────────────────── */
/* Forward declarations for symbols defined in other translation units      */
extern void uart_puts(const char *s);
extern void filecore_init(void);
extern void filecore_list_root(void);
extern int  filecore_get_root_entry(uint32_t idx, vfs_dirent_t *out);
extern int  filecore_get_child_entry(uint32_t sin, uint32_t idx,
                                      vfs_dirent_t *out);
/* g_fc_bdev is blockdev_t* in filecore.c — forward as void* to avoid
 * including blockdev headers. The pointer value is only stored, never
 * dereferenced in vfs.c.                                                   */
extern void *g_fc_bdev;
/* Simple registry: for now, one slot — extend to array later               */
static const vfs_filesystem_t *registered_fs = NULL;

int vfs_register_filesystem(const vfs_filesystem_t *fs)
{
    if (!fs) return -1;
    registered_fs = fs;
    uart_puts("[VFS] Registered filesystem: "); uart_puts(fs->name); uart_puts("\n");
    return 0;
}

int vfs_mount(const char *fsname, const char *mountpoint, uint32_t flags, void *opts)
{
    (void)opts;
    if (!registered_fs || !registered_fs->mount) return -1;
    void *fsdata = NULL;
    int rc = registered_fs->mount(fsname, mountpoint, flags, &fsdata);
    if (rc == 0) {
        uart_puts("[VFS] Mounted "); uart_puts(fsname);
        uart_puts(" at "); uart_puts(mountpoint); uart_puts("\n");
    }
    return rc;
}

/* ── FileCore VFS driver ────────────────────────────────────────────────── */
/* boot247: Wire the proven SBPr parser into the VFS layer.                 */

/* FileCore readdir callback — returns root entries for "/" path */
static int filecore_vfs_readdir(void *fsdata, const char *path,
                                 vfs_dirent_t *dirent, uint32_t *cookie)
{
    (void)fsdata;
    (void)path;   /* TODO: parse path for subdirectory traversal */

    /* cookie tracks position in the directory listing */
    int rc = filecore_get_root_entry(*cookie, dirent);
    if (rc == 0) {
        (*cookie)++;
        return 0;       /* entry returned, more may follow */
    }
    *cookie = 0;
    return -1;          /* end of directory */
}

/* FileCore mount callback */
static int filecore_vfs_mount(const char *dev, const char *mnt,
                               uint32_t flags, void **fsdata)
{
    (void)dev; (void)mnt; (void)flags;

    uart_puts("[VFS] FileCore: initialising disc scan\n");
    filecore_init();
    filecore_list_root();

    *fsdata = g_fc_bdev;
    uart_puts("[VFS] FileCore: mounted SCSI::laxarusb.$\n");
    return 0;
}

static int filecore_vfs_umount(void *fsdata)
{
    (void)fsdata;
    uart_puts("[VFS] FileCore: unmounted\n");
    return 0;
}

static const vfs_filesystem_t filecore_fs_driver = {
    .name    = "FileCore",
    .flags   = VFS_FS_READONLY | VFS_FS_CASE_INSENSITIVE,
    .mount   = filecore_vfs_mount,
    .umount  = filecore_vfs_umount,
    .readdir = filecore_vfs_readdir,
};

/* Called from kernel_main during boot (optional — kernel.c calls filecore
 * directly for now; this provides the VFS-layer path for future use).     */
void vfs_register_filecore(void)
{
    uart_puts("[VFS] Registering FileCore driver\n");
    int rc = vfs_register_filesystem(&filecore_fs_driver);
    if (rc != 0) {
        uart_puts("[VFS] Registration failed\n");
        return;
    }
    vfs_mount("FileCore", "HardDisc0", 0, NULL);
}
