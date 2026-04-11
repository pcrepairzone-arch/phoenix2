# Milestone: USB Mass Storage BOT Driver — End-to-End Working
## Phoenix RISC OS / Raspberry Pi 4B (DeskPi Pro)
### Achieved: boot163 — April 2026

---

## Summary

The USB Bulk-Only Transport (BOT) mass storage driver is fully working on bare-metal
RISC OS Phoenix. A 500 GB M.2 SSD connected via the DeskPi Pro's internal ASMedia
ASM1153E USB 3.0–SATA bridge is successfully enumerated, issues a SCSI READ
CAPACITY(10) command, receives the real sector count and block size, and registers the
drive with the kernel block device layer as `usb0`.

A second 60 GB USB storage device enumerating simultaneously on hub port 2 registers
as `usb1` with identical success.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Host | Raspberry Pi 4B inside DeskPi Pro enclosure |
| USB Host Controller | VIA Labs VL805 (PCIe, 4× USB-A) |
| Hub | VIA Labs VL812 USB 3.0 hub (behind VL805 port 1) |
| Storage (port 1) | ASMedia ASM1153E USB–SATA bridge, VID=0x174c PID=0x55aa |
| Drive | 500 GB M.2 SSD (internal to DeskPi Pro) |
| Storage (port 2) | VID=0x38ad PID=0x5678, 60 GB |
| HID (port 3) | VID=0x04ca PID=0x004b USB keyboard |

---

## What Works (boot163)

```
[xHCI] SET_CONFIGURATION(0x00000001) rc=0x00000000
[xHCI] ConfigureEP: OK
[xHCI] bulk_xfer: slot=2 dci=4 dir=OUT len=31   ← CBW sent
[xHCI] event type=0x20 cc=0x00000001             ← ACK from device
[xHCI] bulk_xfer: slot=2 dci=3 dir=IN  len=8    ← READ CAPACITY data
[xHCI] event type=0x20 cc=0x00000001
[xHCI] bulk_xfer: slot=2 dci=3 dir=IN  len=13   ← CSW received
[xHCI] event type=0x20 cc=0x00000001
[MSC] LUN 0: capacity=0x000000003a386030 sectors, block_size=0x00000200 bytes
BlockDriver: Registered usb0 (unit 0, 976773168 blocks)
[MSC] USB mass storage registered as 'usb0'
```

`0x3a386030 × 512 = 500,108,118,016 bytes ≈ 500 GB` — the real capacity from the drive.

---

## Code Changes

### `drivers/usb/usb.h`
- Added `uint8_t slot_id` to `usb_endpoint_t` so `xhci_bulk_transfer` can find the
  xHCI slot from just an endpoint pointer, without needing the parent `usb_device_t`.

> **Build note:** This struct change requires `make clean && make` since the project
> Makefile has no header dependency tracking (no `-MMD`). Incremental builds after
> touching any header leave stale `.o` files with the old layout.

### `drivers/usb/usb_xhci.c`

#### `xhci_configure_endpoints()` (new — boot159)
Issues the xHCI **Configure Endpoint** command (TRB type 12) for all bulk endpoints
found on a device. Must be called after Address Device and before any bulk transfers.
Tells the VL805 MCU about the per-slot bulk transfer rings.

Key details:
- Scans all interfaces for bulk endpoints (bmAttributes & 0x03 == 0x02)
- Allocates per-direction bulk rings in the xhci_dma_buf DMA region:
  - Bulk OUT ring: `DMA_BULK_BASE_OFF + (slot−1)×BULK_STRIDE + 0x000`
  - Bulk IN ring:  `DMA_BULK_BASE_OFF + (slot−1)×BULK_STRIDE + 0x400`
  - DMA bounce buf: `DMA_BULK_BASE_OFF + (slot−1)×BULK_STRIDE + 0x800` (6144 B)
- Sets `ep->slot_id = slot_id` on each endpoint for lookup in `xhci_bulk_transfer`
- **EP context DW layout (critical):** EP_Type and CErr go in **DW1** not DW0:
  ```c
  ep_ctx[0] = 0;                                  // Interval=0, Mult=0 (bulk)
  ep_ctx[1] = (mps<<16) | (ep_type<<3) | (3<<1); // MPS | EP_Type | CErr=3
  ep_ctx[2] = (uint32_t)ring_dma | 1u;            // TR Dequeue Ptr + RCS
  ep_ctx[3] = (uint32_t)(ring_dma >> 32);
  ep_ctx[4] = mps;                                // Average TRB Length
  ```
  (Confirmed correct by comparison with EP0 context in `cmd_address_device`, which
  uses `ep0_ctx[1] = (3U<<1)|(4U<<3)|(mps<<16)` and works.)

#### `xhci_bulk_transfer()` (full implementation — boot159)
Replaces the stub that returned -1 immediately.
- Copies OUT data to / clears IN data in the DMA bounce buffer
- Writes a Normal TRB (type 1) to the per-slot bulk ring
- Rings doorbell: `db[slot_id] = dci`
- Polls event ring for Transfer Event (type 0x20), returns 0 on CC=1 or CC=13

DMA bounce buffer: all bulk data passes through `slot_bulk_data(slot_id)` which lives
inside the xhci_dma_buf DMA window visible to the VL805 over PCIe.

#### `SET_CONFIGURATION` (new — boot162)
After reading the config descriptor, a `SET_CONFIGURATION(bConfigurationValue)` control
transfer is issued over EP0 before `xhci_configure_endpoints()`. This moves the device
from "Address" state to "Configured" state, enabling all non-zero endpoints. Without
this, all bulk transfers return CC=4 (USB Transaction Error) because the device NAKs
every packet — bulk endpoints are inactive until configured.

### `drivers/usb/usb_mass_storage.c` (complete rewrite)

- **API fix:** `#include "blockdev.h"` → `#include "blockdriver.h"`.
  The project uses `blockdriver.h` with `blockdev_register(name, size, block_size)`.
- **`bot_scsi_cmd()`** — generic BOT helper: CBW → optional data phase → CSW.
  Replaces ad-hoc CBW/CSW code scattered through the file.
- **`bot_read_capacity()`** — issues SCSI READ CAPACITY(10) (CDB 0x25), parses the
  8-byte big-endian response to extract real sector count and block size.
- **`block_size[]` field** added to `usb_storage_t` — `usb_bot_rw` uses the actual
  block size from the drive instead of a hardcoded 512.
- **UAS detection** — protocol 0x62 is declined early with a clear log message;
  BOT (protocol 0x50) is bound normally.
- **`blockdev_ops_t`** with multi-block `ssize_t (*read)(blockdev_t*, lba, count, buf)`
  and matching write callback hooked into the registered blockdev.

### `drivers/usb/usb_init.c` (boot157 init ordering)
Class drivers are now registered before `xhci_scan_ports()` runs so that
`usb_enumerate_device()` finds them during the first port scan. Hub driver is
registered first so it can enumerate downstream devices before MSC tries to bind.

---

## Bug Timeline

| Boot | Symptom | Root Cause | Fix |
|------|---------|------------|-----|
| 159 | `[MSC] BOT: CBW send failed` | `xhci_bulk_transfer` was a stub returning -1 | Implement full bulk transfer with DMA ring + doorbell |
| 159 | `capacity=0x0x0000...` (double `0x`) | `print_hex64` called `print_hex32` for high half (which prefixes `0x`), then caller also printed `0x` | Rewrite `print_hex64` to emit all 16 raw hex digits, no prefix |
| 160 | `No interfaces populated` (regression) | Adding `slot_id` to `usb_endpoint_t` in `usb.h` without `make clean`; `usb_core.c` compiled against old struct layout | `make clean && make BOARD=pi4` |
| 161 | 60× timeout, no Transfer Events | EP context: EP_Type/CErr in **DW0** instead of **DW1**; VL805 read EP_Type=0 ("Not Valid") and ignored all bulk doorbells | Move EP_Type and CErr to DW1; DW0=0 for bulk |
| 162 | `cc=0x00000004` (Transaction Error) on every CBW | No `SET_CONFIGURATION` issued; devices in "Address" state, bulk endpoints inactive (NAK everything) | Issue `SET_CONFIGURATION(1)` via EP0 after config descriptor read |
| 163 | ✓ | — | — |

---

## DMA Memory Layout (xhci_dma_buf)

```
Offset      Size    Content
0x00000     0x1000  DCBAA + scratch + ERST + event ring (boot-time setup)
0x01000     0x21000 Command ring, misc control structures
0x22000     0x4000  Per-slot contexts, EP0 rings, EP0 data bufs (4 slots × 0x1000)
0x26000     0x8000  Per-slot bulk rings + DMA bounce bufs   (4 slots × 0x2000):
  slot N:   +0x000  Bulk OUT ring  (64 TRBs × 16 B = 1024 B)
            +0x400  Bulk IN ring   (64 TRBs × 16 B = 1024 B)
            +0x800  DMA bounce buf (6144 B — fits 4K-native sector reads)
```

---

## Next Steps

1. **FileCore sector reads** — wire `blockdev_ops_t.read` calls from FileCore to issue
   SCSI READ(10) over BOT and return raw sector data. LBA 0 contains the partition
   table (MBR or GPT) needed to locate the RISC OS FileCore partition.
2. **Partition table parsing** — parse MBR/GPT at LBA 0, find FileCore partition
   (type 0xAD for RISC OS or a GPT equivalent), pass the LBA offset to FileCore.
3. **`make` Makefile dependency tracking** — add `CFLAGS += -MMD -MP` and
   `-include $(OBJS:.o=.d)` so header changes trigger correct incremental rebuilds.
4. **Remove diagnostic prints** (optional) — the `[xHCI] bulk_xfer:` and
   `[xHCI] ConfigureEP: addr=` lines generated per-transfer are useful during
   development but verbose in production. Gate behind a `#ifdef XHCI_VERBOSE`.
5. **UAS driver** — the four UAS endpoints (CMD OUT, STS IN, DATA IN, DATA OUT)
   are already configured by `xhci_configure_endpoints`. A future UAS implementation
   can use them for higher performance without BOT round-trip overhead.
