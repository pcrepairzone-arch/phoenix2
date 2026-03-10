/*
 * filecore.c – FileCore Filesystem for RISC OS Phoenix
 * Implements RISC OS FileCore (ADFS-style) on block devices
 * Supports directories, file types (&FFF, &AFF, etc.), loading/executing by type
 * Integrates with VFS for modern apps and blockdev for storage (NVMe, USB, etc.)
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "vfs.h"
#include "blockdev.h"
#include "filecore.h"
// #include <string.h> /* removed - use kernel.h */

/* FileCore constants */
#define FCORE_DIR_DIR       0xFF
#define FCORE_DIR_FILE      0xFE
#define FCORE_FILE_TYPE_LEN 3  // &FFF format

/* Directory entry (10-byte name + metadata) */
typedef struct {
    uint8_t dir_type;           // 0xFF = directory, 0xFE = file
    uint8_t load_addr[4];       // Load address (bits 12-19 = file type)
    uint8_t exec_addr[4];       // Exec address
    uint8_t size[4];            // File size in bytes
    char    name[10];           // Padded filename (10 chars)
} fcore_dir_entry_t;

/* Superblock (simplified ADFS-like) */
typedef struct {
    uint32_t map_start;
    uint32_t dir_start;
    uint32_t data_start;
    uint32_t sectors;
} fcore_super_t;

/* FileCore private data per mount */
typedef struct {
    blockdev_t *dev;
    fcore_super_t super;
    uint32_t current_dir_lba;   // Current directory block
} fcore_fs_t;

/* FileCore file operations */
file_ops_t fcore_ops = {
    .read  = fcore_read,
    .write = fcore_write,
    .seek  = fcore_seek,
    .poll  = NULL,              // Synchronous I/O
    .close = fcore_close
};

/* Mount FileCore on a block device */
int fcore_mount(blockdev_t *dev, const char *mountpoint)
{
    fcore_fs_t *fs = kmalloc(sizeof(fcore_fs_t));
    if (!fs) return -1;

    fs->dev = dev;
    fs->current_dir_lba = 0;  // Root directory

    // Read superblock (stub – assume known format)
    vfs_block_read(dev, 0, 1, &fs->super);

    // Create root inode
    inode_t *root = vfs_alloc_inode();
    root->i_mode = S_IFDIR;
    root->private = fs;
    root->f_ops = &fcore_ops;

    debug_print("FileCore: Mounted %s at %s (%ld sectors)\n", 
                dev->name, mountpoint, fs->super.sectors);

    return 0;
}

/* Read from FileCore file */
ssize_t fcore_read(file_t *file, void *buf, size_t count)
{
    inode_t *inode = file->f_inode;
    fcore_fs_t *fs = inode->private;

    // Calculate LBA from file metadata (stub)
    uint64_t lba = inode->i_blocks * (fs->dev->block_size / 512) + (file->f_pos / fs->dev->block_size);

    ssize_t read = vfs_block_read(fs->dev, lba, (count + fs->dev->block_size - 1) / fs->dev->block_size, buf);

    if (read > 0) file->f_pos += read;
    return read;
}

/* Write to FileCore file */
ssize_t fcore_write(file_t *file, const void *buf, size_t count)
{
    inode_t *inode = file->f_inode;
    fcore_fs_t *fs = inode->private;

    uint64_t lba = inode->i_blocks * (fs->dev->block_size / 512) + (file->f_pos / fs->dev->block_size);

    ssize_t written = vfs_block_write(fs->dev, lba, (count + fs->dev->block_size - 1) / fs->dev->block_size, buf);

    if (written > 0) {
        file->f_pos += written;
        inode->i_size = file->f_pos > inode->i_size ? file->f_pos : inode->i_size;
        inode->i_blocks = (inode->i_size + fs->dev->block_size - 1) / fs->dev->block_size;
    }

    return written;
}

/* Seek in FileCore file */
off_t fcore_seek(file_t *file, off_t offset, int whence)
{
    inode_t *inode = file->f_inode;

    switch (whence) {
        case SEEK_SET: file->f_pos = offset; break;
        case SEEK_CUR: file->f_pos += offset; break;
        case SEEK_END: file->f_pos = inode->i_size + offset; break;
    }

    if (file->f_pos > inode->i_size) file->f_pos = inode->i_size;

    return file->f_pos;
}

/* Close FileCore file */
void fcore_close(file_t *file)
{
    // Flush if dirty (stub)
    if (file->f_flags & O_CREAT) {
        update_dir_entry(file->f_inode);
    }
}

/* Update directory entry with new size and file type */
static void update_dir_entry(inode_t *inode)
{
    fcore_fs_t *fs = inode->private;

    // Read dir block, update entry (stub – simplified)
    fcore_dir_entry_t entry;
    // ... compute entry LBA, read, update load_addr with file_type, size
    vfs_block_write(fs->dev, entry_lba, 1, &entry);
}

/* Open FileCore file – parse dir entry for type */
file_t *fcore_open(fcore_fs_t *fs, const char *name, int flags)
{
    // Read current dir block (stub)
    fcore_dir_entry_t entries[32];
    vfs_block_read(fs->dev, fs->current_dir_lba, 1, entries);

    for (int i = 0; i < 32; i++) {
        if (strncmp((char*)entries[i].name, name, 10) == 0) {
            inode_t *inode = vfs_alloc_inode();
            inode->i_mode = entries[i].dir_type == 0xFF ? S_IFDIR : S_IFREG;
            inode->i_size = *(uint32_t*)entries[i].size;
            inode->file_type = *(uint16_t*)entries[i].load_addr & 0xFFF;  // Extract RISC OS file type
            inode->private = fs;

            file_t *file = vfs_alloc_file();
            file->f_inode = inode;
            file->f_pos = 0;
            file->f_flags = flags;
            file->f_ops = &fcore_ops;

            return file;
        }
    }

    return NULL;
}

/* Execute file based on file type */
int fcore_exec(const char *path)
{
    file_t *file = fcore_open(/* fs from mount */, path, O_RDONLY);
    if (!file) return -1;

    inode_t *inode = file->f_inode;
    uint16_t type = inode->file_type;

    char *app = get_app_for_file_type(type);
    if (app) {
        vfs_close(file);
        return execve(app, (char*[]){app, path, NULL}, environ);
    }

    // Direct exec if it's an application
    vfs_close(file);
    return execve(path, (char*[]){path, NULL}, environ);
}

/* Module init */
_kernel_oserror *module_init(const char *arg, int podule)
{
    // Auto-mount on all block devices (stub)
    for (int i = 0; i < blockdev_count; i++) {
        fcore_mount(blockdev_list[i], "/");
    }
    debug_print("FileCore loaded –