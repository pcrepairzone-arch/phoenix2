/*
 * bluetooth_stub.c - Minimal Bluetooth stub for Phoenix
 * Prevents build errors until SDIO implemented
 */

#include "kernel.h"

int bluetooth_init(void)
{
    debug_print("BT: Stub - waiting for SDIO implementation\n");
    return 0;
}
