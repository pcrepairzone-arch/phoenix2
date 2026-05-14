/* module.h — RISC OS Module support for Phoenix OS (AArch64)
 *
 * Module header layout per RISC OS PRM 1-31 with 64-bit extensions
 * from Charles Ferguson's RISC OS 64 / Pyromaniac work.
 *
 * Key addition for AArch64: module_flags field at +0x30.
 *   bit 24 set = AArch64 native module (Phoenix target).
 *   bit 24 clear = 32-bit ARM module (future AArch32 EL0 compat path).
 *
 * Author: Phoenix OS project
 * Updated: boot268, April 2026
 */

#ifndef MODULE_H
#define MODULE_H

#include "kernel.h"
#include "vfs.h"

/* ── RISC OS module header (PRM 1-31 + RO64 extensions) ─────────────────── */
typedef struct __attribute__((packed)) risc_os_module_header {
    uint32_t start_offset;      /* +0x00: offset to start code, 0=no start   */
    uint32_t init_entry;        /* +0x04: offset to initialisation code       */
    uint32_t final_entry;       /* +0x08: offset to finalisation code         */
    uint32_t service_entry;     /* +0x0C: offset to service call handler      */
    uint32_t title_ptr;         /* +0x10: offset to title string              */
    uint32_t help_ptr;          /* +0x14: offset to help string               */
    uint32_t keyword_ptr;       /* +0x18: offset to keyword table             */
    uint32_t swi_base;          /* +0x1C: SWI chunk base number               */
    uint32_t swi_handler;       /* +0x20: offset to SWI handler code          */
    uint32_t swi_table;         /* +0x24: offset to SWI name table            */
    uint32_t swi_decode_code;   /* +0x28: offset to SWI decode code           */
    uint32_t messages_file;     /* +0x2C: offset to messages filename (RO5+)  */
    uint32_t module_flags;      /* +0x30: bit 24 = AArch64 native (RO64)      */
} risc_os_module_header_t;

/* module_flags bit 24: set = AArch64, clear = 32-bit ARM */
#define MODULE_FLAG_AARCH64     (1u << 24)

/* ── Module instance ─────────────────────────────────────────────────────── */
typedef struct risc_os_module {
    struct risc_os_module      *next;
    risc_os_module_header_t    *header;
    const char                 *name;
    uint32_t                    swi_base;
    void                       *base_addr;
    uint32_t                    size;
    int                         initialised;
    void                       *workspace;       /* private workspace (x12)    */
    uint32_t                    workspace_size;
    void                       *private_data;
} risc_os_module_t;

/* ── Public API ──────────────────────────────────────────────────────────── */
int  module_register_native(const char *name,
                             int (*init)(void),
                             int (*final)(void),
                             int (*service)(uint32_t reason, uint32_t *regs));
int  module_register(risc_os_module_t *mod);
int  module_load_from_file(const char *path);
int  module_load_from_memory(void *buffer, uint32_t size,
                              const char *suggested_name);
void module_init_all(void);
void module_dump_list(void);   /* renamed from module_list to avoid collision */
int  swi_dispatch(uint32_t swi_number, uint32_t *regs);
int  module_broadcast_service(uint32_t service_reason, uint32_t *regs);

#endif /* MODULE_H */
