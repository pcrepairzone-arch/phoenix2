/**
 * @file usb_xhci.h
 * @brief xHCI (USB 3.0) Host Controller Driver for Phoenix RISC OS
 * 
 * Supports VL805 xHCI controller on Raspberry Pi 4
 * 
 * @author Phoenix RISC OS Team
 * @since v56 (February 2026)
 */

#ifndef USB_XHCI_H
#define USB_XHCI_H

#include <stdint.h>

/* xHCI Capability Registers (offset from base) */
#define XHCI_CAP_CAPLENGTH      0x00  /* Capability Register Length */
#define XHCI_CAP_HCIVERSION     0x02  /* Interface Version Number */
#define XHCI_CAP_HCSPARAMS1     0x04  /* Structural Parameters 1 */
#define XHCI_CAP_HCSPARAMS2     0x08  /* Structural Parameters 2 */
#define XHCI_CAP_HCSPARAMS3     0x0C  /* Structural Parameters 3 */
#define XHCI_CAP_HCCPARAMS1     0x10  /* Capability Parameters 1 */
#define XHCI_CAP_DBOFF          0x14  /* Doorbell Offset */
#define XHCI_CAP_RTSOFF         0x18  /* Runtime Register Space Offset */

/* xHCI Operational Registers (offset from op_base) */
#define XHCI_OP_USBCMD          0x00  /* USB Command */
#define XHCI_OP_USBSTS          0x04  /* USB Status */
#define XHCI_OP_PAGESIZE        0x08  /* Page Size */
#define XHCI_OP_DNCTRL          0x14  /* Device Notification Control */
#define XHCI_OP_CRCR            0x18  /* Command Ring Control Register */
#define XHCI_OP_DCBAAP          0x30  /* Device Context Base Address Array Pointer */
#define XHCI_OP_CONFIG          0x38  /* Configure */

/* Port Status and Control Registers (offset from op_base + 0x400) */
#define XHCI_PORT_PORTSC        0x00  /* Port Status and Control */
#define XHCI_PORT_PORTPMSC      0x04  /* Port Power Management Status and Control */
#define XHCI_PORT_PORTLI        0x08  /* Port Link Info */
#define XHCI_PORT_PORTHLPMC     0x0C  /* Port Hardware LPM Control */

/* USB Command Register bits */
#define XHCI_CMD_RUN            (1 << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST          (1 << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE           (1 << 2)   /* Interrupter Enable */
#define XHCI_CMD_HSEE           (1 << 3)   /* Host System Error Enable */

/* USB Status Register bits */
#define XHCI_STS_HCH            (1 << 0)   /* HC Halted */
#define XHCI_STS_HSE            (1 << 2)   /* Host System Error */
#define XHCI_STS_EINT           (1 << 3)   /* Event Interrupt */
#define XHCI_STS_PCD            (1 << 4)   /* Port Change Detect */
#define XHCI_STS_CNR            (1 << 11)  /* Controller Not Ready */

/* Port Status and Control Register bits */
#define XHCI_PORTSC_CCS         (1 << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED         (1 << 1)   /* Port Enabled/Disabled */
#define XHCI_PORTSC_PR          (1 << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS_MASK    (0xF << 5) /* Port Link State */
#define XHCI_PORTSC_PP          (1 << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10)/* Port Speed */
#define XHCI_PORTSC_CSC         (1 << 17)  /* Connect Status Change */
#define XHCI_PORTSC_PRC         (1 << 21)  /* Port Reset Change */

/* Port Speed values */
#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4

/**
 * @brief xHCI controller state
 */
typedef struct xhci_controller {
    void     *cap_regs;      /**< Capability registers base */
    void     *op_regs;       /**< Operational registers base */
    void     *runtime_regs;  /**< Runtime registers base */
    void     *doorbell_regs; /**< Doorbell registers base */
    
    uint64_t  cap_regs_phys; /**< Physical address of cap regs */
    
    uint8_t   max_slots;     /**< Maximum device slots */
    uint8_t   max_ports;     /**< Number of ports */
    uint8_t   num_intrs;     /**< Number of interrupters */
    
    uint32_t *dcbaa;         /**< Device Context Base Address Array */
    uint64_t  dcbaa_phys;    /**< Physical address of DCBAA */
    
    int       initialized;   /**< Controller is ready */
} xhci_controller_t;

/* Function prototypes */

/**
 * @brief Initialize xHCI controller
 * 
 * Resets the controller, sets up registers, and prepares for device enumeration.
 * Must be called after PCI initialization has found the VL805.
 * 
 * @param base_addr Physical address of xHCI registers (from PCI BAR0)
 * @return 0 on success, negative error code on failure
 * 
 * @note Call this ONCE during system initialization
 * @since v56
 */
int xhci_init(uint64_t base_addr);

/**
 * @brief Check if xHCI controller is ready
 * 
 * @return 1 if initialized and ready, 0 otherwise
 * @since v56
 */
int xhci_is_ready(void);

/**
 * @brief Get xHCI controller instance
 * 
 * @return Pointer to controller structure, or NULL if not initialized
 * @since v56
 */
xhci_controller_t *xhci_get_controller(void);

/**
 * @brief Scan USB ports for connected devices
 * 
 * Checks all ports and enumerates newly connected devices.
 * Should be called after initialization and on port change interrupts.
 * 
 * @return Number of devices found, or negative on error
 * @since v56
 */
int xhci_scan_ports(void);

#endif /* USB_XHCI_H */
