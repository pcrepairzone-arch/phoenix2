/*
 * tcp.c – PhoenixTCP: minimal RFC 793 TCP client state machine
 *
 * boot370: first real TCP implementation for Phoenix.
 * Client-side only — active open, data transfer, active close.
 * Server-side (LISTEN, SYN_RECEIVED) is out of scope for boot370;
 * the goal is an outbound HTTP GET.
 *
 * State machine follows Inet6Sources tcp_fsm.h (OpenBSD, RFC 793):
 *   CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2
 *                                    → TIME_WAIT → CLOSED
 * Remote-initiated close (CLOSE_WAIT) is also handled.
 *
 * Design constraints:
 *   - No heap: all TCBs are static fixed-size slots (TCP_MAX_CONNS=4)
 *   - No blocking: tcp_connect() sends SYN and returns immediately;
 *     caller polls tcp_state() until TCPS_ESTABLISHED
 *   - No retransmit timer: suitable for LAN / loopback testing
 *   - MSS 1460 (Ethernet MTU 1500 − IP 20 − TCP 20)
 *   - Receive ring buffer per connection: 4 KB
 *
 * Based on:
 *   - Inet6Sources obsd/netinet tcp_fsm.h (FSM state numbers)
 *   - Inet6Sources obsd/Lib/netinet/h/tcp (header structure)
 *   - RFC 793 section 3.9 (event processing)
 *
 * Author: R Andrews – boot370
 */

#include "kernel.h"
#include "net/ethernet.h"
#include "net/dhcp.h"
#include "drivers/net/genet.h"

/* Forward declarations for modules called by TCP */
int  arp_resolve  (const uint8_t ip[4], uint8_t mac_out[6]);
void ipv4_output  (const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                   uint8_t proto, const uint8_t *payload, int payload_len);

/* ----------------------------------------------------------------
 * TCP flag bits (byte [tcp_off+13] from Ethernet header)
 * Match RFC 793 / Inet6Sources tcp.h TH_* values
 * ---------------------------------------------------------------- */
#define TF_FIN   0x01
#define TF_SYN   0x02
#define TF_RST   0x04
#define TF_PSH   0x08
#define TF_ACK   0x10
#define TF_URG   0x20

/* ----------------------------------------------------------------
 * TCP state numbers — copied from Inet6Sources tcp_fsm.h (OpenBSD)
 * TCPS_CLOSED=0 … TCPS_TIME_WAIT=10 per RFC 793
 * ---------------------------------------------------------------- */
typedef enum {
    TCPS_CLOSED       = 0,
    TCPS_LISTEN       = 1,   /* server: not used here */
    TCPS_SYN_SENT     = 2,
    TCPS_SYN_RECEIVED = 3,   /* server: not used here */
    TCPS_ESTABLISHED  = 4,
    TCPS_CLOSE_WAIT   = 5,
    TCPS_FIN_WAIT_1   = 6,
    TCPS_CLOSING      = 7,
    TCPS_LAST_ACK     = 8,
    TCPS_FIN_WAIT_2   = 9,
    TCPS_TIME_WAIT    = 10
} tcp_state_t;

#define TCP_MAX_CONNS    4
#define TCP_RCVBUF_SIZE  4096
#define TCP_MSS          1460    /* max segment size over Ethernet */
#define TCP_WINDOW       8192    /* advertised receive window */

/* Transmission Control Block */
typedef struct {
    tcp_state_t  state;
    uint8_t      remote_ip[4];
    uint16_t     local_port;
    uint16_t     remote_port;

    uint32_t     snd_iss;   /* initial send sequence number */
    uint32_t     snd_una;   /* oldest unacknowledged byte   */
    uint32_t     snd_nxt;   /* next byte to send            */
    uint32_t     rcv_nxt;   /* next expected byte from peer */
    uint16_t     snd_wnd;   /* send window (from peer ACK)  */

    /* Receive ring buffer */
    uint8_t      rcvbuf[TCP_RCVBUF_SIZE];
    int          rcv_head;   /* consumer index */
    int          rcv_tail;   /* producer index */

    int          used;       /* 1 = slot occupied */
} tcp_conn_t;

static tcp_conn_t  s_conns[TCP_MAX_CONNS];
static uint16_t    s_next_port  = 49152;        /* ephemeral port base (RFC 6335) */
static uint32_t    s_isn        = 0x78563412;   /* pseudo-ISN seed */

/* ----------------------------------------------------------------
 * Big-endian helpers (avoid packed struct alignment issues)
 * ---------------------------------------------------------------- */
static uint16_t u16be(const uint8_t *p)
{ return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t u32be(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] <<  8) |  (uint32_t)p[3]; }

static void w16be(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void w32be(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
  p[2]=(uint8_t)(v>> 8); p[3]=(uint8_t)v; }

/* ----------------------------------------------------------------
 * TCP checksum using IPv4 pseudo-header (RFC 793 section 3.1)
 *
 *   pseudo-header: src_ip(4) + dst_ip(4) + zero(1) + proto(1=0x06)
 *                  + tcp_segment_length(2)
 *   followed by the TCP header + data
 * ---------------------------------------------------------------- */
static uint16_t tcp_cksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                           const uint8_t *seg, int seg_len)
{
    uint32_t ck = 0;

    /* Pseudo-header */
    ck += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    ck += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    ck += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    ck += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    ck += 0x0006;               /* zero + protocol TCP */
    ck += (uint32_t)seg_len;

    /* TCP segment */
    for (int i = 0; i + 1 < seg_len; i += 2)
        ck += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (seg_len & 1)
        ck += (uint32_t)seg[seg_len - 1] << 8;   /* pad odd byte */

    while (ck >> 16)
        ck = (ck & 0xffff) + (ck >> 16);

    return (uint16_t)(~ck & 0xffff);
}

/* ----------------------------------------------------------------
 * Route an outbound segment: if dst is off-subnet, use gateway MAC
 * ---------------------------------------------------------------- */
static int resolve_dst_mac(const uint8_t dst_ip[4], uint8_t mac_out[6])
{
    uint8_t gw[4], nm[4];
    dhcp_get_gateway(gw);
    dhcp_get_netmask(nm);

    /* Same subnet? (g_our_ip & mask) == (dst_ip & mask) */
    int same = 1;
    for (int i = 0; i < 4; i++)
        if ((g_our_ip[i] & nm[i]) != (dst_ip[i] & nm[i])) { same = 0; break; }

    const uint8_t *route = same ? dst_ip : gw;
    return arp_resolve(route, mac_out);
}

/* ----------------------------------------------------------------
 * Build and transmit a TCP segment for connection c.
 *
 * flags      : TF_SYN, TF_ACK, TF_FIN, TF_PSH etc.
 * data / len : payload bytes (NULL / 0 for control segments)
 *
 * TCP header layout (20 bytes, no options):
 *   [0-1]  source port      [2-3]  destination port
 *   [4-7]  sequence number  [8-11] acknowledgement number
 *   [12]   data offset=0x50 [13]   flags
 *   [14-15] window          [16-17] checksum   [18-19] urgent
 * ---------------------------------------------------------------- */
static void send_segment(tcp_conn_t *c, uint8_t flags,
                          const uint8_t *data, int data_len)
{
    uint8_t dst_mac[6];
    if (!resolve_dst_mac(c->remote_ip, dst_mac)) {
        debug_print("[TCP] ARP miss for %d.%d.%d.%d — segment dropped\n",
            c->remote_ip[0], c->remote_ip[1],
            c->remote_ip[2], c->remote_ip[3]);
        return;
    }

    int seg_len = 20 + data_len;
    if (seg_len > 1480) seg_len = 1480;   /* safety cap */

    static uint8_t seg[1480];

    w16be(seg +  0, c->local_port);
    w16be(seg +  2, c->remote_port);
    w32be(seg +  4, c->snd_nxt);      /* sequence number */
    w32be(seg +  8, c->rcv_nxt);      /* acknowledgement */
    seg[12] = 0x50;                    /* data offset = 5 (20 bytes) */
    seg[13] = flags;
    w16be(seg + 14, TCP_WINDOW);       /* advertised window */
    w16be(seg + 16, 0);                /* checksum: fill below */
    w16be(seg + 18, 0);                /* urgent pointer */

    if (data && data_len > 0)
        for (int i = 0; i < data_len; i++) seg[20 + i] = data[i];

    uint16_t ck = tcp_cksum(g_our_ip, c->remote_ip, seg, seg_len);
    w16be(seg + 16, ck);

    ipv4_output(dst_mac, c->remote_ip, 0x06, seg, seg_len);
}

/* Push data into the receive ring buffer (drop if full) */
static void rcvbuf_push(tcp_conn_t *c, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        int next = (c->rcv_tail + 1) % TCP_RCVBUF_SIZE;
        if (next == c->rcv_head) break;   /* buffer full */
        c->rcvbuf[c->rcv_tail] = data[i];
        c->rcv_tail = next;
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * tcp_tick — periodic retransmission for SYN_SENT state.
 * Call from the main poll loop every ~1 s per connection.
 *
 * TCP requires the SYN to be retransmitted if no SYN-ACK arrives.
 * The most common boot-time case: ARP miss on first SYN attempt
 * (gateway not yet cached), then ARP entry arrives, then tcp_tick
 * retransmits the SYN which now gets a MAC and goes on the wire.
 *
 * SYN carries seq = snd_iss regardless of snd_nxt, so we save and
 * restore snd_nxt around the retransmit call.
 */
void tcp_tick(int handle)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS) return;
    tcp_conn_t *c = &s_conns[handle];
    if (!c->used) return;

    if (c->state == TCPS_SYN_SENT) {
        uint32_t saved = c->snd_nxt;
        c->snd_nxt = c->snd_iss;          /* SYN must carry seq = ISS */
        send_segment(c, TF_SYN, NULL, 0);
        c->snd_nxt = saved;
        debug_print("[TCP] SYN retransmit (handle %d isn=%u)\n",
            handle, c->snd_iss);
    }
}

void tcp_init(void)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) s_conns[i].used = 0;
    debug_print("[TCP] PhoenixTCP client ready (%d slots, MSS=%d)\n",
        TCP_MAX_CONNS, TCP_MSS);
}

/*
 * tcp_connect — initiate an active open to dst_ip:dst_port
 *
 * Sends SYN and returns a handle (0..TCP_MAX_CONNS-1) immediately.
 * Returns -1 if no free slot or ARP miss (gateway not yet cached).
 *
 * Caller must poll tcp_state(handle) == TCPS_ESTABLISHED before
 * calling tcp_write().  Call tcp_rx() from the frame-drain loop
 * to advance the state machine.
 */
int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port)
{
    int h = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (!s_conns[i].used) { h = i; break; }
    if (h < 0) { debug_print("[TCP] no free connection slots\n"); return -1; }

    tcp_conn_t *c = &s_conns[h];
    c->used        = 1;
    c->state       = TCPS_SYN_SENT;
    for (int i = 0; i < 4; i++) c->remote_ip[i] = dst_ip[i];
    c->local_port  = s_next_port++;
    if (s_next_port < 49152) s_next_port = 49152;  /* wrap at 65535 */
    c->remote_port = dst_port;
    c->snd_iss     = s_isn;
    s_isn         += 64000;          /* advance ISN counter */
    c->snd_una     = c->snd_iss;
    c->snd_nxt     = c->snd_iss;
    c->rcv_nxt     = 0;
    c->snd_wnd     = 0;
    c->rcv_head    = 0;
    c->rcv_tail    = 0;

    /* Send SYN — snd_nxt is ISS at this point */
    send_segment(c, TF_SYN, NULL, 0);
    c->snd_nxt = c->snd_iss + 1;    /* SYN consumes one sequence number */

    debug_print("[TCP] SYN → %d.%d.%d.%d:%d sport=%d isn=%u\n",
        dst_ip[0],dst_ip[1],dst_ip[2],dst_ip[3],
        dst_port, c->local_port, c->snd_iss);

    return h;
}

/*
 * tcp_state — return current FSM state for a handle
 * Returns TCPS_CLOSED if handle is invalid or slot not in use.
 */
int tcp_state(int handle)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS || !s_conns[handle].used)
        return TCPS_CLOSED;
    return (int)s_conns[handle].state;
}

/*
 * tcp_write — send data on an ESTABLISHED connection
 * Caps at MSS 1460 per call; returns bytes sent (0 = not ready).
 * For larger payloads, call repeatedly until all bytes are sent.
 */
int tcp_write(int handle, const uint8_t *data, int len)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS) return 0;
    tcp_conn_t *c = &s_conns[handle];
    if (!c->used || c->state != TCPS_ESTABLISHED) return 0;
    if (len <= 0) return 0;

    if (len > TCP_MSS) len = TCP_MSS;

    send_segment(c, TF_ACK | TF_PSH, data, len);
    c->snd_nxt += (uint32_t)len;
    return len;
}

/*
 * tcp_read — non-blocking read from receive buffer
 * Returns bytes copied into buf, or 0 if no data available.
 */
int tcp_read(int handle, uint8_t *buf, int bufsz)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS || !s_conns[handle].used)
        return 0;
    tcp_conn_t *c = &s_conns[handle];

    int avail = (c->rcv_tail - c->rcv_head + TCP_RCVBUF_SIZE) % TCP_RCVBUF_SIZE;
    if (avail > bufsz) avail = bufsz;
    if (avail == 0) return 0;

    for (int i = 0; i < avail; i++)
        buf[i] = c->rcvbuf[(c->rcv_head + i) % TCP_RCVBUF_SIZE];
    c->rcv_head = (c->rcv_head + avail) % TCP_RCVBUF_SIZE;
    return avail;
}

/*
 * tcp_close — initiate active close (send FIN)
 * Moves to FIN_WAIT_1.  The connection slot is freed automatically
 * when TIME_WAIT is entered (tcp_rx processes the final handshake).
 */
void tcp_close(int handle)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS) return;
    tcp_conn_t *c = &s_conns[handle];
    if (!c->used) return;

    if (c->state == TCPS_ESTABLISHED || c->state == TCPS_CLOSE_WAIT) {
        c->state = TCPS_FIN_WAIT_1;
        send_segment(c, TF_FIN | TF_ACK, NULL, 0);
        c->snd_nxt++;   /* FIN consumes one sequence number */
        debug_print("[TCP] FIN sent (handle %d)\n", handle);
    } else {
        /* Not yet established or already closing — just free the slot */
        c->used  = 0;
        c->state = TCPS_CLOSED;
    }
}

/*
 * tcp_rx — process incoming TCP segment
 *
 * Called from ipv4_input when proto == 0x06.
 * frame[0] is the Ethernet header byte 0.
 * Matches by destination port (our local port).
 *
 * RFC 793 section 3.9: simplified event processing for client-side:
 *   SYN_SENT     : expect SYN+ACK → send ACK → ESTABLISHED
 *   ESTABLISHED  : receive data → ACK; receive FIN → half-close
 *   FIN_WAIT_1   : ACK of our FIN → FIN_WAIT_2
 *   FIN_WAIT_2   : receive peer FIN → TIME_WAIT → CLOSED
 */
void tcp_rx(uint8_t *frame, int flen)
{
    if (flen < 54) return;   /* Eth(14) + IP(20) + TCP min(20) */

    int ihl     = (frame[14] & 0x0f) * 4;
    int tcp_off = 14 + ihl;
    if (flen < tcp_off + 20) return;

    uint8_t *tcp = frame + tcp_off;

    uint16_t dport = u16be(tcp + 2);   /* destination port (our port) */
    uint32_t seq   = u32be(tcp + 4);
    uint32_t ack   = u32be(tcp + 8);
    int      doff  = (tcp[12] >> 4) * 4;
    uint8_t  flags = tcp[13];
    uint16_t wnd   = u16be(tcp + 14);

    int data_off = tcp_off + doff;
    int data_len = flen - data_off;
    if (data_len < 0) data_len = 0;

    /* Find matching connection by our local port */
    tcp_conn_t *c = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_conns[i].used && s_conns[i].local_port == dport) {
            c = &s_conns[i];
            break;
        }
    }
    if (!c) return;

    c->snd_wnd = wnd;

    /* --- RST: unconditional abort regardless of state --- */
    if (flags & TF_RST) {
        debug_print("[TCP] RST from %d.%d.%d.%d:%d (handle %d)\n",
            frame[26],frame[27],frame[28],frame[29],
            u16be(tcp + 0), (int)(c - s_conns));
        c->state = TCPS_CLOSED;
        c->used  = 0;
        return;
    }

    switch (c->state) {

    case TCPS_SYN_SENT:
        /* RFC 793 p.66: expect SYN+ACK */
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK)) {
            c->rcv_nxt = seq + 1;    /* SYN consumes one seq number */
            c->snd_una = ack;
            c->state   = TCPS_ESTABLISHED;

            /* Send ACK to complete three-way handshake */
            send_segment(c, TF_ACK, NULL, 0);

            debug_print("[TCP] ESTABLISHED ← %d.%d.%d.%d:%d (handle %d)\n",
                frame[26],frame[27],frame[28],frame[29],
                u16be(tcp + 0), (int)(c - s_conns));
        }
        break;

    case TCPS_ESTABLISHED:
        /* Update send window */
        if (flags & TF_ACK) c->snd_una = ack;

        /* Accept in-sequence data */
        if (data_len > 0 && seq == c->rcv_nxt) {
            rcvbuf_push(c, frame + data_off, data_len);
            c->rcv_nxt += (uint32_t)data_len;
            send_segment(c, TF_ACK, NULL, 0);
        }

        /* Remote close: FIN received */
        if (flags & TF_FIN) {
            c->rcv_nxt++;              /* FIN consumes one seq number */
            c->state = TCPS_CLOSE_WAIT;
            send_segment(c, TF_ACK, NULL, 0);
            /* Immediately send our FIN (simultaneous close, simplified) */
            c->state = TCPS_FIN_WAIT_1;
            send_segment(c, TF_FIN | TF_ACK, NULL, 0);
            c->snd_nxt++;
            debug_print("[TCP] FIN from peer — closing (handle %d)\n",
                (int)(c - s_conns));
        }
        break;

    case TCPS_FIN_WAIT_1:
        if (flags & TF_ACK) {
            c->snd_una = ack;
            /* Our FIN acked? */
            if (ack == c->snd_nxt)
                c->state = TCPS_FIN_WAIT_2;
        }
        /* Also accept any remaining data */
        if (data_len > 0 && seq == c->rcv_nxt) {
            rcvbuf_push(c, frame + data_off, data_len);
            c->rcv_nxt += (uint32_t)data_len;
        }
        if (flags & TF_FIN) {
            c->rcv_nxt++;
            c->state = TCPS_TIME_WAIT;
            send_segment(c, TF_ACK, NULL, 0);
            debug_print("[TCP] FIN_WAIT_1 → TIME_WAIT (handle %d)\n",
                (int)(c - s_conns));
        }
        break;

    case TCPS_FIN_WAIT_2:
        if (data_len > 0 && seq == c->rcv_nxt) {
            rcvbuf_push(c, frame + data_off, data_len);
            c->rcv_nxt += (uint32_t)data_len;
            send_segment(c, TF_ACK, NULL, 0);
        }
        if (flags & TF_FIN) {
            c->rcv_nxt++;
            c->state = TCPS_TIME_WAIT;
            send_segment(c, TF_ACK, NULL, 0);
            debug_print("[TCP] FIN_WAIT_2 → TIME_WAIT (handle %d)\n",
                (int)(c - s_conns));
        }
        break;

    case TCPS_TIME_WAIT:
        /* 2×MSL: for LAN testing just close immediately */
        c->state = TCPS_CLOSED;
        c->used  = 0;
        debug_print("[TCP] connection closed (handle %d)\n",
            (int)(c - s_conns));
        break;

    default:
        break;
    }
}
