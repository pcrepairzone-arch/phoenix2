/*
 * filecore.c – Phoenix FileCore detection + ADFS new-map zone reader
 *
 * boot222 milestones
 *
 * boot215: lba_base=0 revert (full-disc overlay fix).
 * boot217: fc_bit_addr_to_data_lba — removed /id_len (1-bit-per-LFAU map).
 * boot218: correct map-sector LBAs; 64-byte dual probe. Step 2B confirmed
 *          bytes[24..27]=0x02FAD601 at LBA 0x775AC8. Chain traverse still
 *          broken (zone 22 all-free; fc_zone_lba gave DATA LBA not map LBA).
 * boot219: Full 2048-byte dump of LBA 0x775AC8. SBPr NewDir format decoded.
 *          9 root directory entries confirmed. Entry layout (28 bytes each):
 *            +0  load_addr (4)  +4  exec_addr (4)  +8  length (4)
 *            +12 IDA (4)        +16 attr (1) + ext (3)
 *            +20 name_len (4)   +24 name_off (4, from name_table)
 *          Name table at 32 + count*28.  Names 0x0D-terminated, 4-byte padded.
 *          IDA array at 0x7D4.  "oven" at 0x7F8.  end_seq byte at 0x7FC.
 *
 * boot220 — Clean SBPr NewDir parser: formatted directory listing
 * ─────────────────────────────────────────────────────────────────────────
 *   Replaces boot219's verbose 2048-byte hex dump with structured parsing.
 *   Entry count: stop when (load & 0xFFF00000) != 0xFFF00000 (typed-file marker).
 *   All 9 root entries confirmed with correct names, IDAs, frags, chain offsets.
 *
 * boot221 — Step 3: empirical SBPr scan from root_probe+4 (512 LBAs)
 * ─────────────────────────────────────────────────────────────────────────
 *   Found 1 child of root at LBA=0x775AF0 (par=0x02fad601).  Other subdirs
 *   are not near root in the address space — chain traverse is required.
 *
 * boot222 — Enable chain traverse; fix map LBA in Step 1/2A
 * ─────────────────────────────────────────────────────────────────────────
 *   KEY FIX: Step 1 / Step 2A used fc_zone_lba(chain_zone) which gives the
 *   PHYSICAL DATA LBA of zone N.  Correct map LBA = disc_map_lba + zone.
 *   (disc_map_lba is map-copy-1 zone 0; copy-1 zone N = disc_map_lba + N.)
 *
 *   adfs_chain_traverse: was SKIPPED since boot221.  Now enabled via Step 3
 *   loop over all 9 root entries.  Also adds SBPr detection (was Hugo/Nick only).
 *
 *   frag_id IS the raw bit-address in the zone map (not multiplied by id_len):
 *     zone = frag_id / zone_bits  (zone_bits = zone_spare + used_bits = 4096)
 *     bit_in_zone = frag_id % zone_bits
 *   chain_offset = IDA >> id_len = number of forward hops in the chain.
 *   Terminal bit address → data_lba via fc_bit_addr_to_data_lba().
 *
 * boot236 — Detect NVMe full-disc Castle FileCore (no 0xAD partition entry)
 * ─────────────────────────────────────────────────────────────────────────
 *   Problem: Samsung 970 EVO Plus (via Realtek USB-NVMe bridge, usb0) has a
 *   Castle bootstrap MBR with 0x55AA but NO partition table entries (all type
 *   bytes = 0x00).  filecore_parse_mbr() returns -2 ("no recognised partition")
 *   and the old code did `if (mbr_rc < 0) continue` — skipping the device
 *   entirely without trying the full-disc FileCore overlay.
 *
 *   Fix: distinguish mbr_rc==-1 (truly no MBR / GPT protective) from -2 (MBR
 *   present but no recognised RISC OS partition).  For -2, fall through to the
 *   full-disc overlay attempt at absolute LBA 6.  This is the same path that
 *   successfully detects the Lexar USB drive.
 *
 *   NVMe disc parameters (from DiscKnight SCSI 4 output):
 *     disc_name='NVMe'  disc_size=233G (0x3A38B2E000)
 *     log2bpmb=13  LFAU=8192 bytes (16 sectors)  id_len=21
 *     zone_spare=32  nzones=7512
 *     map_copy1=0x1D1D140000  map_copy2=0x1D1D4EB000
 *     root_dir=0x0A8BA001  root_dir_lba=0x1D1D896000
 *
 *   Also adds raw partition table type-byte dump (always) so non-standard
 *   types are visible in the boot log.
 *
 * boot239 — DiscRec struct corrections + disc_name/root_dir_size from zone DiscRec
 * ─────────────────────────────────────────────────────────────────────────
 *   Three correctness fixes identified by DiscReader source comparison:
 *
 *   Fix 1 (struct offsets): DiscReader.h defines DISC_RECORD_FILECORE_VER=44
 *   and DISC_RECORD_ROOT_DIR_SIZE=48.  Phoenix had these labels swapped:
 *   root_dir_size at +44, reserved_48 at +48.  Renamed to filecore_ver (+44)
 *   and root_dir_size (+48).
 *
 *   Fix 2 (cycle_id): DiscReader's authoritative name for offset +20 is cycle_id
 *   (increments on every write — NOT a static fingerprint).  Renamed disc_id→cycle_id
 *   in the struct and all log output.
 *
 *   Fix 3 (disc_name + root_dir_size from zone DiscRec): The boot DiscRec at LBA 6
 *   can have a bad checksum on full-disc Castle drives (NVMe) — it reads an empty
 *   disc_name ('') even though the zone mid DiscRec holds the real name ('NVMe').
 *   After filecore_list_root reads the mid-zone Full DiscRec, if the zone name is
 *   non-empty and the stored name is empty, g_dr_disc_name is updated.  Similarly,
 *   root_dir_size is updated from the zone DiscRec so Step 2B can use the canonical
 *   value directly (no longer needs the two-phase peek as primary source).
 *
 * boot237 — Variable-size BigDir: dynamic oven offset + secperlfau scan step
 * ─────────────────────────────────────────────────────────────────────────
 *   Problem: NVMe root directory has dir_size=0x1000 (4096 bytes) — two
 *   full LFAU extents — but Step 2B only read 4 sectors (2048 bytes) and
 *   checked oven at the fixed offset 2040.  For a 4096-byte directory the
 *   correct offsets are oven=4088, end_seq=4092.  Hardcoded 2040 hits the
 *   middle of the entry data → oven=NO / seq MISMATCH reported in boot236.
 *
 *   Fix 1 (Step 2B): two-phase read.  Peek sector 0 to extract dir_size
 *   from header bytes [12..15].  Sanity-check (must be ≥2048, ≤FC_MAX_DIR_SIZE,
 *   multiple of 512).  Allocate dir_size bytes, read dir_size/512 sectors.
 *   Compute oven_off = dir_size − 8, seq_off = dir_size − 4.  Name-string
 *   upper bound also updated to oven_off rather than hardcoded 2040.
 *
 *   Fix 2 (SCAN): same dynamic dir_size / oven_off logic.  Buffer enlarged
 *   to FC_MAX_DIR_SIZE (16 KB).  After finding SBPr in sector 0, read
 *   dir_size from header and read (dir_size/512 − 1) more sectors.  CAND_FAIL
 *   now logs the computed oven_off so future mismatch root-causes are visible.
 *
 *   Fix 3 (SCAN step): old step=2 was correct for Lexar (secperlfau=2) but
 *   wrong for NVMe (secperlfau=16 — LFAU=8192 bytes=16 sectors).  Changed
 *   to use secperlfau from the stored disc params.  Directories on the NVMe
 *   are always at secperlfau-aligned LBAs; stepping by 2 wastes 14 reads
 *   per slot and misses the correct alignment window.
 *
 * boot235 — Add CAND_FAIL diagnostic for SBPr candidates failing oven/seq
 * ─────────────────────────────────────────────────────────────────────────
 *   Logs [SCAN] CAND_FAIL for any LBA that has 'SBPr' magic but fails the
 *   "oven" tail or master-sequence check.  Also logs [SCAN] RD_FAIL for read
 *   errors on sectors 1-3 of a candidate.  This will expose why Pinboard2 at
 *   LBA 0x775B90 (seen in boot232) is absent from boot234 output.
 *
 * boot234 — Correct SBPr entry_start formula from DiscReader source
 * ─────────────────────────────────────────────────────────────────────────
 *   The three-way flags rule in boot233 was wrong in principle (coincidentally
 *   correct for small names).  DiscReader.h defines the authoritative layout:
 *
 *   BigDir header (E+/F+ SBPr format):
 *     [0]     BIGDIR_STARTMASSEQ  start master sequence number
 *     [4..7]  BIGDIR_STARTNAME    "SBPr" magic
 *     [8..11] BIGDIR_NAMELEN      directory name length (NOT flags)
 *     [12..15]BIGDIR_SIZE         directory size in bytes
 *     [16..19]BIGDIR_ENTRIES      entry count
 *     [20..23]BIGDIR_NAMESSIZE    name heap size
 *     [24..27]BIGDIR_PARENT       parent IDA
 *     [28+]   BIGDIR_NAME         the directory name string (variable length)
 *     [28 + WHOLEWORDS(namelen+1)] first entry (BIGDIR_ENTRYSIZE = 28 each)
 *
 *   Per entry: +0 load, +4 exec, +8 len, +12 IDA, +16 attrs, +20 namelen, +24 nameoff
 *
 *   entry_start = 28 + ((namelen + 1 + 3) & ~3)  [WHOLEWORDS rounds up to 4 bytes]
 *
 *   Verified against bootlog232 + DiscKnight data (11/12 dirs match perfectly):
 *     namelen 1-3 → first=0x20  (e.g. "$"=1 char → root ✓)
 *     namelen 4-7 → first=0x24  (e.g. "!Boot"=5 ✓, "Choices"=7 ✓)
 *     namelen 8-11→ first=0x28  (e.g. "Pinboard"=8 ✓, "Pinboard2"=9 ✓)
 *
 *   Also: what we logged as "flags" was BIGDIR_NAMELEN.  Renamed accordingly.
 *
 *   Root directory entries (confirmed from bootlog219):
 *     1  !Boot        &FFF  2048  IDA=0x02FAD701  frag=93056   coff=95
 *     2  Apps         &FFF  2048  IDA=0x03024001  frag=73728   coff=96
 *     3  Diversions   &FFF  2048  IDA=0x03047901  frag=146560  coff=96
 *     4  Documents    &FFF  2048  IDA=0x0304AC01  frag=153088  coff=96
 *     5  Printing     &FFF  2048  IDA=0x02F09401  frag=18944   coff=94
 *     6  Public       &FFF  2048  IDA=0x02F0DA01  frag=27904   coff=94
 *     7  Updates      &FFF  2048  IDA=0x02F0DB01  frag=28032   coff=94
 *     8  Utilities    &FFF  2048  IDA=0x02F0DD01  frag=28288   coff=94
 *     9  Wallpaper    &DDC  2048  IDA=0x02EFC201  frag=254208  coff=93
 *
 *   Lexar USB disc params (from zone 962 Full DiscRec + DiskKnight):
 *     log2ss=9, id_len=19, log2bpmb=10, bpmb=1024, secperlfau=2
 *     zone_spare=32, used_bits=4064, dr_size=480, total_nzones=1924
 *     disc_map_lba=0x774BC0  map_copy2_lba=0x775344  root_probe=0x775AC8
 *     root_dir IDA=0x02FAD601 (frag_id=92928 chain_off=95)
 *     disc_name='laxarusb'
 *
 * Author: R Andrews – 26 Nov 2025
 * Updated: boot213 – Apr 2026 – lba_base = part_lba fix (WRONG — reverted)
 * Updated: boot214 – Apr 2026 – scan all devices; prefer named disc
 * Updated: boot215 – Apr 2026 – lba_base=0 revert; direct root LBA read
 * Updated: boot217 – Apr 2026 – 1-bit-per-LFAU map; ROOT_LBA=0x02CC80
 * Updated: boot218 – Apr 2026 – correct map-sector LBAs; 64-byte dual probe
 * Updated: boot219 – Apr 2026 – full 2048-byte dir dump; NewDir header parse
 * Updated: boot220 – Apr 2026 – clean SBPr NewDir parser; formatted dir list
 * Updated: boot292 – May 2026 – zone DiscRec name probe in scan + multi-disc scoring
 */

#include "kernel.h"
#include "vfs.h"
#include "blockdriver.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern blockdev_t *blockdev_list[];
extern int blockdev_count;

/* ── Module state ─────────────────────────────────────────────────────────── */
blockdev_t *g_fc_bdev     = NULL;
uint32_t    g_fc_lba_base = 0;
uint32_t    g_fc_sectors  = 0;
uint8_t     g_fc_type     = 0;

/* ── VFS root entry cache (populated during root dir parse) ──────────────── */
#define FC_ROOT_CACHE_MAX  32
static vfs_dirent_t  g_root_cache[FC_ROOT_CACHE_MAX];
static uint32_t      g_root_cache_count = 0u;
static int           g_root_cache_valid = 0;

/* Disc record parameters (populated from boot DiscRec at LBA 6) */
static uint8_t  g_dr_log2ss     = 9;    /* log2 sector size → 512 B       */
static uint8_t  g_dr_log2bpmb   = 10;   /* log2 bytes per map bit → 1024  */
static uint16_t g_dr_zone_spare = 32;   /* spare bits per zone             */
static uint8_t  g_dr_id_len     = 19;   /* map entry width (bits)          */
static uint8_t  g_dr_nzones     = 0;    /* nzones (low byte from DiscRec)  */
static uint8_t  g_dr_nzones_hi  = 0;    /* nzones high byte (big_flag ext) */
static uint8_t  g_dr_big_flag   = 0;
static uint32_t g_dr_root_dir      = 0;    /* IDA of root directory           */
static uint32_t g_dr_root_dir_size = 0;    /* root dir size from DiscRec +48  */
static char     g_dr_disc_name[11] = {0};

/* ── Tiny uart helpers ────────────────────────────────────────────────────── */
static void fc_hex8(uint8_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[5] = "0x";
    buf[2] = h[(v>>4)&0xF]; buf[3] = h[v&0xF]; buf[4] = '\0';
    uart_puts(buf);
}

static void fc_hex32(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[11] = "0x";
    for (int i = 9; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[10] = '\0'; uart_puts(buf);
}

static void fc_dec(uint32_t v) {
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { uart_puts("0"); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    uart_puts(buf + i);
}

/* Boot DiscRec checksum: sum of bytes[0..510], compare with byte[511] */
static uint8_t fc_checksum(const uint8_t *buf) {
    uint32_t sum = 0;
    for (int i = 0; i < 511; i++) sum += buf[i];
    return (uint8_t)(sum & 0xFF);
}

/* Print a 10-char RISC OS space-padded name */
static void fc_print_name(const char *name, int len) {
    for (int i = 0; i < len; i++) {
        char c = name[i];
        if (c == '\0') break;
        if (c == ' ') break;   /* space = end of name in RISC OS convention */
        char s[2] = {c, '\0'};
        uart_puts(s);
    }
}

/* ── MBR structures ────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t num_sectors;
} mbr_part_t;

#define MBR_SIG_OFFSET    510
#define MBR_SIG_LO        0x55
#define MBR_SIG_HI        0xAA
#define MBR_PARTS_OFFSET  446

#define PART_TYPE_RISCOS    0xAD
#define PART_TYPE_FAT32     0x0B
#define PART_TYPE_FAT32_LBA 0x0C
#define PART_TYPE_GPT_PROT  0xEE

static const char *part_type_name(uint8_t t) {
    switch (t) {
        case 0x00: return "Empty";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x82: return "Linux Swap";
        case 0x83: return "Linux";
        case 0xAD: return "RISC OS FileCore";
        case 0xEE: return "GPT Protective MBR";
        default:   return "Unknown";
    }
}

/* ── FileCore DiscRec (64-byte layout, PRM 3-577) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  log2_sector_size;   /* +0  */
    uint8_t  spt;                /* +1  */
    uint8_t  heads;              /* +2  */
    uint8_t  density;            /* +3  */
    uint8_t  id_len;             /* +4  */
    uint8_t  log2_bpmb;         /* +5  */
    uint8_t  skew;               /* +6  */
    uint8_t  boot_opt;           /* +7  */
    uint8_t  low_sector;         /* +8  */
    uint8_t  nzones;             /* +9  */
    uint16_t zone_spare;         /* +10 */
    uint32_t root_dir;           /* +12 */
    uint32_t disc_size_lo;       /* +16 */
    uint16_t cycle_id;           /* +20  increments on every write (DiscReader: DISC_RECORD_CYCLE_ID) */
    char     disc_name[10];      /* +22 */
    uint32_t disc_type;          /* +32 */
    uint32_t disc_size_hi;       /* +36 */
    uint8_t  log2_share_size;    /* +40 */
    uint8_t  big_flag;           /* +41 */
    uint8_t  nzones_hi;          /* +42  high byte of nzones for big disc */
    uint8_t  format_version;     /* +43 */
    uint32_t filecore_ver;       /* +44  DiscReader: DISC_RECORD_FILECORE_VER  */
    uint32_t root_dir_size;      /* +48  DiscReader: DISC_RECORD_ROOT_DIR_SIZE */
    uint32_t reserved_52;        /* +52 */
    uint32_t reserved_56;        /* +56 */
    /* total = 60 bytes; zone check word adds 4 bytes before this in zone sectors */
} filecore_disc_rec_t;

#define FC_BOOT_LBA        6        /* BootAdd% = &C00, sectors of 512 B → LBA 6  */
#define FC_BOOT_OFFSET     0x1C0    /* byte offset of DiscRec within boot sector   */
#define FC_ZONE_DR_OFFSET  4        /* byte offset of DiscRec within zone sector   */
#define FC_MAX_DIR_SIZE    16384u   /* largest BigDir we'll handle (32 sectors)    */

/* ── Log a DiscRec's key fields ──────────────────────────────────────────── */
static void fc_log_discrec(const filecore_disc_rec_t *dr, const char *prefix) {
    uint32_t total_nzones = dr->nzones;
    if (dr->big_flag) total_nzones += (uint32_t)dr->nzones_hi << 8;

    uart_puts(prefix);
    uart_puts("log2ss=");   fc_dec(dr->log2_sector_size);
    uart_puts(" id_len=");  fc_dec(dr->id_len);
    uart_puts(" log2bpmb="); fc_dec(dr->log2_bpmb);
    uart_puts(" nzones=");  fc_dec(total_nzones);
    uart_puts(" zone_spare="); fc_dec(dr->zone_spare);
    uart_puts("\n");
    uart_puts(prefix);
    uart_puts("root_dir="); fc_hex32(dr->root_dir);
    uart_puts(" disc_size="); fc_hex32(dr->disc_size_hi);
    uart_puts(":"); fc_hex32(dr->disc_size_lo);
    uart_puts(" big_flag="); fc_dec(dr->big_flag);
    uart_puts("\n");
    uart_puts(prefix);
    uart_puts("disc_name='");
    fc_print_name(dr->disc_name, 10);
    uart_puts("'  cycle_id="); fc_hex32(dr->cycle_id);
    uart_puts("  disc_type="); fc_hex32(dr->disc_type);
    uart_puts("\n");
}

/* ── Sanity-check a DiscRec: must have plausible log2ss and nzones ─────────
 * Returns 1 if it looks valid, 0 if it's garbage.                         */
static int fc_discrec_plausible(const filecore_disc_rec_t *dr) {
    if (dr->log2_sector_size < 8 || dr->log2_sector_size > 12) return 0;
    if (dr->nzones == 0) return 0;
    if (dr->id_len < 8 || dr->id_len > 24) return 0;
    return 1;
}

/* ── Try to read and validate the boot DiscRec for a given lba_base ─────────
 * PartMgr uses BootAdd% = &C00:
 *   boot_sector = lba_base + &C00 / lba_size
 *   disc_rec    = buffer + (&C00 % lba_size) + &1C0
 *
 * For 512-byte sectors (Lexar): boot_sector = lba_base + 6, disc_rec at +0x1C0
 * For 4096-byte sectors (NVMe): boot_sector = lba_base + 0, disc_rec at +0xDC0
 *
 * Returns 0 = valid + good checksum
 *         1 = valid disc rec but bad checksum (overlay mode)
 *        -1 = garbage                                                      */
static int fc_try_boot_discrec(blockdev_t *bd, uint32_t lba_base, uint8_t *buf,
                                filecore_disc_rec_t *dr_out)
{
    uint32_t lba_size = bd->block_size;
    uint64_t boot_lba = (uint64_t)lba_base + 0xC00UL / lba_size;
    uint32_t boot_off = (uint32_t)(0xC00UL % lba_size) + 0x1C0U;

    uart_puts("[FileCore]   boot_lba="); fc_hex32((uint32_t)boot_lba);
    uart_puts(" boot_off="); fc_hex32(boot_off); uart_puts("\n");

    if (bd->ops->read(bd, boot_lba, 1, buf) < 0) {
        uart_puts("[FileCore]   read error\n");
        return -1;
    }

    filecore_disc_rec_t *dr = (filecore_disc_rec_t *)(buf + boot_off);
    if (!fc_discrec_plausible(dr)) {
        uart_puts("[FileCore]   boot DiscRec not plausible (log2ss=");
        fc_hex8(dr->log2_sector_size); uart_puts(" nzones="); fc_dec(dr->nzones);
        uart_puts(")\n");

        /* boot267: NVMe 4096-byte physical sector fallback via RTL9210 bridge.
         * The bridge presents 512-byte logical sectors but the physical NVMe
         * sector is 4096 bytes.  The DiscRec sits at byte 0xDC0 within the
         * first physical sector (LBA 0 in 512-byte terms).
         * Only attempt this when the standard probe was at the expected location. */
        if (boot_off == 0x1C0u && boot_lba == (uint64_t)lba_base + 6u) {
            uart_puts("[FileCore]   Trying NVMe offset 0xDC0 at LBA 0...\n");
            if (bd->ops->read(bd, (uint64_t)lba_base, 1, buf) >= 0) {
                dr = (filecore_disc_rec_t *)(buf + 0xDC0u);
                if (fc_discrec_plausible(dr)) {
                    uart_puts("[FileCore]   NVMe DiscRec valid at offset 0xDC0\n");
                    goto fc_boot_dr_valid;
                }
            }
        }
        return -1;
    }

fc_boot_dr_valid:;
    uint8_t calc = fc_checksum(buf);
    int ck_ok = (calc == buf[511]);

    uart_puts("[FileCore]   boot DiscRec ");
    uart_puts(ck_ok ? "VALID + checksum OK" : "plausible (bad checksum → overlay)");
    uart_puts("\n");

    if (dr_out) *dr_out = *dr;
    return ck_ok ? 0 : 1;
}

/* ── Try to read the zone-0 DiscRec (at lba_base, byte FC_ZONE_DR_OFFSET) ──
 * Returns 0 = valid, -1 = not valid                                        */
static int fc_try_zone0_discrec(blockdev_t *bd, uint32_t lba_base, uint8_t *buf,
                                 filecore_disc_rec_t *dr_out)
{
    if (bd->ops->read(bd, (uint64_t)lba_base, 1, buf) < 0) {
        uart_puts("[FileCore]   zone-0 read error\n");
        return -1;
    }
    filecore_disc_rec_t *dr = (filecore_disc_rec_t *)(buf + FC_ZONE_DR_OFFSET);
    if (!fc_discrec_plausible(dr)) {
        uart_puts("[FileCore]   zone-0 DiscRec not plausible\n");
        return -1;
    }
    uart_puts("[FileCore]   zone-0 DiscRec VALID\n");
    if (dr_out) *dr_out = *dr;
    return 0;
}

/* ── MBR parser ──────────────────────────────────────────────────────────── */
static int filecore_parse_mbr(const uint8_t *sec, uint8_t *out_type,
                               uint32_t *out_lba, uint32_t *out_sectors)
{
    if (sec[MBR_SIG_OFFSET] != MBR_SIG_LO || sec[MBR_SIG_OFFSET+1] != MBR_SIG_HI) {
        uart_puts("[FileCore] No MBR signature\n");
        return -1;
    }
    uart_puts("[FileCore] MBR 0x55AA confirmed\n");

    const mbr_part_t *parts = (const mbr_part_t *)(sec + MBR_PARTS_OFFSET);

    /* GPT protective? Return -2 so caller tries fc_scan_gpt() before
     * the full-disc overlay fallback.                                    */
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == PART_TYPE_GPT_PROT) {
            uart_puts("[FileCore] GPT protective MBR detected\n");
            return -2;
        }
    }

    /* boot236: always dump raw partition table bytes so we can see non-standard types */
    uart_puts("[FileCore] Partition table (raw types): ");
    for (int i = 0; i < 4; i++) {
        fc_hex8(parts[i].type);
        uart_puts(i < 3 ? " " : "\n");
    }

    int riscos_slot = -1, fat32_slot = -1;
    uart_puts("[FileCore] Partition table:\n");
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == 0) continue;
        uart_puts("[FileCore]   ["); fc_dec((uint32_t)(i+1)); uart_puts("] ");
        uart_puts("type="); fc_hex8(parts[i].type);
        uart_puts(" ("); uart_puts(part_type_name(parts[i].type)); uart_puts(")");
        uart_puts("  start="); fc_hex32(parts[i].lba_start);
        uart_puts("  size=");  fc_dec(parts[i].num_sectors); uart_puts("\n");

        if (parts[i].type == PART_TYPE_RISCOS && riscos_slot < 0)
            riscos_slot = i;
        if ((parts[i].type == PART_TYPE_FAT32 ||
             parts[i].type == PART_TYPE_FAT32_LBA) && fat32_slot < 0)
            fat32_slot = i;
    }

    int winner = (riscos_slot >= 0) ? riscos_slot : fat32_slot;
    if (winner < 0) {
        uart_puts("[FileCore] No RISC OS or FAT32 partition found\n");
        return -2;   /* MBR present but no recognised RISC OS partition */
    }

    *out_type    = parts[winner].type;
    *out_lba     = parts[winner].lba_start;
    *out_sectors = parts[winner].num_sectors;
    return 0;
}

/* ── Compute zone_lba for zone N using disc params ──────────────────────────
 * Exact PartMgr formula:
 *   used_bits   = (1 << log2ss) * 8 - zone_spare    (= 4064 for Lexar)
 *   dr_size     = 60 * 8 = 480 bits
 *   secperlfau  = (1 << log2bpmb) / (1 << log2ss)  (= 2 for Lexar)
 *   zone_lba(0) = lba_base
 *   zone_lba(N) = lba_base + (N * used_bits - dr_size) * secperlfau  (N > 0)
 */
static uint32_t fc_zone_lba(uint32_t zone, uint32_t lba_base,
                             uint32_t used_bits, uint32_t dr_size,
                             uint32_t secperlfau)
{
    if (zone == 0) return lba_base;
    return lba_base + (zone * used_bits - dr_size) * secperlfau;
}

/* ── Read and log DiscRec from zone N ───────────────────────────────────────
 * Returns 0 if valid DiscRec found, -1 otherwise.
 * disc_rec_out is filled on success.                                       */
static int fc_read_zone_discrec(blockdev_t *bd, uint32_t zone,
                                 uint32_t lba_base, uint32_t used_bits,
                                 uint32_t dr_size, uint32_t secperlfau,
                                 uint8_t *buf, filecore_disc_rec_t *dr_out)
{
    uint32_t lba = fc_zone_lba(zone, lba_base, used_bits, dr_size, secperlfau);

    uart_puts("[FileCore]   Zone "); fc_dec(zone);
    uart_puts(" LBA="); fc_hex32(lba); uart_puts("\n");

    if (bd->ops->read(bd, (uint64_t)lba, 1, buf) < 0) {
        uart_puts("[FileCore]   read error\n");
        return -1;
    }

    filecore_disc_rec_t *dr = (filecore_disc_rec_t *)(buf + FC_ZONE_DR_OFFSET);
    if (!fc_discrec_plausible(dr)) {
        uart_puts("[FileCore]   DiscRec not plausible\n");
        return -1;
    }

    fc_log_discrec(dr, "[FileCore]     ");
    if (dr_out) *dr_out = *dr;
    return 0;
}


/* ── fc_scan_gpt ──────────────────────────────────────────────────────────────
 * boot267: Scan GPT partition table for a RISC OS FileCore partition.
 * GPT header at LBA 1, partition entries at LBA 2+.
 * No standard GUID for Castle FileCore -- probe every partition's disc record.
 * Returns 0 on success with out_lba/out_sec filled, -1 on failure.           */
static int fc_scan_gpt(blockdev_t *bd, uint8_t *buf,
                        uint32_t *out_lba, uint32_t *out_sec)
{
    uart_puts("[FileCore] GPT: reading header at LBA 1...\n");
    if (bd->ops->read(bd, 1ULL, 1, buf) < 0) {
        uart_puts("[FileCore] GPT: LBA 1 read error\n"); return -1;
    }

    if (buf[0]!='E'||buf[1]!='F'||buf[2]!='I'||buf[3]!=' '||
        buf[4]!='P'||buf[5]!='A'||buf[6]!='R'||buf[7]!='T') {
        uart_puts("[FileCore] GPT: no EFI PART signature\n"); return -1;
    }
    uart_puts("[FileCore] GPT: EFI PART signature found\n");

    uint32_t part_entry_lba  = (uint32_t)buf[72]|((uint32_t)buf[73]<<8)|
                                ((uint32_t)buf[74]<<16)|((uint32_t)buf[75]<<24);
    uint32_t num_parts       = (uint32_t)buf[80]|((uint32_t)buf[81]<<8)|
                                ((uint32_t)buf[82]<<16)|((uint32_t)buf[83]<<24);
    uint32_t part_entry_size = (uint32_t)buf[84]|((uint32_t)buf[85]<<8)|
                                ((uint32_t)buf[86]<<16)|((uint32_t)buf[87]<<24);

    uart_puts("[FileCore] GPT: part_entry_lba="); fc_hex32(part_entry_lba);
    uart_puts("  num_parts=");  fc_dec(num_parts);
    uart_puts("  entry_size="); fc_dec(part_entry_size); uart_puts("\n");

    if (part_entry_size < 128u || num_parts > 128u) {
        uart_puts("[FileCore] GPT: implausible entry size/count\n"); return -1;
    }

    uint8_t *gpt_buf = (uint8_t *)kmalloc(512u);
    if (!gpt_buf) return -1;

    uint32_t entries_per_sector = 512u / part_entry_size;
    uint32_t sectors_needed     = (num_parts + entries_per_sector - 1u) / entries_per_sector;
    if (sectors_needed > 32u) sectors_needed = 32u;

    for (uint32_t s = 0u; s < sectors_needed; s++) {
        if (bd->ops->read(bd, (uint64_t)(part_entry_lba + s), 1, gpt_buf) < 0) break;
        for (uint32_t e = 0u; e < entries_per_sector; e++) {
            uint8_t *entry = gpt_buf + e * part_entry_size;
            int empty = 1;
            for (int k = 0; k < 16; k++) if (entry[k]) { empty = 0; break; }
            if (empty) continue;

            uint64_t start_lba = (uint64_t)entry[32]|((uint64_t)entry[33]<<8)|
                                  ((uint64_t)entry[34]<<16)|((uint64_t)entry[35]<<24)|
                                  ((uint64_t)entry[36]<<32)|((uint64_t)entry[37]<<40)|
                                  ((uint64_t)entry[38]<<48)|((uint64_t)entry[39]<<56);
            uint64_t end_lba   = (uint64_t)entry[40]|((uint64_t)entry[41]<<8)|
                                  ((uint64_t)entry[42]<<16)|((uint64_t)entry[43]<<24)|
                                  ((uint64_t)entry[44]<<32)|((uint64_t)entry[45]<<40)|
                                  ((uint64_t)entry[46]<<48)|((uint64_t)entry[47]<<56);
            uint32_t plba = (uint32_t)(start_lba & 0xFFFFFFFFu);
            uint32_t psec = (uint32_t)((end_lba - start_lba + 1u) & 0xFFFFFFFFu);

            uart_puts("[FileCore] GPT part: start="); fc_hex32(plba);
            uart_puts("  size="); fc_dec(psec); uart_puts("\n");

            if (bd->ops->read(bd, (uint64_t)(plba + 6u), 1, gpt_buf) >= 0) {
                filecore_disc_rec_t *dr = (filecore_disc_rec_t *)(gpt_buf + 0x1C0u);
                if (fc_discrec_plausible(dr)) {
                    uart_puts("[FileCore] GPT: FileCore disc record found!\n");
                    uart_puts("[FileCore]   disc_name='");
                    fc_print_name(dr->disc_name, 10); uart_puts("'\n");
                    *out_lba = plba;
                    *out_sec = psec;
                    kfree(gpt_buf);
                    return 0;
                }
            }
        }
    }
    kfree(gpt_buf);
    uart_puts("[FileCore] GPT: no FileCore partition found\n");
    return -1;
}

/* ── filecore_init ───────────────────────────────────────────────────────────
 * Exactly mirrors PartMgr's detection sequence.
 * For each block device:
 *   1. Read LBA 0 → parse MBR → find 0xAD partition at part_lba
 *   2. Try zone-0 DiscRec at (part_lba, byte 4)
 *   3. Try boot DiscRec at (part_lba + 6, byte 0x1C0)
 *   4. If both fail → "Full-disc FileCore overlayed over MBR"
 *      Try boot DiscRec at (LBA 6, byte 0x1C0)
 *      → lba_base = part_lba (NOT 0 — THE FIX, boot213)
 *   5. boot292: If boot DiscRec disc_name empty, probe mid-zone DiscRec for
 *      authoritative name (e.g. 'laxarusb' for Lexar, 'NVMe' for Castle NVMe).
 *   6. Score by media_class + has_name (boot293):
 *        NVMe named=14, SSD named=12, USB-Flash named=10, SD named=7
 *        NVMe unnamed=6, SSD unnamed=5, USB-Flash unnamed=3, SD unnamed=1
 *      Accept if score > current best.  Stop only at score=14 (NVMe named).
 *   7. Store lba_base, disc params, g_fc_bdev                              */
void filecore_init(void)
{
    uart_puts("[FileCore] Scanning block devices (boot300)...\n");

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) { uart_puts("[FileCore] kmalloc fail\n"); return; }

    /* boot293: media_class-aware scoring — NVMe > SSD > USB-Flash > SD.
     * boot296: no early exit — always scan all registered devices and keep
     * the highest scorer as boot pointer.  Device order doesn't matter.    */
    int best_score = 0;

    for (int i = 0; i < blockdev_count; i++) {
        blockdev_t *bd = blockdev_list[i];
        if (!bd || bd->size == 0 || !bd->ops || !bd->ops->read) continue;

        uart_puts("[FileCore] Device: "); uart_puts(bd->name);
        uart_puts("  block_size="); fc_dec(bd->block_size); uart_puts("\n");

        /* ── Step 1: Read MBR ─────────────────────────────────────────── */
        if (bd->ops->read(bd, 0ULL, 1, buf) < 0) {
            uart_puts("[FileCore]   LBA 0 read error\n"); continue;
        }

        uint8_t  part_type = 0;
        uint32_t part_lba  = 0;
        uint32_t part_sec  = 0;
        int mbr_rc = filecore_parse_mbr(buf, &part_type, &part_lba, &part_sec);

        /* boot236: distinguish true MBR failure (-1) from "MBR present but no
         * recognised RISC OS partition" (-2).  For -2 we still try the full-
         * disc FileCore overlay — that's how the NVMe (Castle full-disc, no
         * MBR partition entries) is detected.  Only -1 is a hard skip.       */
        if (mbr_rc == -1) {
            uart_puts("[FileCore]   No usable MBR on this device\n"); continue;
        }

        filecore_disc_rec_t dr;
        uint32_t resolved_lba_base = 0;
        int dr_found = 0;

        /* ── Step 2+3: Try partition-relative DiscRec (0xAD only) ────── */
        if (mbr_rc == 0 && part_type == PART_TYPE_RISCOS) {
            uart_puts("[FileCore] 0xAD partition at LBA="); fc_hex32(part_lba); uart_puts("\n");

            uart_puts("[FileCore] Trying zone-0 at part_lba="); fc_hex32(part_lba); uart_puts("\n");
            if (fc_try_zone0_discrec(bd, part_lba, buf, &dr) == 0) {
                uart_puts("[FileCore] Zone-0 DiscRec valid → partitioned ADFS\n");
                resolved_lba_base = part_lba;
                dr_found = 1;
            }

            if (!dr_found) {
                uart_puts("[FileCore] Trying boot DiscRec (part_lba+6)...\n");
                int rc = fc_try_boot_discrec(bd, part_lba, buf, &dr);
                if (rc >= 0) {
                    uart_puts("[FileCore] Boot DiscRec valid at part_lba+6 → partitioned ADFS\n");
                    resolved_lba_base = part_lba;
                    dr_found = 1;
                }
            }
        } else {
            /* mbr_rc==0 non-0xAD winner (FAT32 etc.), or mbr_rc==-2 (no
             * recognised partition at all — e.g. NVMe full-disc Castle).
             * Skip straight to the full-disc overlay attempt.             */
            uart_puts("[FileCore]   No 0xAD partition — trying full-disc overlay\n");
        }

        /* ── Step 4: Full-disc overlay fallback ───────────────────────── */
        /* ADFS full-disc overlay: the ADFS address space starts at LBA 0.
         * The MBR at LBA 0 IS zone 0 of the map.  Zone and data LBAs are
         * absolute (lba_base=0).  PartMgr confirms this: it reads Lexar
         * zone 962 at absolute LBA 0x774BC0 with lba_base=0.
         * boot213's lba_base=part_lba was WRONG — reverted in boot215.
         * boot236: also reached for devices with MBR but no 0xAD partition,
         * allowing full-disc Castle FileCore drives (NVMe) to be detected.  */
        if (!dr_found) {
            uart_puts("[FileCore] Both partition checks failed\n");

            /* boot267: Try GPT scan before full-disc overlay.
             * Handles GPT-partitioned discs (e.g. Samsung NVMe via RTL9210).  */
            uint32_t gpt_lba = 0, gpt_sec = 0;
            if (fc_scan_gpt(bd, buf, &gpt_lba, &gpt_sec) == 0) {
                uart_puts("[FileCore] GPT FileCore partition at LBA=");
                fc_hex32(gpt_lba); uart_puts("\n");
                int rc2 = fc_try_boot_discrec(bd, gpt_lba, buf, &dr);
                if (rc2 >= 0) {
                    uart_puts("[FileCore] GPT disc record valid\n");
                    resolved_lba_base = gpt_lba;
                    dr_found = 1;
                }
            }

            if (!dr_found) {
                uart_puts("[FileCore] → Full-disc FileCore overlayed over MBR\n");
                uart_puts("[FileCore] Trying boot DiscRec at absolute LBA 6...\n");
                int rc = fc_try_boot_discrec(bd, 0, buf, &dr);
                if (rc >= 0) {
                    uart_puts("[FileCore] Boot DiscRec valid at LBA 6 → lba_base=0 (full-disc overlay)\n");
                    resolved_lba_base = 0;
                    dr_found = 1;
                } else {
                    uart_puts("[FileCore] Absolute LBA 6 also bad — no ADFS on this device\n");
                }
            }
        }

        if (!dr_found) continue;

        /* boot292: If boot DiscRec has empty disc_name, probe the mid-zone
         * DiscRec which holds the authoritative name (e.g. 'laxarusb' for
         * Lexar full-disc overlay, 'NVMe' for Castle NVMe drive).
         * Do this BEFORE scoring so preference logic sees the real name.   */
        if (dr.disc_name[0] == '\0' || dr.disc_name[0] == ' ') {
            uint32_t ss_probe    = 1u << dr.log2_sector_size;
            uint32_t splfau_probe = (ss_probe > 0)
                                  ? (1u << dr.log2_bpmb) / ss_probe : 0u;
            uint32_t ubits_probe  = ss_probe * 8u - dr.zone_spare;
            uint32_t drsz_probe   = 60u * 8u;   /* 480 bits — Full DiscRec */
            uint32_t tnz_probe    = dr.nzones;
            if (dr.big_flag) tnz_probe += (uint32_t)dr.nzones_hi << 8;
            uint32_t mid_probe    = tnz_probe / 2u;

            if (mid_probe > 0 && splfau_probe > 0) {
                filecore_disc_rec_t dr_mid;
                if (fc_read_zone_discrec(bd, mid_probe, resolved_lba_base,
                                          ubits_probe, drsz_probe,
                                          splfau_probe, buf, &dr_mid) == 0) {
                    int mhn = (dr_mid.disc_name[0] != '\0' &&
                               dr_mid.disc_name[0] != ' ');
                    if (mhn) {
                        uart_puts("[FileCore]   boot292: zone disc_name='");
                        fc_print_name(dr_mid.disc_name, 10);
                        uart_puts("' overrides empty boot DiscRec\n");
                        for (int k = 0; k < 10; k++)
                            dr.disc_name[k] = dr_mid.disc_name[k];
                    }
                }
            }
        }

        /* ── Evaluate this candidate ─────────────────────────────────── */
        /* boot293: media_class-aware scoring.
         *
         * NVMe and SSD are preferred over USB flash and SD card because they
         * are designed as boot media — faster, higher write endurance, and
         * more reliable under frequent kernel rewrite cycles.
         *
         * Score table (named / unnamed):
         *   NVMe      14 /  6   — USB-NVMe bridge (RTL9210 etc.)
         *   SSD       12 /  5   — USB-SATA SSD enclosure (ASMedia etc.)
         *   USB-Flash 10 /  3   — thumb drive (Lexar laxarusb etc.)
         *   SD         7 /  1   — SD / eMMC card
         *   Unknown   10 /  3   — unclassified USB, treat as flash
         *
         * Stop scanning immediately at score >= 10 (named flash/SSD/NVMe).  */
        int this_has_name = (dr.disc_name[0] != '\0' && dr.disc_name[0] != ' ');
        int this_score;
        switch (bd->media_class) {
            case MEDIA_NVME:      this_score = this_has_name ? 14 :  6; break;
            case MEDIA_SSD:       this_score = this_has_name ? 12 :  5; break;
            case MEDIA_USB_FLASH: this_score = this_has_name ? 10 :  3; break;
            case MEDIA_SD:        this_score = this_has_name ?  7 :  1; break;
            default:              this_score = this_has_name ? 10 :  3; break;
        }
        int accept = (!g_fc_bdev) || (this_score > best_score);

        if (!accept) {
            uart_puts("[FileCore]   "); uart_puts(bd->name);
            uart_puts(": score="); fc_dec(this_score);
            uart_puts(" <= best="); fc_dec(best_score);
            uart_puts(" — keep scanning\n");
            continue;
        }

        /* ── Store disc parameters ────────────────────────────────────── */
        g_fc_bdev     = bd;
        g_fc_lba_base = resolved_lba_base;
        g_fc_sectors  = part_sec;
        g_fc_type     = PART_TYPE_RISCOS;
        best_score    = this_score;

        g_dr_log2ss     = dr.log2_sector_size;
        g_dr_log2bpmb   = dr.log2_bpmb;
        g_dr_zone_spare = dr.zone_spare;
        g_dr_id_len     = dr.id_len;
        g_dr_nzones        = dr.nzones;
        g_dr_nzones_hi     = dr.nzones_hi;
        g_dr_big_flag      = dr.big_flag;
        g_dr_root_dir      = dr.root_dir;
        g_dr_root_dir_size = dr.root_dir_size;  /* +48 per DiscReader */
        for (int k = 0; k < 10; k++) g_dr_disc_name[k] = dr.disc_name[k];
        g_dr_disc_name[10] = '\0';

        /* Log the root_dir_size from DiscRec (useful for variable-size BigDirs) */
        uart_puts("[FileCore]   root_dir_size (DiscRec +48)="); fc_dec(g_dr_root_dir_size);
        uart_puts("\n");

        uint32_t total_nzones = g_dr_nzones;
        if (g_dr_big_flag) total_nzones += (uint32_t)g_dr_nzones_hi << 8;

        uart_puts("[FileCore] *** Candidate: RISC OS FileCore on ");
        uart_puts(bd->name); uart_puts(" ***\n");
        uart_puts("[FileCore]   lba_base="); fc_hex32(g_fc_lba_base);
        uart_puts("  total_nzones="); fc_dec(total_nzones);
        uart_puts("  root_dir="); fc_hex32(g_dr_root_dir);
        uart_puts("\n");
        uart_puts("[FileCore]   disc_name='");
        fc_print_name(g_dr_disc_name, 10);
        uart_puts("'  score="); fc_dec(best_score); uart_puts("\n");
        uart_puts("[FileCore]   → score="); fc_dec(best_score);
        uart_puts(" (");
        uart_puts(best_score >= 14 ? "NVMe named" :
                  best_score >= 12 ? "SSD named"  :
                  best_score >= 10 ? "USB-Flash named" :
                  best_score >=  7 ? "SD named" : "unnamed");
        uart_puts(") — best so far\n");
    }

    kfree(buf);

    if (!g_fc_bdev)
        uart_puts("[FileCore] No RISC OS FileCore disc found\n");
}

/* Forward declarations for helpers defined later in this file */
uint32_t adfs_read_bits(const uint8_t *buf, uint32_t bitpos, int nbits);
static uint32_t fc_bit_addr_to_data_lba(uint32_t B, uint32_t lba_base,
                                         uint32_t zone_spare, uint32_t used_bits,
                                         uint32_t dr_size, uint32_t id_len,
                                         uint32_t secperlfau);
static int fc_ida_to_data_lba(uint32_t ida,
                               uint32_t lba_base,  uint32_t disc_map_lba,
                               uint32_t zone_spare, uint32_t used_bits,
                               uint32_t dr_size,    uint32_t id_len,
                               uint32_t secperlfau, uint32_t nzones,
                               uint32_t *out_lba);
static void adfs_dump_hugo_dir(const uint8_t *dir);
static void filecore_populate_root_cache(const uint8_t *dir, uint32_t count,
                                          uint32_t entry_start, uint32_t name_tab);
static int  adfs_chain_traverse(uint32_t frag_id, uint32_t chain_offset,
                                 uint32_t lba_base,  uint32_t disc_map_lba,
                                 uint32_t zone_spare, uint32_t used_bits,
                                 uint32_t dr_size,    uint32_t id_len,
                                 uint32_t secperlfau, uint32_t total_nzones);
static void adfs_hugo_brute_scan(uint32_t start_lba, uint32_t scan_lbas,
                                  uint32_t secperlfau);
static void filecore_step6_load_module(uint32_t disc_map_lba, uint32_t used_bits,
                                        uint32_t dr_size, uint32_t secperlfau,
                                        uint32_t total_nzones);

/* filecore_get_child_entry is defined later in this file; forward-declare so
 * filecore_list_root (which calls it in Step 5) can reference it.           */
int filecore_get_child_entry(uint32_t dir_sin, uint32_t idx, vfs_dirent_t *out);

/* module_load_from_memory declared in module.h — forward-declare here to
 * avoid pulling the full module.h include into filecore.c's scope.          */
extern int module_load_from_memory(void *buffer, uint32_t size,
                                    const char *suggested_name);

/* ── filecore_list_root ──────────────────────────────────────────────────────
 * Reads the Full DiscRec from zone 0 and zone (nzones_total / 2).
 * For the Lexar (whole-disc ADFS, lba_base=0):
 *   zone 0   = LBA 0       (corrupted by MBR — expected to fail)
 *   zone 962 = LBA 0x774BC0 (Full DiscRec, disc_name="HardDisc0")
 *
 * Also prints the root_dir IDA decode (frag_id, chain_offset) so the next
 * step (chain traverse to the root directory) can be verified.            */
void filecore_list_root(void)
{
    if (!g_fc_bdev) {
        uart_puts("[FileCore] No drive mounted\n"); return;
    }

    uart_puts("\n[FileCore] === Zone DiscRec scan (boot237) ===\n");
    uart_puts("[FileCore]   lba_base="); fc_hex32(g_fc_lba_base); uart_puts("\n");

    /* ── Derive zone-map geometry from stored disc params ─────────────── */
    uint32_t sector_size = 1u << g_dr_log2ss;          /* 512 for Lexar    */
    uint32_t bpmb        = 1u << g_dr_log2bpmb;        /* 1024 for Lexar   */
    uint32_t secperlfau  = bpmb / sector_size;          /* 2   for Lexar    */
    uint32_t bits_per_sec = sector_size * 8u;           /* 4096             */
    uint32_t used_bits   = bits_per_sec - g_dr_zone_spare; /* 4064          */
    uint32_t dr_size     = 60u * 8u;                    /* 480 bits         */

    uint32_t total_nzones = g_dr_nzones;
    if (g_dr_big_flag) total_nzones += (uint32_t)g_dr_nzones_hi << 8;

    uint32_t mid_zone = total_nzones / 2u;              /* 962 for Lexar    */

    /* ── disc_map_lba: LBA of zone-0 sector in map copy 1 ──────────────── *
     * Map copy 1 is stored as a contiguous block of total_nzones sectors.  *
     * Zone N's map sector (copy 1) = disc_map_lba + N.                     *
     * Zone N's map sector (copy 2) = disc_map_lba + total_nzones + N.      *
     * First sector after both copies = disc_map_lba + 2×total_nzones.      *
     *                                                                        *
     * disc_map_lba = lba_base + (mid_zone × used_bits − dr_size) × secperlfau
     *             = (962 × 4064 − 480) × 2 = 7,818,176 = 0x774BC0          *
     * This equals fc_zone_lba(mid_zone) = the physical data start of zone   *
     * 962, which is where RISC OS stores map copy 1.                        */
    uint32_t disc_map_lba = g_fc_lba_base
                          + (mid_zone * used_bits - dr_size) * secperlfau;

    uart_puts("[FileCore]   sector_size="); fc_dec(sector_size);
    uart_puts("  secperlfau="); fc_dec(secperlfau);
    uart_puts("  used_bits="); fc_dec(used_bits);
    uart_puts("  total_nzones="); fc_dec(total_nzones);
    uart_puts("  mid_zone="); fc_dec(mid_zone); uart_puts("\n");
    uart_puts("[FileCore]   disc_map_lba="); fc_hex32(disc_map_lba);
    uart_puts("  map_copy2_lba="); fc_hex32(disc_map_lba + total_nzones);
    uart_puts("  root_probe_lba="); fc_hex32(disc_map_lba + 2u*total_nzones);
    uart_puts("\n");

    /* ── Map Copy 1 address diagnostic ─────────────────────────────────── */
    {
        uint32_t map_lba     = fc_zone_lba(mid_zone, g_fc_lba_base,
                                            used_bits, dr_size, secperlfau);
        uint32_t map_byte_lo = map_lba << g_dr_log2ss;   /* disc_byte low 32b */
        uart_puts("[FileCore]   map_copy1_lba="); fc_hex32(map_lba);
        uart_puts("  disc_byte_lo="); fc_hex32(map_byte_lo); uart_puts("\n");
    }

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) { uart_puts("[FileCore] kmalloc fail\n"); return; }

    /* ── Zone 0 (usually corrupted by MBR overlay) ──────────────────── */
    uart_puts("[FileCore] Zone 0 (lba_base):\n");
    {
        filecore_disc_rec_t dr0;
        fc_read_zone_discrec(g_fc_bdev, 0, g_fc_lba_base,
                             used_bits, dr_size, secperlfau, buf, &dr0);
        /* Note: on Lexar (MBR overlay) this sector is the MBR itself —
         * the DiscRec bytes at +4 will be garbage. Expected.             */
    }

    /* ── Zone mid_zone = total_nzones/2 = 962 (Full DiscRec) ─────────── */
    uart_puts("[FileCore] Zone "); fc_dec(mid_zone); uart_puts(" (Full DiscRec):\n");
    {
        uint32_t zone962_lba = fc_zone_lba(mid_zone, g_fc_lba_base,
                                            used_bits, dr_size, secperlfau);
        uart_puts("[FileCore]   mid_zone="); fc_dec(mid_zone);
        uart_puts("  computed LBA="); fc_hex32(zone962_lba); uart_puts("\n");

        filecore_disc_rec_t dr962;
        if (fc_read_zone_discrec(g_fc_bdev, mid_zone, g_fc_lba_base,
                                  used_bits, dr_size, secperlfau, buf, &dr962) == 0) {
            uart_puts("[FileCore] *** Full DiscRec found in zone ");
            fc_dec(mid_zone); uart_puts(" ***\n");
            uart_puts("[FileCore]   disc_name='");
            fc_print_name(dr962.disc_name, 10);
            uart_puts("'\n");
            uart_puts("[FileCore]   root_dir="); fc_hex32(dr962.root_dir); uart_puts("\n");
            uart_puts("[FileCore]   root_dir_size (DiscRec +48)=");
            fc_dec(dr962.root_dir_size); uart_puts("\n");

            /* Verify it matches the boot DiscRec root_dir */
            if (dr962.root_dir == g_dr_root_dir) {
                uart_puts("[FileCore]   root_dir MATCHES boot DiscRec ✓\n");
            } else {
                uart_puts("[FileCore]   root_dir MISMATCH: boot=");
                fc_hex32(g_dr_root_dir); uart_puts("\n");
            }

            /* boot239: update g_dr_disc_name from the authoritative Full DiscRec.
             * The boot DiscRec (LBA 6) may have a bad checksum (NVMe overlay)
             * and can have an empty disc_name even when the zone DiscRec has the
             * real name (e.g. 'NVMe').  Overwrite with the zone name whenever it
             * is non-empty so the correct name is available to preference logic.  */
            int zone_has_name = (dr962.disc_name[0] != '\0' &&
                                 dr962.disc_name[0] != ' ');
            int boot_has_name = (g_dr_disc_name[0] != '\0' &&
                                 g_dr_disc_name[0] != ' ');
            if (zone_has_name && !boot_has_name) {
                uart_puts("[FileCore]   boot239: updating disc_name from zone DiscRec: '");
                fc_print_name(dr962.disc_name, 10);
                uart_puts("'\n");
                for (int k = 0; k < 10; k++) g_dr_disc_name[k] = dr962.disc_name[k];
                g_dr_disc_name[10] = '\0';
            }

            /* Also update root_dir_size if we got a plausible value from the zone DiscRec */
            if (dr962.root_dir_size >= 2048u && dr962.root_dir_size <= FC_MAX_DIR_SIZE
                    && (dr962.root_dir_size & 511u) == 0u) {
                g_dr_root_dir_size = dr962.root_dir_size;
                uart_puts("[FileCore]   boot239: root_dir_size confirmed from zone DiscRec: ");
                fc_dec(g_dr_root_dir_size); uart_puts(" bytes\n");
            }
        } else {
            uart_puts("[FileCore] Zone "); fc_dec(mid_zone);
            uart_puts(" DiscRec not valid — unexpected\n");
        }
    }

    /* ── IDA decode for root directory ──────────────────────────────────
     * IDA format (id_len=19 bits):
     *   bit 0         = 1 (new-map indirect address flag)
     *   bits [id_len-1:1] = frag_id (bit address of chain start in zone map)
     *   bits [31:id_len]  = chain_offset (hops to terminal LFAU)
     */
    {
        uint32_t ida       = g_dr_root_dir;
        uint32_t id_len    = g_dr_id_len;
        uint32_t id_mask   = (1u << (id_len - 1u)) - 1u;   /* 18 bits = 0x3FFFF */
        uint32_t frag_id   = (ida >> 1) & id_mask;
        uint32_t chain_off = ida >> id_len;

        uart_puts("[FileCore] Root directory IDA decode:\n");
        uart_puts("[FileCore]   disc_addr="); fc_hex32(ida); uart_puts("\n");
        uart_puts("[FileCore]   id_len="); fc_dec(id_len);
        uart_puts("  id_mask="); fc_hex32(id_mask); uart_puts("\n");
        uart_puts("[FileCore]   frag_id="); fc_dec(frag_id);
        uart_puts("  chain_offset="); fc_dec(chain_off); uart_puts("\n");

        /* frag_id is the bit address of the chain start in the zone map.
         * Zone containing bit address B:
         *   zone = B / (zone_spare + used_bits) = B / 4096
         *   bit_in_zone = B % 4096
         *   zone_lba = fc_zone_lba(zone, ...)
         * For frag_id=92928: zone=22, bit=2816, LBA=177856              */
        uint32_t zone_bits = g_dr_zone_spare + used_bits;   /* = 4096 */
        uint32_t chain_zone = frag_id / zone_bits;
        uint32_t chain_bit  = frag_id % zone_bits;
        uint32_t chain_lba  = (chain_zone == 0u) ? g_fc_lba_base
                            : g_fc_lba_base + (chain_zone * used_bits - dr_size) * secperlfau;

        uart_puts("[FileCore]   chain start: zone="); fc_dec(chain_zone);
        uart_puts("  bit_in_zone="); fc_dec(chain_bit);
        uart_puts("  LBA="); fc_hex32(chain_lba); uart_puts("\n");
        uart_puts("[FileCore]   (Follow "); fc_dec(chain_off);
        uart_puts(" map hops from bit "); fc_dec(frag_id);
        uart_puts(" to reach root dir data)\n");

        /* ── Step 1: Dump zone map sector at chain start ───────────────── */
        /* chain_zone / chain_bit computed just above from frag_id.        */
        {
            uint32_t cz_lba = (chain_zone == 0u) ? g_fc_lba_base
                            : g_fc_lba_base + (chain_zone * used_bits - dr_size) * secperlfau;
            uart_puts("[FileCore] Chain zone "); fc_dec(chain_zone);
            uart_puts("  map LBA="); fc_hex32(cz_lba); uart_puts("\n");

            uint8_t *zbuf = (uint8_t *)kmalloc(512);
            if (zbuf) {
                if (g_fc_bdev->ops->read(g_fc_bdev,
                                         (uint64_t)cz_lba, 1, zbuf) >= 0) {
                    /* 16 bytes centred on chain_bit/8 */
                    uint32_t bc = chain_bit / 8u;
                    uint32_t bs = (bc > 8u) ? (bc - 8u) : 0u;
                    uart_puts("[FileCore]   bytes["); fc_dec(bs);
                    uart_puts(".."); fc_dec(bs + 15u);
                    uart_puts("]:\n[FileCore]   ");
                    for (uint32_t k = bs; k < bs + 16u && k < 512u; k++) {
                        fc_hex8(zbuf[k]);
                        uart_puts(k < bs + 15u ? " " : "\n");
                    }
                    uint32_t ev = adfs_read_bits(zbuf, chain_bit,
                                                  (int)g_dr_id_len);
                    uart_puts("[FileCore]   entry @bit"); fc_dec(chain_bit);
                    uart_puts(" ("); fc_dec(g_dr_id_len);
                    uart_puts("b)="); fc_hex32(ev); uart_puts("\n");
                } else {
                    uart_puts("[FileCore]   chain zone read error\n");
                }
                kfree(zbuf);
            }
        }

        /* Estimated data LBA for the chain-start LFAU (0-hop approximation).
         * Used as the brute-scan anchor if chain traverse fails.          */
        uint32_t est_data_lba = fc_bit_addr_to_data_lba(
                                    frag_id, g_fc_lba_base,
                                    g_dr_zone_spare, used_bits, dr_size,
                                    g_dr_id_len, secperlfau);
        uart_puts("[FileCore]   est_data_lba (chain-start)=");
        fc_hex32(est_data_lba); uart_puts("\n");

        /* ── Step 2A: Direct read at root dir LBA (boot217) ──────────────── *
         *                                                                    *
         * ADFS new-map is a 1-BIT-PER-LFAU bitmap (NOT id_len bits/entry). *
         * fc_bit_addr_to_data_lba (corrected) gives the right LBA:         *
         *   frag_id=92928 → zone=22, raw_bit=2816                          *
         *   lfau_in_zone = raw_bit − zone_spare = 2816 − 32 = 2784         *
         *   zone_lfau_start(22) = 22×4064−480 = 88928                      *
         *   physical_lfau = 88928+2784 = 91712                             *
         *   data_lba = 91712×2 = 183424 = 0x02CC80                        *
         *                                                                    *
         * Root dir size = 2048 bytes = 4 sectors.  Format: "Nick" (E+/F+). *
         *   Sector 0 header:  bytes [1..4] = "Hugo" (not at [0x1FB..0x1FF])*
         *   Sector 3 footer:  bytes [2043..2047] = "Nick\r"                *
         * We read all 4 sectors and check both locations.                   */
        {
            /* Compute root dir LBA from corrected formula (no /id_len).     */
            const uint32_t ROOT_LBA = fc_bit_addr_to_data_lba(
                                          frag_id, g_fc_lba_base,
                                          g_dr_zone_spare, used_bits, dr_size,
                                          g_dr_id_len, secperlfau);

            uart_puts("[FileCore] === Step 2A: Direct read at root dir LBA ===\n");
            uart_puts("[FileCore]   frag_id="); fc_dec(frag_id);
            uart_puts("  ROOT_LBA="); fc_hex32(ROOT_LBA); uart_puts("\n");
            uart_puts("[FileCore]   (corrected formula: 1-bit-per-LFAU, no /id_len)\n");

            /* Allocate 2048 bytes (4 sectors) for the full directory.       */
            uint8_t *dir4 = (uint8_t *)kmalloc(2048);
            if (!dir4) {
                uart_puts("[FileCore]   kmalloc(2048) fail\n");
            } else {
                int ok = 1;
                for (int s = 0; s < 4 && ok; s++) {
                    if (g_fc_bdev->ops->read(g_fc_bdev,
                                             (uint64_t)(ROOT_LBA + (uint32_t)s),
                                             1, dir4 + (uint32_t)(s * 512)) < 0) {
                        uart_puts("[FileCore]   read error sector "); fc_dec((uint32_t)s);
                        uart_puts("\n");
                        ok = 0;
                    }
                }

                if (ok) {
                    /* Dump first 16 bytes of sector 0                       */
                    uart_puts("[FileCore]   sector0[0..15]: ");
                    {
                        static const char hx[] = "0123456789ABCDEF";
                        for (int b = 0; b < 16; b++) {
                            char tmp[4] = { hx[(dir4[b]>>4)&0xF],
                                            hx[dir4[b]&0xF], ' ', '\0' };
                            uart_puts(tmp);
                        }
                    }
                    uart_puts("\n");

                    /* ── Check 1: Hugo header at bytes [1..4] of sector 0  */
                    int hugo_hdr = (dir4[1]=='H' && dir4[2]=='u' &&
                                    dir4[3]=='g' && dir4[4]=='o');

                    /* ── Check 2: old single-sector "Hugo\r" at [0x1FB..0x1FF] */
                    int hugo_old = (dir4[0x1FB]=='H' && dir4[0x1FC]=='u' &&
                                    dir4[0x1FD]=='g' && dir4[0x1FE]=='o' &&
                                    dir4[0x1FF]=='\r');

                    /* ── Check 3: Nick footer "Nick\r" at [2043..2047]     */
                    int nick_ftr = (dir4[2043]=='N' && dir4[2044]=='i' &&
                                    dir4[2045]=='c' && dir4[2046]=='k' &&
                                    dir4[2047]=='\r');

                    /* ── Check 4: also "Hugo\r" at end of old-new hybrid   */
                    /* Some RISC OS versions use "Hugo\r" at the end of     */
                    /* the 2048-byte block (bytes [2043..2047] = "Hugo\r"). */
                    int hugo_ftr = (dir4[2043]=='H' && dir4[2044]=='u' &&
                                    dir4[2045]=='g' && dir4[2046]=='o' &&
                                    dir4[2047]=='\r');

                    uart_puts("[FileCore]   hugo_hdr="); fc_dec((uint32_t)hugo_hdr);
                    uart_puts("  hugo_old="); fc_dec((uint32_t)hugo_old);
                    uart_puts("  nick_ftr="); fc_dec((uint32_t)nick_ftr);
                    uart_puts("  hugo_ftr="); fc_dec((uint32_t)hugo_ftr); uart_puts("\n");

                    /* Report footer bytes for diagnosis                     */
                    uart_puts("[FileCore]   bytes[1..4]: ");
                    {
                        static const char hx2[] = "0123456789ABCDEF";
                        for (int b = 1; b <= 4; b++) {
                            char tmp[4] = { hx2[(dir4[b]>>4)&0xF],
                                            hx2[dir4[b]&0xF], ' ', '\0' };
                            uart_puts(tmp);
                        }
                    }
                    uart_puts("\n");
                    uart_puts("[FileCore]   bytes[2043..2047]: ");
                    {
                        static const char hx3[] = "0123456789ABCDEF";
                        for (int b = 2043; b <= 2047; b++) {
                            char tmp[4] = { hx3[(dir4[b]>>4)&0xF],
                                            hx3[dir4[b]&0xF], ' ', '\0' };
                            uart_puts(tmp);
                        }
                    }
                    uart_puts("\n");

                    if (hugo_hdr || hugo_old || nick_ftr || hugo_ftr) {
                        uart_puts("[FileCore] *** DIRECTORY FOUND at LBA ");
                        fc_hex32(ROOT_LBA); uart_puts(" ***\n");
                        adfs_dump_hugo_dir(dir4);
                    } else {
                        uart_puts("[FileCore]   No directory magic — wrong LBA?\n");
                    }
                }
                kfree(dir4);
            }
        }

        /* ── Step 2B: SBPr NewDir parser (boot220) ──────────────────────────
         *                                                                    *
         *   root_probe_lba = disc_map_lba + 2×nzones = 0x775AC8 (confirmed) *
         *   SBPr header 32 bytes, entries 28 bytes each:                     *
         *     +0  load_addr  +4  exec_addr  +8  length   +12 IDA             *
         *     +16 attr+ext   +20 name_len   +24 name_off (from name_table)   *
         *   name_table = dir + 32 + count*28; names 0x0D-terminated.        *
         *   IDA decode: frag_id = (IDA>>1)&0x3FFFF; chain_off = IDA>>19.    *
         *   Tail: "oven" at dir[2040], end_seq byte at dir[2044].            */
        {
            uint32_t root_probe = disc_map_lba + 2u * total_nzones;
            uart_puts("[FileCore] === Step 2B (boot234): SBPr NewDir parser ===\n");
            uart_puts("[FileCore]   root_probe_lba="); fc_hex32(root_probe); uart_puts("\n");

            /* boot239: use root_dir_size from the zone DiscRec (authoritative,
             * per DiscReader DISC_RECORD_ROOT_DIR_SIZE at +48) as primary source.
             * The two-phase peek is kept as a cross-check / fallback for cases
             * where the DiscRec value is absent or implausible (bad checksum disc).
             * Handles both 2048-byte (Lexar) and 4096-byte (NVMe root) BigDirs.  */
            uint32_t dir_alloc = 2048u;  /* safe default */

            /* Primary: root_dir_size from zone DiscRec (set above) */
            if (g_dr_root_dir_size >= 2048u && g_dr_root_dir_size <= FC_MAX_DIR_SIZE
                    && (g_dr_root_dir_size & 511u) == 0u) {
                dir_alloc = g_dr_root_dir_size;
                uart_puts("[FileCore]   dir_alloc from zone DiscRec: ");
                fc_dec(dir_alloc); uart_puts(" bytes\n");
            } else {
                /* Fallback: peek sector 0 header */
                uint8_t *peek = (uint8_t *)kmalloc(512u);
                if (peek) {
                    if (g_fc_bdev->ops->read(g_fc_bdev,
                            (uint64_t)root_probe, 1, peek) >= 0) {
                        uint32_t ds = (uint32_t)peek[12]
                                    | ((uint32_t)peek[13]<<8)
                                    | ((uint32_t)peek[14]<<16)
                                    | ((uint32_t)peek[15]<<24);
                        if (ds >= 2048u && ds <= FC_MAX_DIR_SIZE
                                && (ds & 511u) == 0u)
                            dir_alloc = ds;
                        uart_puts("[FileCore]   dir_alloc from peek: "); fc_dec(dir_alloc);
                        uart_puts("  (peek ds="); fc_hex32(ds); uart_puts(")\n");
                    }
                    kfree(peek);
                }
            }

            uint8_t *dir = (uint8_t *)kmalloc(dir_alloc);
            if (!dir) {
                uart_puts("[FileCore]   kmalloc dir fail\n");
            } else {
                /* Read dir_alloc/512 sectors */
                int rd_ok = 1;
                uint32_t dir_nsecs = dir_alloc / 512u;
                for (uint32_t s = 0; s < dir_nsecs && rd_ok; s++) {
                    if (g_fc_bdev->ops->read(g_fc_bdev,
                            (uint64_t)(root_probe + s),
                            1, dir + s * 512u) < 0) {
                        uart_puts("[FileCore]   sector read error s=");
                        fc_dec(s); uart_puts("\n");
                        rd_ok = 0;
                    }
                }

                if (rd_ok) {
                    static const char HX[] = "0123456789ABCDEF";

                    /* ── Validate SBPr magic and header ──
                     * BigDir header layout (DiscReader.h / FileCore PRM):
                     *   [0]      BIGDIR_STARTMASSEQ  start master sequence
                     *   [4..7]   BIGDIR_STARTNAME    "SBPr" magic
                     *   [8..11]  BIGDIR_NAMELEN       directory name length
                     *   [12..15] BIGDIR_SIZE          directory size
                     *   [16..19] BIGDIR_ENTRIES       entry count
                     *   [20..23] BIGDIR_NAMESSIZE     name heap size
                     *   [24..27] BIGDIR_PARENT        parent IDA
                     *   [28+]    BIGDIR_NAME          directory name string  */
                    int sbpr = (dir[4]=='S' && dir[5]=='B' && dir[6]=='P' && dir[7]=='r');
                    uint8_t  start_seq = dir[0];   /* BIGDIR_STARTMASSEQ */
                    uint32_t namelen   = (uint32_t)dir[8]  | ((uint32_t)dir[9]<<8)
                                       | ((uint32_t)dir[10]<<16) | ((uint32_t)dir[11]<<24);
                    uint32_t dir_size  = (uint32_t)dir[12] | ((uint32_t)dir[13]<<8)
                                       | ((uint32_t)dir[14]<<16) | ((uint32_t)dir[15]<<24);
                    uint32_t count     = (uint32_t)dir[16] | ((uint32_t)dir[17]<<8)
                                       | ((uint32_t)dir[18]<<16) | ((uint32_t)dir[19]<<24);
                    uint32_t par_ida   = (uint32_t)dir[24] | ((uint32_t)dir[25]<<8)
                                       | ((uint32_t)dir[26]<<16) | ((uint32_t)dir[27]<<24);

                    /* entry_start = BIGDIR_NAME + WHOLEWORDS(namelen+1)
                     * WHOLEWORDS(n) = (n+3)&~3  (round up to 4-byte boundary)
                     * e.g. namelen=1("$")→32=0x20, 4-7→36=0x24, 8-11→40=0x28  */
                    uint32_t entry_start = 28u + ((namelen + 1u + 3u) & ~3u);
                    uint32_t name_tab    = entry_start + count * 28u;

                    uart_puts("[FileCore]   magic=");
                    for (int i = 4; i < 8; i++) {
                        char s[2] = { (dir[i]>=0x20&&dir[i]<0x7F)?(char)dir[i]:'.', '\0' };
                        uart_puts(s);
                    }
                    uart_puts(sbpr ? "  OK\n" : "  BAD — not SBPr\n");
                    uart_puts("[FileCore]   start_seq="); fc_dec(start_seq);
                    uart_puts("  namelen="); fc_dec(namelen);
                    uart_puts("  entry_start="); fc_hex32(entry_start);
                    uart_puts("  entries="); fc_dec(count); uart_puts("\n");
                    uart_puts("[FileCore]   dir_size="); fc_hex32(dir_size);
                    uart_puts("  names_heap_start="); fc_hex32(name_tab);
                    uart_puts("  parent_IDA="); fc_hex32(par_ida); uart_puts("\n");

                    /* ── Validate tail: "oven" at [dir_size−8], end_seq at [dir_size−4]
                     * boot237: use computed offsets — dir_size may be 2048 OR 4096
                     * (or larger).  Hardcoded 2040 was wrong for 4096-byte NVMe dirs.  */
                    uint32_t oven_off = dir_size - 8u;
                    uint32_t seq_off  = dir_size - 4u;
                    int oven = (dir_size <= dir_alloc)
                               && (dir[oven_off]=='o' && dir[oven_off+1]=='v' &&
                                   dir[oven_off+2]=='e' && dir[oven_off+3]=='n');
                    uart_puts("[FileCore]   tail: oven="); uart_puts(oven ? "YES" : "NO");
                    uart_puts("  oven_off="); fc_hex32(oven_off);
                    uart_puts("  end_seq=");
                    fc_dec((dir_size <= dir_alloc) ? dir[seq_off] : 0u);
                    uart_puts(((dir_size <= dir_alloc) && (dir[seq_off]==start_seq))
                              ? "  seq match OK\n" : "  seq MISMATCH\n");

                    if (sbpr) {

                        /* ── Print directory listing ── */
                        uart_puts("[FileCore] --- Root directory ---\n");
                        uart_puts("[FileCore]  N  Name              IDA          frag   coff  type  len\n");

                        for (int i = 0; i < (int)count; i++) {
                            uint32_t eoff = entry_start + (uint32_t)i * 28u;

                            uint32_t load  = (uint32_t)dir[eoff+ 0] | ((uint32_t)dir[eoff+ 1]<<8)
                                           | ((uint32_t)dir[eoff+ 2]<<16) | ((uint32_t)dir[eoff+ 3]<<24);
                            uint32_t len   = (uint32_t)dir[eoff+ 8] | ((uint32_t)dir[eoff+ 9]<<8)
                                           | ((uint32_t)dir[eoff+10]<<16) | ((uint32_t)dir[eoff+11]<<24);
                            uint32_t ida   = (uint32_t)dir[eoff+12] | ((uint32_t)dir[eoff+13]<<8)
                                           | ((uint32_t)dir[eoff+14]<<16) | ((uint32_t)dir[eoff+15]<<24);
                            uint32_t nlen  = (uint32_t)dir[eoff+20] | ((uint32_t)dir[eoff+21]<<8)
                                           | ((uint32_t)dir[eoff+22]<<16) | ((uint32_t)dir[eoff+23]<<24);
                            uint32_t noff  = (uint32_t)dir[eoff+24] | ((uint32_t)dir[eoff+25]<<8)
                                           | ((uint32_t)dir[eoff+26]<<16) | ((uint32_t)dir[eoff+27]<<24);

                            /* Decode IDA: frag_id = bits[18:1], chain_off = bits[31:19] */
                            uint32_t fid  = (ida >> 1) & 0x3FFFFu;
                            uint32_t coff = ida >> 19;

                            /* File type from load_addr: bits[19:8] (typed-file format) */
                            uint32_t ftype = (load >> 8) & 0xFFFu;

                            /* Entry number */
                            uart_puts("[DIR]  ");
                            fc_dec((uint32_t)(i + 1));
                            uart_puts("  ");

                            /* Name: up to nlen chars from name_tab + noff.
                             * boot237: upper bound is oven_off, not 2040.    */
                            uint32_t nabs = name_tab + noff;
                            uint32_t printed = 0;
                            for (uint32_t k = nabs;
                                 k < oven_off && printed < nlen && printed < 32u; k++) {
                                if (dir[k] == 0x0Du) break;
                                char s[2] = { (char)dir[k], '\0' };
                                uart_puts(s);
                                printed++;
                            }
                            /* Pad name to 16 chars for alignment */
                            for (uint32_t k = printed; k < 16u; k++) uart_puts(" ");

                            uart_puts("  "); fc_hex32(ida);
                            uart_puts("  frag="); fc_dec(fid);
                            uart_puts("  coff="); fc_dec(coff);
                            uart_puts("  &");
                            {
                                char t[4] = {
                                    HX[(ftype>>8)&0xF],
                                    HX[(ftype>>4)&0xF],
                                    HX[ftype&0xF],
                                    '\0'
                                };
                                uart_puts(t);
                            }
                            uart_puts("  len="); fc_dec(len);
                            uart_puts("\n");
                        }

                        uart_puts("[FileCore] --- end of directory ---\n");

                        /* boot234: Populate VFS root cache using correct entry_start
                         * (computed from BIGDIR_NAMELEN, not hard-coded to 32). */
                        filecore_populate_root_cache(dir, count,
                                                     entry_start, name_tab);

                        /* ── Step 3 (boot231): linear SBPr scan from root+4 ────
                         *                                                        *
                         * Chain traverse is broken on this disc regardless of   *
                         * map strategy: live zone headers (fc_zone_lba) give    *
                         * e=0 for some zones; contiguous map (disc_map_lba+N)   *
                         * gives e=0 for high zones.  boot246 (sibling build)    *
                         * confirms the same failure — its chain runs 125+ hops  *
                         * without finding e=1 and falls back to direct reads.   *
                         *                                                        *
                         * Solution (confirmed by boot246): scan every secperlfau *
                         * sectors from root+secperlfau checking for SBPr magic. *
                         * boot246 finds !Boot at root+76 and a root child with  *
                         * 18 entries at root+722 via this exact method.  boot237*
                         * generalises to secperlfau step (NVMe=16, Lexar=2).    */
                        {
                            /* boot237: step = secperlfau so we land on LFAU-aligned
                             * LBAs for every device (Lexar=2, NVMe=16).             */
                            uint32_t scan_step  = (secperlfau >= 2u) ? secperlfau : 2u;
                            uint32_t scan_start = root_probe + scan_step;
                            uint32_t scan_end   = scan_start + 2048u;
                            uint32_t found      = 0u;

                            uart_puts("[FileCore] === Step 3 (boot237): SBPr linear scan ===\n");
                            uart_puts("[SCAN]   start="); fc_hex32(scan_start);
                            uart_puts("  end="); fc_hex32(scan_end);
                            uart_puts("  step="); fc_dec(scan_step); uart_puts("\n");

                            /* boot237: buffer enlarged to FC_MAX_DIR_SIZE to handle
                             * variable-size directories (4096-byte NVMe BigDirs).   */
                            uint8_t *sbuf = (uint8_t *)kmalloc(FC_MAX_DIR_SIZE);
                            if (!sbuf) {
                                uart_puts("[SCAN] kmalloc fail\n");
                            } else {
                                static const char SHX[] = "0123456789ABCDEF";
                                (void)SHX;

                                for (uint32_t lba = scan_start;
                                     lba < scan_end; lba += scan_step) {

                                    /* Quick probe: read 1 sector, check SBPr */
                                    if (g_fc_bdev->ops->read(g_fc_bdev,
                                            (uint64_t)lba, 1, sbuf) < 0) continue;

                                    if (sbuf[4] != 'S' || sbuf[5] != 'B' ||
                                        sbuf[6] != 'P' || sbuf[7] != 'r') continue;

                                    /* boot237: get dir_size from header before
                                     * reading remaining sectors so we read the
                                     * right amount for 2048 or 4096-byte dirs.  */
                                    uint32_t sdir_size = (uint32_t)sbuf[12]
                                        | ((uint32_t)sbuf[13]<<8)
                                        | ((uint32_t)sbuf[14]<<16)
                                        | ((uint32_t)sbuf[15]<<24);
                                    if (sdir_size < 2048u
                                            || sdir_size > FC_MAX_DIR_SIZE
                                            || (sdir_size & 511u) != 0u)
                                        sdir_size = 2048u;
                                    uint32_t snsecs = sdir_size / 512u;

                                    /* Candidate — read remaining sectors */
                                    int srd = 1;
                                    for (uint32_t s = 1; s < snsecs && srd; s++) {
                                        if (g_fc_bdev->ops->read(g_fc_bdev,
                                                (uint64_t)(lba + s),
                                                1, sbuf + s * 512u) < 0) {
                                            uart_puts("[SCAN] RD_FAIL LBA="); fc_hex32(lba + s);
                                            uart_puts("\n");
                                            srd = 0;
                                        }
                                    }
                                    if (!srd) continue;

                                    /* boot237: dynamic oven/seq offsets             */
                                    uint32_t s_oven_off = sdir_size - 8u;
                                    uint32_t s_seq_off  = sdir_size - 4u;

                                    /* Validate: oven tail + seq match */
                                    uint8_t sseq = sbuf[0];
                                    int oven = (sbuf[s_oven_off+0]=='o' && sbuf[s_oven_off+1]=='v' &&
                                                sbuf[s_oven_off+2]=='e' && sbuf[s_oven_off+3]=='n');
                                    if (!oven || sbuf[s_seq_off] != sseq) {
                                        /* boot235 diagnostic: report failed candidates */
                                        uart_puts("[SCAN] CAND_FAIL LBA="); fc_hex32(lba);
                                        uart_puts(" oven="); uart_puts(oven ? "1" : "0");
                                        uart_puts(" oven_off="); fc_hex32(s_oven_off);
                                        uart_puts(" seq="); fc_dec(sseq);
                                        uart_puts(" tail_seq="); fc_dec(sbuf[s_seq_off]);
                                        uart_puts(" ["); uart_puts((char[]){(char)sbuf[s_oven_off],(char)sbuf[s_oven_off+1],(char)sbuf[s_oven_off+2],(char)sbuf[s_oven_off+3],0});
                                        uart_puts("]\n");
                                        continue;
                                    }

                                    /* Valid SBPr directory — parse header fields.
                                     * boot234: bytes[8..11] = BIGDIR_NAMELEN (dir name
                                     * length), NOT flags.  entry_start is derived from
                                     * the DiscReader formula:
                                     *   entry_start = BIGDIR_NAME + WHOLEWORDS(namelen+1)
                                     *               = 28 + ((namelen+1+3) & ~3)
                                     * This handles all cases without special-casing:
                                     *   namelen 1-3 → estart=0x20, 4-7 → 0x24, 8-11 → 0x28
                                     * Verified against 11/12 scan dirs from bootlog232.  */
                                    uint32_t par_ida  = (uint32_t)sbuf[24]
                                        | ((uint32_t)sbuf[25]<<8)
                                        | ((uint32_t)sbuf[26]<<16)
                                        | ((uint32_t)sbuf[27]<<24);
                                    uint32_t snamelen = (uint32_t)sbuf[8]
                                        | ((uint32_t)sbuf[9]<<8)
                                        | ((uint32_t)sbuf[10]<<16)
                                        | ((uint32_t)sbuf[11]<<24);
                                    uint32_t entry_start = 28u + ((snamelen + 1u + 3u) & ~3u);

                                    found++;
                                    uart_puts("[SCAN] #"); fc_dec(found);
                                    uart_puts(" LBA="); fc_hex32(lba);
                                    uart_puts("  par="); fc_hex32(par_ida);
                                    uart_puts("  namelen="); fc_dec(snamelen);
                                    uart_puts("  estart="); fc_hex32(entry_start);
                                    uart_puts("  seq="); fc_dec(sseq);
                                    uart_puts("\n");

                                    /* boot234: entry count from BIGDIR_ENTRIES [16..19] */
                                    uint32_t scnt = (uint32_t)sbuf[16]
                                        | ((uint32_t)sbuf[17]<<8)
                                        | ((uint32_t)sbuf[18]<<16)
                                        | ((uint32_t)sbuf[19]<<24);
                                    if (scnt > 100u) scnt = 100u;
                                    uint32_t sntab = entry_start + scnt * 28u;

                                    uart_puts("[SCAN]   entries="); fc_dec((uint32_t)scnt);
                                    uart_puts("\n");

                                    /* List entries (up to 24) */
                                    for (int i = 0; i < scnt && i < 24; i++) {
                                        uint32_t eo   = entry_start + (uint32_t)i * 28u;
                                        uint32_t sl   = (uint32_t)sbuf[eo]
                                            | ((uint32_t)sbuf[eo+1]<<8)
                                            | ((uint32_t)sbuf[eo+2]<<16)
                                            | ((uint32_t)sbuf[eo+3]<<24);
                                        uint32_t slen = (uint32_t)sbuf[eo+8]
                                            | ((uint32_t)sbuf[eo+9]<<8)
                                            | ((uint32_t)sbuf[eo+10]<<16)
                                            | ((uint32_t)sbuf[eo+11]<<24);
                                        uint32_t si   = (uint32_t)sbuf[eo+12]
                                            | ((uint32_t)sbuf[eo+13]<<8)
                                            | ((uint32_t)sbuf[eo+14]<<16)
                                            | ((uint32_t)sbuf[eo+15]<<24);
                                        uint32_t snl  = (uint32_t)sbuf[eo+20]
                                            | ((uint32_t)sbuf[eo+21]<<8)
                                            | ((uint32_t)sbuf[eo+22]<<16)
                                            | ((uint32_t)sbuf[eo+23]<<24);
                                        uint32_t sno  = (uint32_t)sbuf[eo+24]
                                            | ((uint32_t)sbuf[eo+25]<<8)
                                            | ((uint32_t)sbuf[eo+26]<<16)
                                            | ((uint32_t)sbuf[eo+27]<<24);
                                        uint32_t sftp       = (sl >> 8) & 0xFFFu;
                                        int      sis_dir    = (sftp==0xFFFu||sftp==0xDDCu);
                                        int      sis_fat32  = (si==0x00000300u);

                                        uart_puts("[SCAN]     ");
                                        if (sis_fat32)   uart_puts("FAT32");
                                        else if (sis_dir) uart_puts("DIR  ");
                                        else              uart_puts("FILE ");

                                        uint32_t sna = sntab + sno;
                                        uint32_t spr = 0;
                                        /* boot237: bound to s_oven_off, not 2040 */
                                        for (uint32_t k = sna;
                                             k < s_oven_off && spr < snl && spr < 28u; k++) {
                                            if (sbuf[k] == 0x0Du) break;
                                            char sc[2] = {(char)sbuf[k], '\0'};
                                            uart_puts(sc);
                                            spr++;
                                        }
                                        uart_puts("  ida="); fc_hex32(si);
                                        uart_puts("  len="); fc_dec(slen);
                                        uart_puts("\n");
                                    }
                                    if (scnt > 24) {
                                        uart_puts("[SCAN]     ... ("); fc_dec((uint32_t)(scnt-24));
                                        uart_puts(" more)\n");
                                    }

                                    /* boot237: skip past this directory.
                                     * Loop adds scan_step; add (snsecs - scan_step)
                                     * more so total advance = snsecs sectors.      */
                                    if (snsecs > scan_step)
                                        lba += snsecs - scan_step;
                                }

                                uart_puts("[SCAN] done  found="); fc_dec(found);
                                uart_puts("\n");
                                kfree(sbuf);
                            }
                        }
                    } /* sbpr */
                } /* rd_ok */
                kfree(dir);
            }
        }

        /* ── Step 4 (boot268): IDA-based child directory navigation ─────────
         *
         * Each entry in the root SBPr dir has a .sin field = raw IDA.
         * fc_ida_to_data_lba() converts IDA→data_lba using the correct
         * DiscReader algorithm:
         *   id        = IDA >> 8
         *   ids_pz    = (zone_bits - zone_spare) / (id_len + 1) = 203
         *   home_zone = id / ids_pz
         *   scan home_zone's map sector for entry whose id_len bits == id
         *   data_lba  = lba_base + (home_zone*used_bits + start - dr_size)
         *               * secperlfau
         *
         * Replaces brute-force linear scan.  Reads each child dir and logs
         * its SBPr header so we can verify chain resolution is correct.    */
        if (g_root_cache_valid && g_root_cache_count > 0u) {
            uart_puts("\n[FileCore] === Step 4: IDA-based child navigation ===\n");
            uart_puts("[FileCore]   disc_map_lba="); fc_hex32(disc_map_lba);
            uart_puts("  nzones="); fc_dec(total_nzones);
            uart_puts("  ids_pz=");
            fc_dec(((g_dr_zone_spare + used_bits) - g_dr_zone_spare)
                   / (g_dr_id_len + 1u));
            uart_puts("\n");

            uint8_t *cbuf = (uint8_t *)kmalloc(FC_MAX_DIR_SIZE);
            if (!cbuf) {
                uart_puts("[Step4] kmalloc fail\n");
            } else {
                for (uint32_t ei = 0u; ei < g_root_cache_count; ei++) {
                    vfs_dirent_t *e = &g_root_cache[ei];
                    uint32_t child_ida = e->sin;

                    uart_puts("[Step4] #"); fc_dec(ei + 1u);
                    uart_puts("  '"); uart_puts(e->name);
                    uart_puts("'  ida="); fc_hex32(child_ida); uart_puts("\n");

                    /* Skip zero/invalid IDAs (empty files have sin=1) */
                    if (child_ida == 0u || child_ida == 1u) {
                        uart_puts("[Step4]   skip (zero-len)\n");
                        continue;
                    }

                    uint32_t child_lba = 0u;
                    int rc = fc_ida_to_data_lba(
                                child_ida,
                                g_fc_lba_base, disc_map_lba,
                                g_dr_zone_spare, used_bits,
                                dr_size, g_dr_id_len,
                                secperlfau, total_nzones,
                                &child_lba);

                    if (rc != 0) {
                        uart_puts("[Step4]   IDA resolve FAILED\n");
                        continue;
                    }

                    uart_puts("[Step4]   child_lba="); fc_hex32(child_lba);
                    uart_puts("\n");

                    /* Read first sector of child object */
                    if (g_fc_bdev->ops->read(g_fc_bdev,
                            (uint64_t)child_lba, 1, cbuf) < 0) {
                        uart_puts("[Step4]   read error\n");
                        continue;
                    }

                    /* Check directory magic */
                    int is_sbpr = (cbuf[4]=='S' && cbuf[5]=='B' &&
                                   cbuf[6]=='P' && cbuf[7]=='r');
                    int is_hugo = (cbuf[1]=='H' && cbuf[2]=='u' &&
                                   cbuf[3]=='g' && cbuf[4]=='o');

                    if (is_sbpr) {
                        uint32_t cdir_size = (uint32_t)cbuf[12]
                            |((uint32_t)cbuf[13]<<8)
                            |((uint32_t)cbuf[14]<<16)
                            |((uint32_t)cbuf[15]<<24);
                        uint32_t ccount    = (uint32_t)cbuf[20]
                            |((uint32_t)cbuf[21]<<8)
                            |((uint32_t)cbuf[22]<<16)
                            |((uint32_t)cbuf[23]<<24);
                        uart_puts("[Step4]   *** SBPr dir  size=");
                        fc_dec(cdir_size);
                        uart_puts("  entries="); fc_dec(ccount);
                        uart_puts(" ***\n");
                    } else if (is_hugo) {
                        uart_puts("[Step4]   *** Hugo dir ***\n");
                    } else {
                        /* Not a directory – might be a file; dump first 16 bytes */
                        uart_puts("[Step4]   (not dir) bytes[0..15]: ");
                        for (int b = 0; b < 16; b++) {
                            fc_hex8(cbuf[b]);
                            if (b < 15) uart_puts(" ");
                        }
                        uart_puts("\n");
                    }
                }
                kfree(cbuf);
            }
            uart_puts("[FileCore] === Step 4 done ===\n\n");
        }

        /* ── Step 5 (boot268): Read a real file via filecore_get_child_entry ──
         *
         * Locate '!Boot' in the root cache, then use filecore_get_child_entry()
         * to walk its entries and find the '!Boot' obey script (len=561).
         * Read the file data and dump the first 128 bytes to UART to prove
         * the full chain works: root → child dir → file data.               */
        if (g_root_cache_valid && g_root_cache_count > 0u) {
            uart_puts("[FileCore] === Step 5: Read !Boot/!Boot file ===\n");

            /* Find '!Boot' directory in root cache (search by name, not type,
             * because type classification depended on attr byte fix above).  */
            uint32_t boot_sin = 0u;
            for (uint32_t ri = 0u; ri < g_root_cache_count; ri++) {
                const char *n = g_root_cache[ri].name;
                if (n[0]=='!' && n[1]=='B' && n[2]=='o' && n[3]=='o' &&
                    n[4]=='t' && n[5]=='\0') {
                    boot_sin = g_root_cache[ri].sin;
                    uart_puts("[Step5] Found !Boot  sin=");
                    fc_hex32(boot_sin);
                    uart_puts("  type=");
                    fc_dec((uint32_t)g_root_cache[ri].type);
                    uart_puts("\n");
                    break;
                }
            }

            if (boot_sin == 0u) {
                uart_puts("[Step5] !Boot not in root cache\n");
            } else {
                /* Scan entries of !Boot dir to find '!Boot' file */
                vfs_dirent_t fent;
                uint32_t fidx = 0u;
                int found_boot_file = 0;

                /* We know from SCAN log !Boot dir has 204 entries; search first 256 */
                uart_puts("[Step5] Scanning !Boot dir entries...\n");
                for (fidx = 0u; fidx < 256u; fidx++) {
                    if (filecore_get_child_entry(boot_sin, fidx, &fent) != 0) {
                        uart_puts("[Step5] get_child_entry end at idx=");
                        fc_dec(fidx); uart_puts("\n");
                        break;
                    }
                    /* Check for the '!Boot' file by name */
                    const char *fn = fent.name;
                    if (fn[0]=='!' && fn[1]=='B' && fn[2]=='o' && fn[3]=='o' &&
                        fn[4]=='t' && fn[5]=='\0') {
                        uart_puts("[Step5] Found !Boot file  idx="); fc_dec(fidx);
                        uart_puts("  sin="); fc_hex32(fent.sin);
                        uart_puts("  len="); fc_dec((uint32_t)fent.size);
                        uart_puts("\n");
                        found_boot_file = 1;
                        break;
                    }
                }

                if (!found_boot_file) {
                    uart_puts("[Step5] !Boot file not found in !Boot dir\n");
                } else {
                    /* Resolve file IDA → data LBA */
                    uint32_t sector_size2 = 1u << g_dr_log2ss;
                    uint32_t bpmb2        = 1u << g_dr_log2bpmb;
                    uint32_t secperlfau2  = bpmb2 / sector_size2;
                    uint32_t used_bits2   = (sector_size2 * 8u) - g_dr_zone_spare;
                    uint32_t dr_size2     = 60u * 8u;
                    uint32_t nzones2      = g_dr_nzones;
                    if (g_dr_big_flag) nzones2 += (uint32_t)g_dr_nzones_hi << 8u;
                    uint32_t mid2         = nzones2 / 2u;
                    uint32_t dml2         = g_fc_lba_base
                                          + (mid2 * used_bits2 - dr_size2) * secperlfau2;

                    uint32_t file_lba = 0u;
                    if (fc_ida_to_data_lba(fent.sin,
                                            g_fc_lba_base, dml2,
                                            g_dr_zone_spare, used_bits2,
                                            dr_size2, g_dr_id_len,
                                            secperlfau2, nzones2,
                                            &file_lba) != 0) {
                        uart_puts("[Step5] file IDA resolve FAILED\n");
                    } else {
                        uart_puts("[Step5] file_lba="); fc_hex32(file_lba);
                        uart_puts("  size="); fc_dec((uint32_t)fent.size);
                        uart_puts(" bytes\n");

                        /* Read up to 512 bytes (one sector) */
                        uint8_t *fbuf = (uint8_t *)kmalloc(512u);
                        if (!fbuf) {
                            uart_puts("[Step5] kmalloc fail\n");
                        } else {
                            if (g_fc_bdev->ops->read(g_fc_bdev,
                                    (uint64_t)file_lba, 1, fbuf) < 0) {
                                uart_puts("[Step5] read error\n");
                            } else {
                                uint32_t dump_len = (uint32_t)fent.size;
                                if (dump_len > 128u) dump_len = 128u;

                                uart_puts("[Step5] File content (first ");
                                fc_dec(dump_len); uart_puts(" bytes):\n[Step5] ");

                                for (uint32_t b = 0u; b < dump_len; b++) {
                                    if (fbuf[b] >= 0x20u && fbuf[b] < 0x7Fu) {
                                        char cs[2] = {(char)fbuf[b], '\0'};
                                        uart_puts(cs);
                                    } else if (fbuf[b] == 0x0Au || fbuf[b] == 0x0Du) {
                                        /* newline → print as \n in log */
                                        uart_puts("\n[Step5] ");
                                    } else {
                                        uart_puts("["); fc_hex8(fbuf[b]); uart_puts("]");
                                    }
                                }
                                uart_puts("\n[Step5] === file read OK ===\n");
                            }
                            kfree(fbuf);
                        }
                    }
                }
            }
            uart_puts("[FileCore] === Step 5 done ===\n\n");
        }

        /* ── Step 6 (boot269): Navigate $.!Boot → Modules → load module ────
         *
         * Using filecore_get_child_entry() + filecore_read_file_buf() to
         * navigate two directory levels and load the first plausible RISC OS
         * module binary into the module manager.
         *
         * Path tried in order:  $.!Boot.Modules  then  $.!Boot.Loader
         * Any file with 0x34 ≤ size ≤ 4 MB is read and passed to
         * module_load_from_memory().  First successful load breaks the loop.  */
        if (g_root_cache_valid && g_root_cache_count > 0u) {
            uart_puts("[FileCore] === Step 6: Load module from disc ===\n");
            filecore_step6_load_module(disc_map_lba, used_bits, dr_size,
                                        secperlfau, total_nzones);
            uart_puts("[FileCore] === Step 6 done ===\n\n");
        }

        /* Suppress unused-variable warnings for old chain traverse vars */
        (void)frag_id; (void)chain_off; (void)est_data_lba;
    }

    kfree(buf);
    uart_puts("[FileCore] Zone scan complete\n");
}

/* ── filecore_read_file_buf ──────────────────────────────────────────────────
 * Read a complete file from disc into a newly allocated buffer.
 * Resolves IDA → LBA, reads ceil(size/sector_size) sectors.
 * Returns kmalloc'd buffer on success (caller must kfree on failure).
 * Module manager retains the buffer on successful load — do NOT free after
 * a successful module_load_from_memory() call.                              */
static uint8_t *filecore_read_file_buf(uint32_t sin, uint32_t size,
                                        uint32_t disc_map_lba,
                                        uint32_t used_bits,  uint32_t dr_size,
                                        uint32_t secperlfau, uint32_t nzones)
{
    if (!g_fc_bdev || size == 0u || size > 4u * 1024u * 1024u) return NULL;

    uint32_t sector_size = 1u << g_dr_log2ss;
    uint32_t file_lba    = 0u;

    if (fc_ida_to_data_lba(sin,
                            g_fc_lba_base, disc_map_lba,
                            g_dr_zone_spare, used_bits,
                            dr_size, g_dr_id_len,
                            secperlfau, nzones,
                            &file_lba) != 0) {
        uart_puts("[FileRead] IDA resolve failed sin="); fc_hex32(sin); uart_puts("\n");
        return NULL;
    }

    uint32_t nsecs = (size + sector_size - 1u) / sector_size;
    uint8_t *buf   = (uint8_t *)kmalloc(nsecs * sector_size);
    if (!buf) { uart_puts("[FileRead] kmalloc fail\n"); return NULL; }

    uart_puts("[FileRead] lba="); fc_hex32(file_lba);
    uart_puts("  nsecs=");        fc_dec(nsecs);
    uart_puts("  size=");         fc_dec(size); uart_puts("\n");

    for (uint32_t s = 0u; s < nsecs; s++) {
        if (g_fc_bdev->ops->read(g_fc_bdev,
                (uint64_t)(file_lba + s), 1, buf + s * sector_size) < 0) {
            uart_puts("[FileRead] read error sector="); fc_dec(s); uart_puts("\n");
            kfree(buf);
            return NULL;
        }
    }
    return buf;
}

/* ── fc_name_eq ──────────────────────────────────────────────────────────────
 * Simple strcmp without libc.                                               */
static int fc_name_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ── fc_find_in_dir ──────────────────────────────────────────────────────────
 * Scan a directory for an entry whose name matches 'name'.
 * Returns 0 and fills *out_ent on success, -1 if not found.                */
static int fc_find_in_dir(uint32_t dir_sin, const char *name, vfs_dirent_t *out_ent)
{
    for (uint32_t i = 0u; i < 256u; i++) {
        if (filecore_get_child_entry(dir_sin, i, out_ent) != 0) return -1;
        if (fc_name_eq(out_ent->name, name)) return 0;
    }
    return -1;
}

/* ── fc_try_load_modules_from_dir ────────────────────────────────────────────
 * List every entry in dir_sin.  For each file in the module size range,
 * read it and attempt module_load_from_memory().
 * Returns 1 if a module was successfully loaded, 0 otherwise.              */
static int fc_try_load_modules_from_dir(uint32_t dir_sin, const char *label,
                                         uint32_t disc_map_lba,
                                         uint32_t used_bits,  uint32_t dr_size,
                                         uint32_t secperlfau, uint32_t nzones)
{
    uart_puts("[Step6] Listing '"); uart_puts(label); uart_puts("'...\n");
    vfs_dirent_t ent;

    for (uint32_t i = 0u; i < 128u; i++) {
        if (filecore_get_child_entry(dir_sin, i, &ent) != 0) {
            uart_puts("[Step6]   ("); fc_dec(i); uart_puts(" entries)\n");
            break;
        }
        uart_puts("[Step6]   ["); fc_dec(i); uart_puts("] '");
        uart_puts(ent.name);
        uart_puts(ent.type == VFS_DIRENT_DIR ? "'  DIR" : "'  FILE");
        uart_puts("  sz="); fc_dec((uint32_t)ent.size); uart_puts("\n");

        if (ent.type != VFS_DIRENT_FILE) continue;
        if (ent.size < 0x34u || ent.size > 4u * 1024u * 1024u) continue;

        uart_puts("[Step6] Trying '"); uart_puts(ent.name);
        uart_puts("' size="); fc_dec((uint32_t)ent.size); uart_puts("\n");

        uint8_t *fbuf = filecore_read_file_buf(ent.sin, (uint32_t)ent.size,
                                                disc_map_lba, used_bits, dr_size,
                                                secperlfau, nzones);
        if (!fbuf) continue;

        int mrc = module_load_from_memory(fbuf, (uint32_t)ent.size, ent.name);
        if (mrc == 0) {
            uart_puts("[Step6] *** '"); uart_puts(ent.name);
            uart_puts("' loaded OK ***\n");
            return 1;   /* buffer owned by module manager — don't free */
        }
        uart_puts("[Step6] rc="); fc_dec((uint32_t)(-mrc)); uart_puts(" skip\n");
        kfree(fbuf);
    }
    return 0;
}

/* ── filecore_step6_load_module ──────────────────────────────────────────────
 * Two search paths (confirmed from bootlog244):
 *
 *   Path A: $.!Boot.Resources.!System[.Modules]
 *     !Boot (sin=0x0a8ba101) → Resources (entry[9], ida=0x0a85ea01)
 *     → !System → try files directly, then Modules subdir
 *
 *   Path B: $.!LanMan98
 *     Root cache entry '!LanMan98' (ida=0x0a70ab01)
 *     → scan directly for LanManFS module file
 *
 * Path A is tried first (standard RISC OS system module location).          */
static void filecore_step6_load_module(uint32_t disc_map_lba,
                                        uint32_t used_bits,
                                        uint32_t dr_size,
                                        uint32_t secperlfau,
                                        uint32_t total_nzones)
{
    vfs_dirent_t ent;

    /* ── Path A: $.!Boot.Resources.!System ── */
    uart_puts("[Step6] Path A: $.!Boot.Resources.!System\n");

    /* Step A1: !Boot from root cache */
    uint32_t boot_sin = 0u;
    for (uint32_t ri = 0u; ri < g_root_cache_count; ri++) {
        if (fc_name_eq(g_root_cache[ri].name, "!Boot")) {
            boot_sin = g_root_cache[ri].sin; break;
        }
    }
    if (!boot_sin) { uart_puts("[Step6] A: !Boot not found\n"); goto path_b; }

    /* Step A2: Resources inside !Boot */
    if (fc_find_in_dir(boot_sin, "Resources", &ent) != 0 ||
            ent.type != VFS_DIRENT_DIR) {
        uart_puts("[Step6] A: Resources not found in !Boot\n"); goto path_b;
    }
    uart_puts("[Step6] A: Resources sin="); fc_hex32(ent.sin); uart_puts("\n");
    {
        uint32_t res_sin = ent.sin;

        /* Step A3: !System inside Resources */
        if (fc_find_in_dir(res_sin, "!System", &ent) != 0 ||
                ent.type != VFS_DIRENT_DIR) {
            uart_puts("[Step6] A: !System not found in Resources\n"); goto path_b;
        }
        uart_puts("[Step6] A: !System sin="); fc_hex32(ent.sin); uart_puts("\n");
        {
            uint32_t sys_sin = ent.sin;

            /* Step A4a: try files directly inside !System */
            if (fc_try_load_modules_from_dir(sys_sin, "!System",
                                              disc_map_lba, used_bits, dr_size,
                                              secperlfau, total_nzones))
                return;

            /* Step A4b: try !System.Modules subdir */
            if (fc_find_in_dir(sys_sin, "Modules", &ent) == 0 &&
                    ent.type == VFS_DIRENT_DIR) {
                uart_puts("[Step6] A: found !System/Modules sin=");
                fc_hex32(ent.sin); uart_puts("\n");
                if (fc_try_load_modules_from_dir(ent.sin, "!System/Modules",
                                                  disc_map_lba, used_bits, dr_size,
                                                  secperlfau, total_nzones))
                    return;
            }
        }
    }

path_b:
    /* ── Path B: $.!LanMan98 ── */
    uart_puts("[Step6] Path B: $.!LanMan98\n");

    uint32_t lanman_sin = 0u;
    for (uint32_t ri = 0u; ri < g_root_cache_count; ri++) {
        if (fc_name_eq(g_root_cache[ri].name, "!LanMan98")) {
            lanman_sin = g_root_cache[ri].sin; break;
        }
    }
    if (!lanman_sin) { uart_puts("[Step6] B: !LanMan98 not in root cache\n"); return; }
    uart_puts("[Step6] B: !LanMan98 sin="); fc_hex32(lanman_sin); uart_puts("\n");

    if (fc_try_load_modules_from_dir(lanman_sin, "!LanMan98",
                                      disc_map_lba, used_bits, dr_size,
                                      secperlfau, total_nzones))
        return;

    uart_puts("[Step6] No module loaded from either path\n");
}

/* ── Stubs (safe, preserve existing link interfaces) ─────────────────────── */
int filecore_try_wholedisk(blockdev_t *bd, uint8_t *buf)
    { (void)bd; (void)buf; return -1; }

int filecore_wholedisk_fat32(uint8_t *buf)
    { (void)buf; return -1; }

void filecore_read_disc_record(void) {}

/* adfs_read_bits — extract nbits bits (LSB-first) from buf at bit position bitpos */
uint32_t adfs_read_bits(const uint8_t *buf, uint32_t bitpos, int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        uint32_t bp = bitpos + (uint32_t)i;
        if ((buf[bp >> 3] >> (bp & 7u)) & 1u)
            val |= (1u << i);
    }
    return val;
}

/* ── fc_bit_addr_to_data_lba ─────────────────────────────────────────────────
 * Convert a zone-map BIT ADDRESS (frag_id) to the physical disc LBA.
 *
 * ADFS new-map (E+/F+) is a 1-BIT-PER-LFAU bitmap — each bit in the zone's
 * used_bits section corresponds to exactly ONE LFAU of bpmb bytes.  The old
 * assumption that entries are id_len bits wide was WRONG (boot217 fix).
 *
 * Verified against DiskKnight SPARCE output (Lexar 7.45 GB):
 *   Zone 0  bit &01E0=480 → disc_byte=0          (lfau_in_zone = 480-480 = 0)
 *   Zone 1  bit &0000=0   → disc_byte=&380000    (zone_lfau_start=3584)
 *   Zone 962 bit &0784=1924 → disc_byte=&EEB59000 (zone_lfau_start=3909088)
 *
 * Bit address B = zone × zone_bits + raw_bit_in_zone
 * lfau_in_zone  = raw_bit_in_zone − zone_spare  [− dr_size for zone 0 only]
 * zone_lfau_start(0)  = 0
 * zone_lfau_start(N)  = N × used_bits − dr_size  (N > 0)
 * physical_lfau       = zone_lfau_start + lfau_in_zone
 * data_lba            = lba_base + physical_lfau × secperlfau               */
static uint32_t fc_bit_addr_to_data_lba(uint32_t B,
                                         uint32_t lba_base,
                                         uint32_t zone_spare,
                                         uint32_t used_bits,
                                         uint32_t dr_size,
                                         uint32_t id_len,      /* unused — kept for ABI */
                                         uint32_t secperlfau)
{
    (void)id_len;   /* map is 1-bit-per-LFAU; id_len not used here */

    uint32_t zone_bits   = zone_spare + used_bits;   /* = 4096 */
    uint32_t zone        = B / zone_bits;
    uint32_t bit_in_zone = B % zone_bits;

    /* Subtract zone_spare (check word).  For zone 0, also subtract dr_size
     * because the DiscRecord occupies the first dr_size bits of used_bits.  */
    uint32_t header_bits = zone_spare + (zone == 0u ? dr_size : 0u);
    uint32_t lfau_in_zone = (bit_in_zone >= header_bits)
                                ? (bit_in_zone - header_bits) : 0u;
    /* NOTE: no /id_len here — each bit = one LFAU (1-bit-per-LFAU bitmap) */

    uint32_t zone_lfau_start = (zone == 0u)
                               ? 0u
                               : (zone * used_bits - dr_size);

    uint32_t physical_lfau = zone_lfau_start + lfau_in_zone;
    return lba_base + physical_lfau * secperlfau;
}

/* ── fc_ida_to_data_lba ──────────────────────────────────────────────────────
 * Convert a new-map IDA (Indirect Disc Address) to the physical data LBA
 * of the FIRST LFAU of the named object.
 *
 * Algorithm verified against DiscReader Map.c (AddID / MapAddr):
 *
 *   id        = IDA >> 8
 *               The id_len-bit value stored in the map entry.  DiscReader
 *               indexes objects[sin>>8] where sin=IDA — confirmed in
 *               Directories.c ReadEntry().
 *
 *   ids_pz    = (zone_bits - zone_spare) / (id_len + 1)
 *               Maximum IDs per zone (from Map.c line 74):
 *               ((8<<log2secsize) - zone_spare) / (idlen+1)
 *               For Lexar: (4096-32)/(19+1) = 4064/20 = 203
 *
 *   home_zone = id / ids_pz
 *               The zone whose allocation area was formatted to hold this
 *               object's map entry.  For id=195286 (root dir), zone=962 ✓.
 *
 *   Scan home_zone's map sector (disc_map_lba + home_zone) for the entry
 *   whose leading id_len bits == id.  Zone 0 starts scan at dr_size bits
 *   (after DiscRecord); other zones start at bit 0 of allocation area.
 *   Allocation area is accessed via adfs_read_bits(zbuf, zone_spare + allocbit, …)
 *   because the 4-byte zone check / spare header precedes the allocation bits
 *   in the sector (MAP_BITS_OFFSET = zone_spare bits = 4 bytes = 32 bits).
 *
 *   data_lba  = lba_base + (home_zone * used_bits + found_start - dr_size)
 *                          * secperlfau
 *               = MapAddr(home_zone, found_start) * secperlfau + lba_base
 *               For root dir: zone=962, start=1924 → lfauno=3911012
 *               → data_lba = 0x775AC8  ✓
 *
 * chain_lfau (IDA>>id_len) = fragment size minus one (not used for first-LFAU
 * access).  Callers reading from the start of an object pass chain_lfau=0.
 *
 * Returns 0 on success (sets *out_lba), -1 on error.                        */
static int fc_ida_to_data_lba(uint32_t ida,
                               uint32_t lba_base,  uint32_t disc_map_lba,
                               uint32_t zone_spare, uint32_t used_bits,
                               uint32_t dr_size,    uint32_t id_len,
                               uint32_t secperlfau, uint32_t nzones,
                               uint32_t *out_lba)
{
    if (!out_lba) return -1;

    /* IDA structure (new-map, verified against bootlog248 !Boot sub-entries):
     *   bits[31:8] = object id (stored in the map entry's id_len-bit field)
     *   bits[7:1]  = chain_lfau (LFAU offset within the object's allocation)
     *   bit 0      = new-map flag (usually 1, but CAN be 0 for interior LFAUs
     *                such as !Run IDA=0x02FAD708; do NOT reject on bit0=0)
     *
     * Examples confirmed from bootlog248:
     *   !Boot dir  IDA=0x02FAD701 → chain_lfau=0 (start of !Boot block)
     *   !Boot file IDA=0x02FAD705 → chain_lfau=2 (2 LFAUs into !Boot block)
     *   !Help file IDA=0x02FAD707 → chain_lfau=3
     *   !Run  file IDA=0x02FAD708 → chain_lfau=4 (bit0=0 but perfectly valid)
     */
    uint32_t id         = ida >> 8;                        /* object id */
    uint32_t chain_lfau = (ida & 0xFFu) >> 1u;            /* bits[7:1]  */
    uint32_t zone_bits  = zone_spare + used_bits;          /* = 4096    */
    /* ids_pz = max IDs per zone = (zone_bits - zone_spare) / (id_len + 1) */
    uint32_t ids_pz     = (zone_bits - zone_spare) / (id_len + 1u);
    uint32_t home_zone  = (ids_pz > 0u) ? (id / ids_pz) : 0u;

    if (home_zone >= nzones) {
        uart_puts("[IDA] home_zone="); fc_dec(home_zone);
        uart_puts(" OOB (nzones="); fc_dec(nzones);
        uart_puts(") ida="); fc_hex32(ida); uart_puts("\n");
        return -1;
    }

    uart_puts("[IDA] ida="); fc_hex32(ida);
    uart_puts("  id="); fc_dec(id);
    uart_puts("  ids_pz="); fc_dec(ids_pz);
    uart_puts("  home_zone="); fc_dec(home_zone); uart_puts("\n");

    /* Read the home zone's map sector from the contiguous map copy */
    uint8_t *zbuf = (uint8_t *)kmalloc(512);
    if (!zbuf) { uart_puts("[IDA] kmalloc fail\n"); return -1; }

    uint32_t map_sec = disc_map_lba + home_zone;
    if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)map_sec, 1, zbuf) < 0) {
        uart_puts("[IDA] map read fail zone="); fc_dec(home_zone); uart_puts("\n");
        kfree(zbuf);
        return -1;
    }

    /* Scan allocation area for the entry whose id_len bits == id.
     * allocbit is 0-indexed from the start of the allocation area
     * (i.e. AFTER zone_spare=32 bits).  adfs_read_bits(zbuf, …) takes
     * bit positions from zbuf[0], so we add zone_spare to allocbit.        */
    uint32_t allocbit = (home_zone == 0u) ? dr_size : 0u;
    uint32_t allocend = used_bits;                         /* = 4064 */
    uint32_t found_start = 0xFFFFFFFFu;

    while (allocbit + id_len < allocend) {
        uint32_t start    = allocbit;
        /* read id_len bits from allocation area position allocbit */
        uint32_t entry_id = adfs_read_bits(zbuf, zone_spare + allocbit,
                                            (int)id_len);
        allocbit += id_len;

        if (entry_id == id) {
            found_start = start;
            uart_puts("[IDA] found id="); fc_dec(id);
            uart_puts(" at alloc_bit="); fc_dec(found_start); uart_puts("\n");
            break;
        }

        /* Skip 0-bits (body LFAUs) until 1-bit (terminator) */
        while (allocbit < allocend) {
            uint32_t raw_bit = zone_spare + allocbit;
            uint8_t  bit     = (zbuf[raw_bit >> 3] >> (raw_bit & 7u)) & 1u;
            allocbit++;
            if (bit == 1u) break;
        }

        /* Stop if too little space for another full entry */
        if (allocend - allocbit < id_len + 1u) break;
    }

    kfree(zbuf);

    if (found_start == 0xFFFFFFFFu) {
        uart_puts("[IDA] id="); fc_dec(id);
        uart_puts(" NOT found in zone="); fc_dec(home_zone);
        uart_puts(" (ida="); fc_hex32(ida); uart_puts(")\n");
        return -1;
    }

    /* MapAddr: lfauno = home_zone * used_bits + found_start - dr_size
     * Add chain_lfau (= bits[7:1] of IDA low byte) so we land on the
     * correct LFAU within the contiguous block.
     * Verified from bootlog248:
     *   !Boot dir  chain_lfau=0 → data_lba=0x775AF0
     *   !Run file  chain_lfau=4 → data_lba=0x775AF0+8=0x775AF8            */
    uint32_t lfauno  = home_zone * used_bits + found_start - dr_size
                       + chain_lfau;
    *out_lba         = lba_base + lfauno * secperlfau;

    uart_puts("[IDA] zone="); fc_dec(home_zone);
    uart_puts("  start="); fc_dec(found_start);
    uart_puts("  chain_lfau="); fc_dec(chain_lfau);
    uart_puts("  lfauno="); fc_dec(lfauno);
    uart_puts("  data_lba="); fc_hex32(*out_lba); uart_puts("\n");

    return 0;
}

/* ── adfs_dump_hugo_dir ──────────────────────────────────────────────────────
 * Parse and print entries from a 2048-byte Hugo-format ADFS directory.
 * Hugo directory layout (2048 bytes, 4 × 512-byte sectors):
 *   [0]      master sequence number
 *   [1..4]   "Hugo" magic
 *   [5..]    entries, 26 bytes each (up to 77)
 *              +0  name[10]   (null/space terminated; bit7 of [0] = deleted)
 *              +10 load[4]
 *              +14 exec[4]
 *              +18 length[3]  (24-bit)
 *              +21 attrib[1]
 *              +22 IDA[4]     (disc address of this object)
 *   [2043]   master sequence (repeated)
 *   [2044..2047] "Nick" magic                                               */
static void adfs_dump_hugo_dir(const uint8_t *dir)
{
    uart_puts("[ADFS] Hugo dir seq="); fc_hex8(dir[0]); uart_puts("\n");

    int found = 0;
    for (int i = 0; i < 77; i++) {
        const uint8_t *e = dir + 5 + i * 26;
        if (e[0] == 0x00) break;                      /* end of entries    */
        if (e[0] & 0x80u) continue;                   /* deleted entry     */

        /* Extract IDA (little-endian 4 bytes at offset +22) */
        uint32_t ida = (uint32_t)e[22] | ((uint32_t)e[23] << 8)
                     | ((uint32_t)e[24] << 16) | ((uint32_t)e[25] << 24);
        int is_dir = (ida & 1u) ? 1 : 0;   /* IDA bit 0 = indirect = dir  */

        uart_puts("[ADFS]   ");
        uart_puts(is_dir ? "DIR  " : "FILE ");
        /* Print name (up to 10 chars) */
        for (int k = 0; k < 10; k++) {
            char c = (char)(e[k] & 0x7Fu);
            if (c == '\0' || c == ' ') break;
            char s[2] = {c, '\0'}; uart_puts(s);
        }
        uart_puts("  ida="); fc_hex32(ida);

        if (!is_dir) {
            uint32_t len = (uint32_t)e[18] | ((uint32_t)e[19] << 8)
                         | ((uint32_t)e[20] << 16);
            uart_puts("  len="); fc_dec(len);
        }
        uart_puts("\n");
        found++;
    }

    if (found == 0)
        uart_puts("[ADFS]   (no entries — empty directory)\n");
    else {
        uart_puts("[ADFS] "); fc_dec((uint32_t)found);
        uart_puts(" object(s) in root directory\n");
    }
}

/* ── VFS root entry cache ─────────────────────────────────────────────────
 * Populated during Step 2B root dir parse.  Queried by vfs.c readdir.     */

static void filecore_populate_root_cache(const uint8_t *dir,
                                          uint32_t count,
                                          uint32_t entry_start,
                                          uint32_t name_tab)
{
    g_root_cache_count = 0u;
    g_root_cache_valid = 0;

    for (uint32_t i = 0u; i < count && g_root_cache_count < FC_ROOT_CACHE_MAX; i++) {
        uint32_t eoff = entry_start + i * 28u;
        uint32_t load = (uint32_t)dir[eoff+ 0]|((uint32_t)dir[eoff+ 1]<<8)|
                        ((uint32_t)dir[eoff+ 2]<<16)|((uint32_t)dir[eoff+ 3]<<24);
        uint32_t len  = (uint32_t)dir[eoff+ 8]|((uint32_t)dir[eoff+ 9]<<8)|
                        ((uint32_t)dir[eoff+10]<<16)|((uint32_t)dir[eoff+11]<<24);
        uint32_t ida  = (uint32_t)dir[eoff+12]|((uint32_t)dir[eoff+13]<<8)|
                        ((uint32_t)dir[eoff+14]<<16)|((uint32_t)dir[eoff+15]<<24);
        /* BIGDIRENTRY_ATT at +16: RISC OS attribute byte.
         * Bit 3 = D (directory) — reliable for both &DDC dirs and &FFD image dirs.
         * This is the correct discriminator; checking load_addr bits[31:20]==0xFFF
         * is wrong because ALL BigDir entries (files AND dirs) have 0xFFF there.  */
        uint32_t attr = (uint32_t)dir[eoff+16]|((uint32_t)dir[eoff+17]<<8)|
                        ((uint32_t)dir[eoff+18]<<16)|((uint32_t)dir[eoff+19]<<24);
        uint32_t nlen = (uint32_t)dir[eoff+20]|((uint32_t)dir[eoff+21]<<8)|
                        ((uint32_t)dir[eoff+22]<<16)|((uint32_t)dir[eoff+23]<<24);
        uint32_t noff = (uint32_t)dir[eoff+24]|((uint32_t)dir[eoff+25]<<8)|
                        ((uint32_t)dir[eoff+26]<<16)|((uint32_t)dir[eoff+27]<<24);

        vfs_dirent_t *d = &g_root_cache[g_root_cache_count];
        d->load_addr   = load;
        d->exec_addr   = (uint32_t)dir[eoff+4]|((uint32_t)dir[eoff+5]<<8)|
                         ((uint32_t)dir[eoff+6]<<16)|((uint32_t)dir[eoff+7]<<24);
        d->size        = (uint64_t)len;
        d->sin         = ida;
        d->riscos_type = (uint16_t)((load >> 8) & 0xFFFu);

        /* Determine entry type using RISC OS attribute D-bit (bit 3) */
        if (ida == 0x00000300u)
            d->type = VFS_DIRENT_SPECIAL;
        else if (attr & 0x08u)
            d->type = VFS_DIRENT_DIR;
        else
            d->type = VFS_DIRENT_FILE;

        /* Copy name (0x0D-terminated) */
        uint32_t nabs = name_tab + noff;
        uint32_t k;
        for (k = 0u; k < nlen && k < VFS_NAME_MAX-1u; k++) {
            if (nabs+k >= 2048u || dir[nabs+k] == 0x0Du) break;
            d->name[k] = (char)(dir[nabs+k] & 0x7Fu);
        }
        d->name[k] = '\0';

        g_root_cache_count++;
    }
    g_root_cache_valid = 1;
    uart_puts("[VFS] Root cache populated: "); fc_dec(g_root_cache_count);
    uart_puts(" entries\n");
}

/* Called by vfs.c readdir to retrieve a cached root entry */
int filecore_get_root_entry(uint32_t idx, vfs_dirent_t *out)
{
    if (!g_root_cache_valid || idx >= g_root_cache_count) return -1;
    *out = g_root_cache[idx];
    return 0;
}

/* ── filecore_get_child_entry ────────────────────────────────────────────────
 * Resolve a subdirectory by its IDA (dir_sin) and return entry[idx] from it.
 *
 * Algorithm (boot268 IDA-based navigation):
 *   1. Derive disc geometry from global disc-record params.
 *   2. Call fc_ida_to_data_lba() to resolve IDA → data LBA.
 *   3. Read dir_size bytes from that LBA.
 *   4. Validate SBPr header.
 *   5. Return the entry at index idx as a vfs_dirent_t.
 *
 * Returns 0 on success, -1 on any failure (not mounted, resolve fail,
 * read error, not a BigDir, idx out of range).                              */
int filecore_get_child_entry(uint32_t dir_sin, uint32_t idx,
                              vfs_dirent_t *out)
{
    if (!g_fc_bdev || !out) return -1;

    /* ── Derive zone-map geometry (same as filecore_list_root) ── */
    uint32_t sector_size  = 1u << g_dr_log2ss;
    uint32_t bpmb         = 1u << g_dr_log2bpmb;
    uint32_t secperlfau   = bpmb / sector_size;
    uint32_t bits_per_sec = sector_size * 8u;
    uint32_t used_bits    = bits_per_sec - g_dr_zone_spare;
    uint32_t dr_size      = 60u * 8u;

    uint32_t total_nzones = g_dr_nzones;
    if (g_dr_big_flag) total_nzones += (uint32_t)g_dr_nzones_hi << 8u;
    uint32_t mid_zone     = total_nzones / 2u;
    uint32_t disc_map_lba = g_fc_lba_base
                          + (mid_zone * used_bits - dr_size) * secperlfau;

    /* ── IDA → data LBA ── */
    uint32_t dir_lba = 0u;
    if (fc_ida_to_data_lba(dir_sin,
                            g_fc_lba_base, disc_map_lba,
                            g_dr_zone_spare, used_bits,
                            dr_size, g_dr_id_len,
                            secperlfau, total_nzones,
                            &dir_lba) != 0) {
        uart_puts("[GCE] IDA resolve failed for sin=");
        fc_hex32(dir_sin); uart_puts("\n");
        return -1;
    }

    /* ── Read first sector; get dir_size from SBPr header ── */
    uint8_t *dbuf = (uint8_t *)kmalloc(FC_MAX_DIR_SIZE);
    if (!dbuf) return -1;

    if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)dir_lba, 1, dbuf) < 0) {
        uart_puts("[GCE] read error lba="); fc_hex32(dir_lba); uart_puts("\n");
        kfree(dbuf); return -1;
    }

    if (dbuf[4] != 'S' || dbuf[5] != 'B' || dbuf[6] != 'P' || dbuf[7] != 'r') {
        uart_puts("[GCE] not SBPr at lba="); fc_hex32(dir_lba); uart_puts("\n");
        kfree(dbuf); return -1;
    }

    uint32_t dir_size = (uint32_t)dbuf[12] | ((uint32_t)dbuf[13]<<8)
                      | ((uint32_t)dbuf[14]<<16) | ((uint32_t)dbuf[15]<<24);
    if (dir_size < 2048u || dir_size > FC_MAX_DIR_SIZE || (dir_size & 511u) != 0u)
        dir_size = 2048u;

    /* ── Read remaining sectors ── */
    uint32_t nsecs = dir_size / sector_size;
    for (uint32_t s = 1u; s < nsecs; s++) {
        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)(dir_lba + s),
                                  1, dbuf + s * sector_size) < 0) {
            kfree(dbuf); return -1;
        }
    }

    /* ── Decode SBPr header ── */
    uint32_t namelen     = (uint32_t)dbuf[8]  | ((uint32_t)dbuf[9]<<8)
                         | ((uint32_t)dbuf[10]<<16) | ((uint32_t)dbuf[11]<<24);
    uint32_t count       = (uint32_t)dbuf[16] | ((uint32_t)dbuf[17]<<8)
                         | ((uint32_t)dbuf[18]<<16) | ((uint32_t)dbuf[19]<<24);

    if (idx >= count) { kfree(dbuf); return -1; }

    uint32_t entry_start = 28u + ((namelen + 1u + 3u) & ~3u);
    uint32_t name_tab    = entry_start + count * 28u;
    uint32_t oven_off    = dir_size - 8u;

    /* ── Extract entry at idx ── */
    uint32_t eoff = entry_start + idx * 28u;

    uint32_t load = (uint32_t)dbuf[eoff+ 0]|((uint32_t)dbuf[eoff+ 1]<<8)|
                    ((uint32_t)dbuf[eoff+ 2]<<16)|((uint32_t)dbuf[eoff+ 3]<<24);
    uint32_t exec = (uint32_t)dbuf[eoff+ 4]|((uint32_t)dbuf[eoff+ 5]<<8)|
                    ((uint32_t)dbuf[eoff+ 6]<<16)|((uint32_t)dbuf[eoff+ 7]<<24);
    uint32_t len  = (uint32_t)dbuf[eoff+ 8]|((uint32_t)dbuf[eoff+ 9]<<8)|
                    ((uint32_t)dbuf[eoff+10]<<16)|((uint32_t)dbuf[eoff+11]<<24);
    uint32_t ida  = (uint32_t)dbuf[eoff+12]|((uint32_t)dbuf[eoff+13]<<8)|
                    ((uint32_t)dbuf[eoff+14]<<16)|((uint32_t)dbuf[eoff+15]<<24);
    uint32_t gce_attr = (uint32_t)dbuf[eoff+16]|((uint32_t)dbuf[eoff+17]<<8)|
                    ((uint32_t)dbuf[eoff+18]<<16)|((uint32_t)dbuf[eoff+19]<<24);
    uint32_t nlen = (uint32_t)dbuf[eoff+20]|((uint32_t)dbuf[eoff+21]<<8)|
                    ((uint32_t)dbuf[eoff+22]<<16)|((uint32_t)dbuf[eoff+23]<<24);
    uint32_t noff = (uint32_t)dbuf[eoff+24]|((uint32_t)dbuf[eoff+25]<<8)|
                    ((uint32_t)dbuf[eoff+26]<<16)|((uint32_t)dbuf[eoff+27]<<24);

    out->load_addr   = load;
    out->exec_addr   = exec;
    out->size        = (uint64_t)len;
    out->sin         = ida;
    out->riscos_type = (uint16_t)((load >> 8) & 0xFFFu);

    /* Use RISC OS attribute D-bit (bit 3) to distinguish directories from files */
    if (ida == 0x00000300u)
        out->type = VFS_DIRENT_SPECIAL;
    else if (gce_attr & 0x08u)
        out->type = VFS_DIRENT_DIR;
    else
        out->type = VFS_DIRENT_FILE;

    /* ── Copy name (0x0D terminated, bounded by oven_off) ── */
    uint32_t nabs = name_tab + noff;
    uint32_t k;
    for (k = 0u; k < nlen && k < VFS_NAME_MAX - 1u; k++) {
        if (nabs + k >= oven_off || dbuf[nabs + k] == 0x0Du) break;
        out->name[k] = (char)(dbuf[nabs + k] & 0x7Fu);
    }
    out->name[k] = '\0';

    kfree(dbuf);
    return 0;
}

/* ── filecore_show_results ───────────────────────────────────────────────────
 * Display a FileCore status panel on the framebuffer after boot.
 * Shows disc detection results and root directory listing.
 * Called from kernel.c after filecore_list_root().                          */
void filecore_show_results(void)
{
    extern void con_printf(const char *fmt, ...);
    extern void con_set_colours(uint32_t fg, uint32_t bg);

    /* Header: white on dark blue */
    con_set_colours(0xFFFFFFFFu, 0xFF000080u);
    con_printf("  FileCore: Phoenix disc scan\n");
    con_set_colours(0xFF202020u, 0xFFE0E0E0u);   /* dark on light grey */

    if (!g_fc_bdev) {
        con_printf("  No RISC OS disc found\n");
        return;
    }

    /* Disc name (trim trailing spaces) */
    char name[11];
    int ni;
    for (ni = 0; ni < 10; ni++) name[ni] = g_dr_disc_name[ni];
    name[10] = '\0';
    for (ni = 9; ni >= 0 && (name[ni] == ' ' || name[ni] == '\0'); ni--)
        name[ni] = '\0';

    con_printf("  Disc: %s\n", name[0] ? name : "unnamed");

    /* Root directory listing */
    if (g_root_cache_valid && g_root_cache_count > 0u) {
        con_printf("  $: %u objects:", (unsigned)g_root_cache_count);
        for (uint32_t i = 0u; i < g_root_cache_count && i < 8u; i++)
            con_printf("  %s", g_root_cache[i].name);
        con_printf("\n");
    } else {
        con_printf("  $ (empty or not read)\n");
    }

    con_set_colours(0xFF004000u, 0xFFE0E0E0u);   /* dark green */
    con_printf("  FileCore ready\n");
    con_set_colours(0xFF202020u, 0xFFE0E0E0u);
}

/* ── adfs_chain_traverse ─────────────────────────────────────────────────────
 * Follow the FileCore new-map allocation chain for chain_offset hops.
 *
 * Algorithm (bit-address model):
 *   cur_lfau = frag_id            (starting bit address in zone map)
 *   for each hop in 0..chain_offset-1:
 *     zone        = cur_lfau / zone_bits          (zone_bits = 4096)
 *     bit_in_zone = cur_lfau % zone_bits
 *     lba         = zone_lba(zone)                (ONE sector per zone)
 *     entry       = read id_len bits at bit_in_zone from sector at lba
 *     entry == 0  → chain broken (LFAU free — corrupt map)
 *     entry == 1  → chain end reached before expected (object shorter)
 *     else        → cur_lfau = entry (follow the link)
 *   After chain_offset hops, cur_lfau is the terminal LFAU's bit address.
 *   Convert to data_lba via fc_bit_addr_to_data_lba().
 *
 * Returns 0 if Hugo/Nick directory magic found in the data sectors, -1 otherwise. */
static int adfs_chain_traverse(uint32_t frag_id, uint32_t chain_offset,
                                uint32_t lba_base,  uint32_t disc_map_lba,
                                uint32_t zone_spare, uint32_t used_bits,
                                uint32_t dr_size,    uint32_t id_len,
                                uint32_t secperlfau, uint32_t total_nzones)
{
    uint32_t zone_bits = zone_spare + used_bits;   /* = 4096 */
    uint32_t cur_lfau  = frag_id;

    uart_puts("[ADFS] Chain traverse: frag_id="); fc_dec(frag_id);
    uart_puts(" hops="); fc_dec(chain_offset); uart_puts("\n");
    uart_puts("[ADFS] disc_map_lba="); fc_hex32(disc_map_lba); uart_puts("\n");

    uint8_t *zbuf = (uint8_t *)kmalloc(512);
    if (!zbuf) { uart_puts("[ADFS] kmalloc fail (zone buf)\n"); return -1; }

    /* ── Follow chain_offset forward links ──────────────────────────── */
    /*
     * Chain link values are raw sector bit addresses:
     *   B = zone * zone_bits + sector_bit   (zone_bits = zone_spare + used_bits = 4096)
     * Read id_len bits at sector_bit = B % zone_bits inside zone B/zone_bits.
     *
     * Entry semantics (ADFS new map):
     *   0       = LFAU is FREE — should not appear in a valid chain
     *   1       = END OF CHAIN — this LFAU is the last one in the fragment
     *   >= 2    = bit address of the NEXT LFAU's map entry in the zone map
     *
     * boot211: chain_offset here is num_hops = byte_offset / lfau_size.
     * For root_dir (byte_offset=95, lfau_size=1024): num_hops=0 → loop
     * does not execute → data_lba = fc_bit_addr_to_data_lba(frag_id).
     *
     * History:
     *   boot207/208: chain_offset was passed as raw IDA>>id_len = 95 hops.
     *   boot209: subtracting zone_spare caused zone-0/MBR hit — reverted.
     *   boot211: IDA>>id_len is a BYTE OFFSET, not a hop count.
     */
    /* boot227: zone-0 header area boundary.  Zone 0's first (zone_spare+dr_size)
     * bits are the zone check-word + DiscRecord and are NOT LFAU map entries.
     * Any chain link whose bit address falls in this region is invalid.        */
    uint32_t z0_hdr_bits = zone_spare + dr_size;   /* = 32+480 = 512 */

    for (uint32_t hop = 0u; hop < chain_offset; hop++) {

        uint32_t zone        = cur_lfau / zone_bits;
        uint32_t bit_in_zone = cur_lfau % zone_bits;

        /* boot227: guard — zone 0 header/DiscRecord area is not a valid LFAU */
        if (zone == 0u && bit_in_zone < z0_hdr_bits) {
            uart_puts("[ADFS] WARN z0-hdr: hop="); fc_dec(hop);
            uart_puts(" b="); fc_dec(cur_lfau);
            uart_puts(" bz="); fc_dec(bit_in_zone);
            uart_puts(" (hdr<"); fc_dec(z0_hdr_bits);
            uart_puts(") — chain broken\n");
            break;
        }

        /* boot227: live zone header via fc_zone_lba.
         * disc_map_lba+zone (contiguous backup) returns e=0 for all high zones
         * on this disc — confirmed broken in boot222 and boot227.
         * fc_zone_lba gives valid entries for most zones (22, 5, 70, 98 etc).  */
        uint32_t map_lba = (zone == 0u) ? lba_base
                         : lba_base + (zone * used_bits - dr_size) * secperlfau;

        /* Log every hop when zone==0 (diagnostic), else every 10 hops */
        int do_log = (zone == 0u || hop == 0u || (hop % 10u) == 0u);
        if (do_log) {
            uart_puts("[ADFS]  h="); fc_dec(hop);
            uart_puts(" b="); fc_dec(cur_lfau);
            uart_puts(" z="); fc_dec(zone);
            uart_puts(" bz="); fc_dec(bit_in_zone);
            uart_puts(" L="); fc_hex32(map_lba); uart_puts("\n");
        }

        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)map_lba, 1, zbuf) < 0) {
            uart_puts("[ADFS] read err hop="); fc_dec(hop); uart_puts("\n");
            kfree(zbuf);
            return -1;
        }

        uint32_t entry = adfs_read_bits(zbuf, bit_in_zone, (int)id_len);

        if (do_log) {
            uart_puts("[ADFS]  e="); fc_dec(entry); uart_puts("\n");
        }

        /* boot227: no copy-2 fallback */
        if (entry == 0u) {
            uart_puts("[ADFS]  e=0 hop="); fc_dec(hop);
            uart_puts(" z="); fc_dec(zone);
            uart_puts(" bz="); fc_dec(bit_in_zone);
            uart_puts(" — chain broken\n");
            break;
        }

        if (entry == 1u) {
            uart_puts("[ADFS]  chain end (e=1) hop="); fc_dec(hop);
            uart_puts(" b="); fc_dec(cur_lfau); uart_puts("\n");
            break;
        }

        /* Warn if next link points into zone-0 header area */
        if (entry < z0_hdr_bits) {
            uart_puts("[ADFS] WARN: e="); fc_dec(entry);
            uart_puts(" → z0-hdr hop="); fc_dec(hop);
            uart_puts(" from z="); fc_dec(zone);
            uart_puts(" bz="); fc_dec(bit_in_zone); uart_puts("\n");
        }

        cur_lfau = entry;
    }

    kfree(zbuf);

    /* ── Compute physical data LBA from terminal bit address ──────────── */
    uint32_t data_lba = fc_bit_addr_to_data_lba(cur_lfau, lba_base,
                                                  zone_spare, used_bits,
                                                  dr_size, id_len, secperlfau);

    uart_puts("[ADFS] Terminal: bit_addr="); fc_dec(cur_lfau);
    uart_puts(" zone="); fc_dec(cur_lfau / zone_bits);
    uart_puts(" bit_in_zone="); fc_dec(cur_lfau % zone_bits);
    uart_puts(" → data_lba="); fc_hex32(data_lba); uart_puts("\n");

    /* ── Read 4 sectors (2048 bytes = one Hugo directory) ─────────────── */
    uint8_t *dirbuf = (uint8_t *)kmalloc(2048u);
    if (!dirbuf) { uart_puts("[ADFS] kmalloc fail (dirbuf)\n"); return -1; }

    int read_ok = 1;
    for (int s = 0; s < 4; s++) {
        if (g_fc_bdev->ops->read(g_fc_bdev,
                                  (uint64_t)(data_lba + (uint32_t)s),
                                  1, dirbuf + (uint32_t)s * 512u) < 0) {
            uart_puts("[ADFS]   data read error at sector "); fc_dec((uint32_t)s); uart_puts("\n");
            read_ok = 0; break;
        }
    }

    if (!read_ok) { kfree(dirbuf); return -1; }

    /* ── SECOND 64-byte dump: chain traverse terminal ─────────────────────
     * boot218: expanded from 32 to 64 bytes for comparison with Step 2B   *
     * (disc_map_lba+2×nzones probe) and DiskKnight data.                  */
    uart_puts("[ADFS] === Chain terminal: 64 bytes at data_lba ===\n");
    {
        static const char hxt[] = "0123456789ABCDEF";
        for (int row = 0; row < 4; row++) {
            uart_puts("[ADFS]   +0x");
            char rh[3] = { hxt[(row*16)>>4], hxt[(row*16)&0xF], '\0' };
            uart_puts(rh); uart_puts(": ");
            for (int b = 0; b < 16; b++) {
                uint8_t v = dirbuf[row*16 + b];
                char tmp[4] = { hxt[(v>>4)&0xF], hxt[v&0xF], ' ', '\0' };
                uart_puts(tmp);
            }
            uart_puts("\n");
        }
    }

    /* Search for root IDA in first 64 bytes of terminal data */
    {
        /* root IDA is not directly available here; log bytes[24..27] as
         * per bootlog230 (found root IDA at that offset at LBA 0x775AC8) */
        uint32_t b24 = (uint32_t)dirbuf[24]
                     | ((uint32_t)dirbuf[25] << 8)
                     | ((uint32_t)dirbuf[26] << 16)
                     | ((uint32_t)dirbuf[27] << 24);
        uart_puts("[ADFS]   bytes[24..27]="); fc_hex32(b24); uart_puts("\n");
    }

    /* ── Check for directory magic: SBPr (E+/F+ big dir) or Hugo/Nick ─── */
    int sbpr_ok = (dirbuf[4]=='S' && dirbuf[5]=='B' &&
                   dirbuf[6]=='P' && dirbuf[7]=='r');
    int hugo    = (dirbuf[1]=='H' && dirbuf[2]=='u' &&
                   dirbuf[3]=='g' && dirbuf[4]=='o');
    int nick    = (dirbuf[2044]=='N' && dirbuf[2045]=='i' &&
                   dirbuf[2046]=='c' && dirbuf[2047]=='k');

    if (sbpr_ok) {
        uart_puts("[ADFS] *** SBPr directory at data_lba=");
        fc_hex32(data_lba); uart_puts(" ***\n");
        kfree(dirbuf);
        return 0;
    }
    if (hugo || nick) {
        uart_puts("[ADFS] *** Hugo/Nick directory at data_lba=");
        fc_hex32(data_lba); uart_puts(" ***\n");
        kfree(dirbuf);
        return 0;
    }
    uart_puts("[ADFS] No directory magic at data_lba="); fc_hex32(data_lba); uart_puts("\n");
    kfree(dirbuf);
    return -1;
}

/* ── adfs_hugo_brute_scan ────────────────────────────────────────────────────
 * Scan disc for a Hugo-format directory by checking the magic bytes directly.
 *
 * Hugo layout: byte[1]='H', byte[2]='u', byte[3]='g', byte[4]='o'.
 * Hugo directories are always aligned to a full LFAU boundary (secperlfau
 * sectors). We read one sector at a time and check for the magic.
 *
 * Parameters:
 *   start_lba   – first LBA to check (typically just above FAT32)
 *   scan_lbas   – how many LBAs to scan
 *   secperlfau  – used to align scan to LFAU boundaries (step = secperlfau)
 *
 * Prints the first LBA where Hugo magic is found and dumps the directory.   */
static void adfs_hugo_brute_scan(uint32_t start_lba, uint32_t scan_lbas,
                                  uint32_t secperlfau)
{
    uart_puts("[ADFS] Brute-force Hugo scan: LBA ");
    fc_hex32(start_lba); uart_puts(" + "); fc_dec(scan_lbas); uart_puts(" sectors\n");

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) { uart_puts("[ADFS] kmalloc fail (scan buf)\n"); return; }

    uint32_t step  = (secperlfau > 0u) ? secperlfau : 1u;
    uint32_t found = 0u;

    for (uint32_t lba = start_lba; lba < start_lba + scan_lbas; lba += step) {
        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)lba, 1, buf) < 0) continue;

        if (buf[1] == 'H' && buf[2] == 'u' && buf[3] == 'g' && buf[4] == 'o') {
            uart_puts("[ADFS] *** Hugo magic at LBA="); fc_hex32(lba); uart_puts(" ***\n");

            /* Read all 4 sectors of the directory and dump it */
            uint8_t *dirbuf = (uint8_t *)kmalloc(2048u);
            if (dirbuf) {
                int ok = 1;
                for (int s = 0; s < 4; s++) {
                    if (g_fc_bdev->ops->read(g_fc_bdev,
                                              (uint64_t)(lba + (uint32_t)s),
                                              1, dirbuf + (uint32_t)s * 512u) < 0) {
                        uart_puts("[ADFS]   read error at sector ");
                        fc_dec((uint32_t)s); uart_puts("\n");
                        ok = 0; break;
                    }
                }
                if (ok) adfs_dump_hugo_dir(dirbuf);
                kfree(dirbuf);
            }
            found++;
            /* Keep scanning — there might be more than one Hugo dir (sub-dirs).
             * Stop after 4 to avoid flooding the log.                        */
            if (found >= 4u) break;
        }
    }

    kfree(buf);

    if (found == 0u) {
        uart_puts("[ADFS] Hugo not found in scan range LBA ");
        fc_hex32(start_lba); uart_puts("–");
        fc_hex32(start_lba + scan_lbas); uart_puts("\n");
        uart_puts("[ADFS] Hint: check FAT32 partition range (LBA 40–102479)\n");
        uart_puts("[ADFS]   → the disc may be pure ADFS overlaying that range too\n");
    } else {
        uart_puts("[ADFS] Scan found "); fc_dec(found);
        uart_puts(" Hugo director(ies)\n");
    }
}

/* ── filecore_find_path ──────────────────────────────────────────────────────
 * Resolve a path string of the form "HardDisc0.$.name1.name2" into a
 * vfs_dirent_t.  Strips the optional "HardDisc0.$." prefix, then walks
 * path components through root cache and child directories.
 *
 * Returns 0 and fills *out on success, -1 if not found.
 * (boot299 stub — path walk not yet exercised in boot300)                   */
int filecore_find_path(const char *path, vfs_dirent_t *out)
{
    (void)path; (void)out;
    return -1;
}

/* ── filecore_read_file ──────────────────────────────────────────────────────
 * Public wrapper: read a file by IDA (sin) into a newly-allocated buffer.
 * Derives disc geometry from g_dr_* globals and calls filecore_read_file_buf.
 *
 * Returns kmalloc'd buffer on success (caller must kfree), NULL on failure.
 * (boot299 stub — not exercised in boot300)                                 */
uint8_t *filecore_read_file(uint32_t sin, uint32_t size)
{
    if (!g_fc_bdev || size == 0u) return NULL;

    uint32_t sector_size = 1u << g_dr_log2ss;
    uint32_t bpmb        = 1u << g_dr_log2bpmb;
    uint32_t secperlfau  = bpmb / sector_size;
    uint32_t used_bits   = (sector_size * 8u) - g_dr_zone_spare;
    uint32_t dr_size     = 60u * 8u;
    uint32_t nzones      = g_dr_nzones;
    if (g_dr_big_flag) nzones += (uint32_t)g_dr_nzones_hi << 8u;
    uint32_t mid_zone    = nzones / 2u;
    uint32_t dml         = g_fc_lba_base
                         + (mid_zone * used_bits - dr_size) * secperlfau;

    return filecore_read_file_buf(sin, size, dml, used_bits, dr_size,
                                   secperlfau, nzones);
}
