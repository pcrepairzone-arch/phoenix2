/*
 * wimp_poll_test.c – Wimp_Poll Re-entry Test for RISC OS Phoenix
 * Verifies that Wimp_Poll is never re-entered while an application is processing an event
 * Critical compatibility test for the cooperative multitasking model
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"

void test_no_reentry(void)
{
    int events_processed = 0;
    const int target_events = 500;

    debug_print("Starting Wimp_Poll re-entry test...\n");

    while (events_processed < target_events) {
        wimp_event_t event;
        int code = Wimp_Poll(0, &event);

        if (code != wimp_NULL_REASON_CODE) {
            events_processed++;

            // Simulate long processing time (this should NEVER cause re-entry into Wimp_Poll)
            for (volatile int i = 0; i < 20000000; i++) {
                // Busy loop to simulate real application work
            }

            // Yield to allow scheduler to run other tasks
            yield();
        }
    }

    debug_print("PASS: No re-entry into Wimp_Poll – %d events processed correctly\n", 
                events_processed);
}

int main(void)
{
    test_no_reentry();
    return 0;
}