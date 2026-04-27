# Phoenix OS — RISC OS-inspired Bare-Metal AArch64 Kernel

A bare-metal AArch64 operating system for the Raspberry Pi 4/5,
inspired by RISC OS. No Linux, no UEFI — runs directly on hardware.

**Status: Active development — boot254 milestone. ~22,000 lines of C, Assembly and headers.**

---

## 🎉 Boot 254 Milestone — First RISC OS File Read

Phoenix OS can now read real RISC OS 5 FileCore discs and load file
content from them bare-metal. This is the complete output from boot254,
reading `$.!Boot/!Boot` — the RISC OS boot Obey script — directly from
a Lexar USB stick:

```
[ADFS] *** SBPr DIRECTORY FOUND at LBA=0x775AC8 ***
[ADFS]   DIR  !Boot      DIR  Apps       DIR  Diversions  DIR  Documents
[ADFS]   DIR  Printing   DIR  Public     DIR  Updates     DIR  Utilities
[ADFS]   DIR  Wallpaper
[ADFS] 9 object(s) in root directory
[ADFS]   FILE !Boot  sin=0x02fad705  len=561  type=&FEB
[ADFS]   FILE !Help  sin=0x02fad707  len=126  type=&FFF
[ADFS]   FILE !Run   sin=0x02fad708  len=459  type=&FEB
[ADFS]   DIR  Choices   DIR  Library   FAT32 Loader   DIR  Resources
[ADFS]   DIR  RO310Hook .. RO530Hook   DIR  Themes   DIR  Utils
[ADFS] 18 object(s) in root directory
[ADFS] === Reading $.!Boot/!Boot ===
[ADFS] Read 561 bytes of $.!Boot/!Boot:
Set Alias$BootEnd Unset Alias$BootEnd|m
ObeyIf "<Boot$Dir>"="" Then Set Alias$BootEnd Unset Alias$BootEnd
Iconsprites <Obey$Dir>.Themes.!Sprites
If "<Boot$Path>"="" Then SetMacro Boot$Path <Boot$Dir>.
If "<Boot$Dir>"="" Then Set Boot$Dir <Obey$Dir>
If "<Run$Path>" = ",%.," then Set Run$Path ,%.,<Boot$Dir>.Library.
If "<BootResources$Dir>"<>"" then Do Repeat Filer_Boot <BootResources$Dir>
BootEnd
RMEnsure UtilityModule 3.50 ObeyError No Boot application has been run
[ADFS] === End of $.!Boot/!Boot ===
```

Verified against `*cat SCSI::laxarusb.$.!Boot` on a CM4 running RISC OS 5.

---

## What Works (boot254)

- Full AArch64 bare-metal boot (BCM2711, Pi 4B)
- Identity-mapped MMU with Normal-NC DMA regions
- GIC-400 interrupt controller, ARM timer
- 4-CPU scheduler skeleton
- GPU framebuffer (1920×1080)
- PCIe RC bring-up — VL805 USB 3.0 controller via PCIe
- **VL805 xHCI driver — full USB enumeration (boot143+)**
- **USB mass storage (BOT) — Lexar USB, Samsung NVMe via RTL9210**
- **FileCore disc record read** — boot block + mid-zone cross-check
- **SBPr directory format** — root dir (9 entries) + child dirs (18 entries)
- **DIR/FILE/FAT32 type detection** — matches RISC OS `*cat` exactly
- **RISC OS file type decode** — &FEB=Obey, &FFF=Text/Dir, &DDC=WrDir
- **Multi-disc scan** — USB preferred over MMC, named disc over unnamed
- **First file read — `$.!Boot/!Boot` Obey script (561 bytes)** ✅
- VFS layer — FileCore driver registered, root cache populated
- WIMP, networking — scaffolded stubs

---

## 🙏 Acknowledgements

### David J Ruck

**Special thanks to David J Ruck** (david@ruck.me.uk) for his invaluable
help during the FileCore driver development. David's deep knowledge of
RISC OS FileCore internals provided the critical breakthrough on chain
traversal: the insight that `chain_offset` counts hops **from the END**
of the chain, not the start. This single piece of knowledge unblocked
weeks of debugging and is now documented in
`docs/SBPr_Directory_Format.pdf`.

```c
/* David J Ruck's insight — chain_offset counts from END of chain */
if (chain_offset == 0 || chain_len <= chain_offset)
    desired_idx = chain_len - 1;
else
    desired_idx = chain_len - 1 - chain_offset;
```

Phoenix OS stands on the shoulders of people like David who keep
RISC OS knowledge alive. Thank you David. 🙏

### Circle Project

**Huge thanks to R. Stange and the Circle project** for the VL805 xHCI
breakthrough at boot 143. Circle is a C++ bare-metal environment for
Raspberry Pi: https://github.com/rsta2/circle

By analysing Circle's compiled source and object files, we identified
three undocumented VL805-specific requirements not in the xHCI spec:

1. `CMD_INTE` must be written alone BEFORE `CMD_RS | CMD_INTE`
2. `ERSTSZ=1` must be written BEFORE `ERSTBA`
3. `ERDP` must be written with `EHB=1` on init (undocumented handshake)

### Others

- [K2 by Simon Willcocks](https://codeberg.org/Simon_Willcocks/K2) —
  RISC OS replacement kernel; Wimp running on 4 cores on Pi 3.
- [dwelch67 bare-metal Pi](https://github.com/dwelch67/raspberrypi) —
  Extensive bare-metal Pi examples and community resource.

---

## SBPr Directory Format

The RISC OS 5 `SBPr` new-map directory format was fully reverse-engineered
during boots 228–254. The complete specification is in
`docs/SBPr_Directory_Format.pdf`.

**Key facts:**
- Detection: `buf[4..7] == 'SBPr'` (not the classic `'Hugo'` at buf[1..4])
- Root dirs: `flags=1`, header 32 bytes, entries at offset `0x20`
- Child dirs: `flags=5`, header 36 bytes, entries at offset `0x24`
- Entry stride: 28 bytes — `load(4)+exec(4)+len(4)+IDA(4)+attr(4)+nlen(4)+noff(4)`
- Names: ASCII, `0x0D`-terminated, stored in name table after entries
- Directories: `ftype==0xFFF` or `0xDDC`; Files: all other types
- FAT32 partition ref: `IDA==0x00000300` (special case)

---

## Hardware

| Item | Detail |
|------|--------|
| Board | Raspberry Pi 4B (BCM2711, Cortex-A72) |
| USB A ports | VL805 xHCI via PCIe |
| Test disc | Lexar USB 8GB — RISC OS 5 (disc name: `laxarusb`) |
| NVMe | Samsung 970 Evo Plus 250GB via RTL9210 USB adaptor |
| SD card | 64GB — basic RISC OS 5 install (`sdcard`) |
| Build | `aarch64-linux-gnu-gcc -mcpu=cortex-a72 -ffreestanding -nostdlib` |
| Serial | GPIO 14/15, 115200 8N1 |

---

## Building

```bash
make                    # build kernel8.img
make clean && make      # clean rebuild
```

Copy `kernel8.img` to the SD card FAT32 boot partition alongside
`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, and `config.txt`.

---

## Key Files

| File | Purpose |
|------|---------|
| `kernel/filecore.c` | FileCore + ADFS driver — SBPr decoder, file read |
| `kernel/vfs.c` | VFS layer + FileCore driver registration |
| `kernel/vfs.h` | VFS types: vfs_dirent_t, vfs_filesystem_t |
| `drivers/usb/usb_xhci.c` | VL805 xHCI driver |
| `drivers/usb/usb_mass_storage.c` | USB MSC/BOT driver |
| `kernel/mmu.c` | Identity map + Normal-NC DMA pages |
| `kernel/pci.c` | BCM2711 PCIe RC + VL805 init |
| `docs/WORKFLOW.md` | Session guide — read at start of every dev session |
| `docs/SBPr_Directory_Format.pdf` | RISC OS 5 SBPr format specification |
| `docs/journal.txt` | Full session-by-session debug journal |

---

## Milestone History

| Boot | Milestone |
|------|-----------|
| 118 | DWC2 OTG USB-C EP0 working |
| 143 | VL805 xHCI event ring working — USB breakthrough |
| 163 | USB hub enumeration |
| 181 | USB mass storage (BOT) — Lexar USB reads |
| 184 | FAT32 partition listed; FileCore 0xAD partition found |
| 232 | SBPr directory format identified |
| 239 | Correct SBPr entry format decoded |
| 246 | Root dir all 9 entries correct |
| 249 | Child dir `$.!Boot` all 18 entries correct — DIR/FILE/FAT32 perfect |
| **254** | **First file read — `$.!Boot/!Boot` Obey script (561 bytes)** ✅ |

---

## Licence

See `LICENSE`.
