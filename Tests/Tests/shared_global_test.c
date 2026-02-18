/*
 * shared_global_test.c – Shared Global Data Test for RISC OS Phoenix
 * Verifies that shared module data is properly serialized under preemptive scheduling
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"

static int shared_counter = 0;

void task_increment(void)
{
    for (int i = 0; i < 10000; i++) {
        shared_counter++;   // This must be atomic / serialized by kernel
    }
}

void test_shared_globals(void)
{
    debug_print("Starting shared global data test...\n");

    task_create("inc1", task_increment, 10, 0);
    task_create("inc2", task_increment, 10, 0);
    task_create("inc3", task_increment, 10, 0);

    wait(NULL);
    wait(NULL);
    wait(NULL);

    if (shared_counter == 30000) {
        debug_print("PASS: Shared globals safe – no race conditions (counter = 30000)\n");
    } else {
        debug_print("FAIL: Shared counter = %d (expected 30000)\n", shared_counter);
    }
}

int main(void)
{
    test_shared_globals();
    return 0;
}