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

/* boot334: Diagnostic counters for the [WIMP] heartbeat.
 *   genet_rx_count_raw  — software count of frames successfully received.
 *   genet_tx_cons_raw   — hardware TX DMA consumer index (frames consumed).
 *   genet_rx_fcs_raw    — software count of frames dropped due to CRC error. */
uint32_t genet_rx_count_raw(void);
uint32_t genet_tx_cons_raw(void);
uint32_t genet_rx_fcs_raw(void);

/* Poll for a received frame.  Copies into buf (up to maxlen bytes).
 * Returns frame length on success, 0 if no frame waiting, -1 on error. */
int  genet_poll_rx(void *buf, uint32_t maxlen);

/* Returns 1 if PHY link is up (autoneg complete), 0 otherwise. */
int  genet_link_up(void);

/* Board MAC address populated by genet_init() */
extern uint8_t g_genet_mac[6];

#endif /* GENET_H */
