BLUETOOTH DRIVER STATUS
=======================

CURRENT STATE: SKELETON ONLY

Your original BTSdioTypeA driver has been analyzed and the
good parts (ring buffers, HEX parser, HCI protocol) have been
preserved in a bare-metal skeleton.

WHAT WORKS:
-----------
✓ Ring buffer implementation (ready to use)
✓ HCI packet type definitions
✓ Intel HEX parser for firmware
✓ Data structures for BT state

WHAT'S MISSING:
---------------
✗ SDIO host controller driver (hardware layer)
✗ Heap allocator for malloc()
✗ Firmware file loading from SD card
✗ IRQ handling for SDIO card interrupts
✗ Actual HCI command/event processing

DEPENDENCIES:
-------------
Before Bluetooth can work, we need:

1. SDIO Host Driver (drivers/sdio/)
   - BCM2711 EMMC2 controller at 0xFE340000
   - SDIO command/data transfer
   - Card detect and interrupts
   
2. Memory Allocator
   - malloc/free implementation
   - Or static buffer allocation
   
3. File System
   - Read BCM4345C0.hcd firmware from SD
   - Or embed firmware in kernel binary

NEXT STEPS:
-----------
1. Wait for serial cable (Friday) - debug output
2. Add keyboard/mouse input - more critical
3. Build SDIO driver - enables WiFi + BT
4. Complete BT implementation

The skeleton is here and ready for when the infrastructure
is in place!
