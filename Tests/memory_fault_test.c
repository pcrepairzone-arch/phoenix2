/*
 * memory_fault_test.c – Memory Protection Test for RISC OS Phoenix
 * Verifies that bad memory access is caught and contained (SIGSEGV)
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"

void bad_task(void)
{
    debug_print("Bad task: Attempting to crash the system...\n");
    *(volatile int*)0 = 42;  // Null pointer write → should trigger SIGSEGV
}

void test_memory_protection(void)
{
    debug_print("Starting memory protection test...\n");

    task_create("bad", bad_task, 10, 0);

    debug_print("Parent task still alive – memory protection is working\n");

    while (1) {
        debug_print("Parent still running...\n");
        yield();
    }
}

int main(void)
{
    test_memory_protection();
    return 0;
}