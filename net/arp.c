/*
 * arp.c – ARP (Address Resolution Protocol) for RISC OS Phoenix
 * Handles ARP requests/replies, cache management
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "net.h"
#include "ethernet.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define ARP_CACHE_SIZE  128
#define ARP_TIMEOUT     3600000  // 1 hour in ms

#define ARP_HW_ETHER    1
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

typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint64_t expire_time;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_count = 0;
static spinlock_t arp_lock = SPINLOCK_INIT;

/* ARP input – from Ethernet */
void arp_input(netdev_t *dev, void *data, size_t len) {
    arp_hdr_t *arp = (arp_hdr_t*)(data + ETH_HDR_SIZE);

    if (len < sizeof(arp_hdr_t) || ntohs(arp->hw_type) != ARP_HW_ETHER ||
        ntohs(arp->proto_type) != ETH_P_IP || arp->hw_len != 6 ||
        arp->proto_len != 4) return;

    // Update cache with sender
    arp_cache_update(arp->src_ip, arp->src_mac);

    if (ntohs(arp->opcode) == ARP_OP_REQUEST && arp->dst_ip == local_ip) {
        arp_send_reply(dev, arp->src_ip, arp->src_mac);
    }
}

/* Send ARP reply */
void arp_send_reply(netdev_t *dev, uint32_t dst_ip, uint8_t *dst_mac) {
    uint8_t buf[ETH_HDR_LEN + sizeof(arp_hdr_t)];
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    arp_hdr_t *arp = (arp_hdr_t*)(buf + ETH_HDR_LEN);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, dev->mac, 6);
    eth->type = htons(ETH_P_ARP);

    arp->hw_type = htons(ARP_HW_ETHER);
    arp->proto_type = htons(ETH_P_IP);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REPLY);
    memcpy(arp->src_mac, dev->mac, 6);
    arp->src_ip = local_ip;
    memcpy(arp->dst_mac, dst_mac, 6);
    arp->dst_ip = dst_ip;

    net_tx_packet(dev, buf, sizeof(buf));
}

/* Resolve IP to MAC – request if not cached */
int arp_resolve(netdev_t *dev, uint32_t ip, uint8_t *mac) {
    unsigned long flags;
    spin_lock_irqsave(&arp_lock, &flags);

    for (int i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].ip == ip && get_time_ms() < arp_cache[i].expire_time) {
            memcpy(mac, arp_cache[i].mac, 6);
            spin_unlock_irqrestore(&arp_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&arp_lock, flags);

    // Send ARP request
    uint8_t buf[ETH_HDR_LEN + sizeof(arp_hdr_t)];
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    arp_hdr_t *arp = (arp_hdr_t*)(buf + ETH_HDR_LEN);

    memset(eth->dst, 0xFF, 6);  // Broadcast
    memcpy(eth->src, dev->mac, 6);
    eth->type = htons(ETH_P_ARP);

    arp->hw_type = htons(ARP_HW_ETHER);
    arp->proto_type = htons(ETH_P_IP);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REQUEST);
    memcpy(arp->src_mac, dev->mac, 6);
    arp->src_ip = local_ip;
    memset(arp->dst_mac, 0, 6);
    arp->dst_ip = ip;

    net_tx_packet(dev, buf, sizeof(buf));

    // Wait for reply (poll cache with timeout)
    uint64_t end = get_time_ms() + 1000;
    while (get_time_ms() < end) {
        yield();
        spin_lock_irqsave(&arp_lock, &flags);
        for (int i = 0; i < arp_cache_count; i++) {
            if (arp_cache[i].ip == ip) {
                memcpy(mac, arp_cache[i].mac, 6);
                spin_unlock_irqrestore(&arp_lock, flags);
                return 0;
            }
        }
        spin_unlock_irqrestore(&arp_lock, flags);
    }

    return -1;  // Timeout
}

/* Update ARP cache */
void arp_cache_update(uint32_t ip, uint8_t *mac) {
    unsigned long flags;
    spin_lock_irqsave(&arp_lock, &flags);

    for (int i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].expire_time = get_time_ms() + ARP_TIMEOUT;
            spin_unlock_irqrestore(&arp_lock, flags);
            return;
        }
    }

    if (arp_cache_count < ARP_CACHE_SIZE) {
        arp_cache[arp_cache_count].ip = ip;
        memcpy(arp_cache[arp_cache_count].mac, mac, 6);
        arp_cache[arp_cache_count].expire_time = get_time_ms() + ARP_TIMEOUT;
        arp_cache_count++;
    } else {
        // Evict oldest (stub – FIFO)
        memmove(arp_cache, arp_cache + 1, (ARP_CACHE_SIZE - 1) * sizeof(arp_entry_t));
        arp_cache[ARP_CACHE_SIZE - 1].ip = ip;
        memcpy(arp_cache[ARP_CACHE_SIZE - 1].mac, mac, 6);
        arp_cache[ARP_CACHE_SIZE - 1].expire_time = get_time_ms() + ARP_TIMEOUT;
    }

    spin_unlock_irqrestore(&arp_lock, flags);
}

/* Init ARP subsystem */
void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    spinlock_init(&arp_lock);
    debug_print("ARP initialized\n");
}