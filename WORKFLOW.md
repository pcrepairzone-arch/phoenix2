# Phoenix OS — Development Workflow & Session Guide

> **Read this at the start of every session.**\
> It tells Claude exactly what this project is, what the current state is, what tools to use, and how we work together.

---

## 0. Project Vision & Aims

Phoenix is a **native 64-bit OS** for Raspberry Pi, inspired by RISC OS but built clean for AArch64. Not a port, not a distro — a bare-metal OS with a WIMP desktop, FileCore filesystem compatibility, and modern hardware support.

**Goal:** What would RISC OS look like if designed for 64-bit ARM from the start?

### Long-term aims

| Horizon | Goal | Status |
|---------|------|--------|
| **Now** | FileCore root + child dirs + file read | ✅ boot254 |
| **Near** | Walk full directory tree, load !Boot sequence | In progress |
| **Near** | RTL9210 NVMe USB adaptor | Bug open |
| **Medium** | WIMP layer, network stack, app loader | Scaffolding |
| **Far** | Pi 5 port, RISC OS 5 app compat layer | Planned |
| **Aspirational** | The go-to 64-bit RISC OS desktop for Pi | The dream |

---

## 1. Project Identity

| Item | Value |
|------|-------|
| **Project** | Phoenix OS |
| **Hardware** | Raspberry Pi 4B (BCM2711, Cortex-A72) |
| **Build** | `aarch64-linux-gnu-gcc -mcpu=cortex-a72 -ffreestanding -nostdlib` |
| **Build machine** | CM5 running Pi OS, cross-compile over RDP |
| **Reference machine** | CM4 running RISC OS 5 — for *cat, DiscKnight, PartMgr |
| **Work folder** | `C:\Users\rob\phoenix_fixed3` |
| **GitHub** | https://github.com/pcrepairzone-arch/phoenix2 |
| **Serial** | GPIO 14/15, 115200 8N1 |

---

## 2. Current Status (boot254, April 2026)

### What works
- Full AArch64 boot, MMU, GIC-400, ARM timer
- GPU framebuffer 1920×1080
- PCIe + VL805 xHCI — USB hub, mass storage (BOT)
- Samsung NVMe via RTL9210 USB adaptor (usb0, 233GB) ✅
- FileCore disc record read — boot block + mid-zone cross-check ✅
- SBPr root directory — 9 entries correct ✅
- SBPr child directory `$.!Boot` — 18 entries correct ✅
- DIR/FILE/FAT32 type detection — matches RISC OS *cat exactly ✅
- Multi-disc scan — USB > MMC priority, named > unnamed ✅
- **First file read — `$.!Boot/!Boot` Obey script (561 bytes)** ✅
- VFS integration — FileCore registered, root cache populated ✅

### Active work
- `kernel/filecore.c` — FileCore driver
- Next: fix chain traverse on Lexar (broken zone map), use NVMe chain
- Next: read `$.!Boot/!Run` and `$.!Boot/!Boot` on NVMe
- Next: Obey script parser (minimal subset)

---

## 3. Hardware Setup

```
Raspberry Pi 4B (test rig)
  USB hub (4-port, VID 0x2109)
    usb0: Samsung 970 Evo Plus 250GB NVMe via RTL9210 (233GB, disc='NVMe')
    usb1: Lexar USB stick 8GB (disc='laxarusb')  ← primary for FileCore tests
  mmc:  64GB SD card (disc='sdcard')  ← same basic RISC OS install

CM4 reference machine:
  Internal eMMC: CM4EMMc
  USB: Lexar USB = SCSI::laxarusb (for *cat, DiscKnight verification)

CM5 build machine:
  Cross-compile (aarch64-linux-gnu-gcc) over RDP
```

---

## 4. FileCore Key Parameters (Lexar USB)

| Parameter | Value |
|-----------|-------|
| `lba_base` | 0 |
| `log2bpmb` | 10 (bpmb=1024) |
| `secperlfau` | 2 |
| `id_len` | 19 |
| `nzones` | 1924 |
| `disc_map_lba` | 0x774BC0 |
| Root dir LBA | 0x775AC8 |
| Root dir format | SBPr (RISC OS 5, flags=1) |

```
Zone N LBA  = (N × used_bits - dr_size) × secperlfau
Copy 2 LBA  = copy1_lba + total_nzones
root_probe  = disc_map_lba + 2 × total_nzones
chain_offset = hops FROM END: desired_idx = chain_len - 1 - chain_offset
```

---

## 5. SBPr Directory Format (RISC OS 5)

Full spec: `docs/SBPr_Directory_Format.pdf`

| | Root (flags=1) | Child (flags=5) |
|-|----------------|-----------------|
| Header | 32 bytes | 36 bytes |
| Entries start | 0x20 | 0x24 |
| Name table | 0x20 + count×28 | 0x24 + count×28 |

Entry stride: 28 bytes. Names: ASCII, 0x0D-terminated.
DIR detection: `ftype==0xFFF` or `ftype==0xDDC`.
FAT32 ref: `IDA==0x00000300`.

---

## 6. Multi-Disc Priority

```
USB named disc  > MMC/SD named disc
First USB named > subsequent USB named
Unnamed device  = never set as primary
Global g_dr_*   = only set for first valid device (is_first_device guard)
```

---

## 7. How We Work Together

1. Rob uploads `bootlogNNN.txt` (and optionally source files)
2. Claude reads via Desktop Commander from `C:\Users\rob\phoenix_fixed3\`
3. Filter noise: `grep -v "xHCI\|MESS:\|bulk_xfer"`
4. Analysis → fix → edit `kernel/filecore.c` directly
5. Rob builds on CM5, runs on Pi 4B, sends next bootlog

**Git:** Always use `git commit -F commit_msg.txt` (avoid PowerShell quoting issues with `-m`)

---

## 8. Milestone History

| Boot | Milestone |
|------|-----------|
| 143 | VL805 xHCI working |
| 181 | USB mass storage (BOT) |
| 232 | SBPr directory format identified |
| 239 | Correct entry format decoded |
| 246 | Root dir 9 entries correct |
| 249 | Child dir 18 entries correct, DIR/FILE/FAT32 perfect |
| **254** | **First file read — `$.!Boot/!Boot` 561 bytes** ✅ |

---

## 9. Next Steps

1. Fix chain traverse — use NVMe (clean chain) for file reads
2. Read `$.!Boot/!Run` (459 bytes, Obey script)
3. Minimal Obey parser — process `Set`, `If`, `RMEnsure` commands
4. Walk full directory tree using SINs
5. Fix RTL9210 BOT stall (separate issue)

---

## 10. Important Gotchas

- `disc_map_lba` = mid-zone LBA, NOT zone 0
- Zone map is **interleaved** — Zone N LBA = `(N×used_bits-dr_size)×secperlfau`
- `chain_offset` = hops **FROM END** (David J Ruck's insight)
- SBPr magic at `buf[4..7]` NOT `buf[1..4]` (that's Hugo)
- Root header 32 bytes (flags=1), child header 36 bytes (flags=5)
- Entry count from `dir[16..19]` — NOT typed-file scan
- Names are **0x0D-terminated** ASCII (carriage return, not NUL)
- `&FFF` = directory in SBPr context; `&DDC` = writable directory
- Global `g_dr_*` params only set for first valid device — guards against sdcard overwriting laxarusb
- Serial terminal overflows at ~2048 bytes — keep dumps to first+last 64 bytes
- Git commits: always use `-F commit_msg.txt`, never inline `-m` with special chars

---

## 11. Acknowledgements

**Special thanks to David J Ruck** (david.ruck@armclub.org.uk) for his invaluable
help on FileCore chain traversal — specifically that `chain_offset` counts hops
**from the END** of the chain. This was the key breakthrough that unblocked
the correct directory location algorithm.

**Thanks to R. Stange / Circle project** for the VL805 xHCI breakthrough
at boot 143. https://github.com/rsta2/circle

---

*Last updated: April 2026 — Boot 254*
