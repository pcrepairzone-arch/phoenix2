# Phoenix RISC OS - Boot Instructions

## Files Needed on SD Card

Copy these files to your SD card's boot partition:

1. **Phoenix kernel**: `phoenix64.img` → rename to `kernel8.img`
2. **Boot config**: `config.txt` (provided)
3. **Pi firmware files** (get from official Raspberry Pi OS):
   - `start4.elf` (Pi 4) or `start_cd.elf` (Pi 5)
   - `fixup4.dat` (Pi 4) or `fixup_cd.dat` (Pi 5)
   - `bcm2711-rpi-4-b.dtb` (Pi 4) or `bcm2712-rpi-5-b.dtb` (Pi 5)

## Hardware Setup

### Serial Console (Recommended for first boot)
Connect UART to GPIO pins:
- GPIO 14 (TXD) → RX on USB-Serial adapter  
- GPIO 15 (RXD) → TX on USB-Serial adapter
- Ground → Ground

Settings: 115200 baud, 8N1, no flow control

### Monitor via Serial Console
```bash
# On Linux/Mac:
screen /dev/ttyUSB0 115200

# Or use minicom:
minicom -D /dev/ttyUSB0 -b 115200
```

## Expected Output

You should see:
```
Phoenix RISC OS Kernel v0.1
Booting on ARM64...
[CPU 0] Initializing...
Parsing device tree at 0x...
Memory: 1024 MB
Kernel initialization complete!
```

## Troubleshooting

**No output on serial?**
- Check wiring (TX/RX might be swapped)
- Verify `enable_uart=1` in config.txt
- Try 9600 baud if 115200 doesn't work

**Kernel doesn't boot?**
- Verify firmware files are present
- Check kernel8.img file size (should be >100KB)
- Try with official Raspberry Pi OS first to verify hardware

**Hangs after "Booting"?**
- This is expected! Most subsystems are stubs
- You're seeing the boot process work!
