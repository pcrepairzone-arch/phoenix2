/*
 * usb_hub.c — USB Hub Class Driver (class code 9)
 *
 * Handles USB 2.0 and USB 3.0 external hubs enumerated by the xHCI driver.
 * On probe it powers hub ports, resets connected ports, determines device
 * speed from GET_STATUS, and calls xhci_enumerate_hub_port() for each live
 * downstream device.
 *
 * The VIA Labs VL812 USB 3.0 hub on the DeskPi Pro (VID=2109, PID=3431) is
 * the primary target.  Downstream devices (e.g. SanDisk USB drive) are
 * reached through this hub.
 *
 * boot178: Speed detection fix — for a USB2 hub's port, wPortStatus bits:
 *   bit 9  (PS_LS_DEV) = 1 → Low-Speed  (1.5 Mbps)
 *   bit 10 (PS_HS_DEV) = 1 → High-Speed (480 Mbps)
 *   neither set         → Full-Speed (12 Mbps), NOT SuperSpeed.
 * Previous code fell through to XHCI_SPEED_SS which caused Address Device
 * timeouts for FS keyboards/mice.
 *
 * boot178: hub_poll_hotplug() added — polls each hub port for C_PORT_CONNECTION
 * change bits and enumerates newly connected devices.  Called from wimp_task.
 */

#include "usb.h"
#include "usb_xhci.h"

/* uart_puts is non-static — exported from uart.c */
extern void uart_puts(const char *s);

/* print_hex32 and get_time_ms are static in usb_xhci.c, so replicate
 * minimal private versions here rather than changing linkage. */
static void hub_print_hex(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[11] = "0x00000000\0";
    for (int i = 7; i >= 0; i--) { buf[2+i] = h[v & 0xF]; v >>= 4; }
    uart_puts(buf);
}

/* System timer: ARM CNTPCT_EL0. On Pi4 CNTFRQ=54 MHz → 1ms ≈ 54000 ticks. */
static inline uint32_t hub_ms(void) {
    uint64_t cnt, freq;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)(cnt / (freq / 1000ULL));
}

/* ── USB Hub class-specific request codes ──────────────────────────────── */
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_SET_CONFIG      0x09

/* bmRequestType for hub port feature operations (class, other recipient) */
#define HUB_PORT_OUT_TYPE  0x23   /* OUT: host→hub-port              */
#define HUB_PORT_IN_TYPE   0xA3   /* IN:  hub-port→host              */
/* Hub-level descriptor request */
#define HUB_CLASS_IN_TYPE  0xA0   /* IN:  hub→host (class, device)   */

/* Hub Feature Selectors (USB 2.0 spec Table 11-17) */
#define PORT_CONNECTION   0
#define PORT_ENABLE       1
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET      20

/* Hub Descriptor types */
#define USB_DT_HUB      0x29
#define USB_DT_SS_HUB   0x2A

/* Port Status word bits (USB 2.0 spec §11.24.2.7) */
#define PS_CONNECTION   (1U << 0)
#define PS_ENABLE       (1U << 1)
#define PS_RESET        (1U << 4)
#define PS_POWER        (1U << 8)
#define PS_LS_DEV       (1U << 9)   /* Low-Speed device attached       */
#define PS_HS_DEV       (1U << 10)  /* High-Speed device attached      */
/* Port Change bits */
#define PC_C_CONNECTION (1U << 0)
#define PC_C_RESET      (1U << 4)

/* xHCI speed codes */
#define XHCI_SPEED_FS  1
#define XHCI_SPEED_LS  2
#define XHCI_SPEED_HS  3
#define XHCI_SPEED_SS  4

/* Helpers */
static void hub_delay_ms(uint32_t ms) {
    uint32_t end = hub_ms() + ms;
    while (hub_ms() < end) { asm volatile("nop"); }
}

/* ── Persistent hub state (for hotplug polling) ──────────────────────────── */
static usb_device_t *g_hub_dev       = NULL;
static uint8_t       g_hub_num_ports = 0;

/* ── Hub Descriptor ─────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;      /* in 2 ms units */
    uint8_t  bHubContrCurrent;
    uint8_t  data[8];             /* DeviceRemovable + PortPwrCtrlMask */
} __attribute__((packed)) hub_descriptor_t;

/* ── Port GET_STATUS reply (4 bytes) ────────────────────────────────────── */
typedef struct {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} __attribute__((packed)) port_status_t;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int hub_get_port_status(usb_device_t *dev, uint8_t port,
                                port_status_t *ps) {
    uint8_t buf[4] = {0};
    int r = usb_control_transfer(dev, HUB_PORT_IN_TYPE, USB_REQ_GET_STATUS,
                                  0, port, buf, 4, 200);
    if (r < 0) return -1;
    ps->wPortStatus = (uint16_t)(buf[0] | (buf[1] << 8));
    ps->wPortChange = (uint16_t)(buf[2] | (buf[3] << 8));
    return 0;
}

static int hub_set_port_feature(usb_device_t *dev, uint8_t port, uint16_t feat) {
    return usb_control_transfer(dev, HUB_PORT_OUT_TYPE, USB_REQ_SET_FEATURE,
                                 feat, port, NULL, 0, 200);
}

static int hub_clear_port_feature(usb_device_t *dev, uint8_t port, uint16_t feat) {
    return usb_control_transfer(dev, HUB_PORT_OUT_TYPE, USB_REQ_CLEAR_FEATURE,
                                 feat, port, NULL, 0, 200);
}

/* ── probe ──────────────────────────────────────────────────────────────── */

static int hub_probe(usb_device_t *dev, usb_interface_t *intf) {
    (void)intf;
    uart_puts("[HUB] hub_probe: VID="); hub_print_hex(dev->idVendor);
    uart_puts(" PID="); hub_print_hex(dev->idProduct); uart_puts("\n");
    /* Save reference for hub_poll_hotplug() */
    g_hub_dev = dev;

    /* ── SET_CONFIGURATION(1) ────────────────────────────────────────── */
    usb_control_transfer(dev, 0x00, USB_REQ_SET_CONFIG, 1, 0, NULL, 0, 200);

    /* ── GET_DESCRIPTOR(Hub) ─────────────────────────────────────────── */
    uint8_t hd_buf[16] = {0};
    int r = usb_control_transfer(dev, HUB_CLASS_IN_TYPE, USB_REQ_GET_DESCRIPTOR,
                                  USB_DT_HUB << 8, 0, hd_buf, sizeof(hd_buf), 200);
    if (r < 4) {
        /* Try SuperSpeedHub descriptor (0x2A) */
        r = usb_control_transfer(dev, HUB_CLASS_IN_TYPE, USB_REQ_GET_DESCRIPTOR,
                                  USB_DT_SS_HUB << 8, 0, hd_buf, sizeof(hd_buf), 200);
    }

    hub_descriptor_t *hd = (hub_descriptor_t *)hd_buf;
    uint8_t num_ports   = (r >= 3) ? hd->bNbrPorts : 4;   /* default 4 */
    uint32_t pwrgood_ms = (r >= 5) ? (uint32_t)hd->bPwrOn2PwrGood * 2U : 100U;

    uart_puts("[HUB] num_ports="); hub_print_hex(num_ports);
    uart_puts(" pwrgood_ms="); hub_print_hex(pwrgood_ms); uart_puts("\n");

    if (num_ports == 0 || num_ports > 15) num_ports = 4;
    if (pwrgood_ms < 100) pwrgood_ms = 100;  /* spec minimum 100ms */
    g_hub_num_ports = num_ports;

    /* ── Power on all ports ──────────────────────────────────────────── */
    for (uint8_t p = 1; p <= num_ports; p++)
        hub_set_port_feature(dev, p, PORT_POWER);

    hub_delay_ms(pwrgood_ms);

    /* ── Per-port reset and enumeration ─────────────────────────────── */
    for (uint8_t p = 1; p <= num_ports; p++) {
        port_status_t ps;
        if (hub_get_port_status(dev, p, &ps) != 0) {
            uart_puts("[HUB]   Port "); hub_print_hex(p);
            uart_puts(": GET_STATUS failed\n");
            continue;
        }
        uart_puts("[HUB]   Port "); hub_print_hex(p);
        uart_puts(": status="); hub_print_hex(ps.wPortStatus);
        uart_puts(" change="); hub_print_hex(ps.wPortChange); uart_puts("\n");

        if (!(ps.wPortStatus & PS_CONNECTION)) {
            uart_puts("[HUB]   Port "); hub_print_hex(p);
            uart_puts(": not connected\n");
            continue;
        }

        /* Clear C_PORT_CONNECTION if set */
        if (ps.wPortChange & PC_C_CONNECTION)
            hub_clear_port_feature(dev, p, C_PORT_CONNECTION);

        /* ── Reset the port ─────────────────────────────────────────── */
        hub_set_port_feature(dev, p, PORT_RESET);

        /* Wait up to 500 ms for C_PORT_RESET */
        int reset_ok = 0;
        for (int t = 0; t < 50; t++) {
            hub_delay_ms(10);
            if (hub_get_port_status(dev, p, &ps) != 0) break;
            if (ps.wPortChange & PC_C_RESET) { reset_ok = 1; break; }
        }
        if (!reset_ok) {
            uart_puts("[HUB]   Port "); hub_print_hex(p);
            uart_puts(": reset timeout\n");
            continue;
        }
        hub_clear_port_feature(dev, p, C_PORT_RESET);

        /* Re-read status after reset */
        if (hub_get_port_status(dev, p, &ps) != 0) continue;

        if (!(ps.wPortStatus & PS_ENABLE)) {
            uart_puts("[HUB]   Port "); hub_print_hex(p);
            uart_puts(": not enabled after reset\n");
            continue;
        }

        /* ── Determine device speed ──────────────────────────────────── */
        /* USB2 hub wPortStatus speed bits (§11.24.2.7):
         *   bit9  PS_LS_DEV = Low-Speed  (1.5 Mbps)
         *   bit10 PS_HS_DEV = High-Speed (480 Mbps)
         *   neither set     = Full-Speed (12 Mbps)   ← boot178 fix
         * SuperSpeed devices appear on separate SS companion ports
         * with a different wPortStatus encoding (USB3 §10.16.2.6). */
        uint32_t spd;
        if      (ps.wPortStatus & PS_LS_DEV) spd = XHCI_SPEED_LS;
        else if (ps.wPortStatus & PS_HS_DEV) spd = XHCI_SPEED_HS;
        else                                 spd = XHCI_SPEED_FS; /* boot178: FS not SS */

        uart_puts("[HUB]   Port "); hub_print_hex(p);
        uart_puts(": device connected, speed="); hub_print_hex(spd); uart_puts("\n");

        /* ── Enumerate via xHCI ──────────────────────────────────────── */
        hub_delay_ms(10);  /* brief settle after reset */
        if (xhci_enumerate_hub_port(dev, p, spd) != 0) {
            uart_puts("[HUB]   Port "); hub_print_hex(p);
            uart_puts(": enumerate failed\n");
        }
    }

    uart_puts("[HUB] hub_probe done\n");
    return 0;
}

static void hub_disconnect(usb_device_t *dev) {
    (void)dev;
}

/*
 * hub_poll_hotplug — check all hub ports for connect/disconnect changes.
 *
 * Called periodically from the wimp_task main loop (boot178).  Issues
 * GET_PORT_STATUS for each port and acts on C_PORT_CONNECTION changes:
 *   connect   → reset the port and enumerate the new device
 *   disconnect → print message (slot cleanup is future work)
 *
 * Rate-limited by the caller (every ~500 ms is sufficient).
 */
void hub_poll_hotplug(void)
{
    if (!g_hub_dev || g_hub_num_ports == 0) return;

    for (uint8_t p = 1; p <= g_hub_num_ports; p++) {
        port_status_t ps;
        if (hub_get_port_status(g_hub_dev, p, &ps) != 0) continue;

        /* Only act when the connection-change bit is set */
        if (!(ps.wPortChange & PC_C_CONNECTION)) continue;

        /* Clear the C_PORT_CONNECTION change bit */
        hub_clear_port_feature(g_hub_dev, p, C_PORT_CONNECTION);

        if (ps.wPortStatus & PS_CONNECTION) {
            /* ── New device: reset port then enumerate ───────────────── */
            uart_puts("[HUB] Hotplug: device connected on port ");
            hub_print_hex(p); uart_puts(" — resetting...\n");

            hub_set_port_feature(g_hub_dev, p, PORT_RESET);

            /* Wait for C_PORT_RESET (up to 500 ms) */
            int reset_ok = 0;
            for (int t = 0; t < 50; t++) {
                hub_delay_ms(10);
                if (hub_get_port_status(g_hub_dev, p, &ps) != 0) break;
                if (ps.wPortChange & PC_C_RESET) { reset_ok = 1; break; }
            }
            if (!reset_ok) {
                uart_puts("[HUB] Hotplug: reset timeout on port ");
                hub_print_hex(p); uart_puts("\n");
                continue;
            }
            hub_clear_port_feature(g_hub_dev, p, C_PORT_RESET);

            if (hub_get_port_status(g_hub_dev, p, &ps) != 0) continue;
            if (!(ps.wPortStatus & PS_ENABLE)) {
                uart_puts("[HUB] Hotplug: port not enabled after reset\n");
                continue;
            }

            /* Determine speed (same logic as hub_probe) */
            uint32_t spd;
            if      (ps.wPortStatus & PS_LS_DEV) spd = XHCI_SPEED_LS;
            else if (ps.wPortStatus & PS_HS_DEV) spd = XHCI_SPEED_HS;
            else                                 spd = XHCI_SPEED_FS;

            uart_puts("[HUB] Hotplug: enumerating port ");
            hub_print_hex(p);
            uart_puts(" speed="); hub_print_hex(spd); uart_puts("\n");

            hub_delay_ms(10);
            if (xhci_enumerate_hub_port(g_hub_dev, p, spd) != 0) {
                uart_puts("[HUB] Hotplug: enumerate failed on port ");
                hub_print_hex(p); uart_puts("\n");
            }
        } else {
            /* ── Device disconnected ─────────────────────────────────── */
            uart_puts("[HUB] Hotplug: device disconnected from port ");
            hub_print_hex(p); uart_puts("\n");
            /* TODO: Disable Slot + free hid_device entry (future work) */
        }
    }
}

/* ── Registration ────────────────────────────────────────────────────────── */

static usb_class_driver_t hub_driver = {
    .name       = "usb-hub",
    .class_code = 9,
    .probe      = hub_probe,
    .disconnect = hub_disconnect,
};

int usb_hub_init(void) {
    usb_register_class_driver(&hub_driver);
    return 0;
}
