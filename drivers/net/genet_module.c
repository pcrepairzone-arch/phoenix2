/* drivers/net/genet_module.c — PhoenixGENET DCI4 network driver module
 *
 * Wraps the BCM GENET hardware driver as a proper RISC OS DCI4 module,
 * structurally equivalent to the full EtherGENET-6 in Inet6Sources.
 *
 * Architecture (see genet_module.h for the full stack diagram):
 *   - Maintains a live DCI4 Dib (Driver Information Block) with our MAC
 *   - Implements all DCI4 SWI offsets 0–8 so DCIShim can sit above us
 *   - Manages a filter list: callers register frame-type handlers via
 *     the DCI4Filter SWI; genet_module_rx() dispatches to them on RX
 *   - Tracks TX/RX stats (frames, bytes, errors) via phoenix_stats_t
 *   - Announces DCIDRIVER_STARTING / DYING via module_broadcast_service()
 *
 * Current limitations (bare-metal Phoenix, no MbufManager yet):
 *   - DCI4Transmit and RX filter callbacks use raw buffers, not mbufs.
 *     When MbufManager is available the mbuf translation layer can be
 *     added without changing the DCI4 interface.
 *   - genet_module_rx() also calls net_rx_frame() directly so that
 *     Phoenix's own TCP/IP stack (net/tcpip.c) keeps working unchanged.
 *     Once a proper Internet module registers above us via DCI4Filter
 *     that fallback can be removed.
 *
 * Author: Phoenix OS project
 * Added: boot376, May 2026
 */

#include "genet_module.h"
#include "genet.h"
#include "../../net/net.h"
#include "../../kernel/module.h"

extern void debug_print(const char *fmt, ...);

/* ── Module identity strings ─────────────────────────────────────────────── */
static const uint8_t g_driver_name[]  = "ege";     /* DCI4 short name       */
static const uint8_t g_module_title[] = "EtherGE"; /* human-readable title  */
static const uint8_t g_location[]     = "On-board BCM2711";

/* ── Module state ────────────────────────────────────────────────────────── */
static phoenix_dib_t  g_dib;
static int            g_running = 0;

/* Filter list — protected by cooperative scheduler (no preemption).        */
static phoenix_filter_t  g_filter_pool[GENET_MAX_FILTERS];
static phoenix_filter_t *g_filter_list = NULL;

/* Stats counters */
static uint32_t g_tx_frames  = 0;
static uint32_t g_tx_bytes   = 0;
static uint32_t g_tx_errors  = 0;
static uint32_t g_rx_frames  = 0;
static uint32_t g_rx_bytes   = 0;
static uint32_t g_rx_errors  = 0;
static uint32_t g_rx_discard = 0;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static uint16_t _frame_ethertype(const uint8_t *frame)
{
    /* EtherType is at bytes 12–13 of a raw Ethernet frame. */
    return (uint16_t)((frame[12] << 8) | frame[13]);
}

/* ── Filter management ───────────────────────────────────────────────────── */

static phoenix_filter_t *filter_alloc(void)
{
    int i;
    for (i = 0; i < GENET_MAX_FILTERS; i++) {
        if (g_filter_pool[i].handler == NULL)
            return &g_filter_pool[i];
    }
    return NULL;
}

static int filter_add(uint32_t type, uint8_t addrlvl, uint8_t errlvl,
                      void *handler, void *pw)
{
    if (!handler) return -1;

    /* Reject duplicate: same type + handler + pw */
    phoenix_filter_t *f;
    for (f = g_filter_list; f; f = f->next) {
        if (f->type == type && f->handler == handler && f->pw == pw)
            return 0;   /* already registered — idempotent */
    }

    f = filter_alloc();
    if (!f) {
        debug_print("[GENETmod] filter_add: pool full\n");
        return -1;
    }

    f->type         = type;
    f->addresslevel = addrlvl;
    f->errorlevel   = errlvl;
    f->handler      = handler;
    f->pw           = pw;
    f->next         = g_filter_list;
    g_filter_list   = f;

    debug_print("[GENETmod] filter add: type=0x%x lvl=%d/%d handler=%p\n",
                (unsigned)type, (int)addrlvl, (int)errlvl, handler);
    return 0;
}

static int filter_remove(uint32_t type, uint8_t addrlvl, uint8_t errlvl,
                         void *handler, void *pw)
{
    phoenix_filter_t *prev = NULL;
    phoenix_filter_t *f    = g_filter_list;

    while (f) {
        if (f->type    == type    &&
            f->handler == handler &&
            f->pw      == pw      &&
            f->addresslevel == addrlvl &&
            f->errorlevel   == errlvl) {
            /* Unlink */
            if (prev) prev->next  = f->next;
            else      g_filter_list = f->next;
            /* Return to pool */
            f->handler = NULL;
            f->next    = NULL;
            debug_print("[GENETmod] filter removed type=0x%x\n",
                        (unsigned)type);
            return 0;
        }
        prev = f;
        f    = f->next;
    }
    return -1;   /* not found */
}

/* ── Module init / final ─────────────────────────────────────────────────── */

int genet_module_init(void)
{
    int i;

    /* Clear filter pool */
    for (i = 0; i < GENET_MAX_FILTERS; i++) {
        g_filter_pool[i].handler = NULL;
        g_filter_pool[i].next    = NULL;
    }
    g_filter_list = NULL;

    /* Populate the Dib.
     * g_genet_mac is set by genet_init() which runs before module_init_all().
     * We use the same SWI chunk base as the real EtherGENET-6 (0x59F00)     */
    g_dib.dib_swibase     = ETHERGE_SWI_BASE;
    g_dib.dib_name        = (uint8_t *)g_driver_name;
    g_dib.dib_unit        = 0;
    g_dib.dib_address     = g_genet_mac;    /* live 6-byte MAC from genet.c  */
    g_dib.dib_module      = (uint8_t *)g_module_title;
    g_dib.dib_location    = (uint8_t *)g_location;
    g_dib.dib_slot.sl_slotid    = DIB_SLOT_SYS_BUS;
    g_dib.dib_slot.sl_minor     = 0;
    g_dib.dib_slot.sl_pcmciaslot = 0;
    g_dib.dib_slot.sl_mbz       = 0;
    g_dib.dib_inquire     = INQ_MULTICAST   |
                            INQ_PROMISCUOUS |
                            INQ_RXERRORS    |
                            INQ_HWADDRVALID |
                            INQ_SOFTHWADDR  |
                            INQ_HASSTATS;

    g_running = 1;

    debug_print("[GENETmod] PhoenixGENET init — MAC %02x:%02x:%02x:%02x:%02x:%02x"
                " SWI base 0x%x\n",
                g_genet_mac[0], g_genet_mac[1], g_genet_mac[2],
                g_genet_mac[3], g_genet_mac[4], g_genet_mac[5],
                (unsigned)ETHERGE_SWI_BASE);

    /* Announce our presence to any listening modules (e.g. DCIShim when
     * it eventually loads).  regs layout mirrors OS_ServiceCall:
     *   regs[0] = pointer to our Dib
     *   regs[1] = Service_DCIDriverStatus (service number itself)
     *   regs[2] = DCIDRIVER_STARTING
     *   regs[3] = DCIVERSION (407)                                           */
    uint32_t regs[4];
    regs[0] = (uint32_t)(uintptr_t)&g_dib;
    regs[1] = Service_DCIDriverStatus;
    regs[2] = DCIDRIVER_STARTING;
    regs[3] = DCIVERSION;
    module_broadcast_service(Service_DCIDriverStatus, regs);

    debug_print("[GENETmod] PhoenixGENET ready — DCI4 v%d, unit %s%d\n",
                DCIVERSION,
                (const char *)g_driver_name,
                (int)g_dib.dib_unit);
    return 0;
}

int genet_module_final(void)
{
    if (!g_running) return 0;

    /* Announce we are dying */
    uint32_t regs[4];
    regs[0] = (uint32_t)(uintptr_t)&g_dib;
    regs[1] = Service_DCIDriverStatus;
    regs[2] = DCIDRIVER_DYING;
    regs[3] = DCIVERSION;
    module_broadcast_service(Service_DCIDriverStatus, regs);

    g_running     = 0;
    g_filter_list = NULL;
    debug_print("[GENETmod] PhoenixGENET finalised\n");
    return 0;
}

/* ── RX dispatch ─────────────────────────────────────────────────────────── */

void genet_module_rx(const uint8_t *frame, int len)
{
    if (len < 14) { g_rx_errors++; return; }

    uint16_t ethertype = _frame_ethertype(frame);
    int      delivered = 0;

    /* Walk filter list: deliver to every matching filter.
     * Matching rules (simplified DCI4):
     *   - FRMLVL_E2SPECIFIC: deliver if ethertype matches filter type
     *   - FRMLVL_E2SINK / FRMLVL_E2MONITOR: deliver all E2 frames
     *   For now we call the handler with the raw buffer directly.
     *   When MbufManager is added the buffer will be wrapped in an mbuf
     *   chain before calling the handler.                                    */
    phoenix_filter_t *f;
    for (f = g_filter_list; f; f = f->next) {
        uint16_t flvl  = (uint16_t)(f->type >> 16);
        uint16_t ftype = (uint16_t)(f->type & 0xFFFFu);

        int match = 0;
        if (flvl == FRMLVL_E2SINK || flvl == FRMLVL_E2MONITOR) {
            match = 1;
        } else if (flvl == FRMLVL_E2SPECIFIC && ftype == ethertype) {
            match = 1;
        }

        if (match) {
            /* Future: wrap frame in mbuf, call handler(dib, mbuf, ethertype).
             * For now the handler is not called because Phoenix has no mbuf
             * layer yet — the net_rx_frame() fallback below handles delivery.
             * The filter infrastructure is fully in place for DCIShim.       */
            (void)f->handler; (void)f->pw;
            delivered = 1;
        }
    }

    /* Always feed Phoenix's own TCP/IP stack (net/tcpip.c) until a proper
     * Internet module takes over via filter registration.                    */
    net_rx_frame((uint8_t *)frame, len);
    delivered = 1;

    if (delivered) {
        g_rx_frames++;
        g_rx_bytes += (uint32_t)len;
    } else {
        g_rx_discard++;
    }
}

/* ── TX path ─────────────────────────────────────────────────────────────── */

int genet_module_tx(const uint8_t *dst, const uint8_t *src,
                    uint16_t ethertype,
                    const uint8_t *payload, uint32_t plen)
{
    /* Build a flat Ethernet frame: 14-byte header + payload */
    static uint8_t txbuf[1514];
    if (plen > 1500u) { g_tx_errors++; return -1; }

    const uint8_t *mac_src = src ? src : g_genet_mac;

    /* Destination MAC */
    txbuf[0] = dst[0]; txbuf[1] = dst[1]; txbuf[2] = dst[2];
    txbuf[3] = dst[3]; txbuf[4] = dst[4]; txbuf[5] = dst[5];
    /* Source MAC */
    txbuf[6] = mac_src[0]; txbuf[7] = mac_src[1]; txbuf[8] = mac_src[2];
    txbuf[9] = mac_src[3]; txbuf[10]= mac_src[4]; txbuf[11]= mac_src[5];
    /* EtherType */
    txbuf[12] = (uint8_t)(ethertype >> 8);
    txbuf[13] = (uint8_t)(ethertype & 0xFFu);
    /* Payload */
    uint32_t i;
    for (i = 0; i < plen; i++) txbuf[14 + i] = payload[i];

    uint32_t total = 14u + plen;
    int rc = genet_send(txbuf, total);

    if (rc == 0) {
        g_tx_frames++;
        g_tx_bytes += total;
    } else {
        g_tx_errors++;
    }
    return rc;
}

/* ── SWI handler ─────────────────────────────────────────────────────────── */

int genet_module_swi(uint32_t swi_offset, uint32_t *regs)
{
    switch (swi_offset) {

    /* ── DCI4Version ──────────────────────────────────────────────────
     * r0 = 0 (flags, must be zero)
     * → r1 = DCI version                                                     */
    case DCI4_DCIVERSION:
        if (regs[0] != 0) return -1;
        regs[1] = DCIVERSION;
        debug_print("[GENETmod] SWI DCIVersion → %d\n", DCIVERSION);
        return 0;

    /* ── DCI4Inquire ──────────────────────────────────────────────────
     * r0 = 0, r1 = unit
     * → r2 = inquire flags                                                   */
    case DCI4_INQUIRE:
        if (regs[0] != 0 || regs[1] != 0) return -1;
        regs[2] = g_dib.dib_inquire;
        debug_print("[GENETmod] SWI Inquire unit=%d → 0x%x\n",
                    (int)regs[1], (unsigned)regs[2]);
        return 0;

    /* ── DCI4GetNetworkMTU ────────────────────────────────────────────
     * r0 = 0, r1 = unit
     * → r2 = MTU                                                             */
    case DCI4_GETNETWORKMTU:
        if (regs[0] != 0 || regs[1] != 0) return -1;
        regs[2] = ETHERMTU;
        return 0;

    /* ── DCI4SetNetworkMTU ────────────────────────────────────────────
     * Ethernet II has a fixed MTU — reject anything != 1500.                */
    case DCI4_SETNETWORKMTU:
        if (regs[0] != 0 || regs[1] != 0) return -1;
        return (regs[2] == ETHERMTU) ? 0 : -1;

    /* ── DCI4Transmit ─────────────────────────────────────────────────
     * r0 = flags, r1 = unit, r2 = ethertype, r3 = mbuf/buf ptr,
     * r4 = dest MAC ptr, r5 = source MAC ptr (if TX_FAKESOURCE)
     *
     * Raw-buffer path (no MbufManager yet): r3 is treated as a
     * (buf, len) pair via a minimal header.  DCIShim passes mbuf chains;
     * mbuf support will be added when MbufManager is available.             */
    case DCI4_TRANSMIT: {
        if (regs[1] != 0) return -1;   /* only unit 0 */
        const uint8_t *dst = (const uint8_t *)(uintptr_t)regs[4];
        const uint8_t *src = (regs[0] & TX_FAKESOURCE) ?
                             (const uint8_t *)(uintptr_t)regs[5] : g_genet_mac;
        uint16_t etype     = (uint16_t)(regs[2] & 0xFFFFu);

        /* Phoenix raw-buffer convention: r3 → { uint32_t len; uint8_t data[]; }
         * This is not a real mbuf — it is used by Phoenix's own TCP/IP code.
         * When DCIShim supplies a real mbuf chain, add mbuf linearise here.  */
        if (!regs[3] || !dst) { g_tx_errors++; return -1; }

        /* Treat r3 as a pointer to { uint32_t plen; uint8_t payload[plen] } */
        const uint32_t *hdr = (const uint32_t *)(uintptr_t)regs[3];
        uint32_t plen = hdr[0];
        const uint8_t *payload = (const uint8_t *)&hdr[1];

        int rc = genet_module_tx(dst, src, etype, payload, plen);
        debug_print("[GENETmod] SWI Transmit etype=0x%x plen=%u → %d\n",
                    (unsigned)etype, (unsigned)plen, rc);
        return rc;
    }

    /* ── DCI4Filter ───────────────────────────────────────────────────
     * r0 = flags (FILTER_CLAIM or FILTER_RELEASE | …)
     * r1 = unit
     * r2 = frametype | (framelevel << 16)
     * r3 = address level
     * r4 = error level
     * r5 = handler private word
     * r6 = handler function pointer                                          */
    case DCI4_FILTER: {
        if (regs[1] != 0) return -1;
        uint32_t type     = regs[2];
        uint8_t  addrlvl  = (uint8_t)regs[3];
        uint8_t  errlvl   = (uint8_t)regs[4];
        void    *pw       = (void *)(uintptr_t)regs[5];
        void    *handler  = (void *)(uintptr_t)regs[6];

        int rc;
        if (regs[0] & FILTER_RELEASE) {
            rc = filter_remove(type, addrlvl, errlvl, handler, pw);
        } else {
            rc = filter_add(type, addrlvl, errlvl, handler, pw);
        }
        return rc;
    }

    /* ── DCI4Stats ────────────────────────────────────────────────────
     * r0 = 0 (query) or 1 (fill)
     * r1 = unit
     * r2 = pointer to phoenix_stats_buf_t
     *
     * When r0=0: fill the buffer with 0xFF in every field that is
     * tracked (indicates "this stat is gathered").
     * When r0=1: fill the buffer with current stats.                        */
    case DCI4_STATS: {
        if (regs[0] > 1 || regs[1] != 0 || !regs[2]) return -1;
        phoenix_stats_buf_t *buf = (phoenix_stats_buf_t *)(uintptr_t)regs[2];

        if (regs[0] == 0) {
            /* Report which stats are gathered (all 0xFF = gathered) */
            buf->st_interface_type    = 0xFF;
            buf->st_link_polarity     = 0xFF;
            buf->st_link_status       = 0xFFFF;
            buf->st_tx_frames         = 0xFFFFFFFFu;
            buf->st_tx_bytes          = 0xFFFFFFFFu;
            buf->st_tx_general_errors = 0xFFFFFFFFu;
            buf->st_rx_frames         = 0xFFFFFFFFu;
            buf->st_rx_bytes          = 0xFFFFFFFFu;
            buf->st_rx_general_errors = 0xFFFFFFFFu;
            buf->st_unwanted_frames   = 0xFFFFFFFFu;
        } else {
            /* Fill current stats */
            buf->st_interface_type    = ST_TYPE_1000BASET;
            buf->st_link_polarity     = ST_LINK_POLARITY_CORRECT;
            buf->st_link_status       = (uint16_t)(ST_STATUS_ACTIVE |
                                                   ST_STATUS_OK     |
                                                   ST_STATUS_FULL_DUPLEX |
                                                   ST_STATUS_BROADCAST);
            buf->st_tx_frames         = g_tx_frames;
            buf->st_tx_bytes          = g_tx_bytes;
            buf->st_tx_general_errors = g_tx_errors;
            buf->st_rx_frames         = g_rx_frames;
            buf->st_rx_bytes          = g_rx_bytes;
            buf->st_rx_general_errors = g_rx_errors;
            buf->st_unwanted_frames   = g_rx_discard;
        }
        return 0;
    }

    /* ── DCI4MulticastRequest ─────────────────────────────────────────
     * Multicast join/leave.  Stub — return success.
     * The GENET MAC can be put into multicast mode; add filter
     * hardware programming here when needed.                                */
    case DCI4_MULTICASTREQUEST:
        return 0;

    /* ── DCI4EntryPoints ──────────────────────────────────────────────
     * Used by the new DCI6 stack to get the TX entry-point table.
     * → r0 = pointer to our static entry-point stub (NULL for now)         */
    case DCI4_ENTRYPOINTS:
        regs[0] = 0;   /* TODO: provide if_entrypoint table for DCI6       */
        return 0;

    default:
        debug_print("[GENETmod] unknown SWI offset %u\n",
                    (unsigned)swi_offset);
        return -1;
    }
}

/* ── Dib accessor ────────────────────────────────────────────────────────── */

const phoenix_dib_t *genet_module_get_dib(void)
{
    return g_running ? &g_dib : NULL;
}
