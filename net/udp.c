/*
 * udp.c – PhoenixUDP stub
 * boot370: udp_rx() stub added so ipv4_input() can dispatch UDP frames
 * without a linker error. Full UDP implementation is future work.
 * Author: R Andrews – boot370
 */
#include "kernel.h"

void udp_init(void)
{
    debug_print("[UDP] PhoenixUDP ready (stub)\n");
}

/*
 * udp_rx — called from ipv4_input when IP protocol == 0x11 (UDP).
 * Silently discards all UDP frames until the full implementation lands.
 * frame[0] is Ethernet header byte 0 (same convention as tcp_rx).
 */
void udp_rx(uint8_t *frame, int len)
{
    (void)frame;
    (void)len;
    /* TODO boot371+: demux by destination port, deliver to sockets */
}
