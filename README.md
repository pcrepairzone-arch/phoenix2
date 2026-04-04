[README.md](https://github.com/user-attachments/files/26479871/README.md)
# Phoenix OS — RISC OS-inspired Bare-Metal AArch64 Kernel

A bare-metal AArch64 operating system for the Raspberry Pi 4/5,
inspired by RISC OS. No Linux, no UEFI — runs directly on hardware.

**Status: Active development. ~18,500 lines of C, Assembly and headers.**

---

## What Works

- Full AArch64 bare-metal boot (BCM2711, Pi 4)
- Identity-mapped MMU with Normal-NC DMA regions
- GIC-400 interrupt controller
- 4-CPU scheduler skeleton
- GPU framebuffer (2560×1080 confirmed)
- PCIe RC bring-up — VL805 USB 3.0 controller via PCIe
- VL805 xHCI driver: MCU initialisation, scratchpad DMA, MFINDEX running
- **DWC2 SoC USB OTG (USB-C port) — device detection working (Boot 118)**
  - GSNPSID confirmed (0x4f54280a = Synopsys DWC2 rev 2.80a)
  - Port reset and enable working — High-Speed 480 Mbps device detected
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

---

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
0x2000   SCRATCH[0]    0xC0012000
0x20000  SCRATCH[30]   0xC0030000
```

**Critical:** VL805 firmware only accepts inbound DMA with PCIe top
nibble `0xC`. The linker script must place `.xhci_dma` at `0x00010000`.

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
| `drivers/usb/usb_xhci.c` | VL805 xHCI driver (~2,700 lines) |
| `drivers/usb/usb_dwc2.c` | DWC2 OTG driver |
| `kernel/mmu.c` | Identity map + Normal-NC DMA pages |
| `kernel/pci.c` | BCM2711 PCIe RC + VL805 init |
| `Documents/journal.txt` | Full session-by-session debug journal |

---

## Related Work

- [K2 by Simon Willcocks](https://codeberg.org/Simon_Willcocks/K2) —
  another RISC OS replacement kernel; Wimp running on 4 cores on Pi 3.
- [dwelch67 bare-metal Pi](https://github.com/dwelch67/raspberrypi)

---

## Licence

See `LICENSE`.
