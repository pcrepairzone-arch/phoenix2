[README.md](https://github.com/user-attachments/files/26479871/README.md)
# Phoenix OS — RISC OS-inspired Bare-Metal AArch64 Kernel

A bare-metal AArch64 operating system for the Raspberry Pi 4/5,
inspired by RISC OS. No Linux, no UEFI — runs directly on hardware.

**Status: Active development. ~18,500 lines of C, Assembly and headers.**

---

## What Works

## What Works

- Full AArch64 bare-metal boot (BCM2711, Pi 4)
- Identity-mapped MMU with Normal-NC DMA regions
- GIC-400 interrupt controller
- 4-CPU scheduler skeleton
- GPU framebuffer (2560×1080 confirmed)
- PCIe RC bring-up — VL805 USB 3.0 controller via PCIe
- **VL805 xHCI driver — event ring working (Boot 143)**
  - MCU initialisation, scratchpad DMA, MFINDEX
  - No-op CCE received, Enable Slot, Address Device all working
- **DWC2 SoC USB OTG (USB-C port) — EP0 GET_DESCRIPTOR working (Boot 118)**
  - GSNPSID confirmed (0x4f54280a = Synopsys DWC2 rev 2.80a)
  - Full EP0 control transfer engine
- SD/MMC, UART serial, mailbox, device tree parser
- WIMP, networking, filesystem — scaffolded stubs

---

## Boot 118 Milestone — DWC2 USB Device Detected

After 118 boot iterations debugging the VL805 xHCI PCIe controller,
the Synopsys DWC2 OTG controller (USB-C port) successfully detected
a connected USB keyboard at High-Speed (480 Mbps):

```
[DWC2] Core reset done
[DWC2] Device detected — issuing port reset
[DWC2] Port enabled — speed: High-Speed (480 Mbps)
[DWC2]   HPRT=0x0000100f  ENA=1  CONN=1  SPD=0
[DWC2] Device ready for enumeration
```

The DWC2 controller lives at CPU physical address `0xFE980000`
(VideoCore `0x7E980000`), completely independent of the VL805.
This gives a working USB path while xHCI event ring debugging continues.

**Next step:** EP0 control transfer → GET_DESCRIPTOR → HID keyboard input.

## Boot 143 Milestone — VL805 xHCI Event Ring Working

After 143 boot iterations and months of debugging, the VL805 xHCI
event ring is fully operational. The MCU is writing events, CCEs are
arriving, and Address Device is completing successfully:

```
[BOOT116]   *** 1 event(s) received — event ring WORKING! ***
[xHCI] No-op CCE received! CC=0x00000001 — MCU DMA write-back confirmed
[xHCI] *** EVENT RING WORKING ***
[xHCI] TRB[0]: MCU WROTE TRB[0]! type=CCE
[xHCI] Address Device OK  slot=1
[boot142] MFINDEX post-portscan: t0=0x0000062a  (RUNNING)
```

This unblocks full USB-A keyboard enumeration. Next: fix EP0
GET_DESCRIPTOR to complete enumeration and get HID reports.
---

## 🙏 Huge Thanks to the Circle Project

**This breakthrough would not have happened without Circle.**

[Circle](https://github.com/rsta2/circle) is a C++ bare-metal
environment for the Raspberry Pi by R. Stange, and it is the only
other bare-metal project with a working VL805 xHCI driver for the Pi 4.

When we were completely stuck on the VL805 event ring after exhausting
every approach the xHCI specification suggested, we built Circle's
`sample/08-usbkeyboard`. The link step failed due to a toolchain
incompatibility, but **the compile step succeeded** — producing `.o`
object files containing Circle's compiled xHCI driver code.

By analysing those source files & compiled object files, we 
identified three undocumented VL805-specific requirements that are not 
in the xHCI specification and are not visible from register state alone:

1. **`CMD_INTE` must be written alone BEFORE `CMD_RS | CMD_INTE`** —
   the MCU needs to see the interrupt enable bit set before the run
   bit in order to know where to deliver events.

2. **`ERSTSZ=1` must be written BEFORE `ERSTBA`** — the MCU latches
   the segment count at `ERSTBA` write time. With `ERSTSZ=0` when
   `ERSTBA` arrives, the MCU treats the event ring as having zero
   valid segments and never writes to it.

3. **`ERDP` must be written with `EHB=1` (bit 3 set) on init** —
   Circle always ORs `0x8` into the initial ERDP write. The VL805
   MCU uses this as an undocumented "host ready" handshake before
   it will begin posting events to the ring.

None of these behaviours are documented anywhere. We would not have
found them without Circle's source code to compare against.

**R. Stange and the Circle project: thank you.** Phoenix OS stands on
your shoulders for this milestone.

Circle repository: https://github.com/rsta2/circle

## Hardware

| Item | Detail |
|------|--------|
| Board | Raspberry Pi 4B (BCM2711, boardrev c03112) |
| CPU | Cortex-A72, AArch64 |
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

### VL805 xHCI Status

The MCU initialises cleanly every boot:
- TRUE RUNNING at 5ms, zero HSE retries
- MFINDEX running (frame timer confirmed)
- Scratchpad DMA: 21/31 pages written by MCU (deterministic)
- **Event ring: silent** — MCU not writing events (under investigation)

The event ring problem is VL805-firmware-specific. All standard xHCI
requirements have been met. Community input welcome — see `docs/USB_XHCI_VL805.md`.

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

***Critical:** VL805 firmware only accepts inbound DMA with PCIe top
nibble `0xC`. The linker script must place `.xhci_dma` at `0x00010000`.

---

## VL805 xHCI — Key Lessons (Rules 38-41)

Discovered via Circle object file analysis (Boot 143):

| Rule | Finding |
|------|---------|
| 38 | Write `CMD_INTE` alone BEFORE `CMD_RS\|CMD_INTE` — separate writes required |
| 39 | Write `ERSTSZ=1` BEFORE `ERSTBA` — MCU latches segment count at ERSTBA time |
| 40 | Write `ERDP` with `EHB=1` (OR `0x8`) on init — undocumented MCU "host ready" signal |
| 41 | MFINDEX starts after first port reset, not at RS=1 — tied to PHY activity |

Full rule set (Rules 1-42) documented in `Documents/journal.txt`.

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
| `drivers/usb/usb_xhci.c` | VL805 xHCI driver (~2,750 lines) |
| `drivers/usb/usb_dwc2.c` | DWC2 OTG driver |
| `drivers/usb/usb_init.c` | USB subsystem init |
| `drivers/usb/usb_core.c` | HCD-agnostic USB API |
| `kernel/mmu.c` | Identity map + Normal-NC DMA pages |
| `kernel/pci.c` | BCM2711 PCIe RC + VL805 init |
| `Documents/journal.txt` | Full session-by-session debug journal |

---

## Related Work & Acknowledgments

- **[Circle by R. Stange](https://github.com/rsta2/circle)** —
  C++ bare-metal environment for Raspberry Pi with working USB.
  The VL805 xHCI breakthrough in Boot 143 was made possible by
  analysing Circle's compiled source & object files. An invaluable reference.

- [K2 by Simon Willcocks](https://codeberg.org/Simon_Willcocks/K2) —
  RISC OS replacement kernel; Wimp running on 4 cores on Pi 3.

- [dwelch67 bare-metal Pi](https://github.com/dwelch67/raspberrypi) —
  Extensive bare-metal Pi examples and community resource.
---

## Licence

See `LICENSE`.
