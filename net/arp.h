/*
 * arp.h – ARP (Address Resolution Protocol) Headers for RISC OS Phoenix
 * Defines arp_hdr_t and ARP constants
 * Author: R Andrews  – 10 Dec 2025
 */

#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#define ARP_HDR_LEN     28
#define ARP_HW_ETHER    1
#define ARP_PROTO_IP    0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct arp_hdr {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  src_mac[6];
    uint32_t src_ip;
    uint8_t  dst_mac[6];
    uint32_t dst_ip;
} arp_hdr_t;

/* boot370: updated signatures — PhoenixARP uses raw byte arrays,
 * no netdev_t dependency (genet_send() called directly).          */
void arp_init    (void);
void arp_input   (uint8_t *frame, int len);
int  arp_resolve (const uint8_t ip[4], uint8_t mac_out[6]);

#endif /* ARP_H */
