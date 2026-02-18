/*
 * dl.c – Dynamic Linker Stub for RISC OS Phoenix
 * Supports dlopen, dlsym, dlclose for ELF64 shared libraries
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "vfs.h"
#include "elf64.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define MAX_LIBS  32

typedef struct loaded_lib {
    char path[256];
    Elf64_Dyn *dynamic;
    Elf64_Sym *symtab;
    char *strtab;
    Elf64_Rela *rela;
    uint64_t *got;
} loaded_lib_t;

static loaded_lib_t loaded_libs[MAX_LIBS];
static int num_libs = 0;

/* Simple symbol resolver (stub) */
uint64_t resolve_symbol(const char *name) {
    if (strcmp(name, "printf") == 0) return (uint64_t)debug_print;
    return 0;
}

/* dlopen – load shared library */
void *dlopen(const char *filename, int flags) {
    if (!filename) return NULL;

    file_t *file = vfs_open(filename, O_RDONLY);
    if (!file) return NULL;

    Elf64_Ehdr ehdr;
    if (vfs_read(file, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        vfs_close(file);
        return NULL;
    }

    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_type != ET_DYN) {
        vfs_close(file);
        return NULL;
    }

    loaded_lib_t *lib = kmalloc(sizeof(loaded_lib_t));
    strncpy(lib->path, filename, 255);

    // Parse PT_DYNAMIC (stub – simplified)
    Elf64_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        vfs_seek(file, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
        vfs_read(file, &phdr, sizeof(phdr));
        if (phdr.p_type == PT_DYNAMIC) {
            lib->dynamic = kmalloc(phdr.p_filesz);
            vfs_seek(file, phdr.p_offset, SEEK_SET);
            vfs_read(file, lib->dynamic, phdr.p_filesz);
            break;
        }
    }

    vfs_close(file);

    // Register library
    if (num_libs < MAX_LIBS) {
        loaded_libs[num_libs++] = *lib;
    }

    debug_print("dlopen: Loaded %s\n", filename);
    return lib;
}

/* dlsym – lookup symbol */
void *dlsym(void *handle, const char *symbol) {
    loaded_lib_t *lib = (loaded_lib_t*)handle;
    if (!lib) return NULL;

    Elf64_Sym *sym = lib->symtab;
    while (sym->st_name) {
        if (strcmp(lib->strtab + sym->st_name, symbol) == 0) {
            return (void*)sym->st_value;
        }
        sym++;
    }
    return NULL;
}

/* dlclose – unload library */
int dlclose(void *handle) {
    // Stub – free memory
    debug_print("dlclose called\n");
    return 0;
}