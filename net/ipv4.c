/*
 * ipv4.c – PhoenixIPv4: IPv4 input dispatch, ICMP echo, and output
 *
 * boot370: extracted ICMP ping handler from kernel/lib.c.
 * Adds IPv4 output for TCP/UDP to use, and dispatches incoming
 * frames to ICMP, TCP, or UDP handlers.
 *
 * RFC 791 (IPv4), RFC 792 (ICMP)
 *
 * Frame layout convention (all protocol handlers receive the
 * complete Ethernet frame starting at byte 0):
 *   [14]    IP version (high nibble) + IHL in 32-bit words (low nibble)
 *   [15]    DSCP/ECN
 *   [16-17] Total length (IP header + payload)
 *   [22]    TTL
 *   [23]    Protocol (0x01=ICMP, 0x06=TCP, 0x11=UDP)
 *   [24-25] Header checksum
 *   [26-29] Source IP
 *   [30-33] Destination IP
 *   [34..]  Transport header (offset = 14 + IHL×4)
 *
 * Author: R Andrews – boot370
 */

#include "kernel.h"
#include "net/ethernet.h"
#include "net/dhcp.h"
#include "drivers/net/genet.h"

/* Forward declarations for transport-layer handlers */
void tcp_rx(uint8_t *frame, int len);
void udp_rx(uint8_t *frame, int len);

/* Forward declaration for ARP resolution (arp.c) */
int arp_resolve(const uint8_t ip[4], uint8_t mac_out[6]);

/* ----------------------------------------------------------------
 * RFC 1071 one's complement checksum
 * Used for both IP header checksum and ICMP checksum.
 * data must be 16-bit aligned or len padded to even.
 * ---------------------------------------------------------------- */
static uint16_t ip_checksum(const uint8_t *data, int len)
{
    uint32_t ck = 0;
    for (int i = 0; i + 1 < len; i += 2)
        ck += ((uint32_t)data[i] << 8) | data[i + 1];
    if (len & 1)
        ck += (uint32_t)data[len - 1] << 8;   /* pad odd byte */
    while (ck >> 16)
        ck = (ck & 0xffff) + (ck >> 16);
    return (uint16_t)(~ck & 0xffff);
}

/* ----------------------------------------------------------------
 * ICMP echo request handler (ping reply)
 * Extracted from boot326/lib.c; now lives here as private helper.
 * ---------------------------------------------------------------- */
static void icmp_input(uint8_t *frame, int flen, int ihl)
{
    int icmp_off = 14 + ihl;
    if (flen < icmp_off + 8) return;

    /* Only handle echo request (type=8, code=0) addressed to us */
    if (frame[icmp_off]     != 0x08) return;   /* not echo request */
    if (frame[icmp_off + 1] != 0x00) return;   /* code != 0        */
    if (frame[30] != g_our_ip[0] || frame[31] != g_our_ip[1] ||
        frame[32] != g_our_ip[2] || frame[33] != g_our_ip[3]) return;

    int ip_total = (frame[16] << 8) | frame[17];
    int pkt_len  = 14 + ip_total;
    if (pkt_len > flen)  pkt_len = flen;
    if (pkt_len > 1514)  return;              /* safety guard */

    static uint8_t pkt[1514];
    for (int i = 0; i < pkt_len; i++) pkt[i] = frame[i];

    /* Ethernet: swap src/dst MAC */
    for (int i = 0; i < 6; i++) {
        pkt[i]     = frame[6 + i];    /* dst = sender   */
        pkt[6 + i] = g_genet_mac[i];  /* src = us       */
    }

    /* IP: swap src/dst, TTL=64, recompute header checksum */
    for (int i = 0; i < 4; i++) {
        pkt[26 + i] = g_our_ip[i];     /* src = us       */
        pkt[30 + i] = frame[26 + i];   /* dst = sender   */
    }
    pkt[22] = 64;                       /* TTL            */
    pkt[24] = 0; pkt[25] = 0;          /* zero checksum  */
    uint16_t ip_ck = ip_checksum(pkt + 14, ihl);
    pkt[24] = (uint8_t)(ip_ck >> 8);
    pkt[25] = (uint8_t)(ip_ck & 0xff);

    /* ICMP: type=0 (reply), recompute ICMP checksum */
    pkt[icmp_off] = 0x00;
    pkt[icmp_off + 2] = 0; pkt[icmp_off + 3] = 0;
    int icmp_len = ip_total - ihl;
    uint16_t icmp_ck = ip_checksum(pkt + icmp_off, icmp_len);
    pkt[icmp_off + 2] = (uint8_t)(icmp_ck >> 8);
    pkt[icmp_off + 3] = (uint8_t)(icmp_ck & 0xff);

    genet_send(pkt, pkt_len);

    debug_print("[ICMP] echo reply → %d.%d.%d.%d seq=%d\n",
        frame[26], frame[27], frame[28], frame[29],
        (int)(((unsigned)frame[icmp_off + 6] << 8) | frame[icmp_off + 7]));
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void ipv4_init(void)
{
    debug_print("[IPv4] PhoenixIPv4 ready\n");
}

/*
 * ipv4_input — dispatch an incoming IPv4 frame
 * frame[0] is the first byte of the Ethernet header.
 * Validates IP version, IHL, then dispatches by protocol number.
 */
void ipv4_input(uint8_t *frame, int len)
{
    if (len < 34) return;   /* Eth(14) + IP min(20) */

    /* Version must be 4 */
    if ((frame[14] >> 4) != 4) return;

    int ihl = (frame[14] & 0x0f) * 4;
    if (ihl < 20 || len < 14 + ihl) return;

    uint8_t proto = frame[23];

    switch (proto) {
    case 0x01:  icmp_input(frame, len, ihl); break;
    case 0x06:  tcp_rx(frame, len);          break;
    case 0x11:  udp_rx(frame, len);          break;
    /* silently drop IGMP, ESP, etc. */
    }
}

/*
 * ipv4_output — build Ethernet + IPv4 header and transmit
 *
 * dst_mac     : resolved destination MAC (call arp_resolve first)
 * dst_ip      : destination IPv4 (4 bytes, host byte order is fine —
 *               we store as individual bytes, no byte-swap needed)
 * proto       : IP protocol number (0x06=TCP, 0x11=UDP, 0x01=ICMP)
 * payload     : transport-layer segment (TCP/UDP header + data)
 * payload_len : byte length of payload
 *
 * The complete frame is built in a static buffer and sent via
 * genet_send().  Only call from cooperative (non-interrupt) context.
 */
void ipv4_output(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                 uint8_t proto, const uint8_t *payload, int payload_len)
{
    int ip_total  = 20 + payload_len;
    int frame_len = 14 + ip_total;
    if (frame_len > 1514) return;   /* exceeds Ethernet max */

    static uint8_t frame[1514];

    /* --- Ethernet header ---                                        */
    for (int i = 0; i < 6; i++) frame[i]     = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = g_genet_mac[i];
    frame[12] = 0x08; frame[13] = 0x00;   /* EtherType = IPv4 */

    /* --- IPv4 header (20 bytes, no options) ---                    */
    frame[14] = 0x45;                      /* Version=4, IHL=5       */
    frame[15] = 0x00;                      /* DSCP/ECN               */
    frame[16] = (uint8_t)(ip_total >> 8);
    frame[17] = (uint8_t)(ip_total & 0xff);
    frame[18] = 0x00; frame[19] = 0x00;   /* ID (no fragmentation)  */
    frame[20] = 0x40; frame[21] = 0x00;   /* DF=1, frag offset=0    */
    frame[22] = 64;                        /* TTL                    */
    frame[23] = proto;
    frame[24] = 0x00; frame[25] = 0x00;   /* checksum (fill below)  */
    for (int i = 0; i < 4; i++) frame[26 + i] = g_our_ip[i];
    for (int i = 0; i < 4; i++) frame[30 + i] = dst_ip[i];

    /* IP header checksum */
    uint16_t ck = ip_checksum(frame + 14, 20);
    frame[24] = (uint8_t)(ck >> 8);
    frame[25] = (uint8_t)(ck & 0xff);

    /* --- Payload (TCP/UDP segment) ---                              */
    for (int i = 0; i < payload_len; i++) frame[34 + i] = payload[i];

    genet_send(frame, frame_len);
}
