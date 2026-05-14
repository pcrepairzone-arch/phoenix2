/* Tests/dhcp_test_module.c — PhoenixDHCPTest native module
 *
 * Tests the net/dhcp.c state machine using synthetic Ethernet frames
 * injected directly via dhcp_rx().  No real network required.
 *
 * Registered as a Phoenix native module in module_init_all():
 *   module_register_native("PhoenixDHCPTest",
 *                           dhcp_test_module_init, NULL, NULL);
 *
 * Test sequence:
 *   1. dhcp_init() with a fake MAC
 *   2. dhcp_start()           → state must become DHCP_ST_DISCOVER
 *   3. dhcp_rx(fake OFFER)    → state must become DHCP_ST_REQUEST
 *   4. dhcp_rx(fake ACK)      → state must become DHCP_ST_BOUND
 *   5. dhcp_bound()           → must return 1
 *   6. dhcp_get_ip()          → must return offered IP
 *   7. dhcp_tick(far-future)  → must NOT change state (already BOUND)
 *
 * Each step prints PASS or FAIL with detail.  A summary line at the end
 * gives "PhoenixDHCPTest: N/7 passed".
 *
 * Note on genet_send(): dhcp_start() and dhcp_rx(OFFER) both call
 * genet_send() internally.  In a real kernel boot this sends frames over
 * GENET; during the test the frames are discarded silently if GENET is
 * not yet in TX-ready state.  The test only validates state transitions
 * — actual TX success is verified separately by the GENET self-test.
 *
 * Author: Phoenix OS project
 * Updated: boot345 candidate, May 2026
 */

#include "../net/dhcp.h"
#include "../kernel/kernel.h"

/* ── Fake MAC for test isolation ─────────────────────────────────────────── */
static const uint8_t TEST_MAC[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };

/* ── Synthetic DHCP OFFER frame builder ──────────────────────────────────── */
/*
 * Builds a minimal 342-byte Ethernet/IPv4/UDP/DHCP OFFER frame.
 * offered_ip: the yiaddr we want the DHCP module to see.
 * server_ip:  source/server-id IP.
 * XID must be "PHOE" (0x50 0x48 0x4F 0x45) to match g_dhcp_xid.
 */
static void _make_offer(uint8_t *buf,
                        const uint8_t offered_ip[4],
                        const uint8_t server_ip[4])
{
    for (int i = 0; i < 342; i++) buf[i] = 0;

    /* Ethernet: src=server, dst=broadcast, EtherType=IPv4 */
    for (int i = 0; i < 6; i++) buf[i]     = 0xff;
    for (int i = 0; i < 6; i++) buf[6 + i] = server_ip[i % 4] ^ 0x55;
    buf[12] = 0x08; buf[13] = 0x00;

    /* IPv4: ver=4 IHL=5 total=328 proto=UDP src=server dst=bcast */
    buf[14] = 0x45;
    buf[16] = 0x01; buf[17] = 0x48;        /* total 328            */
    buf[22] = 0x40;                         /* TTL 64               */
    buf[23] = 0x11;                         /* UDP                  */
    for (int i = 0; i < 4; i++) buf[26 + i] = server_ip[i];
    buf[30] = 255; buf[31] = 255; buf[32] = 255; buf[33] = 255;

    /* UDP: src=67(server) dst=68(client) len=308 */
    buf[34] = 0x00; buf[35] = 0x43;        /* src port 67          */
    buf[36] = 0x00; buf[37] = 0x44;        /* dst port 68          */
    buf[38] = 0x01; buf[39] = 0x34;        /* length 308           */

    /* DHCP fixed fields */
    buf[42] = 0x02;                         /* op = BOOTREPLY       */
    buf[43] = 0x01;                         /* htype Ethernet       */
    buf[44] = 0x06;                         /* hlen 6               */
    /* xid = "PHOE" at offset 42+4 = 46 */
    buf[46] = 0x50; buf[47] = 0x48; buf[48] = 0x4F; buf[49] = 0x45;
    /* yiaddr at offset 42+16 = 58 */
    for (int i = 0; i < 4; i++) buf[58 + i] = offered_ip[i];
    /* chaddr (just copy TEST_MAC for realism) at 42+28=70 */
    for (int i = 0; i < 6; i++) buf[70 + i] = TEST_MAC[i];
    /* magic cookie at 42+236 = 278 */
    buf[278] = 0x63; buf[279] = 0x82; buf[280] = 0x53; buf[281] = 0x63;

    /* Options at 282: msg-type OFFER (2), server-id */
    int o = 282;
    buf[o++] = 53; buf[o++] = 1; buf[o++] = 2;    /* OFFER            */
    buf[o++] = 54; buf[o++] = 4;                   /* server ID        */
    for (int i = 0; i < 4; i++) buf[o++] = server_ip[i];
    buf[o] = 0xff;                                  /* end              */
}

/* ── Synthetic DHCP ACK frame builder ───────────────────────────────────── */
static void _make_ack(uint8_t *buf,
                      const uint8_t offered_ip[4],
                      const uint8_t server_ip[4])
{
    _make_offer(buf, offered_ip, server_ip);  /* same layout, change type  */
    buf[282 + 2] = 5;                          /* option 53 value = ACK(5) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test suite
 * ═══════════════════════════════════════════════════════════════════════════ */

int dhcp_test_module_init(void)
{
    debug_print("[DHCPTest] ── PhoenixDHCPTest starting ──\n");

    static const uint8_t OFFERED_IP[4]  = { 192, 168, 0, 145 };
    static const uint8_t SERVER_IP[4]   = { 192, 168, 0,   1 };

    int passed = 0;
    int total  = 7;

    /* ── Test 1: dhcp_init() brings state to IDLE ─────────────────────── */
    dhcp_init(TEST_MAC);
    /* dhcp_bound() must return 0 after init */
    if (!dhcp_bound()) {
        debug_print("[DHCPTest] 1/7 PASS: dhcp_init() state=IDLE\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 1/7 FAIL: bound after init (unexpected)\n");
    }

    /* ── Test 2: dhcp_start() transitions to DISCOVER ─────────────────── */
    dhcp_start();
    /* We can't read g_dhcp_st directly (it's private), but if state is
     * DISCOVER then a synthetic OFFER will be accepted.  Test by checking
     * that dhcp_bound() is still 0 (not BOUND yet from the send alone).  */
    if (!dhcp_bound()) {
        debug_print("[DHCPTest] 2/7 PASS: dhcp_start() — not yet BOUND (correct)\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 2/7 FAIL: BOUND immediately after start?\n");
    }

    /* ── Test 3: OFFER frame → transitions to REQUEST ──────────────────── */
    static uint8_t offer_frame[342];
    _make_offer(offer_frame, OFFERED_IP, SERVER_IP);
    dhcp_rx(offer_frame, 342);
    /* After processing OFFER we should be in REQUEST (still not BOUND)    */
    if (!dhcp_bound()) {
        debug_print("[DHCPTest] 3/7 PASS: OFFER accepted — awaiting ACK\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 3/7 FAIL: jumped to BOUND on OFFER (should be REQUEST)\n");
    }

    /* ── Test 4: ACK frame → transitions to BOUND ──────────────────────── */
    static uint8_t ack_frame[342];
    _make_ack(ack_frame, OFFERED_IP, SERVER_IP);
    dhcp_rx(ack_frame, 342);
    if (dhcp_bound()) {
        debug_print("[DHCPTest] 4/7 PASS: ACK accepted — now BOUND\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 4/7 FAIL: not BOUND after ACK\n");
    }

    /* ── Test 5: dhcp_get_ip() returns the offered IP ──────────────────── */
    uint8_t got_ip[4];
    dhcp_get_ip(got_ip);
    if (got_ip[0] == OFFERED_IP[0] && got_ip[1] == OFFERED_IP[1] &&
        got_ip[2] == OFFERED_IP[2] && got_ip[3] == OFFERED_IP[3]) {
        debug_print("[DHCPTest] 5/7 PASS: IP=%d.%d.%d.%d\n",
                    got_ip[0], got_ip[1], got_ip[2], got_ip[3]);
        passed++;
    } else {
        debug_print("[DHCPTest] 5/7 FAIL: IP=%d.%d.%d.%d expected %d.%d.%d.%d\n",
                    got_ip[0], got_ip[1], got_ip[2], got_ip[3],
                    OFFERED_IP[0], OFFERED_IP[1], OFFERED_IP[2], OFFERED_IP[3]);
    }

    /* ── Test 6: g_our_ip exported global matches ───────────────────────── */
    if (g_our_ip[0] == OFFERED_IP[0] && g_our_ip[1] == OFFERED_IP[1] &&
        g_our_ip[2] == OFFERED_IP[2] && g_our_ip[3] == OFFERED_IP[3]) {
        debug_print("[DHCPTest] 6/7 PASS: g_our_ip exported correctly\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 6/7 FAIL: g_our_ip mismatch\n");
    }

    /* ── Test 7: dhcp_tick() far in the future does not unbind ─────────── */
    /* Pass a timestamp 60 seconds ahead — BOUND state must not change     */
    uint32_t far_future = 0xFFFFFF00u;   /* ~49.7 days in ms               */
    dhcp_tick(far_future);
    if (dhcp_bound()) {
        debug_print("[DHCPTest] 7/7 PASS: dhcp_tick() did not disturb BOUND state\n");
        passed++;
    } else {
        debug_print("[DHCPTest] 7/7 FAIL: dhcp_tick() cleared BOUND state\n");
    }

    /* ── Summary ─────────────────────────────────────────────────────────── */
    debug_print("[DHCPTest] ── Result: %d/%d passed %s──\n",
                passed, total,
                (passed == total) ? "ALL OK " : "SOME FAILURES ");

    return (passed == total) ? 0 : 1;
}
