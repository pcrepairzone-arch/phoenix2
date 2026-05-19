
# Phoenix OS — RISC OS-inspired Bare-Metal AArch64 Kernel

A bare-metal AArch64 operating system for the Raspberry Pi 4,
inspired by RISC OS. No Linux, no UEFI — runs directly on hardware.

**Status: Active development. ~29,000 lines of C, Assembly and headers.**

---

## 🎉 Boot 391 Milestone — RISC OS Modules Loading from NVMe

After 391 boot iterations, Phoenix OS now discovers and registers
RISC OS ARM32 modules directly from a live RISC OS FileCore disc on
NVMe, with correct filetype-gated filtering and heap-safe name storage:

```
[Step6] Trying 'ABIMod'        type=&FFA sz=36220  → ARM32 stub registered
[Step6] Trying 'Colours'       type=&FFA sz=3512   → ARM32 stub registered
[Step6] Trying 'IconBorderFob' type=&FFA sz=8924   → ARM32 stub registered
[Step6] Trying 'LanMan98'      type=&FFA sz=110852 → ARM32 stub registered
[Step6] Total modules loaded: 4

[Module] Loaded modules:
  PhoenixResolver [OK]
  PhoenixGENET [OK]
  PhoenixDHCP [OK]
  LanMan98
  IconBorderFob
  Colours
  ABIMod
```

ARM32 modules are registered as stubs (init deferred — AArch32 EL0
mode switching required to execute them).

---

## 🎉 Boot 143 Milestone — VL805 xHCI Event Ring Working

After months of debugging, the VL805 xHCI event ring became fully
operational. The MCU writes events, CCEs arrive, and Address Device
completes successfully:

```
[xHCI] No-op CCE received! CC=0x00000001 — MCU DMA write-back confirmed
[xHCI] *** EVENT RING WORKING ***
[xHCI] Address Device OK  slot=1
```

This unblocked full USB-A keyboard and mouse enumeration.

---

## 🙏 Huge Thanks to the Circle Project

**The Boot 143 xHCI breakthrough would not have happened without Circle.**

[Circle](https://github.com/rsta2/circle) is a C++ bare-metal
environment for the Raspberry Pi by R. Stange — the only other
bare-metal project with a working VL805 xHCI driver for the Pi 4.

By analysing Circle's compiled source and object files, we identified
three undocumented VL805-specific requirements not in the xHCI spec:

1. **`CMD_INTE` must be written alone BEFORE `CMD_RS | CMD_INTE`** —
   the MCU needs to see the interrupt enable bit set before the run bit.

2. **`ERSTSZ=1` must be written BEFORE `ERSTBA`** — the MCU latches
   the segment count at `ERSTBA` write time. With `ERSTSZ=0` at that
   moment the MCU treats the event ring as having zero valid segments.

3. **`ERDP` must be written with `EHB=1` (bit 3 set) on init** —
   an undocumented MCU "host ready" handshake before it will post events.

**R. Stange and the Circle project: thank you.**
Circle repository: https://github.com/rsta2/circle

---

## 🙏 Thanks to David J Ruck

**Special thanks to David J Ruck**
([david.ruck@armclub.org.uk](mailto:david.ruck@armclub.org.uk))
for his invaluable help during FileCore driver development. David's
deep knowledge of RISC OS FileCore internals provided the critical
breakthrough on chain traversal: the insight that `chain_offset` counts
hops **from the END** of the chain, not the start. This single piece of
knowledge unblocked weeks of debugging and is documented in
`docs/SBPr_Directory_Format.pdf`.

---

## What Works

- **Full AArch64 bare-metal boot** (BCM2711, Raspberry Pi 4)
- **Identity-mapped MMU** with Normal-NC DMA regions and cache-coherent PCIe
- **GIC-400 interrupt controller** with per-CPU IRQ routing
- **4-CPU scheduler** (round-robin, idle tasks per CPU)
- **GPU framebuffer** (1920×1080, mailbox init, con_printf overlay)
- **PCIe RC bring-up** — VL805 USB 3.0 controller via BCM2711 PCIe
- **VL805 xHCI driver** — full event-ring, CCE, port scan, hub enumeration
  - USB-A keyboard (HID boot protocol) — interrupt-driven, live keystrokes
  - USB-A mouse (HID native report protocol) — interrupt-driven
  - USB MSC — Samsung 970 EVO Plus NVMe via USB-A (BOT/SCSI)
- **DWC2 SoC USB OTG** — EP0 control transfer engine (USB-C port)
- **GENET GENETv5** — 1 Gbps Ethernet, TX+RX, interrupt-driven
- **TCP/IP stack** — ARP, IPv4, ICMP, TCP client (4 slots, MSS=1460), UDP
  - TestA: single HTTP GET ✅
  - TestB: 4 parallel connections ✅
  - TestC: RST on closed port ✅
  - TestD: 8 sequential GETs, ring wrap ✅
- **DHCP client** — PhoenixDHCP module, lease + DNS server acquisition
- **DNS resolver** — PhoenixResolver module, A-record lookup
- **FileCore filesystem** — RISC OS new-map FileCore on NVMe (full-disc overlay)
  - IDA/SIN-based directory traversal, case-insensitive path resolution
  - filecore_find_path(), filecore_read_file() public API
- **RISC OS Module loader** — Step 6 disc scan
  - &FFA filetype gate from directory entry load_addr
  - 64-bit discriminator: bit 30 of init_entry (riscos64-clib scheme)
  - ARM32 stubs: ABIMod, Colours, IconBorderFob, LanMan98 registered
  - AArch64 native modules: PhoenixDHCP, PhoenixGENET, PhoenixResolver
- **SD/MMC**, UART serial, mailbox, device tree parser

---

## Hardware

| Item | Detail |
|------|--------|
| Board | Raspberry Pi 4B (BCM2711 rev C0, boardrev d03114) |
| Case | DeskPi Pro |
| CPU | Cortex-A72, AArch64 |
| RAM | 1 GB (896 MB ARM, 128 MB GPU) |
| NVMe | Samsung 970 EVO Plus (232 GB) via USB-A MSC |
| USB (external) | VL805 xHCI via PCIe — 4× USB-A ports |
| USB (OTG) | Synopsys DWC2 — USB-C port, `0xFE980000` |
| Build target | `aarch64-linux-gnu-gcc -mcpu=cortex-a72 -ffreestanding -nostdlib` |

---

## USB Architecture

The Pi 4 has **two completely independent USB subsystems:**

```
USB-A ports (×4) ──→ VL805 xHCI (PCIe, 0x600000000)  ←── usb_xhci.c
USB-C OTG port   ──→ DWC2 SoC   (MMIO, 0xFE980000)   ←── usb_dwc2.c
```

These share no hardware. DWC2 init does not affect VL805 and vice versa.

---

## DMA Layout (.xhci_dma section, phys 0x00010000)

```
PCIe address base: 0xC0010000  (DMA_OFFSET = 0xC0000000)

Offset   Name          PCIe addr
0x0000   DCBAA         0xC0010000
0x0800   CMD_RING      0xC0010800
0x0C00   EVT_RING      0xC0010C00
0x1000   MSI_PAD       0xC0011000
0x1040   ERST          0xC0011040
0x1080   SCRATCH_ARR   0xC0011080
0x2000+  SCRATCH[0-30] 0xC0012000+
```

**Critical:** VL805 firmware only accepts inbound DMA with PCIe top
nibble `0xC`. The linker script must place `.xhci_dma` at `0x00010000`.

---

## VL805 xHCI — Key Lessons (Rules 38–41)

Discovered via Circle object file analysis (Boot 143):

| Rule | Finding |
|------|---------|
| 38 | Write `CMD_INTE` alone BEFORE `CMD_RS\|CMD_INTE` — separate writes required |
| 39 | Write `ERSTSZ=1` BEFORE `ERSTBA` — MCU latches segment count at ERSTBA time |
| 40 | Write `ERDP` with `EHB=1` (OR `0x8`) on init — undocumented MCU "host ready" signal |
| 41 | MFINDEX starts after first port reset, not at RS=1 — tied to PHY activity |

---

## FileCore — Key Lessons (Rules 50–65)

| Rule | Finding |
|------|---------|
| 50 | RISC OS FileCore new-map stores directory entries in fixed 26-byte slots |
| 61 | FileCore is case-insensitive — all directory lookups must use fc_name_ieq() |
| 62 | File identity is the &FFA filetype in load_addr[19:8], not the file content |
| 63 | Never store a pointer to a stack-allocated dirent name as mod->name — kmalloc a copy |
| 64 | uart_set_quiet(1) silences Step 5/6 output — call uart_set_quiet(0) before those steps |
| 65 | ARM32 modules: bit 30 of init_entry CLEAR. AArch64: bit 30 SET. Never init ARM32 from EL1 |

Full rule set (Rules 1–65) in `docs/journal.txt`.

---

## Building

```bash
make                    # build kernel8.img
make clean && make      # clean rebuild
```

Copy to SD card boot partition alongside `start4.elf`, `fixup4.dat`,
`bcm2711-rpi-4-b.dtb`, and `config.txt`.

---

## Serial Console

GPIO 14 (TXD) → RX, GPIO 15 (RXD) → TX. Settings: 115200 8N1.

---

## Key Files

| File | Purpose |
|------|---------|
| `drivers/usb/usb_xhci.c` | VL805 xHCI driver |
| `drivers/usb/usb_dwc2.c` | DWC2 OTG driver |
| `drivers/usb/usb_init.c` | USB subsystem init |
| `drivers/usb/usb_core.c` | HCD-agnostic USB API |
| `kernel/mmu.c` | Identity map + Normal-NC DMA pages |
| `kernel/pci.c` | BCM2711 PCIe RC + VL805 init |
| `kernel/filecore.c` | RISC OS FileCore filesystem driver |
| `kernel/module.c` | RISC OS module loader (ARM32 stubs + AArch64 native) |
| `net/genet.c` | BCM2711 GENET Gigabit Ethernet driver |
| `net/tcp.c` | Phoenix TCP client stack |
| `docs/journal.txt` | Full session-by-session debug journal |

---

## Related Work & Acknowledgments

- **[Circle by R. Stange](https://github.com/rsta2/circle)** —
  C++ bare-metal environment for Raspberry Pi with working USB.
  The VL805 xHCI breakthrough in Boot 143 was made possible by
  analysing Circle's compiled source and object files. An invaluable reference.

- **[K2 by Simon Willcocks](https://codeberg.org/Simon_Willcocks/K2)** —
  RISC OS replacement kernel; Wimp running on 4 cores on Pi 3.

- **[dwelch67 bare-metal Pi](https://github.com/dwelch67/raspberrypi)** —
  Extensive bare-metal Pi examples and community resource.

---

## Licence

See `LICENSE`.
