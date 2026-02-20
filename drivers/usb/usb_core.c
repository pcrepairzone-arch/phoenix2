/*
 * usb_core.c - USB core for Phoenix RISC OS
 * Device enumeration and class driver management
 */

#include "kernel.h"
#include "usb.h"

static usb_class_driver_t *class_drivers[16];
static int class_driver_count = 0;

/* Register USB class driver */
void usb_register_class_driver(usb_class_driver_t *driver)
{
    if (class_driver_count >= 16) {
        debug_print("USB: Too many class drivers\n");
        return;
    }
    
    class_drivers[class_driver_count++] = driver;
    debug_print("USB: Registered class driver: %s (class 0x%02x)\n",
               driver->name, driver->class_code);
}

/* Control transfer stub */
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, int timeout)
{
    /* TODO: Implement via xHCI control endpoint
     * Setup packet → Data stage → Status stage
     */
    debug_print("USB: control_transfer (stub) req=0x%02x\n", request);
    return -1;
}

/* Bulk transfer stub */
int usb_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout)
{
    /* TODO: Implement via xHCI bulk rings */
    return -1;
}

/* Interrupt transfer stub */
int usb_interrupt_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout)
{
    /* TODO: Implement via xHCI interrupt rings */
    return -1;
}

/* Enumerate new USB device */
int usb_enumerate_device(usb_device_t *dev, int port)
{
    debug_print("USB: Enumerating device on port %d\n", port);
    
    /* TODO:
     * 1. Get device descriptor (8 bytes)
     * 2. Set address
     * 3. Get full device descriptor
     * 4. Get configuration descriptor
     * 5. Parse interfaces and endpoints
     * 6. Probe class drivers
     */
    
    /* Probe class drivers */
    for (int i = 0; i < class_driver_count; i++) {
        usb_class_driver_t *drv = class_drivers[i];
        
        for (int j = 0; j < dev->num_interfaces; j++) {
            usb_interface_t *intf = &dev->interfaces[j];
            
            if (intf->bInterfaceClass == drv->class_code) {
                debug_print("USB: Probing %s for interface %d\n",
                           drv->name, intf->bInterfaceNumber);
                
                if (drv->probe(dev, intf) == 0) {
                    debug_print("USB: %s driver bound\n", drv->name);
                }
            }
        }
    }
    
    return 0;
}

/* USB subsystem initialization */
int usb_init(void)
{
    debug_print("USB: Initializing USB subsystem\n");
    
    /* TODO:
     * 1. Initialize xHCI host controller
     * 2. Register HID class driver
     * 3. Register mass storage class driver
     * 4. Enumerate existing devices
     */
    
    debug_print("USB: Subsystem initialized (stub)\n");
    return 0;
}
