# RISC OS Phoenix

**A modern 64-bit reimplementation of RISC OS**  
Built from the ground up for 2026 hardware while preserving the classic RISC OS experience.

---

## Overview

**RISC OS Phoenix** is a clean-room 64-bit operating system that brings the legendary RISC OS desktop into the modern era.

It combines:
- The elegant, efficient, and familiar **RISC OS Wimp** user interface
- Full **64-bit AArch64** kernel with **SMP** (multi-core) support
- **Memory protection** and **preemptive multitasking** (while keeping Wimp cooperative for compatibility)
- **GPU acceleration** via Vulkan (120 FPS desktop) with OpenGL ES fallback
- Modern drivers: **NVMe**, **USB 3.2 UASP**, **Bluetooth**, **SATA AHCI**, **MMC/SD**
- Full **PhoenixNet** TCP/IP stack (IPv4 + IPv6, BSD sockets)
- **FileCore** filesystem with native RISC OS file types (&FFF, &AFF, etc.)
- Dynamic linking support (ELF64 + dlopen/dlsym)

---

## Features

- **64-bit kernel** with per-core scheduling and load balancing
- **Full memory protection** (per-task page tables, SIGSEGV, guard pages)
- **Vulkan GPU acceleration** – 4K@120Hz, hardware compositing, alpha blending
- **Multi-core NVMe** (up to 4.8 GB/s on Pi 5)
- **USB 3.2** with full **UASP** and multi-stream support
- **Bluetooth** with pairing and SPP
- **POSIX compatibility** layer (signals, timers, pipes, select/poll, sockets)
- **RISC OS file type** metadata preserved (&FFF, &AFF, etc.)
- **Context-sensitive menus** (middle mouse button)
- **Select double-click** opens files based on type (same as classic RISC OS)

---

## Hardware Support

- **Raspberry Pi 4** (VideoCore VI) – 112 FPS desktop
- **Raspberry Pi 5** (VideoCore VII) – 120 FPS desktop, 4K@120Hz
- **PCIe NVMe** (Samsung, WD, etc.)
- **USB 3.2** storage and devices
- **Bluetooth** (Classic + SPP)
- **SATA AHCI** (via PCIe)
- **MMC/SD** cards

---

## Building

```bash
git clone https://github.com/pcrepairzone-arch/phoenix2.git
cd phoenix

make clean all