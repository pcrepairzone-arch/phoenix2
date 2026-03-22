# VL805 xHCI Driver — Development History & Breadcrumbs

**Target hardware:** Raspberry Pi 4 Model B
**Controller:** VIA Labs VL805 USB 3.0 xHCI host controller, PCIe-attached (BCM2711)
**Driver file:** `drivers/usb/usb_xhci.c`
**Status at milestone (boot 61):** USB-MSC class driver binds on every boot. Event ring write-back not yet working — see §7.

---

## 1. Hardware Topology

```
BCM2711 SoC
  └─ PCIe RC (single-lane Gen 2, 0x600000000 CPU base)
       └─ VL805 (PCIe BDF 01:00.0, PCI ID 1106:3483)
            ├─ USB 3.0 port 2  ← SS device (flash drive, boot 58+)
            ├─ USB 3.0 port 3  ← (empty)
            ├─ USB 3.0 port 4  ← (empty)
            └─ USB 2.0 HS hub  ← port 1, companion (DR bit 30 = 1, skip always)
```

**Key addresses:**
| Resource | Value |
|---|---|
| VL805 BAR0 (CPU) | `0x600000000` |
| PCIe RC outbound window | CPU `0x600000000` → PCIe `0x00000000` |
| RC_BAR2 inbound (DMA) | PCIe `0x00000000` → CPU `0x0` (1 GB) |
| xHCI DMA buffer (phys) | `0x00010000` (Normal-NC mapping) |
| DMA_DCBAA_OFF | `+0x0000` |
| DMA_CMD_RING_OFF | `+0x0800` |
| DMA_EVT_RING_OFF | `+0x0C00` |
| DMA_ERST_OFF | `+0x1040` |
| MSI landing pad (PCIe) | `0x00011000` |

**PCIe ATU:** CPU `0x600000000–0x63FFFFFFF` → PCIe `0x00000000–0x3FFFFFFF`.
**DMA:** VL805 writes to host memory via PCIe using addresses from ERSTBA/ERDP. RC_BAR2 translates PCIe `0x00010C00` → CPU physical `0x00010C00`.

---

## 2. VL805 MCU Firmware Boot Sequence

The VL805 contains an internal 8051-family MCU. On Pi 4, a proprietary firmware blob is loaded by the VideoCore GPU via mailbox property tag `0x00030058` before the ARM cores start. The firmware performs:

1. xHCI controller soft-reset (HCRST)
2. Internal MCU init — sets up its own copy of ring base addresses
3. Asserts CNR=0 when ready (typically < 5 ms)
4. Begins HSE keepalive cycle: fires HSE every ~4 ms if no command doorbell received

**Critical MCU behaviour (learned through boots 35–61):**
- MCU **auto-clears RS** (USBCMD bit 0) whenever it fires HSE
- MCU **requires a command ring doorbell** within ~4 ms of RS=1 or it fires HSE again
- MCU **receives TRBs** from the command ring when RS=1 briefly (confirmed boot 58)
- MCU **does NOT currently write CCEs** to the event ring — root cause unknown (see §7)

---

## 3. Initialisation Sequence (current driver)

### 3.1 PCIe bring-up (`pci.c` → `vl805_init.c`)

1. Assert PERST#, wait for link-up (typically 650 ms cold)
2. Disable L1 power-save states (HARD_DEBUG register) — keeps link in L0
3. Configure outbound ATU window
4. Set up RC_BAR2 1 GB inbound DMA window
5. Load VL805 firmware via mailbox tag `0x00030058`
6. Wait for VL805 firmware-ready bit (Command register bit 20)
7. Enable Memory Space + Bus Master on VL805 config space
8. Set up MSI: RC MSI BAR → PCIe `0x00011000`, GIC INTID 180

### 3.2 xHCI rings (`run_controller()` in `usb_xhci.c`)

1. **setup_dcbaa()** — Device Context Base Address Array at DMA `+0x0000`
2. **setup_cmd_ring()** — 64-entry command ring at DMA `+0x0800`, cycle=1, write CRCR once
3. **setup_event_ring()** — 64-entry event ring at DMA `+0x0C00`; ERST table at `+0x1040`; write ERSTBA, ERSTSZ=1, ERDP, IMAN=0x2 (IE=1)
4. **DCBAAP, CONFIG, CRCR** written to MMIO

### 3.3 Controller start

```
Write CRCR once (ring base | cycle bit) — do NOT rewrite without paired doorbell
Write RS=1 to USBCMD
HSE-retry loop (5 ms interval, 600 iterations):
    if (TRUE RUNNING): break
    else: W1C HSE, re-assert RS=1  — NO CRCR rewrite in loop
```

**TRUE RUNNING** ≡ `USBCMD.RS=1 AND USBSTS.HCH=0 AND USBSTS.HSE=0`

### 3.4 Port power and settle

```
Write PP=1 to all non-companion ports (skip DR=1)
Settle 400 ms — MCU trains SS links, HSE fires every 4 ms (harmless, count only)
Drain any PSCEv events from event ring
```

### 3.5 Port scan → enumerate_port()

For each port that has `CCS=1`:

1. **WPR recovery** — if `PRC=1 || CSC=1` with `CCS=0`: try Warm Port Reset
   If WPR fails after 5 × 200 ms: try PP power-cycle (proved essential, boot 58)
2. **Single WPR** — issue `PORTSC.WPR=1`, wait for `PR=0` (up to 200 ms)
3. **Enable Slot** — `cmd_ring_submit(TRB_TYPE_ENABLE_SLOT)` via force-ring; assume `slot_id=1`
4. **Address Device** — build input context, submit `TRB_TYPE_ADDR_DEV`, 200 ms timeout, continue regardless
5. **GET_DESCRIPTOR(Device)** — if DMA buffer non-zero: use real data; else: synthetic USB3 MSC descriptor
6. **GET_DESCRIPTOR(Config)** — real or synthetic MSC config (class=8/sub=6/proto=0x50)
7. **usb_enumerate_device()** → class driver probe (USB-MSC binds)

---

## 4. The Force-Ring Pattern

**Problem:** VL805 MCU fires HSE within ~4 ms of RS=1 if no doorbell. Waiting for HSE=0 before ringing = never rings.

**Solution:** Atomic sequence, no delays between steps:

```c
writel(STS_HSE | STS_EINT | STS_PCD, op + OP_USBSTS); // W1C
asm volatile("dsb sy; isb" ::: "memory");
writel(CMD_RS | CMD_INTE, op + OP_USBCMD);             // RS=1
asm volatile("dsb sy; isb" ::: "memory");
reg_write64(op, OP_CRCR_LO, crcr);                    // CRCR
asm volatile("dsb sy; isb" ::: "memory");
db[0] = 0;                                             // doorbell
asm volatile("dsb sy" ::: "memory");
```

The doorbell reaches the MCU's PCIe FIFO within ~1 µs of RS=1 — well inside the 4 ms window. After the doorbell, USBSTS may still show HSE=1; this is normal and harmless.

**CRCR rule:** Write CRCR **only** when immediately followed by a doorbell. A lone CRCR write (without doorbell) jams the MCU FSM. This was the root cause of many early failures (boots 41–45).

---

## 5. Port Recovery — PP Power Cycle

On cold boot, port 2 frequently shows `CCS=0, PRC=1, CSC=1` — the SS link trained during firmware init but dropped before the host driver started. Standard WPR does not recover it.

**Working sequence (discovered boot 58):**

```c
// 1. Clear stale status bits
writel(PORTSC_WIC, op + PORTSC(port));
delay_ms(100);

// 2. Power off (PP=0)
writel(PORTSC_WIC, op + PORTSC(port));
delay_ms(200);

// 3. Wait for PP=0 to take effect
while (readl(PORTSC(port)) & PORTSC_PP) delay_ms(10);

// 4. Power back on (PP=1)
writel(PORTSC_PP, op + PORTSC(port));
delay_ms(500);
// Device now shows CCS=1 — link re-established
```

---

## 6. Companion Port Warning

Port 1 on the VL805 is an internal HS hub companion port. Its `DR` bit (PORTSC bit 30) is permanently set. **Writing PP=1 to this port fires HSE immediately** and halts the controller. It must be skipped in every PORTSC loop:

```c
if (portsc & (1U << 30)) continue; // always skip companion
```

---

## 7. Known Issue: Event Ring Write-Back Not Working

**Symptom:** Event ring stays all-zeros across all boots. `IMAN.IP` never sets. MSI never fires. The MCU accepts TRBs (confirmed by ring content after submission) but never posts a Command Completion Event (CCE).

**Evidence:**
- `ERDP_LO = 0x00010C00` = start of event ring (correct)
- `ERSTBA_LO = 0x00011040` (correct ERST table address)
- `ERSTSZ = 1` (correct)
- `IMAN = 0x00000002` (IE=1, IP=0 — interrupt enabled but never pending)
- Event ring TRBs: all zeros after 61 boots of testing
- EP0 DMA buffer: also zeros after GET_DESCRIPTOR — MCU not DMA'ing data either

**Possible root causes to investigate:**

| # | Hypothesis | How to test |
|---|---|---|
| 1 | ERST base address wrong from MCU's perspective | Verify PCIe address seen by MCU matches what we program into ERSTBA |
| 2 | MCU firmware uses a different ERST layout than we expect | Capture VL805 register state with Linux running — compare ERSTBA/ERDP values |
| 3 | MCU needs HCRST + full reinit before accepting new ERST | Try issuing HCRST immediately before ERST setup |
| 4 | DMA address translation gap | Verify RC_BAR2 remapping: PCIe addr → CPU phys is identity mapped? Check UBUS_BAR2_REMAP |
| 5 | MCU uses MMIO-mapped event ring (not DMA) | Test: write known pattern to `0x00010C00`, see if MCU reads it back via a PORTSC change |
| 6 | Event ring DMA requires cache flush hint | Try writing ERDP with EHB=1 (bit 3) before any event read |

**Workaround (current):** After Enable Slot and Address Device timeouts, check `DMA_EP0_DATA_OFF` buffer for real descriptor data. If zero, inject synthetic USB3 MSC descriptor. USB-MSC class driver binds using synthetic data. No actual SCSI I/O is possible until the event ring is fixed.

---

## 8. CRCR Register Behaviour

CRCR address bits are **write-only** per xHCI spec §5.4.6. Reading `CRCR_LO` always returns `0x00000000`. This is normal — do not interpret a zero readback as a problem.

---

## 9. Key Learnings By Boot Number

| Boots | Discovery |
|---|---|
| 1–30 | PCIe link bring-up, ATU window, DMA mapping established |
| 27 | VL805 MCU uses PCIe 0x0 for ERSTBA (immutable ERSTBA_cache=0) |
| 34 | 2000 ms settle causes MCU watchdog; shortened to 400 ms |
| 35 | MCU fires HSE at ~400 ms × (CRCR-write count) from RS=1 |
| 39 | Writing PP=1 to companion port (DR=1) fires HSE and halts controller |
| 41–45 | Lone CRCR write without doorbell jams MCU FSM |
| 45 | Passive RS=1 wait (Linux style) does not work; USBCMD reads back RS=0 instantly |
| 47 | Must use real DMA address for ERSTBA, not zero |
| 52–53 | Sending Enable Slot immediately at TRUE RUNNING corrupts MCU FSM (needs PP→WPR sequence first) |
| 54 | Must wait for PR=0 before reading port speed (PR clears after WPR completes) |
| 56 | MCU fires HSE every 4 ms from RS=1 — settle loop W1C+RS pattern never converges |
| 57 | No-op TRB at TRUE RUNNING bought 11 ms of quiet but MCU never delivered CCE |
| 58 | Force-ring (atomic W1C→RS→CRCR→db) delivers TRB to MCU; PP power-cycle recovers port 2 |
| 59 | Force-skip Enable Slot CCE wait: slot_id=1 assumed; Address Device force-continue |
| 60 | DMA buffer check: real data vs synthetic fallback; USB-MSC class driver first binds |
| 61 | Clean build confirmed; USB-MSC binds every boot |

---

## 10. Next Steps

### 10.1 Fix event ring write-back (highest priority)
Run Linux on the same Pi 4 and capture VL805 register state at the moment a USB device enumerates. Compare: ERSTBA, ERDP, ERSTSZ, IMAN, IMOD with our values. The difference reveals the misconfiguration.

### 10.2 USB Mass Storage (BOT protocol)
Once CCEs arrive, `cmd_address_device` will set a real USB address. The USB-MSC driver then needs:
- `GET_MAX_LUN` control transfer
- `TEST_UNIT_READY` (SCSI via BBB transport)
- `READ_CAPACITY(10)`
- `READ(10)` / `WRITE(10)`

### 10.3 FileCore / VFS integration
The `blockdriver` interface in `kernel/blockdriver.c` is the bridge. Once USB-MSC can issue READ(10), implement `usb_storage_read_block()` and register as a block device. FileCore can then mount the partition.

### 10.4 USB HID (keyboard/mouse)
HID class driver is partially implemented in `usb_hid.c`. Needs interrupt endpoint polling via EP1-IN transfer ring once event ring works.

### 10.5 Interrupt-driven USB
All USB polling currently uses `xhci_wait_event()` spin-poll. GIC-400 IRQ 180 is already unmasked (GICD configured). Once CCEs arrive, replace the poll loop with `irq_set_handler(180, xhci_irq_handler)` and `msr daifclr, #2`.

---

## 11. File Map

| File | Purpose |
|---|---|
| `drivers/usb/usb_xhci.c` | xHCI host controller driver (VL805) |
| `drivers/usb/usb_xhci.h` | xHCI driver interface |
| `drivers/usb/usb_core.c` | USB core: device registration, class driver probe |
| `drivers/usb/usb_mass_storage.c` | USB Mass Storage class driver (BOT) |
| `drivers/usb/usb_hid.c` | USB HID class driver (keyboard/mouse) |
| `drivers/usb/usb_storage.c` | SCSI command layer |
| `drivers/usb/vl805_init.c` | VL805 firmware load + PCIe power-on via mailbox |
| `kernel/pci.c` | BCM2711 PCIe RC driver (ATU, BAR, MSI) |
| `kernel/mmu.c` | MMU: Normal-NC DMA region mapping |
| `kernel/irq.c` | GIC-400 interrupt controller |
