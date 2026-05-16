/* drivers/net/genet_module.h — PhoenixGENET DCI4 module
 *
 * Wraps the BCM GENET hardware driver as a proper RISC OS DCI4
 * network driver module, following the same structural pattern as the
 * full RISC OS EtherGENET-6 module in Inet6Sources.
 *
 * DCI4 (Driver-Controller Interface version 4) is the contract between
 * an Ethernet driver and the Internet module above it.  The real-world
 * path on RISC OS is:
 *
 *   Internet module (Inet6Sources, DCI6/v600)
 *        ↕  Service_EnumerateNetworkDrivers6 / Service_DCIDriverStatus
 *   DCIShim  (translates DCI6↔DCI4, mbuf6↔mbuf4)
 *        ↕  DCI4 Filter + Transmit SWIs, Service_DCIDriverStatus v407
 *   PhoenixGENET (this module — DCI4, raw buffers for now)
 *        ↕  genet_send() / genet_poll_rx()
 *   BCM2711 GENET hardware
 *
 * DCIShim explicitly targets EtherGENET (driver.c comment: "I'm looking
 * at you, EtherGENET").  Once the Phoenix SWI+mbuf infrastructure is
 * ready, DCIShim can sit above PhoenixGENET without further driver changes.
 *
 * SWI chunk base: 0x59F00  (EtherGE_00 — RISC OS allocated chunk)
 * SWI offsets (DCI4 standard):
 *   +0  DCIVersion          returns DCI version (407 for DCI4)
 *   +1  Inquire             returns capability flags
 *   +2  GetNetworkMTU       returns MTU (1500)
 *   +3  SetNetworkMTU       returns ENOTTY (fixed Ethernet MTU)
 *   +4  Transmit            send packet to hardware
 *   +5  Filter              register / release an RX frame filter
 *   +6  Stats               return TX/RX statistics
 *   +7  MulticastRequest    multicast join/leave (stub)
 *   +8  EntryPoints         return DCI6 entry-point table pointer
 *
 * Author: Phoenix OS project
 * Added: boot376, May 2026
 */

#ifndef GENET_MODULE_H
#define GENET_MODULE_H

#include <stdint.h>

/* ── DCI4 constants ──────────────────────────────────────────────────────── */

#define ETHERGE_SWI_BASE        0x59F00u    /* EtherGE_00                    */

/* SWI offsets from base */
#define DCI4_DCIVERSION         0
#define DCI4_INQUIRE            1
#define DCI4_GETNETWORKMTU      2
#define DCI4_SETNETWORKMTU      3
#define DCI4_TRANSMIT           4
#define DCI4_FILTER             5
#define DCI4_STATS              6
#define DCI4_MULTICASTREQUEST   7
#define DCI4_ENTRYPOINTS        8

/* DCI version reported by this driver */
#define DCIVERSION              407         /* DCI4                          */
#define DCIVERSION6             600         /* DCI6 — for future use         */

/* Service call numbers */
#define Service_DCIDriverStatus         0x9Du
#define Service_DCIProtocolStatus       0x9Eu
#define Service_EnumerateNetworkDrivers 0x9Bu

/* Service_DCIDriverStatus r2 values */
#define DCIDRIVER_STARTING              0
#define DCIDRIVER_DYING                 1

/* Service_DCIProtocolStatus r2 values */
#define DCIPROTOCOL_STARTING            0
#define DCIPROTOCOL_DYING               1

/* Inquire capability flags */
#define INQ_MULTICAST           0x0002u     /* accepts multicast addresses   */
#define INQ_PROMISCUOUS         0x0004u     /* supports promiscuous mode     */
#define INQ_RXERRORS            0x0008u     /* can report RX errors          */
#define INQ_HWADDRVALID         0x0100u     /* hardware address is valid     */
#define INQ_SOFTHWADDR          0x0200u     /* address can be changed in sw  */
#define INQ_HASSTATS            0x0400u     /* DCI4Stats SWI is implemented  */

/* DIB slot id for on-board devices */
#define DIB_SLOT_SYS_BUS        0x27u

/* Filter flags (r0 to DCI4Filter SWI) */
#define FILTER_CLAIM            0x00u       /* register new filter           */
#define FILTER_RELEASE          0x01u       /* deregister filter             */
#define FILTER_NO_UNSAFE        0x02u       /* refuse unsafe filters         */
#define FILTER_1STRESERVED      0x10u       /* first reserved flag value     */

/* Frame level (top 16 bits of r2 to DCI4Filter) */
#define FRMLVL_E2SPECIFIC       0x0001u     /* specific EtherType            */
#define FRMLVL_E2SINK           0x0002u     /* all Ethernet II frames        */
#define FRMLVL_E2MONITOR        0x0003u     /* monitor all (including errs)  */
#define FRMLVL_IEEE             0x0004u     /* 802.3 frames                  */

/* Address level */
#define ADDRLVL_SPECIFIC        0x00u       /* unicast to our MAC only       */
#define ADDRLVL_NORMAL          0x01u       /* unicast + broadcast           */
#define ADDRLVL_MULTICAST       0x02u       /* + multicast                   */
#define ADDRLVL_PROMISCUOUS     0x03u       /* all frames                    */

/* Error level */
#define ERRLVL_NO_ERRORS        0x00u       /* deliver only good frames      */
#define ERRLVL_ERRORS           0x01u       /* deliver error frames too      */

/* Transmit flags (r0 to DCI4Transmit) */
#define TX_FAKESOURCE           0x01u       /* r5 = source MAC override      */
#define TX_DRIVERSDATA          0x02u       /* mbuf is driver-owned          */
#define TX_PROTOSDATA           0x04u       /* mbuf is protocol-owned        */
#define TX_1STRESERVED          0x08u

/* Ethernet constants */
#define ETHER_ADDR_LEN          6
#define ETHERMTU                1500
#define ETHERTYPE_IP            0x0800u
#define ETHERTYPE_ARP           0x0806u
#define ETHERTYPE_IPV6          0x86DDu
#define ETHERTYPE_REVARP        0x8035u
#define ETHERTYPE_VLAN          0x8100u

/* ── DCI4 data structures ────────────────────────────────────────────────── */

/* Driver Information Block — identifies this driver to the network stack. */
typedef struct {
    uint32_t  dib_swibase;              /* SWI chunk base (ETHERGE_SWI_BASE) */
    uint8_t  *dib_name;                 /* driver short name ("ege")         */
    uint32_t  dib_unit;                 /* unit number (0)                   */
    uint8_t  *dib_address;             /* 6-byte MAC address                */
    uint8_t  *dib_module;              /* module title ("EtherGE")          */
    uint8_t  *dib_location;           /* location string ("On-board")      */
    struct {
        uint8_t sl_slotid;              /* DIB_SLOT_SYS_BUS                  */
        uint8_t sl_minor;
        uint8_t sl_pcmciaslot;
        uint8_t sl_mbz;
    } dib_slot;
    uint32_t  dib_inquire;             /* OR of INQ_* flags                 */
} phoenix_dib_t;

/* Chain of DIBs — used by Service_EnumerateNetworkDrivers.                  */
typedef struct phoenix_chdib_s {
    struct phoenix_chdib_s *chd_next;
    phoenix_dib_t          *chd_dib;
} phoenix_chdib_t;

/* RX filter entry — registered by the Internet module (or DCIShim).        */
#define GENET_MAX_FILTERS       8

typedef struct phoenix_filter_s {
    struct phoenix_filter_s *next;
    uint32_t  type;          /* frametype | (framelevel << 16)              */
    uint8_t   addresslevel;
    uint8_t   errorlevel;
    void     *handler;       /* called with (dib*, buf*, len, frametype)    */
    void     *pw;            /* private word passed to handler              */
} phoenix_filter_t;

/* DCI4 stats buffer layout (mirrors struct stats from dcistructs.h). */
typedef struct {
    uint8_t  st_interface_type;
    uint8_t  st_link_polarity;
    uint16_t st_link_status;
    uint32_t st_tx_frames;
    uint32_t st_tx_bytes;
    uint32_t st_tx_general_errors;
    uint32_t st_rx_frames;
    uint32_t st_rx_bytes;
    uint32_t st_rx_general_errors;
    uint32_t st_unwanted_frames;
} phoenix_stats_buf_t;

/* ST_TYPE values for st_interface_type */
#define ST_TYPE_10BASET         0x00u
#define ST_TYPE_100BASETX       0x08u
#define ST_TYPE_1000BASET       0x0Fu

/* ST_STATUS bit flags for st_link_status */
#define ST_STATUS_ACTIVE        0x01u
#define ST_STATUS_OK            0x02u
#define ST_STATUS_FULL_DUPLEX   0x04u
#define ST_STATUS_HALF_DUPLEX   0x00u
#define ST_STATUS_DIRECT        0x10u
#define ST_STATUS_BROADCAST     0x20u
#define ST_STATUS_MULTICAST     0x40u
#define ST_STATUS_PROMISCUOUS   0x80u

/* ST_LINK_POLARITY */
#define ST_LINK_POLARITY_CORRECT 0x00u

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Module init / final — registered via module_register_native().
 * genet_module_init() calls genet_init() (hardware already up via
 * PhoenixDHCP) and announces the driver via Service_DCIDriverStatus.
 * Returns 0 on success.                                                      */
int  genet_module_init(void);
int  genet_module_final(void);

/* genet_module_rx — dispatch a received raw Ethernet frame.
 * Called from the WIMP loop RX drain (replaces direct net_rx_frame call).
 * Walks the registered filter list and calls matching handlers.
 * Also always feeds net_rx_frame() for Phoenix's own TCP/IP stack until
 * a proper Internet module registers above us.
 *   frame : pointer to raw Ethernet frame (dst MAC at offset 0)
 *   len   : total frame length in bytes                                      */
void genet_module_rx(const uint8_t *frame, int len);

/* genet_module_tx — transmit a raw Ethernet frame.
 * Builds the Ethernet header and calls genet_send().
 *   dst      : 6-byte destination MAC
 *   src      : 6-byte source MAC (NULL → use our hardware MAC)
 *   ethertype: big-endian EtherType (e.g. ETHERTYPE_IP)
 *   payload  : frame payload (after the 14-byte Ethernet header)
 *   plen     : payload length in bytes                                       */
int  genet_module_tx(const uint8_t *dst, const uint8_t *src,
                     uint16_t ethertype,
                     const uint8_t *payload, uint32_t plen);

/* genet_module_swi — SWI handler, called from swi_dispatch().
 * swi_offset : offset from ETHERGE_SWI_BASE (0=DCIVersion … 8=EntryPoints)
 * regs       : caller's register bank r0–r8 in/out                          */
int  genet_module_swi(uint32_t swi_offset, uint32_t *regs);

/* genet_module_get_dib — return pointer to our live Dib (read-only).        */
const phoenix_dib_t *genet_module_get_dib(void);

#endif /* GENET_MODULE_H */
