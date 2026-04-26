# Phoenix OS — Development Workflow & Session Guide

> **Read this at the start of every session.**\
> It tells Claude exactly what this project is, what the current state is, what tools to use, and how we work together.

---

## 0. Project Vision & Aims

### What Phoenix OS is

Phoenix is a **native 64-bit operating system** for the Raspberry Pi family, inspired by RISC OS but built from the ground up for the AArch64 era. It is not a port of RISC OS, not a Linux distribution, and not an emulator. It is a clean-sheet bare-metal OS that takes the best ideas from RISC OS — simplicity, responsiveness, a single-address-space cooperative model, the WIMP desktop, FileCore filesystems — and rebuilds them properly for 64-bit ARM hardware.

### Why this matters

RISC OS is one of the most elegant operating systems ever made. It boots in seconds, has a beautiful and efficient WIMP desktop, runs deterministically, and the entire system is comprehensible to one person. But it is frozen in the 32-bit era. Phoenix answers: **what would RISC OS look like if it had been designed for 64-bit ARM from the start?**

### Core design principles

1. **64-bit native** — AArch64 throughout. No 32-bit legacy.
2. **Single-address-space, cooperative-first** — kernel and apps share one address space, run in EL1.
3. **FileCore compatible** — on-disc format is RISC OS FileCore (new-map, ADFS E+/G). Both Hugo/Nick and SBPr formats supported.
4. **WIMP desktop** — Pi 4B boots to a working WIMP desktop in under 5 seconds.
5. **Modern hardware** — USB 3.0 (VL805 xHCI), PCIe NVMe, HDMI, Ethernet, SD/eMMC.
6. **Comprehensible codebase** — one person should understand the whole system.
7. **Open format documentation** — every discovered format documented in `docs/`.

### Long-term aims

| Horizon | Goal | Status |
|---------|------|--------|
| **Now** | FileCore root + child directory listing | ✅ boot249 |
| **Near** | Walk full directory tree, read files, load !Boot | In progress |
| **Near** | RTL9210 NVMe USB adaptor working | Bug open |
| **Medium** | WIMP layer — window manager, sprite renderer, pointer | Scaffolding exists |
| **Medium** | Network stack — Ethernet, TCP/IP | Stub exists |
| **Medium** | App loader — ELF64, module system | Partial |
| **Far** | Full Pi 5 port (BCM2712, Cortex-A76) | Planned |
| **Aspirational** | The go-to RISC OS-style 64-bit desktop for Pi | The dream |

---

## 1. Project Identity

| Item | Value |
|------|-------|
| **Project name** | Phoenix OS |
| **What it is** | Bare-metal AArch64 RISC OS-inspired kernel for Raspberry Pi 4B |
| **Hardware** | Raspberry Pi 4B (BCM2711, Cortex-A72) |
| **Language** | C + AArch64 assembler |
| **Build** | `aarch64-linux-gnu-gcc -mcpu=cortex-a72 -ffreestanding -nostdlib` |
| **Build machine** | CM5 running Pi OS, cross-compile over RDP |
| **Reference machine** | CM4 running RISC OS 5 — for *cat, DiscKnight, PartMgr |
| **Output** | `kernel8.img` loaded by VideoCore GPU bootloader |
| **Serial debug** | GPIO 14/15, 115200 8N1 |
| **Work folder** | `C:\Users\rob\phoenix_fixed3` |
| **GitHub** | https://github.com/pcrepairzone-arch/phoenix2 |

---

## 2. Current Status (boot249, April 2026)

### What works
- Full AArch64 boot, MMU, GIC-400, ARM timer, 4-CPU scheduler skeleton
- GPU framebuffer 1920×1080, device tree parser
- PCIe + VL805 xHCI — USB hub enumeration, Address Device
- USB mass storage (BOT) — Lexar USB stick fully working
- FAT32 partition listing from USB boot partition
- **FileCore disc record read** — boot block + mid-zone cross-check ✅
- **FileCore SBPr root directory** — all 9 entries correct ✅
- **FileCore SBPr child directory** — $.!Boot all 18 entries correct ✅
- **DIR/FILE/FAT32 type detection** — matches RISC OS *cat exactly ✅
- **RISC OS file type decode** — &FEB=Obey, &FFF=Text/Dir, &DDC=WrDir ✅
- **Subdirectory scan** — finds 12 subdirectories from root_probe+4 ✅
- **VFS integration** — FileCore registered, root cache populated ✅
- **Committed to GitHub** — boot248 tag, commit 15da124 ✅

### Active work area
- `kernel/filecore.c` — FileCore/ADFS filesystem driver
- Next: read file data (chain traverse to get data LBA, read sectors)
- Next: load `$.!Boot/!Boot` Obey script (first file read milestone)
- Next: RTL9210 USB-NVMe adaptor (usb1) — BOT stall open

### Known open bugs
- RTL9210 (usb1) READ CAPACITY times out
- Chain traversal entries=0 at hops 87/125 — root dir uses direct_scan fallback

---

## 3. Hardware Setup

```
Raspberry Pi 4B (test rig)
  USB hub (VID 0x2109 / PID 0x3431, 4-port)
    Port 2: Lexar USB stick (usb0) — 8GB — disc name 'laxarusb'
             Partition 1: FAT32  LBA 0x28,    102400 sectors (~50MB)  [BOOT]
             Partition 4: 0xAD   LBA 0x19028, 15531984 sectors (~7.4GB) [RISC OS]
    Port 3: RTL9210 + Samsung 970 Evo Plus NVMe (usb1) — currently broken
  SD card: 64GB — disc name 'sdcard' — same basic RISC OS install as USB

CM4 reference machine:
  Internal eMMC: CM4EMMc (SDFS)
  USB: Lexar USB = SCSI::laxarusb (for DiscKnight / *cat reference)

CM5 build machine:
  Cross-compile (aarch64-linux-gnu-gcc) over RDP to CM4 RISC OS
```

---

## 4. FileCore Key Parameters (Lexar USB)

| Parameter | Value |
|-----------|-------|
| `lba_base` | 0 |
| `log2ss` | 9 (512 bytes) |
| `log2bpmb` | 10 (bpmb=1024) |
| `secperlfau` | 2 |
| `id_len` | 19 |
| `zone_spare` | 32 bits |
| `nzones` | 1924 (big_flag=1) |
| `used_bits` | 4064 |
| `dr_size` | 480 bits |
| `mid_zone` | 962 |
| `disc_map_lba` | 0x774BC0 |
| `root_dir IDA` | 0x02FAD601 |
| Root dir LBA | 0x775AC8 |
| Root dir format | **SBPr** (RISC OS 5, flags=1) |

### Key formulas
```
Zone N map LBA  = (N × used_bits - dr_size) × secperlfau
Copy 2 of zone  = copy1_lba + total_nzones
disc_map_lba    = zone(mid_zone) LBA
root_probe_lba  = disc_map_lba + 2 × total_nzones = 0x775AC8
IDA frag_id     = (ida >> 1) & ((1<<(id_len-1))-1)
chain_offset    = hops FROM END of chain (not from start!)
desired_idx     = chain_len - 1 - chain_offset
```

---

## 5. SBPr Directory Format (RISC OS 5)

See `docs/SBPr_Directory_Format.pdf` for full spec.

**Detection:** `buf[4..7] == 'SBPr'`

**Header layout:**

| Field | Root (flags=1) | Child (flags=5) |
|-------|----------------|-----------------|
| Header size | 32 bytes | 36 bytes |
| Entries start | 0x20 | 0x24 |
| Name table | 0x20 + count×28 | 0x24 + count×28 |
| Entry count | dir[16..19] | dir[16..19] |

**Entry format (28 bytes):**
```
[+00] load_addr (4)   bits[31:20]=0xFFF = typed file
[+04] exec_addr (4)   timestamp
[+08] length (4)      file size
[+0C] IDA/SIN (4)     object address
[+10] attr (4)        attributes
[+14] name_len (4)    name length
[+18] name_off (4)    offset into name table
```

**Name table:** ASCII, 0x0D-terminated. `name_off` = byte offset from table start.

**DIR/FILE detection:**
```c
ftype = (load_addr >> 8) & 0xFFF
is_dir = (ftype == 0xFFF || ftype == 0xDDC)   // &FFF=dir, &DDC=writable dir
is_file = all other types (&FEB=Obey, &FFF with small len = Text, &FF8=Module)
is_fat32 = (IDA == 0x00000300)                  // FAT32 partition reference
```

**RISC OS file types:**
- `&FEB` = Obey script (text commands, runs on boot/double-click)
- `&FFF` = Text file (also used as directory type in SBPr entries)
- `&FF8` = Absolute/module binary
- `&DDC` = Writeable directory (D/WR in *cat)

---

## 6. Key Source Files

| File | Purpose | Status |
|------|---------|--------|
| `kernel/filecore.c` | FileCore + ADFS driver | Active — boot249 |
| `kernel/vfs.c` | VFS layer + FileCore driver | Active |
| `kernel/vfs.h` | VFS types and API | Updated boot247 |
| `drivers/usb/usb_xhci.c` | VL805 xHCI driver | Working |
| `drivers/usb/usb_mass_storage.c` | USB MSC/BOT | Working for usb0 |
| `kernel/mmu.c` | MMU setup | Working |
| `kernel/kernel.c` | Main entry + init | Working |
| `docs/journal.txt` | Full debug history | Reference |
| `docs/SBPr_Directory_Format.pdf` | SBPr format spec | Complete |
| `docs/WORKFLOW.md` | This file | Updated boot249 |

---

## 7. How We Work Together

### Session startup
1. Rob uploads latest `bootlogNNN.txt` and/or source files
2. Claude reads via Desktop Commander from `C:\Users\rob\phoenix_fixed3\`
3. Filter noise: `grep -v "xHCI\|MESS:\|bulk_xfer"`
4. Analysis → fix → edit `kernel/filecore.c` as needed
5. Rob builds on CM5 and runs on Pi 4B

### Build cycle
```
CM5 Linux: make → kernel8.img → copy to SD card → boot Pi 4B
Boot number increments each build in uart_puts("[FileCore] Scanning block devices (bootNNN)...")
```

### Git workflow
```bash
git add kernel/filecore.c kernel/vfs.c kernel/vfs.h kernel/kernel.c [other changed files]
git commit -F commit_msg.txt   # always use -F to avoid PowerShell quoting issues
git push
```

---

## 8. Debugging Patterns

### Serial overflow
Print only first + last 64 bytes of 2048-byte dirs (4+4 rows of 16 bytes each).

### Finding an LBA from DiscKnight
Byte address from DK ÷ 512 = LBA. DiscKnight uses absolute byte addresses from disc start.

### Two disc records
1. Boot block: LBA 6, offset 0x1C0 — PRIMARY, always updated first
2. Zone mid-point: LBA 0x774BC0 — SECONDARY, may be stale
Always cross-check both.

### Chain traversal broken
- Copy 2 LBA = `copy1_lba + total_nzones` (NOT +1)
- `chain_offset` = hops FROM END: `desired_idx = chain_len - 1 - chain_offset`
- If both copies = entry=0: chain genuinely broken, use direct scan fallback

---

## 9. Milestone History

| Boot | Milestone |
|------|-----------|
| 143 | VL805 xHCI working |
| 181 | USB mass storage (BOT) working |
| 184 | FAT32 partition listed; FileCore 0xAD partition found |
| 221 | Zone map interleaved formula fixed |
| 230 | Copy2 formula fix (+total_nzones not +1) |
| 232 | SBPr directory format identified at LBA 0x775AC8 |
| 239 | Correct entry format: entries at 0x20, stride 28, 0x0D names |
| 241 | Per-entry IDA decode, subdir scan working |
| **246** | **Root dir: all 9 entries correct (names + SINs)** ✅ |
| **249** | **Child dir $.!Boot: all 18 entries correct** ✅ |
| **249** | **DIR/FILE/FAT32 type detection perfect** ✅ |
| **249** | **VFS integration + GitHub commit 15da124** ✅ |

---

## 10. Next Steps (after boot249)

1. **File read** — implement chain traverse to get data LBA, read file sectors
2. **Load `$.!Boot/!Boot`** — first actual file read (561-byte Obey script)
3. **Obey script parser** — minimal implementation to process boot commands
4. **Walk full tree** — recursive directory listing using SINs
5. **RTL9210 fix** — resolve BOT stall on usb1
6. **WIMP bootstrap** — wire VFS into desktop launch sequence

---

## 11. Important Gotchas

- **`disc_map_lba` is the MID-ZONE LBA**, not zone 0.
- **Zone map is interleaved** with data. Zone N LBA = `(N × used_bits - dr_size) × secperlfau`
- **`chain_offset` is hops FROM THE END**, not from the start.
- **SBPr ≠ Hugo**. RISC OS 5 Pi discs use SBPr. Check `buf[4..7]` not `buf[1..4]`.
- **Root dir header = 32 bytes** (flags=1), **child dir header = 36 bytes** (flags=5).
- **Entry count from `dir[16..19]`** — NOT a seq check, NOT the typed-file scan.
- **Names are 0x0D-terminated ASCII** (carriage return, not NUL).
- **`&FFF` = directory** in SBPr context (despite being &FFF = Text type elsewhere).
- **`&DDC` = writable directory** (D/WR in *cat = `Wallpaper`).
- **Serial terminal overflows** at ~2048 bytes — keep dumps to first+last 64 bytes.
- **Disc gets modified on CM4** — always run DiscKnight immediately before Phoenix.
- **Git commits** — always use `git commit -F commit_msg.txt`, never inline `-m` with special chars.

---

*Last updated: April 2026 — Boot 249*\
*File: `docs/WORKFLOW.md`*

---

## Acknowledgements

**Special thanks to David J Ruck** for his invaluable help during the FileCore
development. David's insight into the RISC OS FileCore zone map chain traversal
algorithm — specifically the critical detail that `chain_offset` counts hops
**from the END** of the chain, not the start — was the key that unlocked the
correct `desired_idx` calculation and unblocked weeks of debugging.

His knowledge of RISC OS internals and willingness to share it made the
difference between guessing and knowing. Phoenix OS stands on the shoulders
of people like David who keep RISC OS knowledge alive.

Thank you David. 🙏
