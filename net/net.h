/*
 * net.h – PhoenixNet public API
 * boot370: complete rewrite. Removed Linux-derived netdev_t / net_queue_t /
 * socket_t cruft that referenced undefined types (spinlock_t, socklen_t).
 * All protocol modules use raw uint8_t frame buffers and genet_send()
 * directly — no netdev abstraction layer needed for a single-NIC kernel.
 *
 * Author: R Andrews – boot370 (based on original 10 Dec 2025 skeleton)
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ----------------------------------------------------------------
 * Ethernet constants (shared by all net/ modules)
 * ---------------------------------------------------------------- */
#define ETH_HDR_LEN     14
#define ETH_MTU         1500
#define ETH_P_ARP       0x0806
#define ETH_P_IP        0x0800
#define ETH_P_IPV6      0x86DD

/* ----------------------------------------------------------------
 * PhoenixTCPIP — EtherType frame dispatcher (net/tcpip.c)
 * ---------------------------------------------------------------- */
void  tcpip_init   (void);
void  net_rx_frame (uint8_t *frame, int len);

/* ----------------------------------------------------------------
 * PhoenixARP — RFC 826 ARP handler + 16-entry cache (net/arp.c)
 * ---------------------------------------------------------------- */
void  arp_init     (void);
void  arp_input    (uint8_t *frame, int len);
int   arp_resolve  (const uint8_t ip[4], uint8_t mac_out[6]);

/* ----------------------------------------------------------------
 * PhoenixIPv4 — IPv4 input dispatch + ICMP echo + output (net/ipv4.c)
 * ---------------------------------------------------------------- */
void  ipv4_init    (void);
void  ipv4_input   (uint8_t *frame, int len);
void  ipv4_output  (const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                    uint8_t proto, const uint8_t *payload, int payload_len);

/* ----------------------------------------------------------------
 * PhoenixIPv6 — stub, silently drops all IPv6 frames (net/ipv6.c)
 * ---------------------------------------------------------------- */
void  ipv6_init    (void);
void  ipv6_input   (uint8_t *frame, int len);

/* ----------------------------------------------------------------
 * PhoenixTCP — RFC 793 TCP client state machine (net/tcp.c)
 * TCP state numbers from Inet6Sources obsd/Lib/netinet/h/tcp_fsm
 * ---------------------------------------------------------------- */
#define TCPS_CLOSED       0
#define TCPS_SYN_SENT     2
#define TCPS_ESTABLISHED  4
#define TCPS_CLOSE_WAIT   5
#define TCPS_FIN_WAIT_1   6
#define TCPS_FIN_WAIT_2   9
#define TCPS_TIME_WAIT    10

void  tcp_init     (void);
void  tcp_rx       (uint8_t *frame, int len);
void  tcp_tick     (int handle);   /* retransmit SYN if still in SYN_SENT */
int   tcp_connect  (const uint8_t dst_ip[4], uint16_t dst_port);
int   tcp_state    (int handle);
int   tcp_write    (int handle, const uint8_t *data, int len);
int   tcp_read     (int handle, uint8_t *buf, int bufsz);
void  tcp_close    (int handle);

/* ----------------------------------------------------------------
 * PhoenixUDP — stub, silently drops all UDP frames (net/udp.c)
 * ---------------------------------------------------------------- */
void  udp_init     (void);
void  udp_rx       (uint8_t *frame, int len);

#endif /* NET_H */
