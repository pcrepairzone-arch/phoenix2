/*
 * elf64.h – ELF64 Headers for RISC OS Phoenix
 * Defines structures for execve (Ehdr, Phdr, Shdr, Dyn, Rel, Sym)
 * Author:R Andrews Grok 4 – 10 Dec 2025
 */

#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>

#define ELFMAG          "\177ELF"
#define SELFMAG         4

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EM_AARCH64      183

typedef struct {
    unsigned char e_ident[16];  // Magic + class + data + version + OSABI + ABIVersion + padding
    uint16_t e_type;            // Object file type
    uint16_t e_machine;         // Target machine
    uint32_t e_version;         // Object file version
    uint64_t e_entry;           // Entry point virtual address
    uint64_t e_phoff;           // Program header table file offset
    uint64_t e_shoff;           // Section header table file offset
    uint32_t e_flags;           // Processor-specific flags
    uint16_t e_ehsize;          // ELF header size in bytes
    uint16_t e_phentsize;       // Program header table entry size
    uint16_t e_phnum;           // Program header table entry count
    uint16_t e_shentsize;       // Section header table entry size
    uint16_t e_shnum;           // Section header table entry count
    uint16_t e_shstrndx;        // Section header string table index
} Elf64_Ehdr;

#define PT_LOAD         1  // Loadable segment
#define PT_DYNAMIC      2  // Dynamic linking info

typedef struct {
    uint32_t p_type;            // Segment type
    uint32_t p_flags;           // Segment flags
    uint64_t p_offset;          // Segment file offset
    uint64_t p_vaddr;           // Segment virtual address
    uint64_t p_paddr;           // Segment physical address
    uint64_t p_filesz;          // Segment size in file
    uint64_t p_memsz;           // Segment size in memory
    uint64_t p_align;           // Segment alignment
} Elf64_Phdr;

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        7
#define SHT_DYNAMIC     6

typedef struct {
    uint32_t sh_name;           // Section name (index into string table)
    uint32_t sh_type;           // Section type
    uint64_t sh_flags;          // Section flags
    uint64_t sh_addr;           // Virtual address in memory
    uint64_t sh_offset;         // Offset in file
    uint64_t sh_size;           // Size in bytes
    uint32_t sh_link;           // Link to other section
    uint32_t sh_info;           // Miscellaneous info
    uint64_t sh_addralign;      // Alignment
    uint64_t sh_entsize;        // Entry size if table
} Elf64_Shdr;

/* Dynamic linking */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_JMPREL       23

typedef struct {
    int64_t d_tag;              // Dynamic entry type
    union {
        uint64_t d_ptr;         // Pointer
        uint64_t d_val;         // Value
    } d_un;
} Elf64_Dyn;

typedef struct {
    uint64_t r_offset;          // Address offset
    uint32_t r_info;            // Relocation type and symbol index
} Elf64_Rel;

typedef struct {
    uint64_t r_offset;          // Address offset
    uint32_t r_type;            // Relocation type
    int64_t  r_addend;          // Constant addend
} Elf64_Rela;

typedef struct {
    uint32_t st_name;           // Symbol name index in string table
    uint8_t  st_info;           // Type and binding
    uint8_t  st_other;          // Visibility
    uint16_t st_shndx;          // Section index
    uint64_t st_value;          // Symbol value
    uint64_t st_size;           // Symbol size
} Elf64_Sym;

#endif /* ELF64_H */