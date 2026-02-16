/*
 * ipv6.c – IPv6 Protocol for RISC OS Phoenix
 * Includes packet handling, routing, ND (Neighbor Discovery)
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define IPV6_PROTO_ICMP6  58
#define IPV6_PROTO_TCP    6
#define IPV6_PROTO_UDP    17

typedef struct ipv6_hdr {
    uint32_t ver_tc_fl;  // Version (4 bits), Traffic Class (8), Flow Label (20)
    uint16_t payload_len;
    uint8_t  next_hdr;
    uint8_t  hop_limit;
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
} ipv6_hdr_t;

/* Local IPv6 address (config stub) */
static uint8_t local_ipv6[16] = {0x20,0x01,0x0d,0xb8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01};  // 2001:db8::1

/* IPv6 input – called from net_rx_packet */
void ipv6_input(netdev_t *dev, void *data, size_t len) {
    ipv6_hdr_t *ip6 = (ipv6_hdr_t*)(data + ETH_HDR_SIZE);
    uint8_t *payload = (uint8_t*)ip6 + 40;  // Fixed 40-byte header
    size_t payload_len = ntohs(ip6->payload_len);

    // Validate header
    if (((ip6->ver_tc_fl >> 28) != 6) || len < 40 + payload_len) return;

    // Destination check (stub – check if matches local or multicast)
    if (memcmp(ip6->dst_ip, local_ipv6, 16) != 0 && ip6->dst_ip[0] != 0xFF) return;

    switch (ip6->next_hdr) {
        case IPV6_PROTO_ICMP6: icmp6_input(dev, payload, payload_len); break;
        case IPV6_PROTO_TCP: tcp_input(dev, payload, payload_len); break;
        case IPV6_PROTO_UDP: udp_input(dev, payload, payload_len); break;
    }
}

/* IPv6 output – send packet */
void ipv6_output(netdev_t *dev, const uint8_t *dst_ip, uint8_t next_hdr, void *payload, size_t len) {
    uint8_t buf[ETH_MTU];
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    ipv6_hdr_t *ip6 = (ipv6_hdr_t*)(buf + ETH_HDR_SIZE);

    // Resolve MAC via ND (Neighbor Discovery – stub, similar to ARP)
    uint8_t dst_mac[6];
    if (nd_resolve(dev, dst_ip, dst_mac) != 0) return;

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, dev->mac, 6);
    eth->type = htons(ETH_P_IPV6);

    ip6->ver_tc_fl = htonl((6 << 28) | 0);  // Version 6, no flow
    ip6->payload_len = htons(len);
    ip6->next_hdr = next_hdr;
    ip6->hop_limit = 64;
    memcpy(ip6->src_ip, local_ipv6, 16);
    memcpy(ip6->dst_ip, dst_ip, 16);

    memcpy((uint8_t*)ip6 + 40, payload, len);

    net_tx_packet(dev, buf, 14 + 40 + len);
}

/* ICMPv6 input stub (e.g., Neighbor Solicitation/Advertisement) */
void icmp6_input(netdev_t *dev, void *data, size_t len) {
    // Handle ND, ping6, etc.
    // ... (implement echo reply, NS/NA)
}

/* Neighbor Discovery stub */
int nd_resolve(netdev_t *dev, const uint8_t *ip6, uint8_t *mac) {
    // Send NS, wait for NA – similar to ARP
    // ... (full ND implementation)
    return 0;
}

/* Module init – start IPv6 */
void ipv6_init(void) {
    debug_print("IPv6 initialized\n");
}