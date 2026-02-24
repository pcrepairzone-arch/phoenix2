/*
 * mailbox_property.c - VideoCore Mailbox Property Interface Implementation
 * 
 * Unified library for all VideoCore firmware mailbox operations
 */

#include "mailbox_property.h"
#include <stdint.h>

/* External mailbox low-level functions (from your existing driver) */
extern void mailbox_write(uint8_t channel, uint32_t data);
extern uint32_t mailbox_read(uint8_t channel);

#define MAILBOX_CHANNEL_PROPERTY  8

/*
 * Send a property request to VideoCore firmware
 */
int mbox_property_call(volatile uint32_t *buffer)
{
    /* Get physical address */
    uint32_t addr = (uint32_t)((uint64_t)buffer & 0xFFFFFFFF);
    
    /* Verify 16-byte alignment */
    if (addr & 0xF) {
        return -1;
    }
    
    /* Set request code */
    buffer[1] = MBOX_REQUEST;
    
    /* Write to mailbox */
    mailbox_write(MAILBOX_CHANNEL_PROPERTY, addr);
    
    /* Read response */
    uint32_t response = mailbox_read(MAILBOX_CHANNEL_PROPERTY);
    
    /* Verify response */
    if ((response & 0xFFFFFFF0) != (addr & 0xFFFFFFF0)) {
        return -1;
    }
    
    /* Check response code */
    if (buffer[1] != MBOX_RESPONSE_SUCCESS) {
        return -1;
    }
    
    return 0;
}

/*
 * ============================================================================
 * HIGH-LEVEL CONVENIENCE FUNCTIONS
 * ============================================================================
 */

uint32_t mbox_get_firmware_revision(void)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[7];
    
    buffer[0] = 7 * 4;                      // Buffer size
    buffer[1] = MBOX_REQUEST;               // Request
    buffer[2] = MBOX_TAG_GET_FIRMWARE_REV;  // Tag
    buffer[3] = 4;                          // Value size
    buffer[4] = 0;                          // Request size
    buffer[5] = 0;                          // Value
    buffer[6] = 0;                          // End tag
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[5];
}

uint32_t mbox_get_board_model(void)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[7];
    
    buffer[0] = 7 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_BOARD_MODEL;
    buffer[3] = 4;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[5];
}

uint32_t mbox_get_board_revision(void)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[7];
    
    buffer[0] = 7 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_BOARD_REVISION;
    buffer[3] = 4;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[5];
}

uint64_t mbox_get_board_serial(void)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_BOARD_SERIAL;
    buffer[3] = 8;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return ((uint64_t)buffer[6] << 32) | buffer[5];
}

void mbox_get_mac_address(uint8_t mac[6])
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_MAC_ADDRESS;
    buffer[3] = 6;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return;
    }
    
    uint8_t *data = (uint8_t *)&buffer[5];
    for (int i = 0; i < 6; i++) {
        mac[i] = data[i];
    }
}

void mbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_ARM_MEMORY;
    buffer[3] = 8;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        if (base) *base = 0;
        if (size) *size = 0;
        return;
    }
    
    if (base) *base = buffer[5];
    if (size) *size = buffer[6];
}

void mbox_get_vc_memory(uint32_t *base, uint32_t *size)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_VC_MEMORY;
    buffer[3] = 8;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        if (base) *base = 0;
        if (size) *size = 0;
        return;
    }
    
    if (base) *base = buffer[5];
    if (size) *size = buffer[6];
}

int mbox_get_power_state(uint32_t device_id)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_POWER_STATE;
    buffer[3] = 8;
    buffer[4] = 4;
    buffer[5] = device_id;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return -1;
    }
    
    return (buffer[6] & 0x1) ? 1 : 0;
}

int mbox_set_power_state(uint32_t device_id, int on)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_SET_POWER_STATE;
    buffer[3] = 8;
    buffer[4] = 8;
    buffer[5] = device_id;
    buffer[6] = (on ? 1 : 0) | (1 << 1);  // State | Wait bit
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return -1;
    }
    
    return 0;
}

uint32_t mbox_get_clock_rate(uint32_t clock_id)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_CLOCK_RATE;
    buffer[3] = 8;
    buffer[4] = 4;
    buffer[5] = clock_id;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[6];
}

int mbox_set_clock_rate(uint32_t clock_id, uint32_t rate_hz)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[9];
    
    buffer[0] = 9 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_SET_CLOCK_RATE;
    buffer[3] = 12;
    buffer[4] = 8;
    buffer[5] = clock_id;
    buffer[6] = rate_hz;
    buffer[7] = 0;  // Skip turbo
    buffer[8] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return -1;
    }
    
    return 0;
}

uint32_t mbox_get_max_clock_rate(uint32_t clock_id)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_MAX_CLOCK_RATE;
    buffer[3] = 8;
    buffer[4] = 4;
    buffer[5] = clock_id;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[6];
}

uint32_t mbox_get_min_clock_rate(uint32_t clock_id)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_GET_MIN_CLOCK_RATE;
    buffer[3] = 8;
    buffer[4] = 4;
    buffer[5] = clock_id;
    buffer[6] = 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return 0;
    }
    
    return buffer[6];
}

int mbox_set_led_state(uint32_t led_pin, int on)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;
    buffer[1] = MBOX_REQUEST;
    buffer[2] = MBOX_TAG_SET_LED_STATE;
    buffer[3] = 8;
    buffer[4] = 8;
    buffer[5] = led_pin;
    buffer[6] = on ? 1 : 0;
    buffer[7] = 0;
    
    if (mbox_property_call(buffer) < 0) {
        return -1;
    }
    
    return 0;
}

/*
 * CRITICAL: VL805 USB Firmware Loading
 * 
 * This is THE KEY function for making USB work on Pi 4!
 * After firmware reset, this loads the VL805 firmware into the chip
 */
int mbox_notify_xhci_reset(uint32_t dev_addr)
{
    volatile uint32_t __attribute__((aligned(16))) buffer[8];
    
    buffer[0] = 8 * 4;                      // Buffer size
    buffer[1] = MBOX_REQUEST;               // Request
    buffer[2] = MBOX_TAG_NOTIFY_XHCI_RESET; // Tag 0x00030058
    buffer[3] = 4;                          // Value size
    buffer[4] = 4;                          // Request size
    buffer[5] = dev_addr;                   // Device address (0 for VL805)
    buffer[6] = 0;                          // Padding
    buffer[7] = 0;                          // End tag
    
    if (mbox_property_call(buffer) < 0) {
        return -1;
    }
    
    return 0;
}
