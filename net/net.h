/*
 * net.h – Network Headers for RISC OS Phoenix
 * Defines netdev_t, socket_t, eth_hdr_t, and constants
 * Author:R Andrews Grok 4 – 10 Dec 2025
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

#define ETH_HDR_SIZE    14
#define ETH_MTU         1500
#define ETH_P_ARP       0x0806
#define ETH_P_IP        0x0800
#define ETH_P_IPV6      0x86DD

typedef struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} eth_hdr_t;

typedef struct netdev {
    char     name[16];
    uint8_t  mac[6];
    net_queue_t *rx_queue;
    net_queue_t *tx_queue;
    void     (*tx_func)(struct netdev *dev, void *pkt, size_t len);
    // IP config, MTU, etc.
} netdev_t;

typedef struct net_queue {
    void    *packets[1024];
    size_t   sizes[1024];
    int      head, tail;
    spinlock_t lock;
} net_queue_t;

#define AF_INET         2
#define AF_INET6        10
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

typedef struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} sockaddr_t;

typedef struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char     sin_zero[8];
} sockaddr_in_t;

typedef struct socket socket_t;

void netdev_register(netdev_t *dev);
void net_rx_packet(netdev_t *dev, void *data, size_t len);
void net_tx_packet(netdev_t *dev, void *pkt, size_t len);

void net_queue_init(net_queue_t *q);
void net_queue_enqueue(net_queue_t *q, void *pkt, size_t len);
int net_queue_dequeue(net_queue_t *q, void **pkt, size_t *len);

int socket_create(int domain, int type, int protocol);
socket_t *socket_get(int fd);
int socket_bind(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen);
int socket_listen(socket_t *sock, int backlog);
int socket_accept(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen);
int socket_connect(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen);
ssize_t socket_send(socket_t *sock, const void *buf, size_t len, int flags);
ssize_t socket_recv(socket_t *sock, void *buf, size_t len, int flags);

void arp_init(void);
void arp_input(netdev_t *dev, void *data, size_t len);
int arp_resolve(netdev_t *dev, uint32_t ip, uint8_t *mac);

void ipv4_init(void);
void ipv4_input(netdev_t *dev, void *data, size_t len);
void ipv4_output(netdev_t *dev, uint32_t dst_ip, uint8_t proto, void *payload, size_t len);

void ipv6_init(void);
void ipv6_input(netdev_t *dev, void *data, size_t len);
void ipv6_output(netdev_t *dev, const uint8_t *dst_ip, uint8_t next_hdr, void *payload, size_t len);

void tcp_init(void);
void tcp_input(netdev_t *dev, void *data, size_t len);

void udp_init(void);
void udp_input(netdev_t *dev, void *data, size_t len);

uint16_t ip_checksum(void *data, size_t len);

#endif /* NET_H */