# Phoenix RISC OS — Architecture Overview

**Target hardware:** Raspberry Pi 4 Model B (BCM2711 SoC, Cortex-A72, AArch64) **Secondary target:** Raspberry Pi 5 (BCM2712, Cortex-A76) — same codebase, `BOARD=pi5`**Build output:** `phoenix64.img` / `kernel8.img` — raw binary, loaded by VideoCore GPU bootloader **Boot interface:** Bare-metal, no UEFI, no device tree OS hand-off (DTB parsed internally)

---

## 1. Design Philosophy

Phoenix is a bare-metal AArch64 kernel inspired by RISC OS — a single-address-space, cooperative-first OS where the kernel, drivers, and applications all run in EL1. The goal is a lightweight, responsive desktop OS for embedded hardware with a WIMP (Windows, Icons, Menus, Pointer) interface, FileCore-compatible filesystem, and full USB/network support.

The codebase is written in C with minimal AArch64 assembler for boot, exceptions, and memory barriers. There is no dynamic linking at kernel level; all drivers are compiled into the monolithic image. A simple ELF64 loader (`dl.c`) supports loading user applications.

---

## 2. Memory Map

```
0x00000000 – 0x0000FFFF   Reserved / MMIO trap region
0x00010000 – 0x0001FFFF   xHCI DMA buffer (Normal-NC, uncached)
                            +0x0000  DCBAA
                            +0x0800  Command Ring (64 TRBs)
                            +0x0C00  Event Ring  (64 TRBs)
                            +0x1040  ERST table
                            +0x1000  MSI landing pad (PCIe 0x00011000)
0x00080000                 Kernel load address (kernel8.img entry)
0x00100000 +               Kernel heap (kernel_malloc)
                           Task stacks, VFS buffers, driver buffers
```
