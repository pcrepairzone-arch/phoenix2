/*
 * mailbox_property.h - VideoCore Mailbox Property Interface
 * 
 * A unified library for all VideoCore firmware mailbox operations
 * 
 * This provides a clean, reusable interface for:
 * - Hardware initialization (USB, PCIe, etc)
 * - GPU operations (framebuffer, memory allocation)
 * - Power management
 * - Clock control
 * - Hardware info queries
 * - And much more!
 */

#ifndef MAILBOX_PROPERTY_H
#define MAILBOX_PROPERTY_H

#include <stdint.h>

/*
 * ============================================================================
 * MAILBOX PROPERTY TAGS
 * 
 * These are all the documented VideoCore firmware tags
 * Organized by category for easy reference
 * ============================================================================
 */

/* ---- Hardware Information ---- */
#define MBOX_TAG_GET_FIRMWARE_REV       0x00000001
#define MBOX_TAG_GET_BOARD_MODEL        0x00010001
#define MBOX_TAG_GET_BOARD_REVISION     0x00010002
#define MBOX_TAG_GET_MAC_ADDRESS        0x00010003
#define MBOX_TAG_GET_BOARD_SERIAL       0x00010004
#define MBOX_TAG_GET_ARM_MEMORY         0x00010005
#define MBOX_TAG_GET_VC_MEMORY          0x00010006

/* ---- Power Management ---- */
#define MBOX_TAG_GET_POWER_STATE        0x00020001
#define MBOX_TAG_GET_TIMING             0x00020002
#define MBOX_TAG_SET_POWER_STATE        0x00028001

/* ---- Clocks ---- */
#define MBOX_TAG_GET_CLOCK_STATE        0x00030001
#define MBOX_TAG_GET_CLOCK_RATE         0x00030002
#define MBOX_TAG_GET_MAX_CLOCK_RATE     0x00030004
#define MBOX_TAG_GET_MIN_CLOCK_RATE     0x00030007
#define MBOX_TAG_GET_TURBO              0x00030009
#define MBOX_TAG_SET_CLOCK_STATE        0x00038001
#define MBOX_TAG_SET_CLOCK_RATE         0x00038002
#define MBOX_TAG_SET_TURBO              0x00038009

/* ---- Voltage ---- */
#define MBOX_TAG_GET_VOLTAGE            0x00030003
#define MBOX_TAG_GET_MAX_VOLTAGE        0x00030005
#define MBOX_TAG_GET_MIN_VOLTAGE        0x00030008
#define MBOX_TAG_SET_VOLTAGE            0x00038003

/* ---- GPU Memory ---- */
#define MBOX_TAG_ALLOCATE_MEMORY        0x0003000c
#define MBOX_TAG_LOCK_MEMORY            0x0003000d
#define MBOX_TAG_UNLOCK_MEMORY          0x0003000e
#define MBOX_TAG_RELEASE_MEMORY         0x0003000f
#define MBOX_TAG_EXECUTE_CODE           0x00030010
#define MBOX_TAG_EXECUTE_QPU            0x00030011
#define MBOX_TAG_SET_ENABLE_QPU         0x00030012
#define MBOX_TAG_GET_DISPMANX_HANDLE    0x00030014
#define MBOX_TAG_GET_EDID_BLOCK         0x00030020
#define MBOX_TAG_GET_CUSTOMER_OTP       0x00030021
#define MBOX_TAG_GET_DOMAIN_STATE       0x00030030
#define MBOX_TAG_SET_DOMAIN_STATE       0x00038030

/* ---- Framebuffer ---- */
#define MBOX_TAG_ALLOCATE_BUFFER        0x00040001
#define MBOX_TAG_RELEASE_BUFFER         0x00048001
#define MBOX_TAG_BLANK_SCREEN           0x00040002
#define MBOX_TAG_GET_PHYSICAL_SIZE      0x00040003
#define MBOX_TAG_TEST_PHYSICAL_SIZE     0x00044003
#define MBOX_TAG_SET_PHYSICAL_SIZE      0x00048003
#define MBOX_TAG_GET_VIRTUAL_SIZE       0x00040004
#define MBOX_TAG_TEST_VIRTUAL_SIZE      0x00044004
#define MBOX_TAG_SET_VIRTUAL_SIZE       0x00048004
#define MBOX_TAG_GET_DEPTH              0x00040005
#define MBOX_TAG_TEST_DEPTH             0x00044005
#define MBOX_TAG_SET_DEPTH              0x00048005
#define MBOX_TAG_GET_PIXEL_ORDER        0x00040006
#define MBOX_TAG_TEST_PIXEL_ORDER       0x00044006
#define MBOX_TAG_SET_PIXEL_ORDER        0x00048006
#define MBOX_TAG_GET_ALPHA_MODE         0x00040007
#define MBOX_TAG_TEST_ALPHA_MODE        0x00044007
#define MBOX_TAG_SET_ALPHA_MODE         0x00048007
#define MBOX_TAG_GET_PITCH              0x00040008
#define MBOX_TAG_GET_VIRTUAL_OFFSET     0x00040009
#define MBOX_TAG_TEST_VIRTUAL_OFFSET    0x00044009
#define MBOX_TAG_SET_VIRTUAL_OFFSET     0x00048009
#define MBOX_TAG_GET_OVERSCAN           0x0004000a
#define MBOX_TAG_TEST_OVERSCAN          0x0004400a
#define MBOX_TAG_SET_OVERSCAN           0x0004800a
#define MBOX_TAG_GET_PALETTE            0x0004000b
#define MBOX_TAG_TEST_PALETTE           0x0004400b
#define MBOX_TAG_SET_PALETTE            0x0004800b
#define MBOX_TAG_SET_CURSOR_INFO        0x00008010
#define MBOX_TAG_SET_CURSOR_STATE       0x00008011
#define MBOX_TAG_GET_TOUCHBUF           0x0004000f
#define MBOX_TAG_GET_GPIO_VIRTBUF       0x00040010

/* ---- Config / Command ---- */
#define MBOX_TAG_GET_COMMAND_LINE       0x00050001
#define MBOX_TAG_GET_DMA_CHANNELS       0x00060001

/* ---- Hardware Control ---- */
#define MBOX_TAG_SET_LED_STATE          0x00038041
#define MBOX_TAG_GET_THROTTLED          0x00030046
#define MBOX_TAG_GET_PERIPH_REG         0x00030045
#define MBOX_TAG_SET_PERIPH_REG         0x00038045
#define MBOX_TAG_GET_POE_HAT_VAL        0x00030049
#define MBOX_TAG_SET_POE_HAT_VAL        0x00030050

/* ---- CRITICAL: USB/PCIe Control ---- */
#define MBOX_TAG_NOTIFY_XHCI_RESET      0x00030058  /* VL805 firmware load */

/* ---- Device Tree ---- */
#define MBOX_TAG_GET_DT_SIZE            0x00060001
#define MBOX_TAG_GET_DT                 0x00060002

/*
 * ============================================================================
 * POWER DEVICE IDs
 * ============================================================================
 */
#define POWER_DEVICE_SD_CARD            0
#define POWER_DEVICE_UART0              1
#define POWER_DEVICE_UART1              2
#define POWER_DEVICE_USB_HCD            3
#define POWER_DEVICE_I2C0               4
#define POWER_DEVICE_I2C1               5
#define POWER_DEVICE_I2C2               6
#define POWER_DEVICE_SPI                7
#define POWER_DEVICE_CCP2TX             8

/*
 * ============================================================================
 * CLOCK IDs
 * ============================================================================
 */
#define CLOCK_EMMC                      1
#define CLOCK_UART                      2
#define CLOCK_ARM                       3
#define CLOCK_CORE                      4
#define CLOCK_V3D                       5
#define CLOCK_H264                      6
#define CLOCK_ISP                       7
#define CLOCK_SDRAM                     8
#define CLOCK_PIXEL                     9
#define CLOCK_PWM                       10
#define CLOCK_HEVC                      11
#define CLOCK_EMMC2                     12
#define CLOCK_M2MC                      13
#define CLOCK_PIXEL_BVB                 14

/*
 * ============================================================================
 * RESPONSE CODES
 * ============================================================================
 */
#define MBOX_REQUEST                    0x00000000
#define MBOX_RESPONSE_SUCCESS           0x80000000
#define MBOX_RESPONSE_ERROR             0x80000001

/*
 * ============================================================================
 * MAILBOX PROPERTY INTERFACE
 * ============================================================================
 */

typedef struct {
    uint32_t buffer_size;      /* Total buffer size in bytes */
    uint32_t request_code;     /* Request/response code */
    /* Tags follow here */
    /* End tag (0) at the end */
} __attribute__((aligned(16))) mbox_property_buffer_t;

/*
 * Initialize the mailbox property system
 */
void mbox_property_init(void);

/*
 * Send a property request to VideoCore firmware
 * 
 * @param buffer - Property buffer (must be 16-byte aligned!)
 * @return 0 on success, -1 on error
 */
int mbox_property_call(volatile uint32_t *buffer);

/*
 * ============================================================================
 * HIGH-LEVEL CONVENIENCE FUNCTIONS
 * 
 * These wrap the low-level mailbox calls for common operations
 * ============================================================================
 */

/* Hardware Info */
uint32_t mbox_get_firmware_revision(void);
uint32_t mbox_get_board_model(void);
uint32_t mbox_get_board_revision(void);
uint64_t mbox_get_board_serial(void);
void mbox_get_mac_address(uint8_t mac[6]);
void mbox_get_arm_memory(uint32_t *base, uint32_t *size);
void mbox_get_vc_memory(uint32_t *base, uint32_t *size);

/* Power Management */
int mbox_get_power_state(uint32_t device_id);
int mbox_set_power_state(uint32_t device_id, int on);

/* Clock Control */
uint32_t mbox_get_clock_rate(uint32_t clock_id);
int mbox_set_clock_rate(uint32_t clock_id, uint32_t rate_hz);
uint32_t mbox_get_max_clock_rate(uint32_t clock_id);
uint32_t mbox_get_min_clock_rate(uint32_t clock_id);

/* LED Control */
int mbox_set_led_state(uint32_t led_pin, int on);

/* USB/PCIe Control */
int mbox_notify_xhci_reset(uint32_t dev_addr);  /* Load VL805 firmware */

/* Framebuffer (if not already handled elsewhere) */
int mbox_allocate_framebuffer(uint32_t alignment);
int mbox_set_physical_size(uint32_t width, uint32_t height);
int mbox_set_virtual_size(uint32_t width, uint32_t height);
int mbox_set_depth(uint32_t bpp);

#endif /* MAILBOX_PROPERTY_H */
