/*
 * genet.h — Phoenix interface for BCM2711 GENETv5 Ethernet driver.
 * boot302: polling-mode TX/RX, no IRQs.
 */
#ifndef GENET_H
#define GENET_H

#include <stdint.h>

/* Maximum Ethernet frame size (no VLAN) */
#define GENET_MAX_FRAME  1536

/* Packet buffer size per descriptor slot (must be >= GENET_MAX_FRAME + 2) */
#define GENET_BUF_SIZE   2048

/* Call once from kernel_main() after PCI/USB init */
void genet_init(void);

/* Transmit one frame. buf/len = raw Ethernet frame (header + payload).
 * Returns 0 on success, -1 if no free descriptor or len out of range. */
int  genet_send(const void *buf, uint32_t len);

/* boot329: Return TX DMA producer/consumer indices for diagnostics.
 * Call before and after genet_send to confirm TDMA is consuming descriptors. */
void genet_tx_diag(uint32_t *prod_out, uint32_t *cons_out);

/* Poll for a received frame.  Copies into buf (up to maxlen bytes).
 * Returns frame length on success, 0 if no frame waiting or frame skipped
 * (error/oversized — consumer index still advanced), -1 on driver not init. */
int  genet_poll_rx(void *buf, uint32_t maxlen);

/* boot347: Returns number of frames currently waiting in the RDMA ring.
 * Use to distinguish "ring empty" from "frame skipped due to error":
 *   poll_rx()==0 && rx_available()>0  -> frame was skipped (error/oversized)
 *   poll_rx()==0 && rx_available()==0 -> ring is genuinely empty            */
int  genet_rx_available(void);

/* Returns 1 if PHY link is up (autoneg complete), 0 otherwise. */
int  genet_link_up(void);

/* Diagnostic counters and raw index reads (boot347) */
uint32_t genet_rx_pidx_raw(void);   /* RX DMA producer index (hardware)        */
uint32_t genet_rx_count_raw(void);  /* frames successfully delivered to caller  */
uint32_t genet_rx_fcs_raw(void);    /* frames dropped due to CRC/FCS error      */
uint32_t genet_tx_cons_raw(void);   /* TX DMA consumer index (mib= field)       */

/* Apply negotiated PHY speed/duplex to the UMAC (call on link-up). */
void genet_apply_link(void);

/* Board MAC address populated by genet_init() */
extern uint8_t g_genet_mac[6];

#endif /* GENET_H */
