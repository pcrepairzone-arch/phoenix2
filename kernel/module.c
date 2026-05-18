/* module.c — RISC OS Module Manager for Phoenix OS (AArch64)
 *
 * Fixes applied (boot268, April 2026):
 *   - Removed conflicting extern declarations (kmalloc/memset/strncmp/strlen
 *     already declared in kernel.h via module.h include)
 *   - module_header +0x28 = swi_decode_code (not 'flags')
 *   - module_header +0x2C = messages_file (not 'feature_flags_ptr')
 *   - module_header +0x30 = module_flags (AArch64 bit 24) ADDED
 *   - workspace + workspace_size fields in risc_os_module_t ADDED
 *   - module_dump_list() renamed from module_list() (name collision fix)
 *   - start_offset check corrected (0=no start, non-zero=has start, both valid)
 *   - uart_putdec/uart_puthex32 replaced with self-contained helpers
 *
 * Reference: RISC OS PRM 1-31, Charles Ferguson RISC OS 64 PRM
 *   https://pyromaniac.riscos.online/pyromaniac/prm/overview.html
 */

#include "module.h"
#include "vfs.h"
#include "errno.h"

/* kernel.h (included via module.h) already declares:
 *   kmalloc, kfree, memset, strncmp, strlen, memcpy
 * Only uart_puts needs a forward declaration here.               */
extern void uart_puts(const char *s);

/* ── Internal UART helpers (avoids dependency on static fc_dec/fc_hex32) ─── */
static void mod_dec(uint32_t v) {
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { uart_puts("0"); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    uart_puts(buf + i);
}

static void mod_hex32(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[11]; buf[0]='0'; buf[1]='x';
    for (int i = 9; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[10] = '\0'; uart_puts(buf);
}

static risc_os_module_t *g_module_list = NULL;

#define DEFAULT_WORKSPACE_SIZE  4096

/* ── module_call_with_r12 ────────────────────────────────────────────────────
 * Call an AArch64 module entry point with x12 = workspace pointer.
 * Only works with AArch64-native modules (module_flags bit 24 set).
 * 32-bit ARM modules need AArch32 EL0 mode switching — future work.        */
static int module_call_with_r12(void *code_entry, void *workspace,
                                  uint64_t arg0, uint64_t arg1)
{
    /* AArch64: all registers are 64-bit. Return value in x0, cast to int.
     * Cannot use "mov x20, w0" — must use "mov x20, x0" or "mov w20, w0".
     * Use uint64_t for the result variable, then cast.                    */
    uint64_t result;
    asm volatile(
        "mov x12, %1\n"
        "mov x0,  %2\n"
        "mov x1,  %3\n"
        "blr %4\n"
        "mov %0, x0\n"
        : "=r" (result)
        : "r" ((uint64_t)(uintptr_t)workspace),
          "r" (arg0), "r" (arg1),
          "r" ((uint64_t)(uintptr_t)code_entry)
        : "x0", "x1", "x12", "x30", "memory"
    );
    return (int)(uint32_t)result;
}

/* ── module_register_native ──────────────────────────────────────────────── */
/* boot379: extended with swi_base + swi_fn so native C modules can handle
 * SWIs without needing a binary module header.  Pass 0/NULL for modules that
 * expose no SWIs.                                                            */
int module_register_native(const char *name,
                            int (*init)(void),
                            int (*final)(void),
                            int (*service)(uint32_t reason, uint32_t *regs),
                            uint32_t swi_base,
                            int (*swi_fn)(uint32_t swi_offset, uint32_t *regs))
{
    (void)final; (void)service;

    risc_os_module_t *mod = (risc_os_module_t *)kmalloc(sizeof(risc_os_module_t));
    if (!mod) return -ENOMEM;
    memset(mod, 0, sizeof(*mod));
    mod->name     = name;
    mod->swi_base = swi_base;
    mod->swi_fn   = swi_fn;

    int rc = module_register(mod);
    if (rc == 0 && init) {
        init();
        mod->initialised = 1;
    }
    return rc;
}

/* ── module_register ─────────────────────────────────────────────────────── */
int module_register(risc_os_module_t *mod)
{
    if (!mod || !mod->name) return -EINVAL;
    mod->next     = g_module_list;
    g_module_list = mod;
    uart_puts("[Module] Registered: "); uart_puts(mod->name); uart_puts("\n");
    return 0;
}

/* ── module_load_from_memory ─────────────────────────────────────────────── */
int module_load_from_memory(void *buffer, uint32_t size, const char *suggested_name)
{
    if (!buffer || size < 0x34u) {
        uart_puts("[Module] Buffer too small\n");
        return -ENOEXEC;
    }

    risc_os_module_header_t *hdr = (risc_os_module_header_t *)buffer;

    /* ── boot388: 64-bit discriminator ──────────────────────────────────────
     * Per riscos64-clib/modules/modhead.s, the init_entry word at +0x04 has
     * bit 30 SET for AArch64 native modules ("not ARM32").  The actual init
     * code offset is the word with bits 30-31 cleared.  32-bit ARM modules
     * (and old-style modules) have bit 30 = 0.
     *
     * module_flags (+0x30) bits 4–7 further classify the ISA (value 1 =
     * AArch64), but bit 30 of init_entry is the primary gate.              */
    uint32_t raw_init = hdr->init_entry;
    int      is_64bit = (raw_init & MODULE_INIT_64BIT) ? 1 : 0;
    /* Actual offset strips bits 30-31 */
    uint32_t init_off = raw_init & ~(3u << 30);

    /* Resolve module title from header.  title_ptr is an offset from the
     * start of the module; must be within the buffer.                       */
    const char *mod_name = suggested_name ? suggested_name : "Unnamed";
    if (hdr->title_ptr && hdr->title_ptr < size)
        mod_name = (const char *)((uint8_t *)buffer + hdr->title_ptr);

    uart_puts("[Module] '"); uart_puts(mod_name);
    uart_puts("' "); mod_dec(size); uart_puts(" bytes  ");
    uart_puts(is_64bit ? "AArch64" : "ARM32");
    uart_puts("  init_raw="); mod_hex32(raw_init);
    uart_puts("  flags="); mod_hex32(hdr->module_flags); uart_puts("\n");

    if (!is_64bit) {
        /* 32-bit ARM stub path — register so *Modules shows the disc module,
         * but don't attempt to execute ARM32 init code on AArch64 kernel.
         * Exception: old-style module with flags==0 AND init_off==0 has no
         * init code to call anyway — still register as stub (safe).         */
        uart_puts("[Module] 32-bit ARM stub: '"); uart_puts(mod_name);
        uart_puts("' registered (AArch32 init deferred)\n");

        risc_os_module_t *mod = (risc_os_module_t *)kmalloc(sizeof(risc_os_module_t));
        if (!mod) return -ENOMEM;
        memset(mod, 0, sizeof(*mod));

        mod->workspace = kmalloc(DEFAULT_WORKSPACE_SIZE);
        if (!mod->workspace) { kfree(mod); return -ENOMEM; }
        memset(mod->workspace, 0, DEFAULT_WORKSPACE_SIZE);
        mod->workspace_size = DEFAULT_WORKSPACE_SIZE;

        mod->base_addr   = buffer;
        mod->size        = size;
        mod->header      = hdr;
        mod->name        = mod_name;
        mod->swi_base    = hdr->swi_base;
        mod->initialised = 0;    /* not called — AArch32 EL0 pending */

        int rc = module_register(mod);
        if (rc != 0) { kfree(mod->workspace); kfree(mod); return rc; }
        uart_puts("[Module] *** '"); uart_puts(mod_name);
        uart_puts("' stub registered (AArch32 init deferred) ***\n");
        return 0;
    }

    /* ── AArch64 native — full load path ──────────────────────────────────── */
    uart_puts("[Module] AArch64 loading '"); uart_puts(mod_name);
    uart_puts("' init_off="); mod_hex32(init_off); uart_puts("\n");

    risc_os_module_t *mod = (risc_os_module_t *)kmalloc(sizeof(risc_os_module_t));
    if (!mod) return -ENOMEM;
    memset(mod, 0, sizeof(*mod));

    mod->workspace = kmalloc(DEFAULT_WORKSPACE_SIZE);
    if (!mod->workspace) { kfree(mod); return -ENOMEM; }
    memset(mod->workspace, 0, DEFAULT_WORKSPACE_SIZE);
    mod->workspace_size = DEFAULT_WORKSPACE_SIZE;

    mod->base_addr = buffer;
    mod->size      = size;
    mod->header    = hdr;
    mod->name      = mod_name;
    mod->swi_base  = hdr->swi_base;

    int rc = module_register(mod);
    if (rc != 0) { kfree(mod->workspace); kfree(mod); return rc; }

    /* Call init entry — use stripped offset (bits 30-31 removed).
     * AArch64 ABI: x0 = tail word/regs, x1 = instance, x12 = private word. */
    if (init_off && init_off < size) {
        uart_puts("[Module] Calling Init() for "); uart_puts(mod_name); uart_puts("\n");
        void *init_fn = (uint8_t *)buffer + init_off;
        int init_rc = module_call_with_r12(init_fn, mod->workspace, 0, 0);
        mod->initialised = (init_rc == 0);
        if (!mod->initialised) {
            uart_puts("[Module] Init() failed rc="); mod_dec((uint32_t)init_rc);
            uart_puts("\n");
        }
    }
    return 0;
}

/* ── module_load_from_file ───────────────────────────────────────────────── */
int module_load_from_file(const char *path)
{
    uart_puts("[Module] RMLOAD: "); uart_puts(path); uart_puts("\n");

    file_t *f = vfs_open(path, 0);
    if (!f) return -ENOENT;

#define MAX_MODULE_SIZE (2u * 1024u * 1024u)
    uint8_t *buf = (uint8_t *)kmalloc(MAX_MODULE_SIZE);
    if (!buf) { vfs_close(f); return -ENOMEM; }

    ssize_t total = 0, bytes;
    while (total < (ssize_t)MAX_MODULE_SIZE) {
        bytes = vfs_read(f, buf + total, MAX_MODULE_SIZE - (uint32_t)total);
        if (bytes <= 0) break;
        total += bytes;
    }
    vfs_close(f);

    if (total == 0) { kfree(buf); return -ENOENT; }

    int rc = module_load_from_memory(buf, (uint32_t)total, NULL);
    if (rc != 0) kfree(buf);
    return rc;
}

/* ── swi_dispatch ────────────────────────────────────────────────────────── */
/* boot379: check swi_fn first for native C modules; then fall through to
 * binary module header path for AArch64 binary modules.                     */
int swi_dispatch(uint32_t swi_number, uint32_t *regs)
{
    uint32_t chunk  = swi_number & 0xFFFFFF00u;
    uint32_t offset = swi_number & 0x000000FFu;
    risc_os_module_t *mod = g_module_list;
    while (mod) {
        if (mod->swi_base == chunk) {
            /* Native C module path (boot379) */
            if (mod->swi_fn) {
                uart_puts("[SWI] "); uart_puts(mod->name);
                uart_puts(" "); mod_hex32(swi_number); uart_puts("\n");
                return mod->swi_fn(offset, regs);
            }
            /* Binary AArch64 module path */
            if (mod->header && mod->header->swi_handler) {
                uart_puts("[SWI] "); uart_puts(mod->name);
                uart_puts(" "); mod_hex32(swi_number); uart_puts("\n");
                void *handler = (uint8_t *)mod->base_addr + mod->header->swi_handler;
                return module_call_with_r12(handler, mod->workspace,
                                             (uint64_t)swi_number,
                                             (uint64_t)(uintptr_t)regs);
            }
        }
        mod = mod->next;
    }
    uart_puts("[SWI] Unhandled: "); mod_hex32(swi_number); uart_puts("\n");
    return -ENOSYS;
}

/* ── module_broadcast_service ────────────────────────────────────────────── */
int module_broadcast_service(uint32_t service_reason, uint32_t *regs)
{
    uart_puts("[Service] "); mod_hex32(service_reason); uart_puts("\n");
    risc_os_module_t *mod = g_module_list;
    while (mod) {
        if (mod->header && mod->header->service_entry && mod->initialised) {
            void *fn = (uint8_t *)mod->base_addr + mod->header->service_entry;
            int rc = module_call_with_r12(fn, mod->workspace,
                                           (uint64_t)service_reason,
                                           (uint64_t)(uintptr_t)regs);
            if (rc == 0) {
                uart_puts("[Service] Claimed by "); uart_puts(mod->name);
                uart_puts("\n");
                return 0;
            }
        }
        mod = mod->next;
    }
    return -ENOENT;
}

/* ── module_dump_list ────────────────────────────────────────────────────── */
void module_dump_list(void)
{
    uart_puts("[Module] Loaded modules:\n");
    risc_os_module_t *mod = g_module_list;
    int count = 0;
    while (mod) {
        uart_puts("  "); uart_puts(mod->name);
        if (mod->initialised) uart_puts(" [OK]");
        uart_puts("\n");
        mod = mod->next;
        count++;
    }
    if (count == 0) uart_puts("  (none)\n");
}

/* ── Native C module declarations ───────────────────────────────────────── */
/* PhoenixDHCP is a plain C module registered via module_register_native(). */
extern int dhcp_module_init(void);
extern int dhcp_module_final(void);

/* PhoenixGENET — DCI4 network driver module (boot376).
 * Wraps BCM GENET as a proper RISC OS DCI4 driver module.  Must be
 * registered AFTER PhoenixDHCP because DHCP waits for link-up and
 * sets g_our_ip; by the time PhoenixGENET announces DCIDRIVER_STARTING
 * the hardware is already initialised and the IP is bound.                 */
extern int genet_module_init(void);
extern int genet_module_final(void);
extern int genet_module_swi(uint32_t offset, uint32_t *regs);
#define ETHERGE_SWI_BASE  0x59F00u   /* DCI4 EtherGENET SWI chunk */

/* PhoenixResolver — DNS stub resolver module (boot379).
 * Implements Inet6Sources Resolver SWI chunk 0x46000.
 * Registered AFTER PhoenixGENET/DHCP so g_our_ip and DNS server
 * are already populated when resolver_module_init() tests a lookup.       */
extern int resolver_module_init(void);
extern int resolver_module_final(void);
extern int resolver_module_swi(uint32_t offset, uint32_t *regs);
#define RESOLVER_SWI_BASE  0x46000u
/* boot368: dhcp_test_module_init removed — test module no longer linked into
 * production build.  See Makefile comment and Tests/dhcp_test_module.c.     */

/* ── Embedded AArch64 test modules ──────────────────────────────────────── */
/*
 * These modules are linked directly into the Phoenix binary as named
 * sections (.text.test_module, .text.cursor_module).  They are proper
 * RISC OS AArch64 module binaries — header + code — loaded the same way
 * a future disc-resident 64-bit module will be loaded.
 *
 * test_module    — proves module_call_with_r12() works (init returns 0)
 * cursor_module  — draws caret + mouse pointer, probes USB HID
 */
extern uint8_t test_module_start[];
extern uint8_t test_module_end[];
extern uint8_t cursor_module_start[];
extern uint8_t cursor_module_end[];

/* ── module_init_all ─────────────────────────────────────────────────────── */
void module_init_all(void)
{
    uart_puts("[Module] Module system initialising...\n");

    /* ── Load embedded AArch64 binary modules ──────────────────────────
     * boot382: guard size == 0 before attempting load.  cursor_module.c
     * and test_module are stubs (no binary payload linked in yet); the
     * linker.ld PROVIDE symbols resolve to the same address so size = 0.
     * module_load_from_memory rejects them with "Buffer too small" (size
     * < 0x34 header minimum) — confusing but harmless.  Skip cleanly.   */
    uart_puts("[Module] Loading embedded AArch64 modules...\n");

    int rc = 0;

    uint32_t tm_size = (uint32_t)(test_module_end - test_module_start);
    if (tm_size == 0) {
        uart_puts("[Module] PhoenixTest: not embedded (stub), skipping\n");
    } else {
        uart_puts("[Module] test_module: size="); mod_dec(tm_size); uart_puts(" bytes\n");
        rc = module_load_from_memory(test_module_start, tm_size, "PhoenixTest");
        if (rc == 0)
            uart_puts("[Module] *** PhoenixTest — first AArch64 module executed ***\n");
        else {
            uart_puts("[Module] PhoenixTest load failed rc="); mod_dec((uint32_t)rc);
            uart_puts("\n");
        }
    }

    uint32_t cm_size = (uint32_t)(cursor_module_end - cursor_module_start);
    if (cm_size == 0) {
        uart_puts("[Module] PhoenixCursor: not embedded (stub), skipping\n");
    } else {
        uart_puts("[Module] cursor_module: size="); mod_dec(cm_size); uart_puts(" bytes\n");
        rc = module_load_from_memory(cursor_module_start, cm_size, "PhoenixCursor");
        if (rc == 0)
            uart_puts("[Module] *** PhoenixCursor — caret + pointer active ***\n");
        else {
            uart_puts("[Module] PhoenixCursor load failed rc="); mod_dec((uint32_t)rc);
            uart_puts("\n");
        }
    }

    uart_puts("[Module] Embedded modules done\n");

    /* ── Register PhoenixDHCP native module ────────────────────────────── */
    rc = module_register_native("PhoenixDHCP",
                                 dhcp_module_init,
                                 dhcp_module_final,
                                 NULL,
                                 0, NULL);   /* no SWIs */
    if (rc != 0) {
        uart_puts("[Module] PhoenixDHCP registration failed rc=");
        mod_dec((uint32_t)rc); uart_puts("\n");
    }

    /* ── Register PhoenixGENET DCI4 module (boot376) ──────────────────────── */
    rc = module_register_native("PhoenixGENET",
                                 genet_module_init,
                                 genet_module_final,
                                 NULL,
                                 ETHERGE_SWI_BASE,
                                 genet_module_swi);
    if (rc != 0) {
        uart_puts("[Module] PhoenixGENET registration failed rc=");
        mod_dec((uint32_t)rc); uart_puts("\n");
    }

    /* ── Register PhoenixResolver DNS module (boot379) ─────────────────────── */
    rc = module_register_native("PhoenixResolver",
                                 resolver_module_init,
                                 resolver_module_final,
                                 NULL,
                                 RESOLVER_SWI_BASE,
                                 resolver_module_swi);
    if (rc != 0) {
        uart_puts("[Module] PhoenixResolver registration failed rc=");
        mod_dec((uint32_t)rc); uart_puts("\n");
    }

    /* boot368: PhoenixDHCPTest registration REMOVED.
     * The unit test ran at every production boot using a fake MAC
     * (de:ad:be:ef:00:01) and synthetic frames, leaving the DHCP state
     * machine bound to 192.168.0.145 before genet_init() ran.  When real
     * DHCP started with the hardware MAC the test binding was discarded and
     * a full discover/offer/request/ack cycle had to run from scratch.
     * dhcp_test_module.c is available for standalone test builds only.      */

    /* External .ffa module loading via module_load_from_file() once
     * VFS is fully wired to the FileCore read path (future work).          */
    uart_puts("[Module] Module system ready\n");
    module_dump_list();
}
