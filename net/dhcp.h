/* net/dhcp.h — Phoenix OS DHCP client module API
 *
 * Extracted from kernel/lib.c inline implementation (boot334–344).
 * Registered as a native Phoenix module via module_register_native().
 *
 * State machine: IDLE → DISCOVER → REQUEST → BOUND
 *   dhcp_start()  — send DISCOVER (state set BEFORE TX — boot345 race fix)
 *   dhcp_tick()   — retransmit / timeout engine
 *   dhcp_rx()     — frame handler; silently ignores non-DHCP frames
 *
 * boot369 module init (blocking):
 *   Registered in kernel/module.c module_init_all() as "PhoenixDHCP".
 *   genet_init() now runs BEFORE module_init_all() so g_genet_mac is live.
 *   dhcp_module_init() blocks until DHCP BOUND (or 30 s static fallback):
 *     1. dhcp_init(g_genet_mac)  — real MAC, clear state
 *     2. wait genet_link_up()    — PHY autoneg, up to 10 s
 *     3. genet_apply_link()      — UMAC speed match
 *     4. dhcp_start()            — DISCOVER
 *     5. poll loop               — genet_poll_rx → dhcp_rx → dhcp_tick
 *   On return the full lease (IP, mask, gw, dns, lease_secs) is stored.
 *   wimp_task() starts with a bound IP — no DHCP calls needed in the loop.
 *
 * Bug fixes vs inline boot334 version:
 *   [boot345 fix-1] g_dhcp_st set BEFORE genet_send() — closes ISR race window
 *   [boot345 fix-2] Caller resets last_net after dhcp_start() — forces RX poll
 *   [boot345 fix-3] Tight RX drain loop inside dhcp_start() for polling mode
 *
 * Author: Phoenix OS project
 * Updated: boot345 candidate, May 2026
 */

#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <stdint.h>

/* ── DHCP state constants ────────────────────────────────────────────────── */
#define DHCP_ST_IDLE      0   /* not started                     */
#define DHCP_ST_DISCOVER  1   /* DISCOVER sent, awaiting OFFER   */
#define DHCP_ST_REQUEST   2   /* REQUEST sent,  awaiting ACK     */
#define DHCP_ST_BOUND     3   /* ACK received, IP assigned       */

/* ── Exported state (read-only for lib.c ARP/ICMP handlers) ─────────────── */
extern uint8_t g_our_ip[4];   /* 0.0.0.0 until BOUND or static fallback */

/* ── Public API ──────────────────────────────────────────────────────────── */

/* dhcp_init — one-time init; must be called before dhcp_start().
 * mac: pointer to our 6-byte Ethernet MAC address (g_genet_mac).        */
void dhcp_init(const uint8_t *mac);

/* dhcp_start — called on link-up.
 * Sets state to DISCOVER BEFORE transmitting so no ISR race window.
 * Caller should also reset its last_net timestamp to force immediate RX. */
void dhcp_start(void);

/* dhcp_tick — retransmit / timeout engine.
 * Call from WIMP loop retransmit block.  now_ms = wimp_ms().
 * Handles 4-second retransmit and 5-retry static-fallback logic.        */
void dhcp_tick(uint32_t now_ms);

/* dhcp_rx — frame handler.
 * Pass every received Ethernet frame.  Silently ignores non-DHCP frames.
 * frame: raw Ethernet frame starting at byte 0 (dst MAC).
 * len:   total frame length in bytes.                                    */
void dhcp_rx(const uint8_t *frame, int len);

/* dhcp_bound — returns 1 if we have a valid IP, 0 otherwise.            */
int dhcp_bound(void);

/* dhcp_get_ip — copies current IP into out[4].
 * Always valid to call; returns 0.0.0.0 until BOUND.                    */
void dhcp_get_ip(uint8_t out[4]);

/* boot369: full lease query API — all populated after module_init returns.
 * Any module or stack component may call these; no network traffic is made.
 * Values come from the ACK options; sensible defaults are set if the server
 * omitted a particular option (e.g. netmask defaults to 255.255.255.0).  */
void     dhcp_get_gateway   (uint8_t out[4]);   /* option  3: router       */
void     dhcp_get_netmask   (uint8_t out[4]);   /* option  1: subnet mask  */
void     dhcp_get_dns       (uint8_t out[4]);   /* option  6: DNS server   */
uint32_t dhcp_get_lease_secs(void);             /* option 51: lease time   */

/* dhcp_module_init — Phoenix module init entry point.
 * Calls dhcp_init(g_genet_mac) and returns 0 on success.
 * Registered via module_register_native("PhoenixDHCP", dhcp_module_init,
 *                                        dhcp_module_final, NULL).       */
int dhcp_module_init(void);
int dhcp_module_final(void);

#endif /* NET_DHCP_H */
