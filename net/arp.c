/*
 * arp.c – PhoenixARP: ARP input handler, 16-entry cache, and resolution
 * RFC 826 ARP for IPv4/Ethernet
 *
 * boot370: extracted from kernel/lib.c inline handler.
 * Adds an ARP table so outbound TCP/UDP can resolve MACs without
 * sending ARP requests over the wire (DHCP grat-ARP populates the
 * gateway entry; who-has replies cache the peer).
 *
 * Module API:
 *   arp_init()                          — clear table
 *   arp_input(frame, len)               — handle incoming ARP frame
 *   arp_resolve(ip, mac_out) → 1/0      — look up cached MAC for IP
 *
 * Author: R Andrews – boot370
 */

#include "kernel.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/net.h"
#include "drivers/net/genet.h"

/* ----------------------------------------------------------------
 * ARP table: 16-entry fixed array, LRU eviction
 * ---------------------------------------------------------------- */
#define ARP_TABLE_SIZE  16

typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t s_table[ARP_TABLE_SIZE];
static int         s_next_slot = 0;   /* LRU: round-robin eviction */

void arp_init(void)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++)
        s_table[i].valid = 0;
    s_next_slot = 0;
    debug_print("[ARP] table init (%d slots)\n", ARP_TABLE_SIZE);
}

/* ----------------------------------------------------------------
 * Cache an IP→MAC mapping (or refresh existing entry)
 * ---------------------------------------------------------------- */
static void table_update(const uint8_t ip[4], const uint8_t mac[6])
{
    /* RFC 5227: ARP probes use sender IP 0.0.0.0 to detect address
     * conflicts before claiming.  Never cache them — the entry would
     * corrupt the table and waste a slot.                             */
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;

    /* Refresh existing entry if IP already cached */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_table[i].valid &&
            s_table[i].ip[0] == ip[0] && s_table[i].ip[1] == ip[1] &&
            s_table[i].ip[2] == ip[2] && s_table[i].ip[3] == ip[3]) {
            for (int j = 0; j < 6; j++) s_table[i].mac[j] = mac[j];
            return;   /* refreshed, no debug needed */
        }
    }

    /* New entry — evict LRU slot */
    int slot = s_next_slot % ARP_TABLE_SIZE;
    s_next_slot++;

    for (int j = 0; j < 4; j++) s_table[slot].ip[j]  = ip[j];
    for (int j = 0; j < 6; j++) s_table[slot].mac[j] = mac[j];
    s_table[slot].valid = 1;

    debug_print("[ARP] cache %d.%d.%d.%d → %02x:%02x:%02x:%02x:%02x:%02x\n",
        ip[0],ip[1],ip[2],ip[3],
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

/*
 * arp_resolve — look up cached MAC for IP
 * Returns 1 and fills mac_out if found, returns 0 if not cached.
 */
int arp_resolve(const uint8_t ip[4], uint8_t mac_out[6])
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_table[i].valid &&
            s_table[i].ip[0] == ip[0] && s_table[i].ip[1] == ip[1] &&
            s_table[i].ip[2] == ip[2] && s_table[i].ip[3] == ip[3]) {
            for (int j = 0; j < 6; j++) mac_out[j] = s_table[i].mac[j];
            return 1;
        }
    }
    return 0;
}

/*
 * arp_send_request — transmit an ARP who-has for target_ip
 * boot380: used by arp_resolve_blocking() to populate the cache for IPs
 * that were never ARP'd directly (e.g. the gateway after DHCP broadcast).
 *
 * Wire format (60-byte minimum Ethernet frame):
 *   Eth dst = broadcast  Eth src = g_genet_mac  EtherType = 0x0806
 *   ARP op = 1 (request)
 *   SHA = g_genet_mac   SPA = g_our_ip
 *   THA = 00:00:00:00:00:00   TPA = target_ip
 */
void arp_send_request(const uint8_t target_ip[4])
{
    static uint8_t req[60];
    for (int i = 0; i < 60; i++) req[i] = 0;

    /* Ethernet header */
    for (int i = 0; i < 6; i++) req[i]     = 0xFF;              /* dst = broadcast  */
    for (int i = 0; i < 6; i++) req[6 + i] = g_genet_mac[i];   /* src = us         */
    req[12] = 0x08; req[13] = 0x06;   /* EtherType = ARP */

    /* ARP header */
    req[14] = 0x00; req[15] = 0x01;   /* HW type = Ethernet     */
    req[16] = 0x08; req[17] = 0x00;   /* Protocol = IPv4         */
    req[18] = 6;    req[19] = 4;       /* HW len, proto len       */
    req[20] = 0x00; req[21] = 0x01;   /* Op = request (1)        */

    /* SHA = our MAC, SPA = our IP */
    for (int i = 0; i < 6; i++) req[22 + i] = g_genet_mac[i];
    for (int i = 0; i < 4; i++) req[28 + i] = g_our_ip[i];
    /* THA = zeroes (unknown), TPA = target */
    for (int i = 0; i < 4; i++) req[38 + i] = target_ip[i];

    genet_send(req, 60);
    debug_print("[ARP] who-has %d.%d.%d.%d (request sent)\n",
        target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
}

/*
 * arp_resolve_blocking — resolve MAC for ip, sending an ARP request if needed.
 * boot380: mirrors the dhcp.c poll pattern (genet_poll_rx → net_rx_frame).
 *
 * Algorithm:
 *   1. Cache hit → return immediately (common case after first resolution).
 *   2. Cache miss → send ARP who-has, then poll GENET for up to timeout_ms.
 *      Each received frame is fed to net_rx_frame() which calls arp_input()
 *      which calls table_update() on any ARP reply → cache hit on next check.
 *   3. Returns 1 on success (mac_out filled), 0 on timeout.
 */
int arp_resolve_blocking(const uint8_t ip[4], uint8_t mac_out[6],
                         uint32_t timeout_ms)
{
    /* Fast path — already cached */
    if (arp_resolve(ip, mac_out)) return 1;

    /* Send ARP request and poll for reply */
    arp_send_request(ip);

    /* ARM Generic Timer for timeout */
    uint64_t freq, t0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));

    static uint8_t s_rx_buf[GENET_MAX_FRAME];
    while (1) {
        /* Check elapsed time */
        uint64_t now;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
        if (((now - t0) * 1000u) >= ((uint64_t)timeout_ms * freq)) break;

        /* Drain one frame — arp_input() will update cache on ARP reply */
        int flen = genet_poll_rx(s_rx_buf, GENET_MAX_FRAME);
        if (flen > 0)
            net_rx_frame(s_rx_buf, flen);

        /* Check cache again */
        if (arp_resolve(ip, mac_out)) {
            debug_print("[ARP] resolve_blocking: %d.%d.%d.%d resolved\n",
                ip[0], ip[1], ip[2], ip[3]);
            return 1;
        }
    }

    debug_print("[ARP] resolve_blocking: timeout for %d.%d.%d.%d\n",
        ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

/* ----------------------------------------------------------------
 * Build and send an ARP reply frame (42 bytes + 18 pad = 60 bytes)
 *
 *   sha / spa : sender (us) — our MAC, our IP
 *   tha / tpa : target (requester) — their MAC, their IP
 * ---------------------------------------------------------------- */
static void send_reply(const uint8_t tha[6], const uint8_t tpa[4])
{
    static uint8_t reply[60];
    for (int i = 0; i < 60; i++) reply[i] = 0;

    /* Ethernet header */
    for (int i = 0; i < 6; i++) reply[i]     = tha[i];          /* dst = requester  */
    for (int i = 0; i < 6; i++) reply[6 + i] = g_genet_mac[i];  /* src = us         */
    reply[12] = 0x08; reply[13] = 0x06;   /* EtherType = ARP */

    /* ARP header */
    reply[14] = 0x00; reply[15] = 0x01;   /* HW type = Ethernet */
    reply[16] = 0x08; reply[17] = 0x00;   /* Protocol = IPv4    */
    reply[18] = 6;    reply[19] = 4;       /* HW len, proto len  */
    reply[20] = 0x00; reply[21] = 0x02;   /* Op = reply (2)     */

    /* SHA (sender hardware addr) = our MAC */
    for (int i = 0; i < 6; i++) reply[22 + i] = g_genet_mac[i];
    /* SPA (sender protocol addr) = our IP */
    for (int i = 0; i < 4; i++) reply[28 + i] = g_our_ip[i];
    /* THA (target hardware addr) = requester MAC */
    for (int i = 0; i < 6; i++) reply[32 + i] = tha[i];
    /* TPA (target protocol addr) = requester IP */
    for (int i = 0; i < 4; i++) reply[38 + i] = tpa[i];

    genet_send(reply, 60);
}

/*
 * arp_input — process an incoming ARP frame
 *
 * Frame layout (bytes from Ethernet header start):
 *  [14-15] HW type         [16-17] Proto type
 *  [18]    HW addr len     [19]    Proto addr len
 *  [20-21] Operation (1=request, 2=reply)
 *  [22-27] SHA (sender HW addr)
 *  [28-31] SPA (sender proto addr / sender IP)
 *  [32-37] THA (target HW addr)
 *  [38-41] TPA (target proto addr / target IP)
 *
 * For every ARP frame (request or reply) we cache SHA/SPA so that
 * outbound TCP/UDP can resolve the gateway MAC after DHCP completes.
 */
void arp_input(uint8_t *frame, int len)
{
    if (len < 42) return;

    uint16_t op  = (uint16_t)((frame[20] << 8) | frame[21]);
    const uint8_t *sha = frame + 22;   /* sender HW addr (6 bytes) */
    const uint8_t *spa = frame + 28;   /* sender IP     (4 bytes) */
    const uint8_t *tpa = frame + 38;   /* target IP     (4 bytes) */

    /* Cache sender's IP→MAC regardless of op type */
    table_update(spa, sha);

    if (op == ARP_OP_REQUEST) {
        /* Is this who-has for our IP? */
        if (tpa[0] == g_our_ip[0] && tpa[1] == g_our_ip[1] &&
            tpa[2] == g_our_ip[2] && tpa[3] == g_our_ip[3]) {

            send_reply(sha, spa);

            /* boot352 pattern: split ≤7 args per debug_print call */
            debug_print("[ARP] who-has %d.%d.%d.%d from "
                "%02x:%02x:%02x\n",
                g_our_ip[0],g_our_ip[1],g_our_ip[2],g_our_ip[3],
                sha[0],sha[1],sha[2]);
            debug_print("[ARP]   reply sent to %02x:%02x:%02x:%02x:%02x:%02x\n",
                sha[0],sha[1],sha[2],sha[3],sha[4],sha[5]);
        }
    }
    /* ARP_OP_REPLY (op==2): sender already cached above — nothing else to do */
}
