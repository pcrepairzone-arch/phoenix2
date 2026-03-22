/*
 * vl805_init.c — VL805 xHCI firmware init via VideoCore mailbox
 *
 * On Pi 4 boards WITHOUT a VLI SPI EEPROM (boardrev d03114, Pi 400, CM4):
 *   The VL805 firmware lives in VideoCore SDRAM, loaded by start4.elf.
 *   After PERST# the VL805 has no firmware until the VC reloads it in
 *   response to mailbox tag 0x00030058 (NOTIFY_XHCI_RESET).
 *
 * On Pi 4 boards WITH SPI EEPROM (boardrev c03111, c03112):
 *   The VL805 self-loads firmware from EEPROM after PERST#.
 *   The mailbox call returns failure but USB still works — safe to ignore.
 *
 * This file now uses mailbox_property.c for all VC communication.
 * The inline mailbox code has been removed — see drivers/gpu/mailbox.c
 * for the low-level implementation.
 */

#include "kernel.h"
#include "mailbox_property.h"

extern void uart_puts(const char *s);

int vl805_init(void)
{
    uart_puts("[VL805] Requesting firmware via mailbox 0x00030058...\n");

    /*
     * mbox_power_on_usb() is called by pci.c BEFORE vl805_init() is called,
     * so USB HCD is already powered on at this point.  Do NOT call it again
     * here — calling SET_POWER_STATE(on=1) when USB is already on causes the
     * VideoCore to issue an internal PERST# equivalent, resetting the VL805
     * MCU mid-init and re-asserting CNR=1.
     *
     * Notify VC to load VL805 firmware.
     * BDF: bus=1, dev=0, fn=0 → 0x00100000
     */
    if (mbox_notify_xhci_reset(0x00100000U) < 0) {
        uart_puts("[VL805] Firmware load failed (EEPROM board or no VC response)\n");
        return -1;
    }

    uart_puts("[VL805] Firmware loaded OK\n");

    /* VL805 needs ~100ms to complete firmware init after VC notification */
    for (volatile int i = 0; i < 1000000; i++) asm volatile("nop");

    return 0;
}
