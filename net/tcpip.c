/*
 * tcpip.c – PhoenixTCPIP frame dispatcher
 * boot370: replaces inline ARP/ICMP handlers in kernel/lib.c.
 * EtherType demux: ARP → arp_input, IPv4 → ipv4_input, IPv6 → (stub).
 * Called from wimp_task RX drain loop as net_rx_frame(g_rx_frame, flen).
 *
 * RISC OS Phoenix module pattern: self-contained, private state,
 * clean init + dispatch API. Stack init order in kernel.c:
 *   tcpip_init() calls arp_init() → ipv4_init() → ipv6_init()
 *                      → tcp_init() → udp_init()
 *
 * Author: R Andrews – boot370
 */

#include "kernel.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/dhcp.h"

/* Sub-protocol input handlers (implemented in their own modules) */
void arp_input  (uint8_t *frame, int len);
void ipv4_input (uint8_t *frame, int len);
void ipv6_input (uint8_t *frame, int len);

/* Sub-protocol init functions */
void arp_init  (void);
void ipv4_init (void);
void ipv6_init (void);
void tcp_init  (void);
void udp_init  (void);

/*
 * tcpip_init — initialise the entire Phoenix TCP/IP stack.
 * Called once from kernel.c after genet_init() and module_init_all()
 * (DHCP module init blocks there until BOUND, so g_our_ip is valid
 * before any frame arrives).
 */
void tcpip_init(void)
{
    arp_init();
    ipv4_init();
    ipv6_init();
    tcp_init();
    udp_init();

    debug_print("[TCPIP] PhoenixTCPIP dispatcher ready\n");
    debug_print("[TCPIP] ARP table | IPv4 ICMP | TCP client | UDP stub\n");
}

/*
 * net_rx_frame — EtherType dispatcher.
 * Called from the wimp_task frame-drain loop once per received frame.
 * All protocol handlers receive the COMPLETE Ethernet frame (byte 0
 * onwards) so they can access MAC addresses and rebuild replies.
 *
 *   frame[0..5]   dst MAC
 *   frame[6..11]  src MAC
 *   frame[12..13] EtherType (big-endian)
 *   frame[14..]   protocol payload
 */
void net_rx_frame(uint8_t *frame, int len)
{
    if (len < ETH_HDR_LEN) return;

    uint16_t etype = (uint16_t)((frame[12] << 8) | frame[13]);

    switch (etype) {

    case ETH_P_ARP:
        /* Minimum: Eth(14) + ARP(28) = 42 bytes */
        if (len >= 42)
            arp_input(frame, len);
        break;

    case ETH_P_IP:
        /* Minimum: Eth(14) + IP min header(20) + 1 byte = 35 */
        if (len >= 34)
            ipv4_input(frame, len);
        break;

    case ETH_P_IPV6:
        /* Minimum: Eth(14) + IPv6 header(40) = 54 */
        if (len >= 54)
            ipv6_input(frame, len);
        break;

    default:
        /* Silently drop: LLDP, CDP, 802.1Q, etc. */
        break;
    }
}
