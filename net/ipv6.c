/*
 * ipv6.c – PhoenixIPv6 stub
 * boot370: ipv6_input() stub added so net_rx_frame() can handle
 * IPv6 EtherType without a linker error.
 * Author: R Andrews – boot370
 */
#include "kernel.h"

void ipv6_init(void)
{
    debug_print("[IPv6] PhoenixIPv6 ready (stub — silently dropping)\n");
}

/*
 * ipv6_input — called from tcpip.c when EtherType == 0x86DD.
 * Silently drops all IPv6 frames (multicast NDP, etc.) until a
 * proper IPv6 implementation is added.
 */
void ipv6_input(uint8_t *frame, int len)
{
    (void)frame;
    (void)len;
    /* TODO: NDP, ICMPv6, dual-stack */
}
