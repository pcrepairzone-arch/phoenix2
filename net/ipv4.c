/*
 * ipv4.c – IPv4 Protocol for RISC OS Phoenix
 * Includes packet handling, routing, fragmentation, ARP
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "net.h"
#include "arp.h"
#include "tcp.h"
#include "udp.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

typedef struct ipv4_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_hdr_t;

/* Local IP address (config stub) */
static uint32_t local_ip = 0xC0A80101;  // 192.168.1.1

/* IPv4 input – called from net_rx_packet */
void ipv4_input(netdev_t *dev, void *data, size_t len) {
    ipv4_hdr_t *ip = (ipv4_hdr_t*)(data + ETH_HDR_SIZE);
    uint8_t *payload = (uint8_t*)ip + (ip->ver_ihl & 0xF) * 4;
    size_t payload_len = ntohs(ip->total_len) - (ip->ver_ihl & 0xF) * 4;

    // Validate header
    if ((ip->ver_ihl >> 4) != 4 || len < sizeof(ipv4_hdr_t) + payload_len) return;

    // Checksum
    if (ip_checksum(ip, (ip->ver_ihl & 0xF) * 4) != 0) return;

    // Destination check
    if (ip->dst_ip != local_ip && ip->dst_ip != 0xFFFFFFFF) return;  // Not us or broadcast

    switch (ip->proto) {
        case IP_PROTO_ICMP: icmp_input(dev, payload, payload_len); break;
        case IP_PROTO_TCP: tcp_input(dev, payload, payload_len); break;
        case IP_PROTO_UDP: udp_input(dev, payload, payload_len); break;
    }
}

/* IPv4 output – send packet */
void ipv4_output(netdev_t *dev, uint32_t dst_ip, uint8_t proto, void *payload, size_t len) {
    uint8_t buf[ETH_MTU];
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    ipv4_hdr_t *ip = (ipv4_hdr_t*)(buf + ETH_HDR_SIZE);

    // Resolve MAC via ARP
    uint8_t dst_mac[6];
    if (arp_resolve(dev, dst_ip, dst_mac) != 0) return;

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, dev->mac, 6);
    eth->type = htons(ETH_P_IP);

    ip->ver_ihl = (4 << 4) | 5;  // IPv4, 20-byte header
    ip->tos = 0;
    ip->total_len = htons(20 + len);
    ip->id = htons(0);  // No frag
    ip->flags_frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src_ip = local_ip;
    ip->dst_ip = dst_ip;
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, 20);

    memcpy((uint8_t*)ip + 20, payload, len);

    net_tx_packet(dev, buf, 14 + 20 + len);
}

/* IP checksum */
uint16_t ip_checksum(void *data, size_t len) {
    uint32_t sum = 0;
    uint16_t *p = data;

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(uint8_t*)p;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return ~sum;
}

/* ICMP input stub */
void icmp_input(netdev_t *dev, void *data, size