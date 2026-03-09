/*
 * mailbox_property.h — VideoCore Mailbox Property Interface
 *
 * Unified library for all VideoCore firmware mailbox operations.
 * Wraps the low-level mbox_call() from drivers/gpu/mailbox.c.
 *
 * Porting notes:
 *   The only hardware dependency is mbox_call() which uses get_mailbox_base()
 *   from periph_base.c.  To port to a new SoC, replace those two functions.
 *   Everything above that layer is SoC-independent.
 *
 * Usage:
 *   #include "mailbox_property.h"
 *   uint32_t rev = mbox_get_firmware_revision();
 *   int r = mbox_notify_xhci_reset(0x00100000);
 */

#ifndef MAILBOX_PROPERTY_H
#define MAILBOX_PROPERTY_H

#include <stdint.h>

/* ============================================================
 * Property tag IDs — full set for Pi 4/5
 * ============================================================ */

/* Hardware info */
#define MBOX_TAG_GET_FIRMWARE_REV       0x00000001
#define MBOX_TAG_GET_BOARD_MODEL        0x00010001
#define MBOX_TAG_GET_BOARD_REVISION     0x00010002
#define MBOX_TAG_GET_MAC_ADDRESS        0x00010003
#define MBOX_TAG_GET_BOARD_SERIAL       0x00010004
#define MBOX_TAG_GET_ARM_MEMORY         0x00010005
#define MBOX_TAG_GET_VC_MEMORY          0x00010006

/* Power management */
#define MBOX_TAG_GET_POWER_STATE        0x00020001
#define MBOX_TAG_SET_POWER_STATE        0x00028001

/* Power device IDs */
#define MBOX_PWR_SD_CARD                0
#define MBOX_PWR_UART0                  1
#define MBOX_PWR_UART1                  2
#define MBOX_PWR_USB_HCD                3   /* USB host controller */
#define MBOX_PWR_I2C0                   4
#define MBOX_PWR_I2C1                   5
#define MBOX_PWR_SPI                    7

/* Clocks */
#define MBOX_TAG_GET_CLOCK_STATE        0x00030001
#define MBOX_TAG_GET_CLOCK_RATE         0x00030002
#define MBOX_TAG_GET_MAX_CLOCK_RATE     0x00030004
#define MBOX_TAG_SET_CLOCK_RATE         0x00038002

/* Clock IDs */
#define MBOX_CLK_EMMC                   1
#define MBOX_CLK_UART                   2
#define MBOX_CLK_ARM                    3
#define MBOX_CLK_CORE                   4
#define MBOX_CLK_PIXEL                  9

/* GPU / VC memory */
#define MBOX_TAG_ALLOCATE_MEMORY        0x0003000c
#define MBOX_TAG_LOCK_MEMORY            0x0003000d
#define MBOX_TAG_UNLOCK_MEMORY          0x0003000e
#define MBOX_TAG_RELEASE_MEMORY         0x0003000f
#define MBOX_TAG_EXECUTE_CODE           0x00030010

/* Framebuffer */
#define MBOX_TAG_ALLOCATE_BUFFER        0x00040001
#define MBOX_TAG_RELEASE_BUFFER         0x00048001
#define MBOX_TAG_SET_PHYSICAL_SIZE      0x00048003
#define MBOX_TAG_SET_VIRTUAL_SIZE       0x00048004
#define MBOX_TAG_SET_DEPTH              0x00048005
#define MBOX_TAG_SET_PIXEL_ORDER        0x00048006
#define MBOX_TAG_GET_PITCH              0x00040008
#define MBOX_TAG_SET_VIRTUAL_OFFSET     0x00048009

/* Hardware control */
#define MBOX_TAG_GET_THROTTLED          0x00030046
#define MBOX_TAG_SET_LED_STATE          0x00038041

/* USB / PCIe — Pi 4 specific */
#define MBOX_TAG_NOTIFY_XHCI_RESET      0x00030058  /* Load VL805 firmware */

/* End tag */
#define MBOX_TAG_END                    0x00000000

/* Response codes */
#define MBOX_REQUEST                    0x00000000
#define MBOX_RESPONSE_OK                0x80000000
#define MBOX_RESPONSE_ERR               0x80000001

/* ============================================================
 * Low-level call — implemented in drivers/gpu/mailbox.c
 * buf must be 16-byte aligned; buf[1]=0 on entry.
 * Returns 0 on success (buf[1]==0x80000000), -1 on failure.
 * ============================================================ */
int mbox_call(volatile uint32_t *buf);

/* ============================================================
 * Hardware info
 * ============================================================ */
uint32_t mbox_get_firmware_revision(void);
uint32_t mbox_get_board_model(void);
uint32_t mbox_get_board_revision(void);
uint64_t mbox_get_board_serial(void);
void     mbox_get_arm_memory(uint32_t *base, uint32_t *size);
void     mbox_get_vc_memory(uint32_t *base, uint32_t *size);

/* ============================================================
 * Power management
 * ============================================================ */
int mbox_get_power_state(uint32_t device_id);

/*
 * mbox_set_power_state — power a device on or off.
 * @wait: if non-zero, wait for the state to take effect.
 * Returns 0 on success.
 */
int mbox_set_power_state(uint32_t device_id, int on, int wait);

/* ============================================================
 * Clocks
 * ============================================================ */
uint32_t mbox_get_clock_rate(uint32_t clock_id);
uint32_t mbox_get_max_clock_rate(uint32_t clock_id);
int      mbox_set_clock_rate(uint32_t clock_id, uint32_t rate_hz);

/* ============================================================
 * USB / PCIe
 * ============================================================ */

/*
 * mbox_notify_xhci_reset — ask VideoCore to load VL805 USB firmware.
 *
 * Must be called after PCIe link-up and RC bus/window setup, but before
 * any EXT_CFG writes to the VL805.
 *
 * @pcie_bdf: PCIe BDF encoded as (bus<<20)|(dev<<15)|(fn<<12).
 *            For Pi 4 VL805 at bus=1,dev=0,fn=0: pass 0x00100000.
 *
 * Returns 0 on success, -1 on failure.
 * Failure is expected and safe on boards with SPI EEPROM (c03111/c03112)
 * — the VL805 auto-loads from EEPROM on those boards.
 * Failure on d03114 (no EEPROM) means USB firmware was not loaded.
 */
int mbox_notify_xhci_reset(uint32_t pcie_bdf);

/*
 * mbox_power_on_usb — power on the USB HCD device via mailbox.
 * May be required on some board variants before NOTIFY_XHCI_RESET.
 * Returns 0 on success.
 */
int mbox_power_on_usb(void);

/* ============================================================
 * LED / diagnostics
 * ============================================================ */
int mbox_set_led_state(uint32_t gpio_pin, int on);

#endif /* MAILBOX_PROPERTY_H */
