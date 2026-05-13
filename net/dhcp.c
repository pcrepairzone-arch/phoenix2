/* net/dhcp.c — Phoenix OS DHCP client (PhoenixDHCP native module)
 *
 * Extracted and refactored from kernel/lib.c inline implementation (boot334).
 *
 * Boot tag history:
 *   boot334 — DHCP state machine first working (inline in lib.c wimp_task)
 *   boot344 — IRQ-driven RX; DHCP still inline
 *   boot345 — DHCP extracted to module; race fix; retransmit fix
 *
 * Race fix (boot345):
 *   Original code in lib.c set g_dhcp_st AFTER genet_send().  If an OFFER
 *   arrived in the IRQ window between send() and the state assignment the
 *   OFFER was silently dropped (state check failed).  Fix: set state BEFORE
 *   calling genet_send() so the ISR sees DHCP_ST_DISCOVER immediately.
 *
 * Retransmit fix (boot345):
 *   Caller (lib.c) resets last_net after dhcp_start() to force an immediate
 *   RX poll — removes the up-to-4 ms blind window after DISCOVER is sent.
 *
 * Author: Phoenix OS project
 * Updated: boot345 candidate, May 2026
 */

#include "dhcp.h"
#include "../drivers/net/genet.h"   /* genet_send(), g_genet_mac */

/* kernel.h (included transitively) provides debug_print, memset etc. */
#include "../kernel/kernel.h"

/* ── DHCP message type constants ─────────────────────────────────────────── */
#define DHCP_DISCOVER     1
#define DHCP_REQUEST      3
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

/* ── Module-private state ─────────────────────────────────────────────────── */
static int     g_dhcp_st       = DHCP_ST_IDLE;
static uint8_t g_dhcp_yiaddr[4];              /* offered IP (yiaddr field)   */
static uint8_t g_dhcp_srvid[4];              /* server identifier option 54 */
static int     g_dhcp_tries    = 0;           /* retransmit count            */
static uint32_t g_dhcp_last_ms = 0;           /* timestamp of last send      */

/* ── boot369: full lease store ───────────────────────────────────────────── */
/* Populated from ACK options; available to any module via query API below.  */
static uint8_t  g_dhcp_netmask[4]  = { 255, 255, 255,   0 }; /* option  1  */
static uint8_t  g_dhcp_gateway[4]  = {   0,   0,   0,   0 }; /* option  3  */
static uint8_t  g_dhcp_dns[4]      = {   0,   0,   0,   0 }; /* option  6  */
static uint32_t g_dhcp_lease_secs  = 0;                       /* option 51  */

/* XID "PHOE" — constant transaction ID identifies our exchange            */
static const uint8_t g_dhcp_xid[4]    = { 0x50, 0x48, 0x4F, 0x45 };

/* Static fallback: used after MAX_TRIES retransmits with no reply         */
static const uint8_t g_dhcp_static[4] = { 192, 168, 0, 200 };

/* MAC pointer — set by dhcp_init()                                        */
static const uint8_t *g_mac = NULL;

#define DHCP_RETRANSMIT_MS  4000u   /* 4 s between retransmits              */
#define DHCP_MAX_TRIES      5       /* give up after 5 attempts             */
#define DHCP_FRAME_LEN      342     /* Ethernet/IPv4/UDP/DHCP packed length */

/* ── Public: exported bound IP ──────────────────────────────────────────── */
uint8_t g_our_ip[4] = { 0, 0, 0, 0 };

/* ── Internal: one's-complement IP checksum ─────────────────────────────── */
static uint16_t _ip_cksum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i += 2)
        s += (uint32_t)((p[i] << 8) | (i + 1 < n ? p[i + 1] : 0));
    while (s >> 16) s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)(~s & 0xffffu);
}

/* ── Internal: millisecond counter (ARM system counter) ─────────────────── */
static inline uint32_t _ms(void)
{
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)(cnt / (freq / 1000ULL));
}

/* ── Internal: build a DHCP Ethernet frame ──────────────────────────────── */
/*
 * Frame layout (342 bytes total):
 *   [  0.. 13] Ethernet header  (14 B)
 *   [ 14.. 33] IPv4 header      (20 B)  total_len = 328
 *   [ 34.. 41] UDP header        (8 B)  udp_len   = 308
 *   [ 42..341] DHCP payload    (300 B)
 *     [42+  0] op  htype hlen hops
 *     [42+  4] xid[4]
 *     [42+  8] secs[2]  flags[2]
 *     [42+ 12] ciaddr[4]  yiaddr[4]  siaddr[4]  giaddr[4]
 *     [42+ 28] chaddr[16]
 *     [42+ 44] sname[64]
 *     [42+108] file[128]
 *     [42+236] magic cookie  63:82:53:63
 *     [42+240] options (TLV, terminated by 0xff)
 */
static void _dhcp_make(uint8_t *buf, int type)
{
    for (int i = 0; i < DHCP_FRAME_LEN; i++) buf[i] = 0;

    /* Ethernet: dst=broadcast, src=our MAC, EtherType=IPv4 */
    for (int i = 0; i < 6; i++) { buf[i] = 0xff; buf[6 + i] = g_mac[i]; }
    buf[12] = 0x08; buf[13] = 0x00;

    /* IPv4: ver=4 IHL=5, total=328, DF, TTL=64, proto=UDP(17)
     * src=0.0.0.0 dst=255.255.255.255                                    */
    buf[14] = 0x45;
    buf[16] = 0x01; buf[17] = 0x48;   /* total length = 328              */
    buf[20] = 0x40;                    /* DF bit                          */
    buf[22] = 0x40;                    /* TTL = 64                        */
    buf[23] = 0x11;                    /* proto = UDP                     */
    buf[30] = 255; buf[31] = 255; buf[32] = 255; buf[33] = 255;
    uint16_t ck = _ip_cksum(buf + 14, 20);
    buf[24] = (uint8_t)(ck >> 8); buf[25] = (uint8_t)ck;

    /* UDP: src=68(client) dst=67(server), len=308, cksum=0 (IPv4 optional) */
    buf[34] = 0x00; buf[35] = 0x44;   /* src port 68                     */
    buf[36] = 0x00; buf[37] = 0x43;   /* dst port 67                     */
    buf[38] = 0x01; buf[39] = 0x34;   /* length = 308                    */

    /* DHCP fixed fields */
    buf[42] = 0x01;                    /* op = BOOTREQUEST                */
    buf[43] = 0x01;                    /* htype = Ethernet                */
    buf[44] = 0x06;                    /* hlen  = 6                       */
    /* xid at [46..49] (offset 42+4) */
    buf[46] = g_dhcp_xid[0]; buf[47] = g_dhcp_xid[1];
    buf[48] = g_dhcp_xid[2]; buf[49] = g_dhcp_xid[3];
    buf[52] = 0x80;                    /* flags: broadcast bit            */
    /* chaddr at [70..75] (offset 42+28) */
    for (int i = 0; i < 6; i++) buf[70 + i] = g_mac[i];
    /* magic cookie at [278..281] (offset 42+236) */
    buf[278] = 0x63; buf[279] = 0x82; buf[280] = 0x53; buf[281] = 0x63;

    /* Options starting at [282] */
    int o = 282;
    buf[o++] = 53; buf[o++] = 1; buf[o++] = (uint8_t)type;  /* msg type */
    if (type == DHCP_REQUEST) {
        /* Option 50: Requested IP Address */
        buf[o++] = 50; buf[o++] = 4;
        buf[o++] = g_dhcp_yiaddr[0]; buf[o++] = g_dhcp_yiaddr[1];
        buf[o++] = g_dhcp_yiaddr[2]; buf[o++] = g_dhcp_yiaddr[3];
        /* Option 54: Server Identifier */
        buf[o++] = 54; buf[o++] = 4;
        buf[o++] = g_dhcp_srvid[0]; buf[o++] = g_dhcp_srvid[1];
        buf[o++] = g_dhcp_srvid[2]; buf[o++] = g_dhcp_srvid[3];
    } else {
        /* Option 55: Parameter Request List — subnet, router, domain, DNS */
        buf[o++] = 55; buf[o++] = 4;
        buf[o++] = 1; buf[o++] = 3; buf[o++] = 15; buf[o++] = 6;
    }
    buf[o] = 0xff;  /* End option */
}

/* ── Internal: send gratuitous ARP announcing our IP ────────────────────── */
static void _send_grat_arp(void)
{
    static uint8_t garp[60];
    for (int i = 0; i < 60; i++) garp[i] = 0;
    for (int i = 0; i < 6; i++) garp[i]     = 0xff;      /* dst broadcast */
    for (int i = 0; i < 6; i++) garp[6 + i] = g_mac[i];  /* src = us      */
    garp[12] = 0x08; garp[13] = 0x06;                     /* ARP           */
    garp[14] = 0x00; garp[15] = 0x01;                     /* HTYPE Ethernet*/
    garp[16] = 0x08; garp[17] = 0x00;                     /* PTYPE IPv4    */
    garp[18] = 6;    garp[19] = 4;                        /* HLEN=6 PLEN=4 */
    garp[20] = 0x00; garp[21] = 0x02;                     /* op = REPLY    */
    for (int i = 0; i < 6; i++) garp[22 + i] = g_mac[i]; /* SHA = us      */
    for (int i = 0; i < 4; i++) garp[28 + i] = g_our_ip[i]; /* SPA = us   */
    garp[32] = 0xff; garp[33] = 0xff; garp[34] = 0xff;
    garp[35] = 0xff; garp[36] = 0xff; garp[37] = 0xff;    /* THA = bcast  */
    for (int i = 0; i < 4; i++) garp[38 + i] = g_our_ip[i]; /* TPA = us   */
    genet_send(garp, 60);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void dhcp_init(const uint8_t *mac)
{
    g_mac         = mac;
    g_dhcp_st     = DHCP_ST_IDLE;
    g_dhcp_tries  = 0;
    g_dhcp_last_ms = 0;
    for (int i = 0; i < 4; i++) {
        g_dhcp_yiaddr[i] = 0;
        g_dhcp_srvid[i]  = 0;
        g_our_ip[i]      = 0;
        g_dhcp_gateway[i] = 0;
        g_dhcp_dns[i]     = 0;
    }
    g_dhcp_netmask[0] = 255; g_dhcp_netmask[1] = 255;
    g_dhcp_netmask[2] = 255; g_dhcp_netmask[3] =   0; /* default /24  */
    g_dhcp_lease_secs = 0;
    debug_print("[DHCP] module initialised (mac=%02x:%02x:%02x:%02x:%02x:%02x)\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void dhcp_start(void)
{
    if (!g_mac) return;

    /* boot345 race fix: set state BEFORE send.
     * If the IRQ fires between genet_send() and the state assignment in the
     * original boot334 code the OFFER is processed while g_dhcp_st is still
     * IDLE and silently dropped.  Setting state first closes the window.   */
    g_dhcp_st    = DHCP_ST_DISCOVER;
    g_dhcp_tries = 1;

    static uint8_t disc[DHCP_FRAME_LEN];
    _dhcp_make(disc, DHCP_DISCOVER);
    genet_send(disc, DHCP_FRAME_LEN);

    g_dhcp_last_ms = _ms();

    debug_print("[DHCP] DISCOVER sent (xid=PHOE, try=1)\n");

    /* Caller (lib.c wimp_task) must reset its last_net timestamp to now so
     * the 4 ms RX poll fires immediately rather than waiting up to 4 ms.
     * See comment in lib.c link-up handler.                                */
}

void dhcp_tick(uint32_t now_ms)
{
    if (g_dhcp_st != DHCP_ST_DISCOVER &&
        g_dhcp_st != DHCP_ST_REQUEST) return;

    if ((now_ms - g_dhcp_last_ms) < DHCP_RETRANSMIT_MS) return;
    g_dhcp_last_ms = now_ms;

    g_dhcp_tries++;
    if (g_dhcp_tries > DHCP_MAX_TRIES) {
        /* Static fallback — no DHCP server responded                      */
        for (int i = 0; i < 4; i++) g_our_ip[i] = g_dhcp_static[i];
        g_dhcp_st = DHCP_ST_BOUND;
        debug_print("[DHCP] timeout — static fallback %d.%d.%d.%d\n",
                    g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3]);
        _send_grat_arp();
        return;
    }

    int type = (g_dhcp_st == DHCP_ST_DISCOVER) ? DHCP_DISCOVER : DHCP_REQUEST;
    static uint8_t retry[DHCP_FRAME_LEN];
    _dhcp_make(retry, type);
    genet_send(retry, DHCP_FRAME_LEN);
    debug_print("[DHCP] retransmit try=%d (%s)\n",
                g_dhcp_tries,
                (type == DHCP_DISCOVER) ? "DISCOVER" : "REQUEST");
}

void dhcp_rx(const uint8_t *frame, int len)
{
    /* boot349: verbose reject logging so bootlog shows exactly which check
     * kills the frame.  Every silent return now prints a reason.           */

    /* Must be in an active state (not IDLE or already BOUND)               */
    if (g_dhcp_st == DHCP_ST_IDLE || g_dhcp_st == DHCP_ST_BOUND) return;

    /* Minimum frame length (DHCP is always > 300 bytes)                    */
    if (len < 300) {
        debug_print("[DHCP] rx drop: len=%d < 300 (st=%d)\n", len,
                    (int)g_dhcp_st);
        return;
    }

    /* IP protocol must be UDP (17)                                         */
    if (frame[23] != 0x11) {
        debug_print("[DHCP] rx drop: proto=0x%02x not UDP (st=%d)\n",
                    (unsigned)frame[23], (int)g_dhcp_st);
        return;
    }

    int ihl     = (frame[14] & 0x0f) * 4;
    int udp_off = 14 + ihl;
    int doff    = udp_off + 8;       /* DHCP payload start                 */

    /* UDP destination port must be 68 (DHCP client)                       */
    int udp_dp = (frame[udp_off + 2] << 8) | frame[udp_off + 3];
    if (udp_dp != 68) {
        debug_print("[DHCP] rx drop: udp_dst=%d not 68 (st=%d)\n",
                    udp_dp, (int)g_dhcp_st);
        return;
    }

    /* Minimum DHCP payload length                                          */
    if (len < doff + 240) {
        debug_print("[DHCP] rx drop: too short for DHCP len=%d doff=%d\n",
                    len, doff);
        return;
    }

    /* Must be BOOTREPLY (op=2)                                             */
    if (frame[doff + 0] != 0x02) {
        debug_print("[DHCP] rx drop: op=0x%02x not BOOTREPLY\n",
                    (unsigned)frame[doff + 0]);
        return;
    }

    /* Transaction ID must match ours ("PHOE" = 0x50484F45)                */
    if (frame[doff + 4] != g_dhcp_xid[0] ||
        frame[doff + 5] != g_dhcp_xid[1] ||
        frame[doff + 6] != g_dhcp_xid[2] ||
        frame[doff + 7] != g_dhcp_xid[3]) {
        debug_print("[DHCP] rx drop: xid=%02x%02x%02x%02x != PHOE\n",
                    (unsigned)frame[doff+4], (unsigned)frame[doff+5],
                    (unsigned)frame[doff+6], (unsigned)frame[doff+7]);
        return;
    }

    /* Magic cookie must be present (99.130.83.99)                          */
    if (frame[doff + 236] != 0x63 ||
        frame[doff + 237] != 0x82 ||
        frame[doff + 238] != 0x53 ||
        frame[doff + 239] != 0x63) {
        debug_print("[DHCP] rx drop: bad magic %02x%02x%02x%02x\n",
                    (unsigned)frame[doff+236], (unsigned)frame[doff+237],
                    (unsigned)frame[doff+238], (unsigned)frame[doff+239]);
        return;
    }

    /* Parse options: extract all fields needed for a complete lease        */
    uint8_t  dmsg      = 0;
    uint8_t  dsrv[4]   = { 0, 0, 0, 0 };   /* option 54: server identifier */
    uint8_t  dmask[4]  = { 255, 255, 255, 0 }; /* option  1: subnet mask   */
    uint8_t  dgw[4]    = { 0, 0, 0, 0 };   /* option  3: router/gateway    */
    uint8_t  ddns[4]   = { 0, 0, 0, 0 };   /* option  6: DNS server        */
    uint32_t dlease    = 0;                 /* option 51: IP address lease  */
    int p = doff + 240;
    while (p < len && frame[p] != 0xff) {
        uint8_t tag = frame[p++];
        if (tag == 0) continue;     /* pad byte                           */
        if (p >= len) break;
        uint8_t tl = frame[p++];
        if (tag == 53 && tl >= 1) dmsg = frame[p];
        if (tag ==  1 && tl >= 4) {
            dmask[0]=frame[p]; dmask[1]=frame[p+1];
            dmask[2]=frame[p+2]; dmask[3]=frame[p+3];
        }
        if (tag ==  3 && tl >= 4) {
            dgw[0]=frame[p]; dgw[1]=frame[p+1];
            dgw[2]=frame[p+2]; dgw[3]=frame[p+3];
        }
        if (tag ==  6 && tl >= 4) {
            ddns[0]=frame[p]; ddns[1]=frame[p+1];
            ddns[2]=frame[p+2]; ddns[3]=frame[p+3];
        }
        if (tag == 51 && tl >= 4) {
            dlease = ((uint32_t)frame[p]   << 24) |
                     ((uint32_t)frame[p+1] << 16) |
                     ((uint32_t)frame[p+2] <<  8) |
                      (uint32_t)frame[p+3];
        }
        if (tag == 54 && tl >= 4) {
            dsrv[0]=frame[p]; dsrv[1]=frame[p+1];
            dsrv[2]=frame[p+2]; dsrv[3]=frame[p+3];
        }
        p += tl;
    }

    /* Log what we received so we can see type and state mismatch           */
    debug_print("[DHCP] rx: msg=%d st=%d len=%d\n",
                (int)dmsg, (int)g_dhcp_st, len);

    if (dmsg == DHCP_MSG_OFFER && g_dhcp_st == DHCP_ST_DISCOVER) {
        /* OFFER received — save yiaddr and server ID, send REQUEST        */
        for (int i = 0; i < 4; i++) g_dhcp_yiaddr[i] = frame[doff + 16 + i];
        for (int i = 0; i < 4; i++) g_dhcp_srvid[i]  = dsrv[i];
        debug_print("[DHCP] OFFER %d.%d.%d.%d from srv %d.%d.%d.%d\n",
                    g_dhcp_yiaddr[0], g_dhcp_yiaddr[1],
                    g_dhcp_yiaddr[2], g_dhcp_yiaddr[3],
                    g_dhcp_srvid[0],  g_dhcp_srvid[1],
                    g_dhcp_srvid[2],  g_dhcp_srvid[3]);

        /* Set state BEFORE send — same race-avoidance discipline          */
        g_dhcp_st    = DHCP_ST_REQUEST;
        g_dhcp_tries = 0;

        static uint8_t dreq[DHCP_FRAME_LEN];
        _dhcp_make(dreq, DHCP_REQUEST);
        genet_send(dreq, DHCP_FRAME_LEN);

        g_dhcp_last_ms = _ms();
        debug_print("[DHCP] REQUEST sent\n");

    } else if (dmsg == DHCP_MSG_ACK && g_dhcp_st == DHCP_ST_REQUEST) {
        /* ACK — bind the offered IP and store complete lease              */
        for (int i = 0; i < 4; i++) g_our_ip[i]       = frame[doff + 16 + i];
        for (int i = 0; i < 4; i++) g_dhcp_netmask[i] = dmask[i];
        for (int i = 0; i < 4; i++) g_dhcp_gateway[i] = dgw[i];
        for (int i = 0; i < 4; i++) g_dhcp_dns[i]     = ddns[i];
        g_dhcp_lease_secs = dlease;
        g_dhcp_st = DHCP_ST_BOUND;
        debug_print("[DHCP] BOUND — IP %d.%d.%d.%d mask %d.%d.%d.%d\n",
                    g_our_ip[0],       g_our_ip[1],
                    g_our_ip[2],       g_our_ip[3],
                    g_dhcp_netmask[0], g_dhcp_netmask[1],
                    g_dhcp_netmask[2], g_dhcp_netmask[3]);
        debug_print("[DHCP]        gw %d.%d.%d.%d dns %d.%d.%d.%d lease %us\n",
                    g_dhcp_gateway[0], g_dhcp_gateway[1],
                    g_dhcp_gateway[2], g_dhcp_gateway[3],
                    g_dhcp_dns[0],     g_dhcp_dns[1],
                    g_dhcp_dns[2],     g_dhcp_dns[3],
                    (unsigned)g_dhcp_lease_secs);
        _send_grat_arp();

    } else if (dmsg == DHCP_MSG_NAK) {
        /* NAK — restart from DISCOVER                                     */
        debug_print("[DHCP] NAK received — restarting DISCOVER\n");
        /* Set state before next send in dhcp_tick()                       */
        g_dhcp_st    = DHCP_ST_DISCOVER;
        g_dhcp_tries = 0;
        g_dhcp_last_ms = _ms() - DHCP_RETRANSMIT_MS;  /* force immediate  */
    }
}

int dhcp_bound(void)
{
    return (g_dhcp_st == DHCP_ST_BOUND) ? 1 : 0;
}

void dhcp_get_ip(uint8_t out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g_our_ip[i];
}

/* ── boot369: full lease query API ──────────────────────────────────────── */

void dhcp_get_gateway(uint8_t out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g_dhcp_gateway[i];
}

void dhcp_get_netmask(uint8_t out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g_dhcp_netmask[i];
}

void dhcp_get_dns(uint8_t out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g_dhcp_dns[i];
}

uint32_t dhcp_get_lease_secs(void)
{
    return g_dhcp_lease_secs;
}

/* ── Module entry points ─────────────────────────────────────────────────── */

/*
 * dhcp_module_init — boot369 blocking module initialisation.
 *
 * Called from module_init_all() AFTER genet_init() so g_genet_mac is live.
 * This function does not return until the DHCP exchange is complete (BOUND)
 * or a 30-second timeout triggers the static fallback.
 *
 * Sequence:
 *   1. dhcp_init(g_genet_mac)       — set real MAC, clear state
 *   2. wait for genet_link_up()     — PHY autoneg (typically ~3.5 s on Pi 4)
 *   3. genet_apply_link()           — set UMAC to negotiated speed
 *   4. dhcp_start()                 — send DISCOVER (state set before TX)
 *   5. tight poll loop              — genet_poll_rx → dhcp_rx → dhcp_tick
 *                                     until dhcp_bound() or 30 s elapsed
 *   6. log complete lease           — IP, mask, gateway, DNS, lease time
 *
 * On return, g_our_ip, g_dhcp_gateway, g_dhcp_netmask, g_dhcp_dns, and
 * g_dhcp_lease_secs are all populated.  Any module or stack component can
 * query them via dhcp_get_ip(), dhcp_get_gateway() etc. without any further
 * network traffic.
 *
 * The wimp_task() no longer drives DHCP at all.  It starts with a bound IP.
 */
int dhcp_module_init(void)
{
    /* Step 1: initialise with real hardware MAC (g_genet_mac set by
     * genet_init() which now runs before module_init_all()).              */
    dhcp_init(g_genet_mac);

    /* Step 2: wait for PHY link-up — BCM54213PE autoneg typically 3.5 s  */
    debug_print("[DHCP] waiting for carrier...\n");
    uint32_t t0 = _ms();
    while (!genet_link_up()) {
        if ((_ms() - t0) > 10000u) {
            debug_print("[DHCP] no carrier after 10 s — DHCP skipped\n");
            return 0;   /* wimp task will log link DOWN; no IP yet         */
        }
        for (volatile int d = 0; d < 50000; d++) {}  /* don't hammer MDIO */
    }
    debug_print("[DHCP] carrier UP after %u ms\n", (unsigned)(_ms() - t0));

    /* Step 3: apply negotiated PHY speed to UMAC                         */
    genet_apply_link();

    /* Step 4: send DISCOVER (state set BEFORE TX — boot345 race fix)     */
    dhcp_start();

    /* Step 5: poll until BOUND or 30 s timeout                           */
    static uint8_t s_frame[GENET_MAX_FRAME];
    uint32_t t1 = _ms();
    while (!dhcp_bound()) {
        int flen = genet_poll_rx(s_frame, GENET_MAX_FRAME);
        if (flen > 0) dhcp_rx(s_frame, flen);
        dhcp_tick(_ms());
        if ((_ms() - t1) > 30000u) {
            debug_print("[DHCP] 30 s timeout — static fallback applied\n");
            break;  /* dhcp_tick() already applied static fallback IP     */
        }
    }

    /* Step 6: log complete lease so it appears in bootlog                */
    debug_print("[Module] PhoenixDHCP ready: IP=%d.%d.%d.%d mask=%d.%d.%d.%d\n",
                g_our_ip[0],       g_our_ip[1],
                g_our_ip[2],       g_our_ip[3],
                g_dhcp_netmask[0], g_dhcp_netmask[1],
                g_dhcp_netmask[2], g_dhcp_netmask[3]);
    debug_print("[Module] PhoenixDHCP ready: gw=%d.%d.%d.%d dns=%d.%d.%d.%d lease=%us\n",
                g_dhcp_gateway[0], g_dhcp_gateway[1],
                g_dhcp_gateway[2], g_dhcp_gateway[3],
                g_dhcp_dns[0],     g_dhcp_dns[1],
                g_dhcp_dns[2],     g_dhcp_dns[3],
                (unsigned)g_dhcp_lease_secs);
    return 0;
}

int dhcp_module_final(void)
{
    g_dhcp_st = DHCP_ST_IDLE;
    debug_print("[Module] PhoenixDHCP finalised\n");
    return 0;
}
