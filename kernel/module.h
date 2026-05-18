/* module.h — RISC OS Module support for Phoenix OS (AArch64)
 *
 * Module header layout per RISC OS PRM 1-31 with 64-bit extensions
 * from Charles Ferguson's RISC OS 64 / riscos64-clib (modhead.s).
 *
 * 64-bit module discriminator (boot388):
 *   init_entry (+0x04) bit 30 SET  → AArch64 native module.
 *     Actual init offset = init_entry & ~(3u<<30).
 *   init_entry bit 30 CLEAR        → 32-bit ARM module (or old-style).
 *
 * module_flags (+0x30) for 64-bit modules:
 *   bits 4–7 = 1 → AArch64 ISA (from riscos64-clib modhead.s).
 *   bit  2   = 1 → zero-init size present at +0x34.
 *   (Not used for discriminating — bit 30 of init_entry is the gate.)
 *
 * Author: Phoenix OS project
 * Updated: boot388, May 2026
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

/* init_entry bit 30: set = AArch64 native (strip before use as offset).
 * module_flags bits 4-7 = 1: AArch64 ISA (secondary confirmation only). */
#define MODULE_INIT_64BIT       (1u << 30)   /* bit 30 of +0x04 init_entry  */
#define MODULE_FLAG_AARCH64     (1u << 24)   /* old Phoenix guess — kept for compat */

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
    /* boot379: native C SWI handler — set for C modules, NULL for binary ones */
    int                       (*swi_fn)(uint32_t swi_offset, uint32_t *regs);
} risc_os_module_t;

/* ── Public API ──────────────────────────────────────────────────────────── */
/* boot379: swi_base + swi_fn added so native C modules can handle SWIs.
 * Pass 0 / NULL for modules that expose no SWIs (e.g. PhoenixDHCP).        */
int  module_register_native(const char *name,
                             int (*init)(void),
                             int (*final)(void),
                             int (*service)(uint32_t reason, uint32_t *regs),
                             uint32_t swi_base,
                             int (*swi_fn)(uint32_t swi_offset, uint32_t *regs));
int  module_register(risc_os_module_t *mod);
int  module_load_from_file(const char *path);
int  module_load_from_memory(void *buffer, uint32_t size,
                              const char *suggested_name);
void module_init_all(void);
void module_dump_list(void);   /* renamed from module_list to avoid collision */
int  swi_dispatch(uint32_t swi_number, uint32_t *regs);
int  module_broadcast_service(uint32_t service_reason, uint32_t *regs);

#endif /* MODULE_H */
