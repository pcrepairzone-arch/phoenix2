/*
 * tcpip.c – PhoenixNet TCP/IP Stack for RISC OS Phoenix
 * Full 64-bit IPv4/IPv6, TCP/UDP/ICMP, sockets, multi-core
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "net.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "tcp.h"
#include "udp.h"
#include "socket.h"

#define NET_RX_QUEUES   4
#define NET_TX_QUEUES   4

/* Per-CPU network queues */
static net_queue_t rx_queue[NET_RX_QUEUES];
static net_queue_t tx_queue[NET_TX_QUEUES];

/* Network device registration */
void netdev_register(netdev_t *dev)
{
    dev->rx_queue = &rx_queue[get_cpu_id() % NET_RX_QUEUES];
    dev->tx_queue = &tx_queue[get_cpu_id() % NET_TX_QUEUES];
    debug_print("Net: %s registered (MAC %02x:%02x:%02x:%02x:%02x:%02x)\n",
                dev->name,
                dev->mac[0], dev->mac[1], dev->mac[2],
                dev->mac[3], dev->mac[4], dev->mac[5]);
}

/* Packet receive – called from NIC driver */
void net_rx_packet(netdev_t *dev, void *data, size_t len)
{
    eth_hdr_t *eth = data;

    switch (htons(eth->type)) {
        case ETH_P_ARP:  arp_input(dev, data, len); break;
        case ETH_P_IP:   ipv4_input(dev, data, len); break;
        case ETH_P_IPV6: ipv6_input(dev, data, len); break;
    }
}

/* Socket API */
int socket(int domain, int type, int protocol)
{
    return socket_create(domain, type, protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return socket_bind(sock, addr, addrlen);
}

int listen(int sockfd, int backlog)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return tcp_listen(sock, backlog);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return tcp_accept(sock, addr, addrlen);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return tcp_connect(sock, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return socket_send(sock, buf, len, flags);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    socket_t *sock = socket_get(sockfd);
    if (!sock) return -1;
    return socket_recv(sock, buf, len, flags);
}

/* Module init – start networking */
_kernel_oserror *module_init(const char *arg, int podule)
{
    for (int i = 0; i < NET_RX_QUEUES; i++)
        net_queue_init(&rx_queue[i]);
    for (int i = 0; i < NET_TX_QUEUES; i++)
        net_queue_init(&tx_queue[i]);

    arp_init();
    ipv4_init();
    ipv6_init();
    tcp_init();
    udp_init();
    socket_init();

    debug_print("PhoenixNet: TCP/IP stack initialized – IPv6 ready\n");
    return NULL;
}