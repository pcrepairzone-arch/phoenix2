/*
 * resolver_module.c — PhoenixResolver: DNS stub resolver
 * boot379
 *
 * Implements Resolver_GetHostByName (SWI 0x46000) backed by real UDP DNS
 * queries to the DHCP-provided DNS server on port 53.
 *
 * Protocol: RFC 1035
 *   - Build a minimal DNS query: ID, flags, 1 question (QTYPE=A, QCLASS=IN)
 *   - Send via udp_send() to dns_server:53 from ephemeral port 1053
 *   - Poll udp_recvfrom() with ARM Generic Timer for timeout (5 seconds)
 *   - Parse response: skip header + question, walk answer RRs for type=1 A record
 *
 * Single-threaded cooperative model — no locks needed.
 *
 * Author: R Andrews – boot379
 */
#include "kernel.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "net/resolver_module.h"
#include "drivers/net/genet.h"   /* genet_poll_rx(), GENET_MAX_FRAME */
#include "errno.h"

extern void uart_puts(const char *s);

/* ── ARM Generic Timer helpers ─────────────────────────────────────────── */
static inline uint64_t cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
/* Returns 1 if `start_ticks` was more than `ms` milliseconds ago */
static int elapsed_ms(uint64_t start_ticks, uint32_t ms)
{
    uint64_t freq    = cntfrq();
    uint64_t elapsed = cntpct() - start_ticks;
    return (elapsed * 1000u) >= ((uint64_t)ms * freq);
}

/* ── Local UART helpers (no printf dependency) ──────────────────────────── */
static void res_puts(const char *s) { uart_puts(s); }

static void res_putip(const uint8_t ip[4])
{
    /* Print dotted-decimal without printf */
    static char buf[16];
    int pos = 0;
    for (int o = 0; o < 4; o++) {
        uint8_t v = ip[o];
        if (v >= 100) { buf[pos++] = (char)('0' + v / 100); v %= 100; }
        if (ip[o] >= 10) { buf[pos++] = (char)('0' + v / 10); v %= 10; }
        buf[pos++] = (char)('0' + v);
        if (o < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    uart_puts(buf);
}

/* ── Static result buffers (single-threaded) ────────────────────────────── */
static char      s_name_buf[256];
static uint8_t   s_addr_buf[4];
static char     *s_aliases[1]    = { NULL };
static char     *s_addr_list[2];  /* [0] = s_addr_buf cast, [1] = NULL */
static hostent_t s_hostent;

/* ── is_dotted_decimal ──────────────────────────────────────────────────── */
static int parse_dotted_decimal(const char *s, uint8_t out[4])
{
    int i = 0, octet = -1;
    const char *p = s;
    int dots = 0;
    while (*p) {
        char c = *p++;
        if (c >= '0' && c <= '9') {
            if (octet < 0) octet = 0;
            octet = octet * 10 + (c - '0');
            if (octet > 255) return 0;
        } else if (c == '.') {
            if (octet < 0 || i >= 3) return 0;
            out[i++] = (uint8_t)octet;
            octet = -1;
            dots++;
        } else {
            return 0;
        }
    }
    if (dots != 3 || octet < 0 || i != 3) return 0;
    out[3] = (uint8_t)octet;
    return 1;
}

/* ── build_dns_query ────────────────────────────────────────────────────── */
/*
 * Writes a minimal RFC 1035 A-record query into buf[].
 * Returns the number of bytes written, or 0 on error.
 *
 * Wire format:
 *   2  Transaction ID (big-endian, echoed in response)
 *   2  Flags: 0x0100 (standard query, recursion desired)
 *   2  QDCOUNT = 1
 *   2  ANCOUNT = 0
 *   2  NSCOUNT = 0
 *   2  ARCOUNT = 0
 *   N  QNAME  (label-encoded)
 *   2  QTYPE  = 0x0001 (A)
 *   2  QCLASS = 0x0001 (IN)
 */
static int build_dns_query(const char *name, uint8_t *buf, int bufsz,
                            uint16_t txid)
{
    if (bufsz < 18) return 0;

    /* Header */
    buf[0] = (uint8_t)(txid >> 8);
    buf[1] = (uint8_t)(txid & 0xFF);
    buf[2] = 0x01; buf[3] = 0x00;   /* flags: recursion desired */
    buf[4] = 0x00; buf[5] = 0x01;   /* QDCOUNT = 1 */
    buf[6] = 0x00; buf[7] = 0x00;   /* ANCOUNT */
    buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT */
    buf[10]= 0x00; buf[11]= 0x00;   /* ARCOUNT */

    int pos = 12;

    /* QNAME: label-encode each dot-separated component */
    const char *p = name;
    while (*p) {
        const char *label_start = p;
        while (*p && *p != '.') p++;
        int label_len = (int)(p - label_start);
        if (label_len == 0 || label_len > 63) return 0;
        if (pos + 1 + label_len + 5 > bufsz) return 0;
        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++)
            buf[pos++] = (uint8_t)label_start[i];
        if (*p == '.') p++;
    }
    buf[pos++] = 0x00;   /* root label */

    /* QTYPE = A (1), QCLASS = IN (1) */
    buf[pos++] = 0x00; buf[pos++] = 0x01;
    buf[pos++] = 0x00; buf[pos++] = 0x01;

    return pos;
}

/* ── parse_dns_response ─────────────────────────────────────────────────── */
/*
 * Walks the answer section of a DNS response looking for a type=1 (A)
 * class=1 (IN) RR with rdlen=4.  Copies the first matching address into
 * addr_out[4].  Returns 1 on success, 0 on failure.
 */
static int parse_dns_response(const uint8_t *pkt, int pkt_len,
                               uint16_t txid, uint8_t addr_out[4])
{
    if (pkt_len < 12) return 0;

    /* Verify transaction ID */
    uint16_t rid = ((uint16_t)pkt[0] << 8) | pkt[1];
    if (rid != txid) return 0;

    /* Check QR bit (bit 15 of flags) — must be response */
    if (!(pkt[2] & 0x80)) return 0;

    /* RCODE (lower 4 bits of byte 3) must be 0 (NOERROR) */
    if ((pkt[3] & 0x0F) != 0) return 0;

    uint16_t qdcount = ((uint16_t)pkt[4] << 8) | pkt[5];
    uint16_t ancount = ((uint16_t)pkt[6] << 8) | pkt[7];
    if (ancount == 0) return 0;

    int pos = 12;

    /* Skip question section */
    for (int q = 0; q < (int)qdcount; q++) {
        /* Skip QNAME (label sequence) */
        while (pos < pkt_len) {
            uint8_t len = pkt[pos];
            if (len == 0) { pos++; break; }
            if ((len & 0xC0) == 0xC0) { pos += 2; break; }  /* compression ptr */
            pos += 1 + len;
        }
        pos += 4;   /* QTYPE + QCLASS */
    }

    /* Walk answer RRs */
    for (int a = 0; a < (int)ancount && pos < pkt_len; a++) {
        /* Skip NAME (may be a compression pointer) */
        if (pos >= pkt_len) break;
        uint8_t first = pkt[pos];
        if ((first & 0xC0) == 0xC0) {
            pos += 2;   /* compression pointer */
        } else {
            while (pos < pkt_len) {
                uint8_t len = pkt[pos];
                if (len == 0) { pos++; break; }
                if ((len & 0xC0) == 0xC0) { pos += 2; break; }
                pos += 1 + len;
            }
        }

        if (pos + 10 > pkt_len) break;

        uint16_t rtype  = ((uint16_t)pkt[pos]     << 8) | pkt[pos + 1];
        uint16_t rclass = ((uint16_t)pkt[pos + 2] << 8) | pkt[pos + 3];
        /* ttl: pkt[pos+4..7] — ignored */
        uint16_t rdlen  = ((uint16_t)pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;

        if (rtype == 1 && rclass == 1 && rdlen == 4) {
            if (pos + 4 > pkt_len) break;
            addr_out[0] = pkt[pos];
            addr_out[1] = pkt[pos + 1];
            addr_out[2] = pkt[pos + 2];
            addr_out[3] = pkt[pos + 3];
            return 1;
        }
        pos += rdlen;
    }
    return 0;
}

/* ── resolver_gethostbyname ─────────────────────────────────────────────── */
/*
 * Blocking DNS A-record lookup.
 * Returns pointer to static hostent on success, NULL on failure.
 *
 * Callers in a cooperative scheduler should call this only when they can
 * afford to poll for up to DNS_TIMEOUT_MS without yielding.
 */
#define DNS_LOCAL_PORT   1053u
#define DNS_SERVER_PORT  53u
#define DNS_TIMEOUT_MS   5000u
#define DNS_RETRIES      3

hostent_t *resolver_gethostbyname(const char *name)
{
    if (!name || !*name) return NULL;

    /* Fast path: already a dotted-decimal address */
    if (parse_dotted_decimal(name, s_addr_buf)) {
        goto fill_hostent;
    }

    /* Get DNS server from DHCP lease */
    uint8_t dns_ip[4];
    dhcp_get_dns(dns_ip);
    if (dns_ip[0] == 0 && dns_ip[1] == 0 && dns_ip[2] == 0 && dns_ip[3] == 0) {
        res_puts("[Resolver] No DNS server from DHCP\n");
        return NULL;
    }

    static uint16_t s_txid = 0x1234;
    s_txid++;

    static uint8_t s_query[512];
    int qlen = build_dns_query(name, s_query, (int)sizeof(s_query), s_txid);
    if (qlen <= 0) {
        res_puts("[Resolver] Failed to build DNS query\n");
        return NULL;
    }

    static uint8_t s_resp[512];

    for (int attempt = 0; attempt < DNS_RETRIES; attempt++) {
        res_puts("[Resolver] DNS query for "); res_puts(name);
        res_puts(" -> ");
        res_putip(dns_ip); res_puts(":53\n");

        udp_send(dns_ip, DNS_SERVER_PORT, DNS_LOCAL_PORT,
                 s_query, qlen);

        /* Poll for response — must drive GENET RX ourselves (no task context).
         * boot381: mirrors the dhcp.c poll pattern: genet_poll_rx → net_rx_frame
         * → udp_rx → udp_rx_enqueue.  Without this the DNS reply sits in the
         * GENET ring unread and udp_recvfrom always returns 0.               */
        static uint8_t s_rx_frame[GENET_MAX_FRAME];
        uint64_t t0 = cntpct();
        while (!elapsed_ms(t0, DNS_TIMEOUT_MS)) {
            /* Drain one frame from GENET into the protocol stack */
            int flen = genet_poll_rx(s_rx_frame, GENET_MAX_FRAME);
            if (flen > 0)
                net_rx_frame(s_rx_frame, flen);

            /* Check if a DNS reply landed in the UDP queue */
            uint8_t  src_ip[4];
            uint16_t src_port = 0;
            int n = udp_recvfrom(DNS_LOCAL_PORT, s_resp, (int)sizeof(s_resp),
                                 src_ip, &src_port);
            if (n > 0) {
                if (parse_dns_response(s_resp, n, s_txid, s_addr_buf)) {
                    res_puts("[Resolver] "); res_puts(name);
                    res_puts(" -> "); res_putip(s_addr_buf); res_puts("\n");
                    goto fill_hostent;
                }
                /* Wrong ID or RCODE — keep polling */
            }
        }
        res_puts("[Resolver] DNS timeout (attempt ");
        /* print attempt number inline */
        char at_buf[4]; at_buf[0] = (char)('1' + attempt); at_buf[1] = ')'; at_buf[2] = '\n'; at_buf[3] = '\0';
        res_puts(at_buf);
    }

    res_puts("[Resolver] DNS lookup failed: "); res_puts(name); res_puts("\n");
    return NULL;

fill_hostent:
    /* Copy name into static buffer */
    {
        int i = 0;
        while (name[i] && i < 254) { s_name_buf[i] = name[i]; i++; }
        s_name_buf[i] = '\0';
    }
    s_addr_list[0] = (char *)s_addr_buf;
    s_addr_list[1] = NULL;
    s_hostent.h_name      = s_name_buf;
    s_hostent.h_aliases   = s_aliases;
    s_hostent.h_addrtype  = AF_INET;
    s_hostent.h_length    = 4;
    s_hostent.h_addr_list = s_addr_list;
    return &s_hostent;
}

/* ── resolver_module_init ───────────────────────────────────────────────── */
int resolver_module_init(void)
{
    res_puts("[Resolver] PhoenixResolver init (boot381)\n");
    res_puts("[Resolver] SWI base 0x46000 (Inet6Sources Resolver compatible)\n");

    /* Sanity-check: attempt a DNS lookup of a known host.
     * Failure is non-fatal — network may not be up yet in all configs. */
    hostent_t *h = resolver_gethostbyname("www.google.com");
    if (h) {
        res_puts("[Resolver] Self-test OK: www.google.com = ");
        res_putip(s_addr_buf); res_puts("\n");
    } else {
        res_puts("[Resolver] Self-test: DNS lookup failed (non-fatal)\n");
    }
    return 0;
}

/* ── resolver_module_final ──────────────────────────────────────────────── */
int resolver_module_final(void)
{
    res_puts("[Resolver] PhoenixResolver finalised\n");
    return 0;
}

/* ── resolver_module_swi ────────────────────────────────────────────────── */
/*
 * SWI dispatch for RESOLVER_SWI_BASE chunk.
 * Called by swi_dispatch() with swi_offset = swi_number & 0xFF.
 *
 * Resolver_GetHostByName (offset 0):
 *   Entry: regs[0] = pointer to hostname string
 *   Exit:  regs[0] = pointer to hostent_t, or 0 on error
 *          returns 0 on success, -ENOSYS / -ENOENT on failure
 */
int resolver_module_swi(uint32_t swi_offset, uint32_t *regs)
{
    switch (swi_offset) {
    case RESOLVER_SWI_GETHOSTBYNAME: {
        const char *hostname = (const char *)(uintptr_t)regs[0];
        if (!hostname) { regs[0] = 0; return -EINVAL; }
        hostent_t *h = resolver_gethostbyname(hostname);
        regs[0] = h ? (uint32_t)(uintptr_t)h : 0u;
        return h ? 0 : -ENOENT;
    }
    default:
        return -ENOSYS;
    }
}
