# Phoenix RISC OS — Architecture Overview

**Target hardware:** Raspberry Pi 4 Model B (BCM2711 SoC, Cortex-A72, AArch64)
**Secondary target:** Raspberry Pi 5 (BCM2712, Cortex-A76) — same codebase, `BOARD=pi5`
**Build output:** `phoenix64.img` / `kernel8.img` — raw binary, loaded by VideoCore GPU bootloader
**Boot interface:** Bare-metal, no UEFI, no device tree OS hand-off (DTB parsed internally)

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
0xFC000000 – 0xFFFFFFFF   BCM2711 peripherals (MMIO)
                            UART0:    0xFE201000
                            GIC-400:  0xFF840000 (GICD), 0xFF841000 (GICC)
                            System timer: 0xFE003000
0x600000000                PCIe outbound window (CPU → PCIe)
                            VL805 BAR0: 0x600000000
```

Memory is managed by `kernel/malloc.c` (a simple first-fit heap allocator). Page tables are set up in `kernel/mmu.c`; the xHCI DMA region is mapped as Normal Non-Cacheable to guarantee coherency with the PCIe DMA engine.

---

## 3. Source Tree

```
phoenix_fixed3/
├── kernel/                 Core kernel
│   ├── boot.S              AArch64 entry point — EL2→EL1 drop, BSS clear, MMU on
│   ├── exceptions.S        EL1 vector table, synchronous/IRQ/FIQ/SError handlers
│   ├── kernel.c            kmain() — subsystem init sequence
│   ├── kernel.h            Global typedefs, panic(), uart_puts() prototypes
│   ├── mmu.c / mmu.h       Page table setup, Normal-NC DMA mapping
│   ├── malloc.c            Kernel heap allocator
│   ├── sched.c             Round-robin cooperative task scheduler
│   ├── task.c              Task create/destroy, context switch
│   ├── signal.c            POSIX-style signal delivery
│   ├── pipe.c / pipe.h     Inter-task byte-stream pipes
│   ├── select.c            Poll/select over pipes and sockets
│   ├── irq.c / irq.h       GIC-400 interrupt controller driver
│   ├── timer.c             BCM2711 system timer, delay_ms()
│   ├── pci.c / pci.h       BCM2711 PCIe RC: ATU, BAR, MSI, config space
│   ├── vfs.c / vfs.h       Virtual filesystem layer
│   ├── filecore.c          FileCore filesystem (ADFS-compatible)
│   ├── blockdriver.c       Block device abstraction (connects VFS ↔ storage)
│   ├── dl.c / elf64.h      ELF64 application loader
│   ├── devicetree.c        Minimal FDT parser (memory size, serial#)
│   ├── mmio.c              Peripheral base detection (Pi4/Pi5)
│   ├── periph_base.c       Board-specific peripheral address resolution
│   ├── spinlock.c          Spinlock primitives (ARMv8 LDXR/STXR)
│   ├── lib.c               String/mem utilities (no libc)
│   ├── led_diag.c          ACT LED blink codes for early-boot diagnostics
│   ├── errno.c / errno.h   Error codes
│   └── linker.ld           Linker script — kernel at 0x80000, sections layout
│
├── drivers/
│   ├── uart/
│   │   ├── uart.c          PL011 UART driver (GPIO 14/15, 115200 8N1)
│   │   └── uart.h
│   ├── gpu/
│   │   ├── mailbox.c       VideoCore mailbox channel 8 driver
│   │   ├── mailbox_property.c  Property tag interface (framebuffer, firmware)
│   │   ├── gpu.c           GPU/display initialisation
│   │   ├── framebuffer.c   Linear framebuffer (pixel plotting, blit)
│   │   ├── font8x8.c       8×8 bitmap font renderer
│   │   └── audio_diag.c    AudioPWM diagnostic output
│   ├── usb/
│   │   ├── vl805_init.c    VL805 PCIe power-on, firmware load via mailbox tag 0x00030058
│   │   ├── usb_xhci.c      xHCI host controller driver (VL805-specific quirks)
│   │   ├── usb_xhci.h      xHCI driver interface
│   │   ├── usb_core.c      USB device registration, class driver probe
│   │   ├── usb_mass_storage.c  USB MSC class driver (BOT)
│   │   ├── usb_storage.c   SCSI command layer (stub — awaits event ring fix)
│   │   ├── usb_hid.c       HID class driver (keyboard/mouse — partial)
│   │   └── usb.h           USB descriptor structs, class codes
│   ├── mmc/
│   │   └── mmc.c           SD/MMC controller driver (eMMC / SD card)
│   ├── nvme/
│   │   └── nvme.c          NVMe driver stub
│   ├── bluetooth/
│   │   └── bluetooth.c     Bluetooth UART stub (CYW43455 chipset)
│   └── input/
│       └── input_stub.c    Keyboard/mouse input abstraction (stub)
│
├── net/
│   ├── tcpip.c             TCP/IP stack entry point
│   ├── socket.c            BSD socket API
│   ├── ipv4.c / ipv6.c     IP layer
│   ├── tcp.c / udp.c       Transport layer
│   └── arp.c               ARP resolution
│
├── wimp/
│   ├── wimp.c              WIMP manager — desktop message pump
│   ├── window.c            Window create/move/redraw
│   ├── event.c             Event queue and dispatch
│   ├── menu.c              Pull-down menu system
│   └── filecore.c          File-open dialogue (WIMP layer)
│
├── apps/
│   ├── paint.c             !Paint equivalent (bitmap editor stub)
│   └── netsurf.c           NetSurf browser interface stub
│
├── docs/
│   ├── ARCHITECTURE.md     This file
│   └── USB_XHCI_VL805.md   VL805 xHCI driver development history & breadcrumbs
│
├── Makefile                Cross-compile build system (aarch64-linux-gnu-gcc)
├── config.txt              Raspberry Pi firmware config (kernel8.img, uart, dtb)
├── README_BOOT.md          SD card setup and serial console instructions
├── LICENSE                 Project licence
└── .gitignore
```

---

## 4. Boot Sequence

```
VideoCore GPU (before ARM cores start)
  └─ Loads VL805 USB 3.0 firmware via mailbox tag 0x00030058
  └─ Loads kernel8.img to 0x80000
  └─ Releases ARM cores

kernel/boot.S  (EL2 or EL1 entry at 0x80000)
  ├─ Drop from EL2 → EL1 if needed (HCR_EL2 setup)
  ├─ Set up stack pointer (SP_EL1)
  ├─ Clear BSS
  ├─ Call kmain()
  └─ Spin forever if kmain returns

kernel/kernel.c :: kmain()
  ├─ uart_init()            Serial console live immediately
  ├─ mmu_init()             MMU on: kernel+peripherals+DMA mapped
  ├─ irq_init()             GIC-400 GICD/GICC configured
  ├─ timer_init()           System timer calibrated
  ├─ malloc_init()          Heap ready
  ├─ devicetree_parse()     Read memory size from FDT
  ├─ gpu_init()             Framebuffer acquired via mailbox
  ├─ pci_init()             BCM2711 PCIe RC: link-up, ATU, RC_BAR2
  ├─ vl805_init()           VL805 firmware-ready, MSI configured
  ├─ usb_xhci_init()        xHCI rings, RS=1, port scan, enumerate
  │    └─ usb_enumerate_device() → USB-MSC class driver binds (boot 61+)
  ├─ mmc_init()             SD card controller
  ├─ vfs_init()             VFS layer
  ├─ filecore_init()        FileCore mounts (SD/USB when ready)
  ├─ sched_init()           Scheduler starts
  └─ wimp_start()           WIMP desktop message pump
```

---

## 5. Subsystem Status (as of boot 61)

| Subsystem | Status | Notes |
|---|---|---|
| UART serial console | Working | PL011, 115200 baud, GPIO 14/15 |
| MMU / memory | Working | Normal + Device + Normal-NC (DMA) |
| GIC-400 interrupts | Working | GICD configured; IRQ 180 (xHCI MSI) unmasked |
| System timer / delay_ms | Working | |
| PCIe RC (BCM2711) | Working | ATU, RC_BAR2 DMA window, MSI |
| VL805 firmware load | Working | Mailbox tag 0x00030058, < 5 ms |
| xHCI rings & RS=1 | Working | Force-ring pattern; TRUE RUNNING every boot |
| USB port scan | Working | PP power-cycle recovery for cold-boot CCS=0 |
| USB enumeration | Working (synthetic) | USB-MSC class driver binds every boot (boot 61+) |
| xHCI event ring | **Broken** | MCU never writes CCEs — see docs/USB_XHCI_VL805.md §7 |
| USB MSC SCSI I/O | Blocked | Needs event ring fix for real transfers |
| MMC / SD card | Partial | Driver present; VFS mount pending |
| Framebuffer / GPU | Partial | Framebuffer acquired; WIMP rendering in progress |
| WIMP desktop | Partial | Message pump, window, event, menu stubs |
| Networking (TCP/IP) | Stub | Stack skeleton present; NIC driver not started |
| USB HID | Partial | Driver skeleton; needs interrupt EP polling |
| Bluetooth | Stub | CYW43455; not started |
| NVMe | Stub | Driver skeleton only |
| FileCore VFS | Partial | Needs working block device (USB-MSC or MMC) |
| ELF64 app loader | Present | Loads AArch64 ELF apps; integration pending |

---

## 6. Key Design Decisions

**Monolithic image.** All drivers compile into `kernel8.img`. This avoids module loading complexity and is appropriate for a fixed hardware target (Pi 4/5). The ELF loader (`dl.c`) handles user-space applications only.

**No libc.** The kernel is built with `-nostdlib -ffreestanding`. String/memory utilities live in `kernel/lib.c`. There is no malloc from the C runtime — `kernel/malloc.c` provides a simple first-fit allocator over a statically-sized heap.

**Cooperative scheduler first.** `kernel/sched.c` implements a round-robin cooperative scheduler. Preemption is planned once the system timer IRQ path is fully exercised.

**Normal-NC DMA mapping.** The xHCI DMA buffer at physical `0x00010000` is mapped as Normal Non-Cacheable. This ensures the CPU sees VL805 DMA writes without explicit cache invalidation. All DMA addresses are identity-mapped: CPU physical = PCIe address via RC_BAR2 (PCIe `0x00000000` → CPU `0x0`, 1 GB).

**Mailbox as firmware interface.** The VideoCore GPU handles low-level hardware that the ARM cannot access directly (framebuffer allocation, VL805 firmware load, clock/voltage management). All GPU communication goes via the BCM mailbox in `drivers/gpu/mailbox.c`.

**Board abstraction via `PI_MODEL`.** The Makefile sets `-DPI_MODEL=4` or `-DPI_MODEL=5`. `kernel/mmio.c` and `kernel/periph_base.c` use this flag to select the correct peripheral base address and PCIe topology.

---

## 7. Build Instructions

### Prerequisites

```bash
# On Ubuntu / Debian
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

# On macOS (Homebrew)
brew install aarch64-elf-gcc
# Then edit Makefile: CC = aarch64-elf-gcc etc.
```

### Build

```bash
make              # Raspberry Pi 4 (default)
make BOARD=pi5    # Raspberry Pi 5
make clean        # Remove build artefacts
```

Output: `phoenix64.img` (also copied to `kernel8.img`).

### Deploy to SD card

1. Copy `kernel8.img` to the FAT32 boot partition of an SD card.
2. Copy `config.txt` to the same partition.
3. Copy official Pi firmware files: `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`.
4. See `README_BOOT.md` for serial console wiring (GPIO 14/15, 115200 baud).

---

## 8. Next Development Steps

In priority order:

1. **Fix xHCI event ring write-back** — run Linux on the same Pi 4, capture ERSTBA/ERDP/ERSTSZ/IMAN at USB enumeration, compare with Phoenix values. This unblocks all real USB I/O. See `docs/USB_XHCI_VL805.md §7` for the full hypothesis list.

2. **USB Mass Storage BOT protocol** — once CCEs arrive: `GET_MAX_LUN`, `TEST_UNIT_READY`, `READ_CAPACITY(10)`, `READ(10)`/`WRITE(10)` via Bulk-Only Transport.

3. **FileCore / VFS integration** — register USB-MSC (or MMC) as a block device via `kernel/blockdriver.c`; FileCore can then mount the partition and expose a RISC OS directory tree.

4. **WIMP framebuffer rendering** — connect the framebuffer to the WIMP window manager so windows actually draw pixels.

5. **USB HID** — interrupt endpoint polling via EP1-IN transfer ring once event ring works; keyboard/mouse input to WIMP.

6. **Interrupt-driven USB** — replace `xhci_wait_event()` spin-poll with IRQ handler on GIC INTID 180 (already unmasked).

7. **Networking** — NIC driver (either USB-to-Ethernet class driver or LAN9514 on Pi 4 USB hub).
