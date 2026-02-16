/*
 * event_order_test.c – Event Ordering Test for RISC OS Phoenix
 * Verifies that events are delivered in strict FIFO order
 * Critical for Wimp compatibility
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"

void test_event_order(void)
{
    wimp_event_t events[100];
    int count = 0;

    debug_print("Starting event ordering test...\n");

    while (count < 100) {
        int code = Wimp_Poll(0, &events[count]);
        if (code != wimp_NULL_REASON_CODE) {
            count++;
        }
        yield();
    }

    // Verify strict FIFO order (events must be in increasing timestamp order)
    for (int i = 1; i < count; i++) {
        if (events[i].time < events[i-1].time) {
            debug_print("FAIL: Event out of order! Event %d came before Event %d\n", i, i-1);
            return;
        }
    }

    debug_print("PASS: Event ordering preserved – %d events in correct sequence\n", count);
}

int main(void)
{
    test_event_order();
    return 0;
}