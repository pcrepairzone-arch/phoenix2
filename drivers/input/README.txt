INPUT DRIVERS - RISC OS THREE-BUTTON MOUSE
===========================================

CURRENT STATUS: STUBS
=====================

The input driver API is defined following RISC OS conventions:
- Three-button mouse (SELECT, MENU, ADJUST)
- Function key standards (F3=Save, F12=CLI)
- RISC OS internal key numbers (INKEY codes)

BUTTON PARADIGM:
================

SELECT (Left):   Primary action, select, open
MENU (Middle):   Context menu under pointer
ADJUST (Right):  Inverse action, toggle, close

IMPLEMENTATION PLAN:
====================

Phase 1: USB HID (BLOCKING)
  Requires:
    - PCIe infrastructure
    - xHCI host controller
    - USB HID class driver
    - Interrupt endpoints
    - Report descriptor parsing

Phase 2: Input Integration
  - keyboard_poll() from USB HID
  - mouse_poll() from USB HID
  - Button state tracking
  - Modifier key tracking

Phase 3: WIMP Integration
  - Feed events to Wimp_Poll
  - Menu triggering on MENU button
  - SELECT/ADJUST window management
  - Function key dispatch

FILES:
======
keyboard.h     - Keyboard API and key codes
mouse.h        - Mouse API and button definitions
input_stub.c   - Stub implementation (temporary)

DEPENDENCIES:
=============
□ USB host stack (in progress)
□ USB HID driver (to be implemented)
□ WIMP event system (to be implemented)

The API is ready, waiting for USB infrastructure!
