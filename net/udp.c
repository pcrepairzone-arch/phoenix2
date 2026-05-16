/*
 * udp.c – PhoenixUDP: minimal UDP TX + RX queue
 * boot379: full implementation replacing the boot370 stub.
 *
 * TX: udp_send() builds an 8-byte UDP header and hands the datagram
 *     to ipv4_output(). ARP resolution done first; gateway MAC fallback.
 * RX: udp_rx() called by ipv4_input() for every IP/UDP frame.
 *     Frames placed in a ring keyed by destination port.
 *     udp_recvfrom() pops from that queue (non-blocking).
 *
 * Author: R Andrews – boot379
 */
#include "kernel.h"
#include "net/net.h"
#include "net/dhcp.h"

/* ── RX queue ──────────────────────────────────────────────────────────── */
#define UDP_RXQUEUE_SIZE  8
#define UDP_PKT_MAX       512

typedef struct {
    uint8_t  used;
    uint16_t dst_port;
    uint16_t src_port;
    uint8_t  src_ip[4];
    uint8_t  data[UDP_PKT_MAX];
    uint16_t len;
} udp_rx_entry_t;

static udp_rx_entry_t g_udp_rxq[UDP_RXQUEUE_SIZE];

/* ── External dependencies ─────────────────────────────────────────────── */
/* ipv4_output and arp_resolve declared in net/net.h */
extern uint8_t g_genet_mac[6];  /* our source MAC */

/* ── udp_init ──────────────────────────────────────────────────────────── */
void udp_init(void)
{
    for (int i = 0; i < UDP_RXQUEUE_SIZE; i++)
        g_udp_rxq[i].used = 0;
    debug_print("[UDP] PhoenixUDP ready (TX+RX queue, boot379)\n");
}

/* ── udp_rx_enqueue — internal ─────────────────────────────────────────── */
static void udp_rx_enqueue(uint16_t dst_port,
                            const uint8_t src_ip[4], uint16_t src_port,
                            const uint8_t *data, int len)
{
    for (int i = 0; i < UDP_RXQUEUE_SIZE; i++) {
        if (!g_udp_rxq[i].used) {
            g_udp_rxq[i].used     = 1;
            g_udp_rxq[i].dst_port = dst_port;
            g_udp_rxq[i].src_port = src_port;
            for (int j = 0; j < 4; j++)
                g_udp_rxq[i].src_ip[j] = src_ip[j];
            int copy = (len > UDP_PKT_MAX) ? UDP_PKT_MAX : len;
            for (int j = 0; j < copy; j++)
                g_udp_rxq[i].data[j] = data[j];
            g_udp_rxq[i].len = (uint16_t)copy;
            return;
        }
    }
    /* Queue full — silently drop */
}

/* ── udp_rx — called from ipv4_input for protocol 0x11 ────────────────── */
void udp_rx(uint8_t *frame, int len)
{
    if (len < 42) return;   /* Eth(14) + IP(20) + UDP(8) minimum */

    int ihl     = (frame[14] & 0x0F) << 2;   /* IP header length */
    int udp_off = 14 + ihl;
    if (len < udp_off + 8) return;

    uint16_t src_port = ((uint16_t)frame[udp_off]     << 8) | frame[udp_off + 1];
    uint16_t dst_port = ((uint16_t)frame[udp_off + 2] << 8) | frame[udp_off + 3];
    uint16_t udp_len  = ((uint16_t)frame[udp_off + 4] << 8) | frame[udp_off + 5];
    /* udp_off + 6,7 = checksum (ignored on RX) */

    const uint8_t *src_ip  = &frame[26];   /* IP source, bytes 26-29 */
    const uint8_t *payload = frame + udp_off + 8;

    int data_len = (int)udp_len - 8;
    if (data_len < 0) data_len = 0;
    if (udp_off + 8 + data_len > len)
        data_len = len - udp_off - 8;

    udp_rx_enqueue(dst_port, src_ip, src_port, payload, data_len);
}

/* ── udp_recvfrom — non-blocking poll for local_port ──────────────────── */
/*
 * Returns: number of bytes copied into buf (>0), or 0 if no packet waiting.
 * src_ip_out and src_port_out may be NULL if the caller doesn't need them.
 */
int udp_recvfrom(uint16_t local_port,
                 uint8_t *buf, int maxlen,
                 uint8_t src_ip_out[4], uint16_t *src_port_out)
{
    for (int i = 0; i < UDP_RXQUEUE_SIZE; i++) {
        if (g_udp_rxq[i].used && g_udp_rxq[i].dst_port == local_port) {
            int n = g_udp_rxq[i].len;
            if (n > maxlen) n = maxlen;
            for (int j = 0; j < n; j++)
                buf[j] = g_udp_rxq[i].data[j];
            if (src_ip_out)
                for (int j = 0; j < 4; j++)
                    src_ip_out[j] = g_udp_rxq[i].src_ip[j];
            if (src_port_out)
                *src_port_out = g_udp_rxq[i].src_port;
            g_udp_rxq[i].used = 0;   /* consume */
            return n;
        }
    }
    return 0;   /* nothing yet */
}

/* ── udp_send ──────────────────────────────────────────────────────────── */
/*
 * Sends a UDP datagram to dst_ip:dst_port from src_port.
 * ARP-resolves the destination; falls back to gateway MAC if not local.
 * Checksum field left as 0x0000 — legal per RFC 768 for IPv4.
 */
void udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
              uint16_t src_port, const uint8_t *data, int len)
{
    if (len < 0 || len > 1464) return;

    uint8_t mac[6];

    /* boot380: arp_resolve_blocking() sends an ARP request + polls if needed.
     * On miss try the gateway — DNS server is usually the router itself.     */
    if (!arp_resolve_blocking(dst_ip, mac, 1000)) {
        uint8_t gw[4];
        dhcp_get_gateway(gw);
        if (!arp_resolve_blocking(gw, mac, 1000)) {
            debug_print("[UDP] send: ARP failed for dst and gateway\n");
            return;
        }
    }

    /* Build UDP segment in static buffer (single-threaded, cooperative) */
    static uint8_t seg[8 + 1464];
    int udp_len = 8 + len;

    seg[0] = (uint8_t)(src_port >> 8);
    seg[1] = (uint8_t)(src_port & 0xFF);
    seg[2] = (uint8_t)(dst_port >> 8);
    seg[3] = (uint8_t)(dst_port & 0xFF);
    seg[4] = (uint8_t)(udp_len >> 8);
    seg[5] = (uint8_t)(udp_len & 0xFF);
    seg[6] = 0x00;   /* checksum high — zero = omitted, valid per RFC 768 */
    seg[7] = 0x00;   /* checksum low  */
    for (int i = 0; i < len; i++)
        seg[8 + i] = data[i];

    ipv4_output(mac, dst_ip, 0x11, seg, udp_len);
}
