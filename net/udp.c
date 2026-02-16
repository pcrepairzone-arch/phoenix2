/*
 * udp.c – UDP Protocol for RISC OS Phoenix
 * Includes packet handling, checksum, socket integration
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "net.h"
#include "ipv4.h"
#include "ipv6.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

typedef struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

/* UDP input – from IP layer */
void udp_input(netdev_t *dev, void *data, size_t len) {
    udp_hdr_t *udp = data;
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t udp_len = ntohs(udp->length);

    if (udp_len < sizeof(udp_hdr_t) || udp_len > len) return;

    // Validate checksum (optional for IPv4, mandatory for IPv6)
    if (udp_checksum(data, udp_len, dev->ip_version) != 0) return;

    // Find socket
    socket_t *sock = socket_find_udp(dst_port);
    if (!sock) return;

    // Enqueue data
    ring_write(&sock->rx_queue, (uint8_t*)udp + sizeof(udp_hdr_t), udp_len - sizeof(udp_hdr_t));

    // Wake waiting task
    task_wakeup(sock->task);
}

/* UDP send */
int udp_send(socket_t *sock, const void *buf, size_t len, int flags) {
    if (sock->type != SOCK_DGRAM) return -1;

    uint8_t pkt[UDP_MAX_LEN];
    udp_hdr_t *udp = (udp_hdr_t*)pkt;

    udp->src_port = htons(sock->local_port);
    udp->dst_port = htons(sock->remote_port);
    udp->length = htons(sizeof(udp_hdr_t) + len);
    udp->checksum = 0;

    memcpy((uint8_t*)udp + sizeof(udp_hdr_t), buf, len);

    // Checksum
    udp->checksum = udp_checksum(pkt, sizeof(udp_hdr_t) + len, sock->domain == AF_INET ? 4 : 6);

    // Output via IP
    if (sock->domain == AF_INET) {
        ipv4_output(sock->dev, sock->remote_addr, IP_PROTO_UDP, pkt, sizeof(udp_hdr_t) + len);
    } else if (sock->domain == AF_INET6) {
        ipv6_output(sock->dev, sock->remote_ipv6, IPV6_PROTO_UDP, pkt, sizeof(udp_hdr_t) + len);
    }

    return len;
}

/* UDP checksum (IPv4 pseudo-header) */
uint16_t udp_checksum(void *data, size_t len, int ip_ver) {
    uint32_t sum = 0;
    uint16_t *p = data;

    // Pseudo-header for IPv4
    if (ip_ver == 4) {
        sum += (local_ip >> 16) + (local_ip & 0xFFFF);
        sum += (dst_ip >> 16) + (dst_ip & 0xFFFF);
        sum += IP_PROTO_UDP;
        sum += len;
    } else if (ip_ver == 6) {
        // IPv6 pseudo-header (stub)
    }

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(uint8_t*)p;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return ~sum;
}

/* Find UDP socket by port */
socket_t *socket_find_udp(uint16_t port) {
    for (int i = 0; i < num_sockets; i++) {
        socket_t *s = &sockets[i];
        if (s->type == SOCK_DGRAM && s->local_port == port) return s;
    }
    return NULL;
}

/* Init UDP subsystem */
void udp_init(void) {
    debug_print("UDP initialized\n");
}