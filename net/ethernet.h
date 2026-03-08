/*
 * ethernet.h – Ethernet Headers for RISC OS Phoenix
 * Defines eth_hdr_t and Ethernet constants
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ADDR_LEN    6
#define ETH_HDR_LEN     14
#define ETH_MTU         1500

#define ETH_P_ARP       0x0806
#define ETH_P_IP        0x0800
#define ETH_P_IPV6      0x86DD

typedef struct eth_hdr {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t type;
} eth_hdr_t;

uint16_t htons(uint16_t val);
uint16_t ntohs(uint16_t val);
uint32_t htonl(uint32_t val);
uint32_t ntohl(uint32_t val);

#endif /* ETHERNET_H */