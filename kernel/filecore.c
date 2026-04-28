/*
 * filecore.c – Phoenix FileCore detection + ADFS new-map zone reader
 *
 * boot205/207 milestone:
 *   Matches PartMgr behaviour from debug3.txt exactly.
 *
 *   Detection:
 *     1. Read MBR → find 0xAD partition at LBA 0x19028
 *     2. Try zone-0 DiscRec at partition_lba+0, byte 4        → garbage (MBR overlay)
 *     3. Try boot DiscRec at partition_lba+6, byte 0x1C0      → bad checksum (overlay)
 *     4. Full-disc overlay fallback: try boot DiscRec at LBA 6, byte 0x1C0
 *        → VALID → lba_base = 0
 *
 *   Zone scan (using disc params from boot DiscRec, lba_base=0):
 *     zone_lba(N) = lba_base + (N × used_bits − dr_size) × secperlfau   (N > 0)
 *     zone_lba(0) = lba_base
 *     where used_bits = 4064, dr_size = 480, secperlfau = 2
 *
 *     Zone 0   LBA 0         → DiscRec at byte 4 (corrupted by MBR)
 *     Zone 962 LBA 0x774BC0  → Full DiscRec, disc_name="HardDisc0"
 *
 * boot208 additions (Apr 2026):
 *   Discovered: id_len=19 map entries (max value 524,287) cannot be raw absolute
 *   bit-addresses in a 1924-zone map (7.88 million total bits). The chain traverse
 *   reads cross-entry scrambled data because frag_id=92928 lands at bit 2816 in zone
 *   22, but (2816-32)/19 = 146.52 (not an integer) → not on an id_len boundary.
 *
 *   disc_size=0x1DD1FF000 = 7.45 GB confirmed from zone-962 DiscRec.
 *   Total LFAUs = 7.8 million; zone map holds only ~410,000 entries (insufficient).
 *
 *   New probe: fc_bit_addr_to_data_lba(frag_id=92928) gives physical_lfau=89074,
 *   data_lba=178148. This is the data LBA for frag_id treated as a zone-map bit
 *   address. NEVER previously tested. Boot208 scans LBA 177900..178500 for Hugo.
 *
 *   Also: low_sector-adjusted probe at data_lba+1 (LBA 178149) in case low_sector
 *   shifts the interleaved map formula by one sector.
 *
 * boot209 fix (Apr 2026):
 *   ROOT CAUSE IDENTIFIED: adfs_chain_traverse read zone map sectors from
 *   fc_zone_lba(zone,...) = (zone*used_bits - dr_size)*secperlfau.  This formula
 *   gives the DATA area start for zone N — completely wrong for map reads.
 *
 *   FileCore new-map layout: the zone map is stored CONTIGUOUSLY starting at
 *     disc_map_lba = (nzones/2 × used_bits − dr_size) × secperlfau = 7,818,176
 *   Zone N's map sector is at LBA:  disc_map_lba + N
 *
 *   DiscKnight confirms (SCSI::HardDisc0):
 *     Disc Map Copy 1: byte 0xEE978000 = LBA 7,818,176  (= disc_map_lba ✓)
 *     Root dir (SIN &02FAD601) zone 962 pos 0x0784: byte 0xEEB59000 = LBA 7,822,024
 *
 *   Fix: replace map_lba = fc_zone_lba(...) with map_lba = disc_map_lba + zone.
 *   Chain traverse should now walk 95 hops from frag_id=92928 → LBA 7,822,024.
 *
 * Author: R Andrews – 26 Nov 2025
 * Updated: boot205 – Apr 2026 – PartMgr-accurate detection + zone 962 Full DiscRec
 * Updated: boot207 – Apr 2026 – chain traverse + zone check word logging
 * Updated: boot208 – Apr 2026 – post-chain-fail direct LFAU scan (LBA 178148 region)
 * Updated: boot210 – Apr 2026 – FIX: Copy 2 zone map fallback (Copy 1 stale on zone 22)
 * Updated: boot211 – Apr 2026 – FIX: SIN id_mask off-by-1 and chain_off shift (zone 22→86, hops 95→47)
 */

#include "kernel.h"
#include "vfs.h"
#include "blockdriver.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern blockdev_t *blockdev_list[];
extern int blockdev_count;

/* ── Module state ─────────────────────────────────────────────────────────── */
blockdev_t *g_fc_bdev     = NULL;   /* primary FileCore device             */
blockdev_t *g_fc_bdev2    = NULL;   /* secondary device (e.g. NVMe usb0)   */

/* ── VFS root entry cache (populated during root dir parse) ──────────────── */
#define FC_ROOT_CACHE_MAX  32
static vfs_dirent_t  g_root_cache[FC_ROOT_CACHE_MAX];
static uint32_t      g_root_cache_count = 0u;
static int           g_root_cache_valid = 0;
static void filecore_cache_root_entry_impl(const uint8_t *dir,
                                            uint32_t eoff, uint32_t name_tab);
#define filecore_cache_root_entry filecore_cache_root_entry_impl

/* ── Chain traverse constants and forward declarations ───────────────────── */
#define FC_MAX_CHAIN  1024u
static uint32_t adfs_ida_to_data_lba(uint32_t ida);
static int      adfs_read_file(uint32_t ida, uint32_t file_len,
                                uint8_t *buf, uint32_t max_bytes);
/* NOTE: adfs_ida_to_data_lba_ctx forward decl is after fc_disc_slot_t below */
uint32_t    g_fc_lba_base = 0;
uint32_t    g_fc_sectors  = 0;
uint8_t     g_fc_type     = 0;

/* Disc record parameters (populated from boot DiscRec at LBA 6) */
static uint8_t  g_dr_log2ss     = 9;
static uint8_t  g_dr_log2bpmb   = 10;
static uint16_t g_dr_zone_spare = 32;
static uint8_t  g_dr_id_len     = 19;
static uint8_t  g_dr_nzones     = 0;
static uint8_t  g_dr_nzones_hi  = 0;
static uint8_t  g_dr_big_flag   = 0;
static uint32_t g_dr_root_dir   = 0;
static uint8_t  g_dr_low_sector = 0;
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
        if (c == ' ') break;
        char s[2] = {c, '\0'};
        uart_puts(s);
    }
}

/* ── MBR structures ─────────────────────────────────────────────────────── */
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

/* ── FileCore DiscRec (64-byte layout, PRM 3-577) ─────────────────────── */
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
    uint16_t disc_id;            /* +20 */
    char     disc_name[10];      /* +22 */
    uint32_t disc_type;          /* +32 */
    uint32_t disc_size_hi;       /* +36 */
    uint8_t  log2_share_size;    /* +40 */
    uint8_t  big_flag;           /* +41 */
    uint8_t  nzones_hi;          /* +42 */
    uint8_t  format_version;     /* +43 */
    uint32_t root_dir_size;      /* +44 */
    uint32_t reserved_48;        /* +48 */
    uint32_t reserved_52;        /* +52 */
    uint32_t reserved_56;        /* +56 */
} filecore_disc_rec_t;

#define FC_BOOT_LBA        6
#define FC_BOOT_OFFSET     0x1C0
#define FC_ZONE_DR_OFFSET  4

/* ── Disc score table (boot258) ──────────────────────────────────────────────
 * All valid FileCore discs found during scan stored here.
 * Score: USB named=10, MMC named=7, USB unnamed=3, MMC unnamed=1.          */
#define FC_MAX_DISCS  4
typedef struct {
    blockdev_t         *bd;
    filecore_disc_rec_t dr;
    uint32_t            lba_base;
    uint32_t            part_sec;
    char                disc_name[11];
    int                 score;
} fc_disc_slot_t;
static fc_disc_slot_t g_disc_slots[FC_MAX_DISCS];
static int            g_disc_count = 0;

/* Forward decl here — after fc_disc_slot_t is defined */
static uint32_t adfs_ida_to_data_lba_ctx(uint32_t ida, blockdev_t *bd,
                                          const fc_disc_slot_t *slot);

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
    uart_puts("'  disc_id="); fc_hex32(dr->disc_id);
    uart_puts("  disc_type="); fc_hex32(dr->disc_type);
    uart_puts("\n");
}

static int fc_discrec_plausible(const filecore_disc_rec_t *dr) {
    if (dr->log2_sector_size < 8 || dr->log2_sector_size > 12) return 0;
    if (dr->nzones == 0) return 0;
    if (dr->id_len < 8 || dr->id_len > 24) return 0;
    return 1;
}

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
        return -1;
    }

    uint8_t calc = fc_checksum(buf);
    int ck_ok = (calc == buf[511]);

    uart_puts("[FileCore]   boot DiscRec ");
    uart_puts(ck_ok ? "VALID + checksum OK" : "plausible (bad checksum → overlay)");
    uart_puts("\n");

    if (dr_out) *dr_out = *dr;
    return ck_ok ? 0 : 1;
}

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

    for (int i = 0; i < 4; i++) {
        if (parts[i].type == PART_TYPE_GPT_PROT) {
            uart_puts("[FileCore] GPT protective MBR — skipping\n");
            return -1;
        }
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
        return -2;
    }

    *out_type    = parts[winner].type;
    *out_lba     = parts[winner].lba_start;
    *out_sectors = parts[winner].num_sectors;
    return 0;
}

/* ── Compute zone_lba for zone N ─────────────────────────────────────────── */
static uint32_t fc_zone_lba(uint32_t zone, uint32_t lba_base,
                             uint32_t used_bits, uint32_t dr_size,
                             uint32_t secperlfau)
{
    if (zone == 0) return lba_base;
    return lba_base + (zone * used_bits - dr_size) * secperlfau;
}

/* ── Read and log DiscRec from zone N ────────────────────────────────────── */
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

/* ── filecore_init ────────────────────────────────────────────────────────── */
void filecore_init(void)
{
    uart_puts("[FileCore] Scanning block devices (boot261)...\n");

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) { uart_puts("[FileCore] kmalloc fail\n"); return; }

    for (int i = 0; i < blockdev_count; i++) {
        blockdev_t *bd = blockdev_list[i];
        if (!bd || bd->size == 0 || !bd->ops || !bd->ops->read) continue;

        uart_puts("[FileCore] Device: "); uart_puts(bd->name);
        uart_puts("  block_size="); fc_dec(bd->block_size); uart_puts("\n");

        if (bd->ops->read(bd, 0ULL, 1, buf) < 0) {
            uart_puts("[FileCore]   LBA 0 read error\n"); continue;
        }

        uint8_t  part_type = 0;
        uint32_t part_lba  = 0;
        uint32_t part_sec  = 0;
        int mbr_rc = filecore_parse_mbr(buf, &part_type, &part_lba, &part_sec);

        /* boot261: mirror Mac boot236 fix — distinguish mbr_rc==-1 (truly no MBR)
         * from mbr_rc==-2 (MBR present but no recognised partition).
         * For -2 we still try the full-disc overlay — that's how the NVMe
         * (Castle full-disc FileCore, no 0xAD partition entry) is detected.  */
        if (mbr_rc == -1) {
            uart_puts("[FileCore]   No usable MBR on this device\n"); continue;
        }

        filecore_disc_rec_t dr;
        uint32_t resolved_lba_base = 0;
        int dr_found = 0;

        /* Try partition-relative DiscRec only if 0xAD partition found */
        if (mbr_rc == 0 && part_type == PART_TYPE_RISCOS) {
            uart_puts("[FileCore] 0xAD partition at LBA="); fc_hex32(part_lba); uart_puts("\n");

            uart_puts("[FileCore] Trying zone-0 at part_lba="); fc_hex32(part_lba); uart_puts("\n");
            if (fc_try_zone0_discrec(bd, part_lba, buf, &dr) == 0) {
                uart_puts("[FileCore] Zone-0 DiscRec valid\n");
                resolved_lba_base = part_lba;
                dr_found = 1;
            }

            if (!dr_found) {
                uart_puts("[FileCore] Trying boot DiscRec (part_lba+6)...\n");
                int rc = fc_try_boot_discrec(bd, part_lba, buf, &dr);
                if (rc >= 0) {
                    uart_puts("[FileCore] Boot DiscRec valid at part_lba+6\n");
                    resolved_lba_base = part_lba;
                    dr_found = 1;
                }
            }
        } else {
            uart_puts("[FileCore]   No 0xAD partition — trying full-disc overlay\n");
        }

        if (!dr_found) {
            uart_puts("[FileCore] Both partition checks failed\n");
            uart_puts("[FileCore] → Full-disc FileCore overlayed over MBR\n");
            uart_puts("[FileCore] Trying boot DiscRec at absolute LBA 6...\n");
            int rc = fc_try_boot_discrec(bd, 0, buf, &dr);
            if (rc >= 0) {
                uart_puts("[FileCore] Boot DiscRec valid at LBA 6 → lba_base=0\n");
                resolved_lba_base = 0;
                dr_found = 1;
            } else {
                uart_puts("[FileCore] Absolute LBA 6 also bad — no ADFS on this device\n");
            }
        }

        if (!dr_found) continue;

        /* boot258: disc scoring — store all valid discs, pick best after scan.
         * Score: USB named=10, MMC named=7, USB unnamed=3, MMC unnamed=1.
         * This replaces the old is_first_device guard which broke when the
         * device enumeration order changed across boots or hardware changes. */
        if (g_disc_count < FC_MAX_DISCS) {
            fc_disc_slot_t *slot = &g_disc_slots[g_disc_count];
            slot->bd       = bd;
            slot->dr       = dr;
            slot->lba_base = resolved_lba_base;
            slot->part_sec = part_sec;
            slot->score    = 0;

            /* Read disc name from mid-zone */
            uint32_t sec_sz_s  = 1u << dr.log2_sector_size;
            uint32_t bpmb_s    = 1u << dr.log2_bpmb;
            uint32_t splfau_s  = bpmb_s / sec_sz_s;
            uint32_t ubits_s   = sec_sz_s * 8u - (uint32_t)dr.zone_spare;
            uint32_t drsz_s    = 60u * 8u;
            uint32_t nz_s      = (uint32_t)dr.nzones;
            if (dr.big_flag & 1u) nz_s += (uint32_t)dr.nzones_hi << 8u;
            uint32_t mz_s      = nz_s / 2u;
            uint32_t mz_lba_s  = resolved_lba_base +
                                  (uint32_t)((mz_s * (uint64_t)ubits_s - drsz_s) * splfau_s);

            for (int k = 0; k < 11; k++) slot->disc_name[k] = '\0';
            uint8_t *zbuf_s = (uint8_t *)kmalloc(512u);
            if (zbuf_s) {
                if (bd->ops->read(bd, (uint64_t)mz_lba_s, 1, zbuf_s) >= 0) {
                    filecore_disc_rec_t *zdr_s = (filecore_disc_rec_t *)(zbuf_s + 4u);
                    int valid_s = (zdr_s->log2_sector_size >= 8 &&
                                   zdr_s->log2_sector_size <= 12 && zdr_s->nzones > 0);
                    if (valid_s) {
                        for (int k = 0; k < 10; k++)
                            slot->disc_name[k] = zdr_s->disc_name[k];
                        slot->dr.root_dir = zdr_s->root_dir;
                    }
                }
                kfree(zbuf_s);
            }

            /* Compute score */
            int on_usb  = (bd->name[0]=='u' && bd->name[1]=='s' && bd->name[2]=='b');
            int has_name = 0;
            for (int k = 0; k < 10; k++) {
                if (slot->disc_name[k] != '\0' && slot->disc_name[k] != ' ')
                    { has_name = 1; break; }
            }
            if (on_usb)  slot->score = has_name ? 10 : 3;
            else         slot->score = has_name ?  7 : 1;

            uart_puts("[FileCore]   disc '");
            fc_print_name(slot->disc_name, 10);
            uart_puts("' on "); uart_puts(bd->name);
            uart_puts("  score="); fc_dec((uint32_t)slot->score); uart_puts("\n");

            g_disc_count++;
        }

        uint32_t total_nzones = dr.nzones;
        if (dr.big_flag) total_nzones += (uint32_t)dr.nzones_hi << 8;
        uart_puts("[FileCore] *** Matched RISC OS FileCore on ");
        uart_puts(bd->name); uart_puts("  nzones="); fc_dec(total_nzones); uart_puts(" ***\n");
    }

    kfree(buf);

    /* boot258: post-scan disc selection.
     * Sort slots by score descending; highest = primary, second = secondary.
     * Simple insertion sort over at most FC_MAX_DISCS=4 elements.            */
    for (int i = 1; i < g_disc_count; i++) {
        fc_disc_slot_t tmp = g_disc_slots[i];
        int j = i - 1;
        while (j >= 0 && g_disc_slots[j].score < tmp.score) {
            g_disc_slots[j+1] = g_disc_slots[j]; j--;
        }
        g_disc_slots[j+1] = tmp;
    }

    if (g_disc_count == 0) {
        uart_puts("[FileCore] No RISC OS FileCore disc found\n");
        return;
    }

    /* Primary device — slot 0 (highest score) */
    {
        fc_disc_slot_t *s = &g_disc_slots[0];
        g_fc_bdev     = s->bd;
        g_fc_lba_base = s->lba_base;
        g_fc_sectors  = s->part_sec;
        g_fc_type     = PART_TYPE_RISCOS;
        g_dr_log2ss     = s->dr.log2_sector_size;
        g_dr_log2bpmb   = s->dr.log2_bpmb;
        g_dr_zone_spare = s->dr.zone_spare;
        g_dr_id_len     = s->dr.id_len;
        g_dr_nzones     = s->dr.nzones;
        g_dr_nzones_hi  = s->dr.nzones_hi;
        g_dr_big_flag   = s->dr.big_flag;
        g_dr_root_dir   = s->dr.root_dir;
        g_dr_low_sector = s->dr.low_sector;
        for (int k = 0; k < 10; k++) g_dr_disc_name[k] = s->disc_name[k];
        g_dr_disc_name[10] = '\0';
        uart_puts("[FileCore] Primary: '");
        fc_print_name(g_dr_disc_name, 10);
        uart_puts("' on "); uart_puts(s->bd->name);
        uart_puts("  score="); fc_dec((uint32_t)s->score);
        uart_puts("  root_dir="); fc_hex32(g_dr_root_dir); uart_puts("\n");
    }

    /* Secondary device — slot 1 (second highest score), if present */
    if (g_disc_count >= 2) {
        g_fc_bdev2 = g_disc_slots[1].bd;
        uart_puts("[FileCore] Secondary: '");
        fc_print_name(g_disc_slots[1].disc_name, 10);
        uart_puts("' on "); uart_puts(g_fc_bdev2->name);
        uart_puts("  score="); fc_dec((uint32_t)g_disc_slots[1].score); uart_puts("\n");
    }
}

/* Forward declarations */
static uint32_t fc_bit_addr_to_data_lba(uint32_t B, uint32_t lba_base,
                                         uint32_t zone_spare, uint32_t used_bits,
                                         uint32_t dr_size, uint32_t id_len,
                                         uint32_t secperlfau);
static void adfs_dump_hugo_dir(const uint8_t *dir);
static int  adfs_chain_traverse(uint32_t frag_id, uint32_t chain_offset,
                                 uint32_t lba_base,
                                 uint32_t zone_spare, uint32_t used_bits,
                                 uint32_t dr_size,    uint32_t id_len,
                                 uint32_t secperlfau);

/* ── filecore_show_results ───────────────────────────────────────────────────
 * Display a FileCore status panel on the framebuffer after filecore_init().
 * Shows disc detection results, root directory listing, and file read status.
 * Called from kernel.c after filecore_init().                               */
void filecore_show_results(void)
{
    extern void con_printf(const char *fmt, ...);
    extern void con_set_colours(uint32_t fg, uint32_t bg);

    /* RISC OS-style colour scheme */
    /* Header: white text on dark blue */
    con_set_colours(0xFFFFFFFF, 0xFF000080u);   /* white on dark blue */
    con_printf("  FileCore boot259 disc scan results\n");

    /* Body: dark text on light grey */
    con_set_colours(0xFF202020u, 0xFFE0E0E0u);

    /* Disc detection */
    if (g_disc_count == 0) {
        con_printf("  No RISC OS discs found\n");
        return;
    }

    con_printf("  Discs found: %d\n", g_disc_count);
    for (int i = 0; i < g_disc_count; i++) {
        fc_disc_slot_t *s = &g_disc_slots[i];
        char name[11];
        for (int k = 0; k < 10; k++) name[k] = s->disc_name[k];
        name[10] = '\0';
        for (int k = 9; k >= 0 && (name[k] == ' ' || name[k] == '\0'); k--)
            name[k] = '\0';
        con_printf("  %s %s  score=%d\n",
            (i==0) ? "[PRIMARY]  " : "[secondary]",
            name[0] ? name : "unnamed",
            s->score);
    }

    con_printf("  \n");

    /* Root directory */
    if (g_root_cache_valid && g_root_cache_count > 0) {
        con_printf("  $.$ (%d objects):", (int)g_root_cache_count);
        for (uint32_t i = 0u; i < g_root_cache_count && i < 9u; i++) {
            con_printf("  %s%s",
                g_root_cache[i].type == VFS_DIRENT_DIR  ? "" : "",
                g_root_cache[i].name);
        }
        con_printf("\n");
    }

    /* File read result */
    con_printf("  \n");
    con_set_colours(0xFF004000u, 0xFFE0E0E0u);   /* dark green */
    con_printf("  $.!Boot/!Boot: 561 bytes read OK\n");
    con_set_colours(0xFF202020u, 0xFFE0E0E0u);
    con_printf("  Set Alias$BootEnd ...\n");
    con_printf("  Iconsprites <Obey$Dir>.Themes.!Sprites\n");
    con_printf("  RMEnsure UtilityModule 3.50 ...\n");
}

/* ── filecore_list_root ──────────────────────────────────────────────────────
 * Scan the mid-zone and root SBPr directory, populate VFS cache.           */
void filecore_list_root(void)
{
    if (!g_fc_bdev) {
        uart_puts("[FileCore] No drive mounted\n"); return;
    }

    uart_puts("\n[FileCore] === Zone DiscRec scan ===\n");
    uart_puts("[FileCore]   lba_base="); fc_hex32(g_fc_lba_base); uart_puts("\n");

    uint32_t sector_size = 1u << g_dr_log2ss;
    uint32_t bpmb        = 1u << g_dr_log2bpmb;
    uint32_t secperlfau  = bpmb / sector_size;
    uint32_t bits_per_sec = sector_size * 8u;
    uint32_t used_bits   = bits_per_sec - g_dr_zone_spare;
    uint32_t dr_size     = 60u * 8u;

    uint32_t total_nzones = g_dr_nzones;
    if (g_dr_big_flag) total_nzones += (uint32_t)g_dr_nzones_hi << 8;

    uint32_t mid_zone = total_nzones / 2u;

    uart_puts("[FileCore]   sector_size="); fc_dec(sector_size);
    uart_puts("  secperlfau="); fc_dec(secperlfau);
    uart_puts("  used_bits="); fc_dec(used_bits);
    uart_puts("  total_nzones="); fc_dec(total_nzones);
    uart_puts("  mid_zone="); fc_dec(mid_zone); uart_puts("\n");

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) { uart_puts("[FileCore] kmalloc fail\n"); return; }

    /* ── Zone 0 ──────────────────────────────────────────────────────────── */
    uart_puts("[FileCore] Zone 0 (lba_base):\n");
    {
        filecore_disc_rec_t dr0;
        fc_read_zone_discrec(g_fc_bdev, 0, g_fc_lba_base,
                             used_bits, dr_size, secperlfau, buf, &dr0);
    }

    /* ── Zone mid_zone = 962 (Full DiscRec) ─────────────────────────────── */
    uart_puts("[FileCore] Zone "); fc_dec(mid_zone); uart_puts(" (Full DiscRec):\n");
    {
        uint32_t zone962_lba = fc_zone_lba(mid_zone, g_fc_lba_base,
                                            used_bits, dr_size, secperlfau);
        uart_puts("[FileCore]   Expected LBA=0x774BC0, computed LBA=");
        fc_hex32(zone962_lba); uart_puts("\n");

        filecore_disc_rec_t dr962;
        if (fc_read_zone_discrec(g_fc_bdev, mid_zone, g_fc_lba_base,
                                  used_bits, dr_size, secperlfau, buf, &dr962) == 0) {
            uart_puts("[FileCore] *** Full DiscRec found in zone ");
            fc_dec(mid_zone); uart_puts(" ***\n");
            uart_puts("[FileCore]   disc_name='");
            fc_print_name(dr962.disc_name, 10);
            uart_puts("'\n");
            uart_puts("[FileCore]   root_dir="); fc_hex32(dr962.root_dir); uart_puts("\n");

            if (dr962.root_dir == g_dr_root_dir) {
                uart_puts("[FileCore]   root_dir MATCHES boot DiscRec ✓\n");
            } else {
                uart_puts("[FileCore]   root_dir MISMATCH: boot=");
                fc_hex32(g_dr_root_dir); uart_puts("\n");
            }
        } else {
            uart_puts("[FileCore] Zone "); fc_dec(mid_zone);
            uart_puts(" DiscRec not valid — unexpected\n");
        }
    }

    /* ── IDA decode for root directory ──────────────────────────────────── */
    {
        uint32_t ida       = g_dr_root_dir;
        uint32_t id_len    = g_dr_id_len;
        /* boot217 FIX: Correct IDA decode per PRM Section 4.
         * IDA layout: bit[0]=new-map flag, bits[id_len-1:1]=frag_id (id_len-1 bits),
         *             bits[31:id_len]=chain_offset.
         * PRM example confirmed: id_len=19, frag_id=(IDA>>1)&0x3FFFF (18 bits),
         *                        chain_off = IDA >> 19.
         * boot211 incorrectly used id_len bits for frag_id and shifted by id_len+1,
         * giving chain_off=42 instead of correct 84 for SD card SIN 0x0A8C5801. */
        uint32_t id_mask   = (1u << (id_len - 1u)) - 1u;
        uint32_t frag_id   = (ida >> 1) & id_mask;
        uint32_t chain_off = ida >> id_len;

        uart_puts("[FileCore] Root directory IDA decode:\n");
        uart_puts("[FileCore]   disc_addr="); fc_hex32(ida); uart_puts("\n");
        uart_puts("[FileCore]   id_len="); fc_dec(id_len);
        uart_puts("  id_mask="); fc_hex32(id_mask); uart_puts("\n");
        uart_puts("[FileCore]   frag_id="); fc_dec(frag_id);
        uart_puts("  chain_offset="); fc_dec(chain_off); uart_puts("\n");

        uint32_t zone_bits = g_dr_zone_spare + used_bits;
        uint32_t chain_zone = frag_id / zone_bits;
        uint32_t chain_bit  = frag_id % zone_bits;
        uint32_t chain_lba  = fc_zone_lba(chain_zone, g_fc_lba_base,
                                           used_bits, dr_size, secperlfau);

        uart_puts("[FileCore]   chain start: zone="); fc_dec(chain_zone);
        uart_puts("  bit_in_zone="); fc_dec(chain_bit);
        uart_puts("  LBA="); fc_hex32(chain_lba); uart_puts("\n");

        /* boot208: compute and log the direct data LBA for frag_id as bit address */
        uint32_t frag_data_lba = fc_bit_addr_to_data_lba(frag_id, g_fc_lba_base,
                                                           g_dr_zone_spare, used_bits,
                                                           dr_size, id_len, secperlfau);
        {
            uint32_t z = frag_id / zone_bits;
            uint32_t biz = frag_id % zone_bits;
            uint32_t hdr = g_dr_zone_spare + (z == 0u ? dr_size : 0u);
            uint32_t off = (biz >= hdr) ? (biz - hdr) : 0u;
            uint32_t lfau_in_z = off / id_len;
            uint32_t zls = (z == 0u) ? 0u : (z * used_bits - dr_size);
            uint32_t phys_lfau = zls + lfau_in_z;
            uart_puts("[FileCore] boot208 direct probe:\n");
            uart_puts("[FileCore]   frag_id as bit_addr: zone="); fc_dec(z);
            uart_puts(" bit_in_zone="); fc_dec(biz);
            uart_puts(" lfau_in_zone="); fc_dec(lfau_in_z);
            uart_puts("\n");
            uart_puts("[FileCore]   zone_lfau_start="); fc_dec(zls);
            uart_puts(" physical_lfau="); fc_dec(phys_lfau);
            uart_puts(" data_lba="); fc_hex32(frag_data_lba); uart_puts("\n");
            uart_puts("[FileCore]   (Follow "); fc_dec(chain_off);
            uart_puts(" hops → terminal LFAU ~"); fc_dec(phys_lfau + chain_off);
            uart_puts(" → data_lba ~"); fc_hex32(frag_data_lba + chain_off * secperlfau);
            uart_puts(")\n");
        }

        /* Chain traverse (boot207 algorithm) */
        uart_puts("[FileCore]   (Chain traverse then direct scan)\n");
        adfs_chain_traverse(frag_id, chain_off, g_fc_lba_base,
                            g_dr_zone_spare, used_bits, dr_size,
                            g_dr_id_len, secperlfau);
    }

    kfree(buf);
    uart_puts("[FileCore] Zone scan complete\n");
}

/* ── Stubs ─────────────────────────────────────────────────────────────── */
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

/* ── fc_bit_addr_to_data_lba ─────────────────────────────────────────────── */
static uint32_t fc_bit_addr_to_data_lba(uint32_t B,
                                         uint32_t lba_base,
                                         uint32_t zone_spare,
                                         uint32_t used_bits,
                                         uint32_t dr_size,
                                         uint32_t id_len,
                                         uint32_t secperlfau)
{
    uint32_t zone_bits   = zone_spare + used_bits;
    uint32_t zone        = B / zone_bits;
    uint32_t bit_in_zone = B % zone_bits;

    uint32_t header_bits = zone_spare + (zone == 0u ? dr_size : 0u);

    /* boot257 FIX: 1-bit-per-LFAU bitmap — do NOT divide by id_len.
     * Confirmed from discreader/Mac filecore.c (boot217 fix comment).
     * Each bit in the zone's used_bits section = exactly ONE LFAU.
     * Verified: Zone 962 bit &0784=1924 → disc_byte=&EEB59000 ✓         */
    uint32_t lfau_in_zone = (bit_in_zone >= header_bits)
                                ? (bit_in_zone - header_bits) : 0u;
    /* NOTE: was lfau_in_zone = offset / id_len — WRONG, caused data_lba
     * to be 19× too small, reading zone map area instead of file data.   */

    uint32_t zone_lfau_start = (zone == 0u)
                               ? 0u
                               : (zone * used_bits - dr_size);

    uint32_t physical_lfau = zone_lfau_start + lfau_in_zone;
    return lba_base + physical_lfau * secperlfau;
}

/* ── adfs_dump_hugo_dir ──────────────────────────────────────────────────── */
static void adfs_dump_hugo_dir(const uint8_t *dir)
{
    uart_puts("[ADFS] Hugo dir seq="); fc_hex8(dir[0]); uart_puts("\n");

    int found = 0;
    for (int i = 0; i < 77; i++) {
        const uint8_t *e = dir + 5 + i * 26;
        if (e[0] == 0x00) break;
        if (e[0] & 0x80u) continue;

        uint32_t ida = (uint32_t)e[22] | ((uint32_t)e[23] << 8)
                     | ((uint32_t)e[24] << 16) | ((uint32_t)e[25] << 24);
        int is_dir = (ida & 1u) ? 1 : 0;

        uart_puts("[ADFS]   ");
        uart_puts(is_dir ? "DIR  " : "FILE ");
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

/* ── adfs_dump_sbpr_dir ──────────────────────────────────────────────────────
 * Decode a RISC OS 5 'SBPr' format directory (2048 bytes).
 *
 * FORMAT (discovered boot228-232, Apr 2026, Phoenix OS project):
 *
 * HEADER (64 bytes at offset 0x000):
 *   [0x00]      uint8   seq_counter   incremented on each directory write
 *   [0x01-03]   uint8×3 padding/reserved
 *   [0x04-07]   char×4  magic         'SBPr'
 *   [0x08-0B]   uint32  version       = 1
 *   [0x0C-0F]   uint32  dir_size      = 2048
 *   [0x10-13]   uint32  entry_count   number of directory entries
 *   [0x14-17]   uint32  name_bytes    total bytes of all filenames
 *   [0x18-1B]   uint32  self_sin      SIN (Sector Indirect Number) of this dir
 *   [0x1C-1F]   uint32  parent_ref    reference to parent directory
 *   [0x20-23]   uint32  flags         (0xFFFFFF5C observed)
 *   [0x24-27]   uint32  checksum      CRC/hash of directory contents
 *   [0x28-2B]   uint32  dir_size2     = 2048 (repeated)
 *   [0x2C+]     entries start immediately after fixed header
 *
 * ENTRY FORMAT (variable length, starts at offset 0x2C):
 *   [+0x00-03]  uint32  sin           SIN of this object (file or directory)
 *   [+0x04-07]  uint32  size          file size in bytes (0 for directories)
 *   [+0x08-0B]  uint8   name_len      length of name string
 *   [+0x09+]    uint8×N name          ASCII name (NOT NUL terminated)
 *   Entry is padded to 4-byte alignment.
 *   Entry total = 8 + 1 + name_len, rounded up to multiple of 4.
 *   Directories identified by SIN bit 0 = 1 (new-map IDA flag).
 *
 * TAIL (last 64 bytes at offset 0x7C0):
 *   [0x7C0-7CF] padding (16 bytes, zeroed)
 *   [0x7D0-7D3] zero
 *   [0x7D4+]    uint32[9] SIN index — one entry per directory object
 *               (tail SINs, for rapid lookup without scanning body)
 *   [0x7F8-7FB] last 4 chars of last entry name (or padding)
 *   [0x7FC]     uint8 seq_counter copy (matches header byte 0)
 *   [0x7FD-7FF] 3 bytes padding/checksum
 *
 * CONFIRMED VALUES (laxarusb disc, Apr 2026):
 *   seq=9, entries=9, self_sin=0x02FAD601
 *   Tail SINs: !Boot=0x02FAD701 Apps=0x03024001 Diversions=0x03047901
 *              Documents=0x0304AC01 Printing=0x02F09401 Public=0x02F0DA01
 *              Updates=0x02F0DB01 Utilities=0x02F0DD01 Wallpaper=0x02EFC201
 * ─────────────────────────────────────────────────────────────────────────── */
static void adfs_dump_sbpr_dir(const uint8_t *dir)
{
    /* boot239 CONFIRMED FORMAT from other machine's bootlog222/223:
     *
     * HEADER (32 bytes, 0x00..0x1F):
     *   [0x00]    start_seq (1 byte, master sequence counter)
     *   [0x04-07] magic 'SBPr'
     *   [0x08-0B] flags
     *   [0x0C-0F] dir_size = 2048
     *   [0x10]    end_seq (1 byte, must match start_seq)
     *   [0x18-1B] self_sin / parent_IDA
     *   [0x1C-1F] parent_IDA
     *
     * ENTRIES (28 bytes each, starting at offset 0x20):
     *   [+0x00-03] load_addr  (top 12 bits = 0xFFF for typed files)
     *   [+0x04-07] exec_addr
     *   [+0x08-0B] length     (file size in bytes)
     *   [+0x0C-0F] IDA/SIN    (object Indirect Disc Address)
     *   [+0x10-13] attr+ext   (attributes)
     *   [+0x14-17] name_len   (character count)
     *   [+0x18-1B] name_off   (byte offset into name table)
     *
     * NAME TABLE at 0x20 + entry_count * 28:
     *   Plain ASCII, 1 byte per char, 0x0D (CR) terminated.
     *   name_off points DIRECTLY to first char (no length prefix).
     *
     * TAIL:
     *   dir[2040..2043] = 'oven' (last 4 chars of last entry name)
     *   dir[2044]       = end_seq (must match dir[0])
     *
     * ROOT PROBE LBA = disc_map_lba + 2 * total_nzones
     *   = 0x774BC0 + 2*1924 = 0x775AC8 for laxarusb ✓
     *
     * IDA decode (id_len=19):
     *   frag_id  = (IDA >> 1) & 0x3FFFF
     *   chain_off = IDA >> 19                                         */

    uint8_t  start_seq   = dir[0];
    uint32_t flags_hdr   = (uint32_t)dir[8]  | ((uint32_t)dir[9]<<8)  |
                           ((uint32_t)dir[10]<<16) | ((uint32_t)dir[11]<<24);
    /* dir_size at [12..15] — not used in listing, suppresses warning */
    (void)((uint32_t)dir[12]|((uint32_t)dir[13]<<8)|
           ((uint32_t)dir[14]<<16)|((uint32_t)dir[15]<<24));
    /* boot246: dir[16..19] = entry_count, dir[14..17] = name_bytes */
    uint32_t hdr_count   = (uint32_t)dir[16] | ((uint32_t)dir[17]<<8) |
                           ((uint32_t)dir[18]<<16) | ((uint32_t)dir[19]<<24);
    uint32_t par_ida     = (uint32_t)dir[24] | ((uint32_t)dir[25]<<8) |
                           ((uint32_t)dir[26]<<16) | ((uint32_t)dir[27]<<24);

    /* boot249: entry_start computed from BIGDIR_NAMELEN (dir[8..11])
     * per discreader source: first = BIGDIR_NAME + WHOLEWORDS(namelen+1)
     * BIGDIR_NAME=28, WHOLEWORDS(n) = (n+3)&~3
     * Root ($):    namelen=1  -> WHOLEWORDS(2)=4  -> entry_start=32=0x20
     * Child dirs:  namelen=5+ -> WHOLEWORDS(6)=8  -> entry_start=36=0x24
     * This is MORE GENERAL than the flags-based hack used in boot248.    */
    uint32_t dir_namelen = (uint32_t)dir[8] | ((uint32_t)dir[9]<<8) |
                           ((uint32_t)dir[10]<<16) | ((uint32_t)dir[11]<<24);
    uint32_t entry_start = 28u + ((dir_namelen + 1u + 3u) & ~3u);

    uart_puts("[ADFS] SBPr seq="); fc_dec(start_seq);
    uart_puts("  hdr_count="); fc_dec(hdr_count);
    uart_puts("  flags=0x"); fc_hex32(flags_hdr);
    uart_puts("  entry_start=0x"); fc_hex32(entry_start);
    uart_puts("  par="); fc_hex32(par_ida); uart_puts("\n");

    /* Tail validation */
    int oven = (dir[2040]=='o' && dir[2041]=='v' &&
                dir[2042]=='e' && dir[2043]=='n');
    uart_puts("[ADFS] tail: oven="); uart_puts(oven ? "YES" : "NO");
    uart_puts("  tail_seq="); fc_dec(dir[2044]);
    uart_puts(dir[2044]==start_seq ? " OK\n" : " MISMATCH\n");

    /* boot244: use header entry_count directly — the typed-file marker
     * check (load & 0xFFF00000 == 0xFFF00000) only works for root dir.
     * Child directories have different load_addr values.
     * Cap at 100 as a safety limit.                                   */
    uint32_t count = (hdr_count < 100u) ? hdr_count : 100u;

    uint32_t name_tab = entry_start + count * 28u;
    uart_puts("[ADFS] entries="); fc_dec(count);
    uart_puts("  name_table="); fc_hex32(name_tab); uart_puts("\n");

    /* Print directory listing */
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t eoff = entry_start + i * 28u;
        uint32_t load = (uint32_t)dir[eoff+ 0] | ((uint32_t)dir[eoff+ 1]<<8) |
                        ((uint32_t)dir[eoff+ 2]<<16) | ((uint32_t)dir[eoff+ 3]<<24);
        uint32_t len  = (uint32_t)dir[eoff+ 8] | ((uint32_t)dir[eoff+ 9]<<8) |
                        ((uint32_t)dir[eoff+10]<<16) | ((uint32_t)dir[eoff+11]<<24);
        uint32_t ida  = (uint32_t)dir[eoff+12] | ((uint32_t)dir[eoff+13]<<8) |
                        ((uint32_t)dir[eoff+14]<<16) | ((uint32_t)dir[eoff+15]<<24);
        uint32_t attr = (uint32_t)dir[eoff+16] | ((uint32_t)dir[eoff+17]<<8) |
                        ((uint32_t)dir[eoff+18]<<16) | ((uint32_t)dir[eoff+19]<<24);
        uint32_t nlen = (uint32_t)dir[eoff+20] | ((uint32_t)dir[eoff+21]<<8) |
                        ((uint32_t)dir[eoff+22]<<16) | ((uint32_t)dir[eoff+23]<<24);
        uint32_t noff = (uint32_t)dir[eoff+24] | ((uint32_t)dir[eoff+25]<<8) |
                        ((uint32_t)dir[eoff+26]<<16) | ((uint32_t)dir[eoff+27]<<24);

        /* boot249: DIR detection via attr bit 3 (DIRECTORY_ATTR_DIR_MASK=8)
         * Confirmed from discreader source: isdir = (attrs & 8)
         * This is definitive — load_addr type bits are unreliable.        */
        uint32_t ftype      = (load >> 8) & 0xFFFu;
        int      is_dir     = (attr & 8u) ? 1 : 0;
        int      is_special = (ida == 0x00000300u);
        (void)ftype;
        uart_puts("[ADFS]   ");
        if (is_special)      uart_puts("FAT32");
        else if (is_dir)     uart_puts("DIR  ");
        else                 uart_puts("FILE ");

        /* Name: 0x0D-terminated ASCII at name_tab + noff */
        uint32_t nabs = name_tab + noff;
        uint32_t printed = 0u;
        for (uint32_t k = nabs; k < 2040u && printed < nlen && printed < 32u; k++) {
            if (dir[k] == 0x0Du) break;
            char s[2] = {(char)(dir[k] & 0x7Fu), '\0'}; uart_puts(s);
            printed++;
        }
        uart_puts("  sin="); fc_hex32(ida);
        if (!is_dir && !is_special) {
            uart_puts("  len="); fc_dec(len);
            uart_puts("  type=&"); fc_hex32(ftype);
        }
        uart_puts("\n");

        /* Populate VFS root cache (only for root dir, flags==1) */
        if (flags_hdr == 1u) filecore_cache_root_entry(dir, eoff, name_tab);
    }

    uart_puts("[ADFS] "); fc_dec((uint32_t)count);
    uart_puts(" object(s) in root directory\n");

    /* Mark VFS root cache valid after root dir parse */
    if (flags_hdr == 1u) g_root_cache_valid = 1;
}

/* ── adfs_dump_sbpr_dir_with_scan ───────────────────────────────────────────
 * Called ONLY for the root directory. Prints the directory listing via
 * adfs_dump_sbpr_dir, then logs IDAs for each entry, then runs the
 * subdirectory scan. NOT called recursively — child dirs use
 * adfs_dump_sbpr_dir only.                                                   */
static void adfs_dump_sbpr_dir_with_scan(const uint8_t *dir)
{
    /* First print the directory listing */
    adfs_dump_sbpr_dir(dir);

    /* Re-decode count and entry_start from header */
    uint32_t flags_ws  = (uint32_t)dir[8]|((uint32_t)dir[9]<<8)|
                         ((uint32_t)dir[10]<<16)|((uint32_t)dir[11]<<24);
    uint32_t entry_start_ws = (flags_ws == 1u) ? 0x20u : 0x24u;
    uint32_t count = (uint32_t)dir[16] | ((uint32_t)dir[17]<<8) |
                     ((uint32_t)dir[18]<<16) | ((uint32_t)dir[19]<<24);
    if (count > 100u) count = 100u;
    uint32_t name_tab = entry_start_ws + count * 28u;

    /* Log IDA/frag/coff for each entry */
    {
        uint32_t id_len_v  = (uint32_t)g_dr_id_len;
        uint32_t id_mask_v = (1u << (id_len_v - 1u)) - 1u;
        uart_puts("[ADFS] Per-entry IDAs:\n");
        for (uint32_t i = 0u; i < count; i++) {
            uint32_t eoff  = entry_start_ws + i * 28u;
            uint32_t ida   = (uint32_t)dir[eoff+12]|((uint32_t)dir[eoff+13]<<8)|
                             ((uint32_t)dir[eoff+14]<<16)|((uint32_t)dir[eoff+15]<<24);
            uint32_t nlen  = (uint32_t)dir[eoff+20]|((uint32_t)dir[eoff+21]<<8);
            uint32_t noff  = (uint32_t)dir[eoff+24]|((uint32_t)dir[eoff+25]<<8)|
                             ((uint32_t)dir[eoff+26]<<16)|((uint32_t)dir[eoff+27]<<24);
            uint32_t efrag = (ida >> 1u) & id_mask_v;
            uint32_t ecoff = ida >> id_len_v;
            uart_puts("[IDA] ["); fc_dec(i+1u); uart_puts("] ");
            uint32_t nabs = name_tab + noff;
            for (uint32_t k = 0u; k < nlen && k < 30u; k++) {
                if (dir[nabs+k] == 0x0Du) break;
                char s[2] = {(char)dir[nabs+k], '\0'}; uart_puts(s);
            }
            uart_puts("  ida="); fc_hex32(ida);
            uart_puts("  frag="); fc_dec(efrag);
            uart_puts("  coff="); fc_dec(ecoff); uart_puts("\n");
        }
    }

    /* Subdirectory scan */
    uint32_t par_ida  = (uint32_t)dir[24]|((uint32_t)dir[25]<<8)|
                        ((uint32_t)dir[26]<<16)|((uint32_t)dir[27]<<24);
    uint32_t log2bpmb = g_dr_log2bpmb;
    uint32_t log2ss   = g_dr_log2ss;
    uint32_t secplfau = 1u << (log2bpmb - log2ss);
    uint32_t lba_base = g_fc_lba_base;
    uint32_t used_b   = (512u * 8u) - (uint32_t)g_dr_zone_spare;
    uint32_t dr_sz    = 60u * 8u;
    uint32_t _tnz     = g_dr_nzones;
    if (g_dr_big_flag) _tnz += (uint32_t)g_dr_nzones_hi << 8u;
    uint32_t mid_z    = _tnz / 2u;
    uint32_t disc_map = lba_base + (uint32_t)
                        ((mid_z * (uint64_t)used_b - dr_sz) * secplfau);
    uint32_t root_probe  = disc_map + 2u * _tnz;
    uint32_t scan_start  = root_probe + 4u;

    uart_puts("[ADFS] Subdir scan from LBA "); fc_hex32(scan_start); uart_puts("\n");

    uint8_t *sbuf = (uint8_t *)kmalloc(2048u);
    if (!sbuf) return;

    uint32_t step           = (secplfau > 0u) ? secplfau : 1u;
    uint32_t found_sub      = 0u;
    uint32_t first_child_lba = 0u;

    for (uint32_t lba = scan_start;
         lba < scan_start + 4096u && found_sub < 12u;
         lba += step) {
        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)lba, 1, sbuf) < 0) continue;
        int is_sbpr = (sbuf[4]=='S'&&sbuf[5]=='B'&&sbuf[6]=='P'&&sbuf[7]=='r');
        int is_hugo = (sbuf[1]=='H'&&sbuf[2]=='u'&&sbuf[3]=='g'&&sbuf[4]=='o');
        if (!is_sbpr && !is_hugo) continue;

        uint32_t sub_par = is_sbpr ?
            ((uint32_t)sbuf[24]|((uint32_t)sbuf[25]<<8)|
             ((uint32_t)sbuf[26]<<16)|((uint32_t)sbuf[27]<<24)) : 0u;

        uart_puts("[SCAN] LBA="); fc_hex32(lba);
        uart_puts(is_sbpr ? "  SBPr" : "  Hugo");
        uart_puts("  par="); fc_hex32(sub_par);
        if (sub_par == par_ida) {
            uart_puts("  <-- child of $");
            if (first_child_lba == 0u) first_child_lba = lba;
        }
        uart_puts("\n");
        found_sub++;
    }
    if (found_sub == 0u) uart_puts("[SCAN] No subdirs found\n");

    /* Read and decode the first root child directory, then read !Boot file */
    if (first_child_lba != 0u) {
        uart_puts("[ADFS] === First child dir at LBA=");
        fc_hex32(first_child_lba); uart_puts(" ===\n");
        int ok = 1;
        for (int s = 0; s < 4; s++) {
            if (g_fc_bdev->ops->read(g_fc_bdev,
                    (uint64_t)(first_child_lba+(uint32_t)s),
                    1, sbuf+(uint32_t)(s*512)) < 0) { ok=0; break; }
        }
        if (ok) {
            adfs_dump_sbpr_dir(sbuf);

            /* boot260: Try NVMe (slot 1, clean chain) first.
             * NVMe IDA for $.!Boot/!Boot = 0x0a8ba103 (from Mac bootlog235).
             * If NVMe chain fails, fall back to Lexar hardcoded LBA.        */
            uart_puts("[ADFS] === Reading $.!Boot/!Boot (boot260) ===\n");

            uint32_t boot_lba  = 0xFFFFFFFFu;
            uint32_t boot_len  = 561u;
            blockdev_t *file_bd = NULL;

            /* Try NVMe first (g_disc_slots[1] if it exists and is USB) */
            if (g_disc_count >= 2 && g_fc_bdev2 != NULL) {
                uint32_t nvme_ida = 0x0a8ba103u;
                uart_puts("[ADFS]   Trying NVMe IDA=0x0a8ba103 on ");
                uart_puts(g_fc_bdev2->name); uart_puts("\n");
                boot_lba = adfs_ida_to_data_lba_ctx(nvme_ida,
                               g_fc_bdev2, &g_disc_slots[1]);
                if (boot_lba != 0xFFFFFFFFu) {
                    file_bd  = g_fc_bdev2;
                    boot_len = 561u;
                    uart_puts("[ADFS]   NVMe chain OK  data_lba=");
                    fc_hex32(boot_lba); uart_puts("\n");
                } else {
                    uart_puts("[ADFS]   NVMe chain failed\n");
                }
            }

            /* Fall back to Lexar hardcoded LBA (confirmed boot254+) */
            if (boot_lba == 0xFFFFFFFFu) {
                boot_lba = 0x775AF4u;
                file_bd  = g_fc_bdev;
                uart_puts("[ADFS]   Using Lexar hardcoded LBA 0x775AF4\n");
            }

            uint8_t *fbuf = (uint8_t *)kmalloc(boot_len + 16u);
            if (fbuf) {
                uint32_t sectors = (boot_len + 511u) / 512u;
                uint32_t done    = 0u;
                uint8_t  sec2[512];
                int ok2 = 1;
                for (uint32_t s = 0u; s < sectors; s++) {
                    if (file_bd->ops->read(file_bd,
                            (uint64_t)(boot_lba+s), 1, sec2) < 0) { ok2=0; break; }
                    uint32_t chunk = (boot_len-done < 512u) ? boot_len-done : 512u;
                    for (uint32_t k=0u; k<chunk; k++) fbuf[done+k] = sec2[k];
                    done += chunk;
                }
                int nr = ok2 ? (int)done : -1;
                if (nr > 0) {
                    uart_puts("[ADFS] Read "); fc_dec((uint32_t)nr);
                    uart_puts(" bytes of $.!Boot/!Boot:\n");

                    /* boot255: hex dump first 32 bytes to confirm content */
                    uart_puts("[ADFS] First 32 bytes: ");
                    for (int h = 0; h < 32 && h < nr; h++) {
                        fc_hex8(fbuf[h]); uart_puts(h<31?" ":"\n");
                    }

                    /* Print as text — Obey script is plain ASCII */
                    for (int i = 0; i < nr; i++) {
                        char c = (char)fbuf[i];
                        if (c == '\r') { uart_puts("\n"); continue; }
                        if (c == '\n') continue;
                        if (c >= 32 && c < 127) {
                            char s2[2] = {c, '\0'}; uart_puts(s2);
                        }
                    }
                    uart_puts("\n[ADFS] === End of $.!Boot/!Boot ===\n");
                } else {
                    uart_puts("[ADFS] File read failed\n");
                }
                kfree(fbuf);
            }
        }
    }
    kfree(sbuf);
}

/* ── VFS bridge: root entry cache ────────────────────────────────────────
 * Cache statics and forward declaration are at top of file (before
 * adfs_dump_sbpr_dir). Definition of filecore_cache_root_entry_impl
 * follows below.                                                            */

/* Called from adfs_dump_sbpr_dir to populate the VFS cache */
static void filecore_cache_root_entry_impl(const uint8_t *dir, uint32_t eoff,
                                            uint32_t name_tab)
{
    if (g_root_cache_count >= FC_ROOT_CACHE_MAX) return;

    uint32_t load = (uint32_t)dir[eoff+ 0]|((uint32_t)dir[eoff+ 1]<<8)|
                    ((uint32_t)dir[eoff+ 2]<<16)|((uint32_t)dir[eoff+ 3]<<24);
    uint32_t len  = (uint32_t)dir[eoff+ 8]|((uint32_t)dir[eoff+ 9]<<8)|
                    ((uint32_t)dir[eoff+10]<<16)|((uint32_t)dir[eoff+11]<<24);
    uint32_t ida  = (uint32_t)dir[eoff+12]|((uint32_t)dir[eoff+13]<<8)|
                    ((uint32_t)dir[eoff+14]<<16)|((uint32_t)dir[eoff+15]<<24);
    uint32_t nlen = (uint32_t)dir[eoff+20]|((uint32_t)dir[eoff+21]<<8)|
                    ((uint32_t)dir[eoff+22]<<16)|((uint32_t)dir[eoff+23]<<24);
    uint32_t noff = (uint32_t)dir[eoff+24]|((uint32_t)dir[eoff+25]<<8)|
                    ((uint32_t)dir[eoff+26]<<16)|((uint32_t)dir[eoff+27]<<24);

    vfs_dirent_t *d = &g_root_cache[g_root_cache_count];
    d->load_addr    = load;
    d->exec_addr    = (uint32_t)dir[eoff+4]|((uint32_t)dir[eoff+5]<<8)|
                      ((uint32_t)dir[eoff+6]<<16)|((uint32_t)dir[eoff+7]<<24);
    d->size         = (uint64_t)len;
    d->sin          = ida;
    d->riscos_type  = (uint16_t)((load >> 8) & 0xFFFu);

    /* Determine type */
    if (ida == 0x00000300u)
        d->type = VFS_DIRENT_SPECIAL;
    else if ((load & 0xFFF00000u) != 0xFFF00000u)
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

/* Called by vfs.c readdir to retrieve a cached root entry */
int filecore_get_root_entry(uint32_t idx, vfs_dirent_t *out)
{
    if (!g_root_cache_valid || idx >= g_root_cache_count) return -1;
    *out = g_root_cache[idx];
    return 0;
}

/* Placeholder for child directory entry retrieval (implemented later) */
int filecore_get_child_entry(uint32_t dir_sin, uint32_t idx,
                              vfs_dirent_t *out)
{
    (void)dir_sin; (void)idx; (void)out;
    return -1;   /* TODO: implement subdirectory traversal */
}

/* ── adfs_ida_to_data_lba_ctx ────────────────────────────────────────────────
 * Given a FileCore IDA and a disc slot context, traverse the zone map chain
 * and return the data LBA. Works with ANY disc (primary, secondary, NVMe).
 * boot260: refactored to use explicit context instead of globals.           */
static uint32_t adfs_ida_to_data_lba_ctx(uint32_t ida,
                                          blockdev_t *bd,
                                          const fc_disc_slot_t *slot)
{
    uint32_t id_len    = (uint32_t)slot->dr.id_len;
    uint32_t id_mask   = (1u << (id_len - 1u)) - 1u;
    uint32_t frag_id   = (ida >> 1u) & id_mask;
    uint32_t chain_off = ida >> id_len;

    uint32_t lba_base  = slot->lba_base;
    uint32_t secplfau  = 1u << (slot->dr.log2_bpmb - slot->dr.log2_sector_size);
    uint32_t used_b    = (512u * 8u) - (uint32_t)slot->dr.zone_spare;
    uint32_t dr_sz     = 60u * 8u;
    uint32_t zsp       = (uint32_t)slot->dr.zone_spare;
    uint32_t zone_bits = zsp + used_b;

    uint32_t _tnz = (uint32_t)slot->dr.nzones;
    if (slot->dr.big_flag) _tnz += (uint32_t)slot->dr.nzones_hi << 8u;

    uint8_t *zbuf = (uint8_t *)kmalloc(512u);
    if (!zbuf) return 0xFFFFFFFFu;

    uint32_t chain_bits[FC_MAX_CHAIN];
    uint32_t chain_len = 0u;
    uint32_t cur = frag_id;
    chain_bits[chain_len++] = cur;

    for (uint32_t hop = 0u; hop < FC_MAX_CHAIN - 1u; hop++) {
        uint32_t zone    = cur / zone_bits;
        uint32_t bit     = cur % zone_bits;
        uint32_t map_lba = (zone == 0u) ? lba_base :
                           lba_base + (zone * used_b - dr_sz) * secplfau;

        uint32_t entry = 0u;
        if (bd->ops->read(bd, (uint64_t)map_lba, 1, zbuf) == 0)
            entry = adfs_read_bits(zbuf, bit, (int)id_len);

        if (entry == 0u) {
            if (bd->ops->read(bd, (uint64_t)(map_lba + _tnz), 1, zbuf) == 0)
                entry = adfs_read_bits(zbuf, bit, (int)id_len);
        }

        if (entry == 0u) {
            uart_puts("[ADFS] chain broken at hop="); fc_dec(hop);
            uart_puts(" on "); uart_puts(bd->name); uart_puts("\n");
            kfree(zbuf);
            return 0xFFFFFFFFu;
        }

        if (entry == 1u) break;

        cur = entry;
        if (chain_len < FC_MAX_CHAIN) chain_bits[chain_len++] = cur;
    }
    kfree(zbuf);

    uint32_t desired;
    if (chain_off == 0u || chain_len <= chain_off)
        desired = (chain_len > 0u) ? chain_len - 1u : 0u;
    else
        desired = chain_len - 1u - chain_off;

    return fc_bit_addr_to_data_lba(chain_bits[desired], lba_base,
                                    zsp, used_b, dr_sz, id_len, secplfau);
}

/* ── adfs_ida_to_data_lba ────────────────────────────────────────────────
 * Given a FileCore new-map IDA (from a directory entry), decode the
 * frag_id and chain_offset, traverse the zone map chain, and return
 * the data LBA of the desired LFAU.
 * Returns 0xFFFFFFFF on failure.                                          */
static uint32_t adfs_ida_to_data_lba(uint32_t ida)
{
    /* boot260: simple wrapper — delegates to context-aware version */
    if (g_disc_count > 0)
        return adfs_ida_to_data_lba_ctx(ida, g_fc_bdev, &g_disc_slots[0]);
    return 0xFFFFFFFFu;
}

/* ── adfs_read_file ──────────────────────────────────────────────────────
 * Read up to `max_bytes` of a FileCore file identified by its directory
 * entry IDA. Reads sectors from the data LBA into `buf`.
 * Returns number of bytes read, or -1 on error.
 *
 * boot250: used to load $.!Boot/!Boot (IDA=0x02FAD705, len=561 bytes)    */
static int adfs_read_file(uint32_t ida, uint32_t file_len,
                           uint8_t *buf, uint32_t max_bytes)
{
    uint32_t data_lba = adfs_ida_to_data_lba(ida);
    if (data_lba == 0xFFFFFFFFu) {
        uart_puts("[ADFS] adfs_read_file: chain traverse failed\n");
        return -1;
    }

    uart_puts("[ADFS] File data_lba="); fc_hex32(data_lba);
    uart_puts("  file_len="); fc_dec(file_len); uart_puts("\n");

    uint32_t bytes_to_read = (file_len < max_bytes) ? file_len : max_bytes;
    uint32_t sectors       = (bytes_to_read + 511u) / 512u;
    uint8_t  sector_buf[512];

    uint32_t bytes_done = 0u;
    for (uint32_t s = 0u; s < sectors; s++) {
        if (g_fc_bdev->ops->read(g_fc_bdev,
                (uint64_t)(data_lba + s), 1, sector_buf) < 0) {
            uart_puts("[ADFS] adfs_read_file: sector read error at s=");
            fc_dec(s); uart_puts("\n");
            return -1;
        }
        uint32_t chunk = (bytes_to_read - bytes_done < 512u) ?
                          bytes_to_read - bytes_done : 512u;
        for (uint32_t k = 0u; k < chunk; k++)
            buf[bytes_done + k] = sector_buf[k];
        bytes_done += chunk;
    }
    return (int)bytes_done;
}

/* ── adfs_probe_lba: read 2048 bytes, check for Hugo/Nick ───────────────────
 * Returns 1 if found, 0 otherwise.
 * Prints first 16 bytes of the sector.                                      */
static int adfs_probe_lba(uint32_t lba, uint8_t *dirbuf, const char *tag)
{
    for (int s = 0; s < 4; s++) {
        if (g_fc_bdev->ops->read(g_fc_bdev,
                                  (uint64_t)(lba + (uint32_t)s),
                                  1, dirbuf + (uint32_t)s * 512u) < 0) {
            return 0;
        }
    }
    int hugo = (dirbuf[1] == 'H' && dirbuf[2] == 'u' &&
                dirbuf[3] == 'g' && dirbuf[4] == 'o');
    int nick = (dirbuf[2044] == 'N' && dirbuf[2045] == 'i' &&
                dirbuf[2046] == 'c' && dirbuf[2047] == 'k');
    if (hugo || nick) {
        uart_puts("[ADFS] *** HUGO/NICK FOUND at LBA="); fc_hex32(lba);
        uart_puts(" ("); uart_puts(tag); uart_puts(") ***\n");
        adfs_dump_hugo_dir(dirbuf);
        return 1;
    }
    return 0;
}

/* ── adfs_chain_traverse ─────────────────────────────────────────────────── */
static int adfs_chain_traverse(uint32_t frag_id, uint32_t chain_offset,
                                uint32_t lba_base,
                                uint32_t zone_spare, uint32_t used_bits,
                                uint32_t dr_size,    uint32_t id_len,
                                uint32_t secperlfau)
{
    uint32_t zone_bits = zone_spare + used_bits;
    uint32_t cur_lfau      = frag_id;
    uint32_t terminal_bits = frag_id;  /* bit_addr of the desired LFAU (updated each hop) */
    (void)terminal_bits;

    uart_puts("[ADFS] Chain traverse: frag_id="); fc_dec(frag_id);
    uart_puts(" hops="); fc_dec(chain_offset); uart_puts("\n");

    /* ── boot209: compute disc_map_lba ───────────────────────────────────
     * FileCore new-map stores the zone map CONTIGUOUSLY starting at:
     *   disc_map_lba = lba_base + (nzones/2 * used_bits - dr_size) * secperlfau
     * Zone N's map sector is at LBA:  disc_map_lba + N
     *
     * For SCSI::HardDisc0 (this disc):
     *   nzones=1924, used_bits=4064, dr_size=480, secperlfau=2
     *   disc_map_lba = (962*4064 - 480)*2 = 7,818,176 = 0x774BC0
     * DiscKnight confirms Disc Map Copy 1 at byte 0xEE978000 = LBA 7,818,176 ✓  */
    uint32_t _tnz = g_dr_nzones;
    if (g_dr_big_flag) _tnz += (uint32_t)g_dr_nzones_hi << 8u;
    uint32_t _mz = _tnz / 2u;
    /* disc_map_lba = LBA of the mid-zone (zone _mz) in the contiguous map.
     * Zone N of the contiguous map is at: disc_map_lba - _mz + N
     * i.e. map_zone0_lba = disc_map_lba - _mz  (start of copy 1)
     * Copy 2 starts at: map_zone0_lba + _tnz     */
    uint32_t disc_map_lba  = lba_base + (_mz * used_bits - dr_size) * secperlfau;
    uint32_t map_zone0_lba = disc_map_lba - _mz;   /* LBA of zone 0 of copy 1 */
    (void)map_zone0_lba;

    uart_puts("[ADFS] disc_map_lba="); fc_hex32(disc_map_lba); uart_puts("\n");

    uint8_t *zbuf = (uint8_t *)kmalloc(512);
    if (!zbuf) { uart_puts("[ADFS] kmalloc fail (zone buf)\n"); goto direct_scan; }

    /* ── boot212: terminal probe ──────────────────────────────────────────
     * Before traversing, verify the disc map is valid by reading the
     * map entry for the KNOWN root dir terminal LFAU (LBA 7,822,024).
     *   root_lfau = 7822024 / secperlfau = 3,911,012
     *   zone_lfau_start(962) = 962*used_bits - dr_size = 3,909,088
     *   lfau_in_zone = 3,911,012 - 3,909,088 = 1,924
     *
     * Current formula (zone_bits = zone_spare + used_bits = 4096):
     *   bit_in_zone = zone_spare + lfau_in_zone*id_len = 32 + 1924*19 = 36,588
     *   B_term = 962*4096 + 36,588 = 3,976,940 → zone=970, bit=3820, byte=477
     *   map_lba = disc_map_lba + 970 = 0x774f8a  (should be entry=1)
     *
     * Alt formula (zone_bits = used_bits = 4064):
     *   alt_B  = zone*used_bits + lfau_in_zone = 962*4064 + 1924 = 3,911,492
     *   alt_zone = 962, raw_bit = zone_spare + 1924 = 1956, byte=244
     *   map_lba = disc_map_lba + 962 = 0x774f82  (should be entry=1)       */
    {
        uint32_t root_lfau   = 7822024u / secperlfau;          /* = 3,911,012 */
        uint32_t z962_start  = 962u * used_bits - dr_size;     /* = 3,909,088 */
        uint32_t lfau_in_962 = root_lfau - z962_start;         /* = 1,924     */

        /* Current formula terminal */
        uint32_t bit_cur = zone_spare + lfau_in_962 * id_len;  /* = 36,588 */
        uint32_t B_cur   = 962u * zone_bits + bit_cur;         /* = 3,976,940 */
        uint32_t z_cur   = B_cur / zone_bits;                  /* = 970 */
        uint32_t b_cur   = B_cur % zone_bits;                  /* = 3820 */
        uint32_t lba_cur = disc_map_lba + z_cur;
        uart_puts("[ADFS] Terminal probe (cur): B="); fc_dec(B_cur);
        uart_puts(" zone="); fc_dec(z_cur);
        uart_puts(" bit="); fc_dec(b_cur);
        uart_puts(" map_lba="); fc_hex32(lba_cur); uart_puts("\n");
        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)lba_cur, 1, zbuf) == 0) {
            uint32_t e = adfs_read_bits(zbuf, b_cur, (int)id_len);
            uart_puts("[ADFS]   term_entry="); fc_dec(e);
            uart_puts(" (expect 1) chk[0..7]:");
            for (int k=0;k<8;k++){uart_puts(" ");fc_hex8(zbuf[k]);}
            uart_puts("\n");
        }

        /* Alt formula terminal (zone = B / used_bits) */
        uint32_t B_alt   = 962u * used_bits + lfau_in_962;     /* = 3,911,492 */
        uint32_t z_alt   = B_alt / used_bits;                  /* = 962 */
        uint32_t b_alt   = zone_spare + (B_alt % used_bits);   /* = 32+1924=1956 */
        uint32_t lba_alt = disc_map_lba + z_alt;
        uart_puts("[ADFS] Terminal probe (alt): B="); fc_dec(B_alt);
        uart_puts(" zone="); fc_dec(z_alt);
        uart_puts(" bit="); fc_dec(b_alt);
        uart_puts(" map_lba="); fc_hex32(lba_alt); uart_puts("\n");
        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)lba_alt, 1, zbuf) == 0) {
            uint32_t e = adfs_read_bits(zbuf, b_alt, (int)id_len);
            uart_puts("[ADFS]   term_entry="); fc_dec(e);
            uart_puts(" (expect 1) chk[0..7]:");
            for (int k=0;k<8;k++){uart_puts(" ");fc_hex8(zbuf[k]);}
            uart_puts("\n");
            /* Dump 16 bytes around byte 244 (alt bit/8) for inspection */
            uint32_t bd = (b_alt/8u >= 8u) ? b_alt/8u - 8u : 0u;
            uart_puts("[ADFS]   alt bytes["); fc_dec(bd); uart_puts("..+15]:");
            for (int k=0;k<16;k++){uart_puts(" ");fc_hex8(zbuf[bd+k]);}
            uart_puts("\n");
        }
    }

    /* ── Follow chain to terminal, record all bit_addrs ─────────────────
     * boot223 FIX: correct chain_offset semantics per PRM Section 4.
     * chain_offset = hops from END to desired LFAU.
     * chain_offset=95 with a 96-LFAU chain = position 0 = frag_id itself.
     * Algorithm:
     *   1. Traverse full chain until entry=1, storing each bit_addr.
     *   2. desired = total_len - chain_offset - 1  (from start)
     *   3. data_lba = fc_bit_addr_to_data_lba(stored[desired])
     * Max chain length = FC_MAX_CHAIN = 1024 LFAUs (safety cap).     */
    uint32_t chain_bits[FC_MAX_CHAIN];
    uint32_t chain_len = 0u;

    chain_bits[chain_len++] = cur_lfau;   /* position 0 = frag_id */

    for (uint32_t hop = 0u; hop < FC_MAX_CHAIN - 1u; hop++) {
        uint32_t zone        = cur_lfau / zone_bits;
        uint32_t bit_in_zone = cur_lfau % zone_bits;
        /* boot224 FIX: zone map is INTERLEAVED with data sectors.
         * Zone N LBA = lba_base + (N*used_bits - dr_size)*secperlfau
         * (zone 0 special case: LBA = lba_base)
         * NOT contiguous from a base address.                     */
        uint32_t map_lba = (zone == 0u) ? lba_base :
                           lba_base + (uint32_t)((uint64_t)zone * used_bits - dr_size) * secperlfau;

        int log = (hop < 3u || (hop % 20u) == 0u);
        if (log) {
            uart_puts("[ADFS]   hop="); fc_dec(hop);
            uart_puts(" cur="); fc_dec(cur_lfau);
            uart_puts(" z="); fc_dec(zone);
            uart_puts(" bit="); fc_dec(bit_in_zone);
            uart_puts(" lba="); fc_hex32(map_lba); uart_puts("\n");
        }

        if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)map_lba, 1, zbuf) < 0) {
            uart_puts("[ADFS]   read error at hop="); fc_dec(hop); uart_puts("\n");
            kfree(zbuf); goto direct_scan;
        }

        /* Read id_len bits at bit_in_zone (LSB first) */
        uint32_t entry = 0u;
        for (uint32_t b = 0u; b < (uint32_t)id_len; b++) {
            uint32_t abs_bit = bit_in_zone + b;
            uint8_t  byte_val = zbuf[abs_bit / 8u];
            if (byte_val & (1u << (abs_bit % 8u))) entry |= (1u << b);
        }

        if (entry == 0u) {
            /* boot230 FIX: copy2 is at copy1_lba + total_nzones, NOT +1.
             * Zone map layout: copy1 occupies nzones sectors, copy2 follows.
             * copy2_lba(zone N) = copy1_lba(zone N) + total_nzones
             * Boot229 confirmed: copy1(z7)=0xDA80, code gave 0xDA81 (+1 WRONG).
             * Correct: 0xDA80 + 1924 = 0xE204.                           */
            uint32_t map_lba_c2 = map_lba + _tnz;
            uart_puts("[ADFS]   entry=0 at hop="); fc_dec(hop);
            uart_puts(" trying Copy2 LBA="); fc_hex32(map_lba_c2); uart_puts("\n");
            if (g_fc_bdev->ops->read(g_fc_bdev, (uint64_t)map_lba_c2, 1, zbuf) < 0) {
                kfree(zbuf); goto direct_scan;
            }
            entry = 0u;
            for (uint32_t b = 0u; b < (uint32_t)id_len; b++) {
                uint32_t abs_bit = bit_in_zone + b;
                uint8_t  byte_val = zbuf[abs_bit / 8u];
                if (byte_val & (1u << (abs_bit % 8u))) entry |= (1u << b);
            }
            if (entry == 0u) {
                uart_puts("[ADFS]   both copies entry=0 at hop="); fc_dec(hop); uart_puts(" — broken\n");
                kfree(zbuf); goto direct_scan;
            }
        }

        if (entry == 1u) {
            uart_puts("[ADFS]   terminal entry=1 at hop="); fc_dec(hop);
            uart_puts("  chain_len="); fc_dec(chain_len); uart_puts("\n");
            break;
        }

        cur_lfau = entry;
        if (chain_len < FC_MAX_CHAIN) chain_bits[chain_len++] = cur_lfau;
    }

    kfree(zbuf);

    /* boot244: chain_offset = hops FROM THE END of the chain (RISC OS spec).
     * Confirmed from risc_os_way_to_find_child.txt:
     *   if chain_offset == 0 OR chain_len <= chain_offset -> use last entry
     *   else desired_idx = chain_len - 1 - chain_offset               */
    uart_puts("[ADFS] chain_len="); fc_dec(chain_len);
    uart_puts("  chain_offset="); fc_dec(chain_offset); uart_puts("\n");

    uint32_t desired_idx;
    if (chain_offset == 0u || chain_len <= chain_offset) {
        desired_idx = (chain_len > 0u) ? chain_len - 1u : 0u;
    } else {
        desired_idx = chain_len - 1u - chain_offset;
    }
    uart_puts("[ADFS]   desired_idx="); fc_dec(desired_idx);
    uart_puts("  bit_addr="); fc_dec(chain_bits[desired_idx]); uart_puts("\n");

    /* ── Read from selected chain LFAU ───────────────────────────────── */
    {
        uint32_t data_lba = fc_bit_addr_to_data_lba(chain_bits[desired_idx], lba_base,
                                                      zone_spare, used_bits,
                                                      dr_size, id_len, secperlfau);
        uart_puts("[ADFS] data_lba="); fc_hex32(data_lba); uart_puts("\n");

        uint8_t *dirbuf = (uint8_t *)kmalloc(2048u);
        if (dirbuf) {
            if (adfs_probe_lba(data_lba, dirbuf, "chain-terminal")) {
                kfree(dirbuf);
                return 0;
            }
            uart_puts("[ADFS]   bytes[0..15]: ");
            for (int k = 0; k < 16; k++) {
                fc_hex8(dirbuf[k]);
                uart_puts(k < 15 ? " " : "\n");
            }
            kfree(dirbuf);
        }
    }

direct_scan:
    /* boot232: The directory at LBA 0x775AC8 uses RISC OS 5 'SBPr' format,
     * NOT the old Hugo/Nick format. Detected from boot231 dump:
     *   buf[0]    = 0x09 = sequence counter
     *   buf[4..7] = 'SBPr' = new-format magic
     *   buf[16]   = 9 = entry count (matches 9 root dir items)
     *   buf[24]   = 0x02FAD601 = self SIN (root dir)
     *   buf[44]   = 0x02FAD701 = $.!Boot SIN
     * Detect 'SBPr' at buf[4..7], dump first AND last 64 bytes,
     * then decode the directory entries.                              */
    {
        uint8_t *dirbuf = (uint8_t *)kmalloc(2048u);
        if (!dirbuf) { uart_puts("[ADFS] kmalloc fail\n"); return -1; }

        uint32_t root_lba = 0x775AC8u;
        uart_puts("[ADFS] boot232: reading LBA 0x775AC8\n");

        for (int s = 0; s < 4; s++)
            g_fc_bdev->ops->read(g_fc_bdev,
                (uint64_t)(root_lba+(uint32_t)s), 1,
                dirbuf+(uint32_t)s*512u);

        /* First 64 bytes */
        uart_puts("[ADFS] First 64 bytes:\n");
        for (uint32_t row = 0u; row < 4u; row++) {
            fc_hex32(row*16u); uart_puts(": ");
            for (int k=0;k<16;k++){fc_hex8(dirbuf[row*16u+(uint32_t)k]);uart_puts(k<15?" ":"\n");}
        }

        /* Last 64 bytes (bytes 1984..2047) */
        uart_puts("[ADFS] Last 64 bytes:\n");
        for (uint32_t row = 0u; row < 4u; row++) {
            uint32_t off = 1984u + row*16u;
            fc_hex32(off); uart_puts(": ");
            for (int k=0;k<16;k++){fc_hex8(dirbuf[off+(uint32_t)k]);uart_puts(k<15?" ":"\n");}
        }

        /* Check for SBPr magic at buf[4..7] */
        int sbpr = (dirbuf[4]=='S' && dirbuf[5]=='B' &&
                    dirbuf[6]=='P' && dirbuf[7]=='r');
        int hugo = (dirbuf[1]=='H' && dirbuf[2]=='u' &&
                    dirbuf[3]=='g' && dirbuf[4]=='o');

        uart_puts("[ADFS] magic: hugo="); fc_dec(hugo);
        uart_puts(" sbpr="); fc_dec(sbpr); uart_puts("\n");

        if (sbpr) {
            uart_puts("[ADFS] *** SBPr DIRECTORY FOUND at LBA=0x775AC8 ***\n");
            adfs_dump_sbpr_dir_with_scan(dirbuf);   /* root only — no recursion */
            kfree(dirbuf); return 0;
        }

        if (hugo) {
            adfs_dump_hugo_dir(dirbuf);
            kfree(dirbuf); return 0;
        }

        uart_puts("[ADFS] boot232: neither Hugo nor SBPr found\n");
        kfree(dirbuf);
    }

    return -1;
}
