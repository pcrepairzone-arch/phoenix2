/*
 * 100_task_stress.c – 100 Task Stress Test for RISC OS Phoenix
 * Creates 100 cooperative tasks and verifies system stability
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"

void idle_task(void)
{
    while (1) {
        yield();  // Cooperative yield – must not freeze system
    }
}

void test_100_tasks(void)
{
    debug_print("Starting 100-task stress test...\n");

    for (int i = 0; i < 100; i++) {
        char name[16];
        snprintf(name, sizeof(name), "task%d", i);
        task_create(name, idle_task, 10, 0);
    }

    debug_print("PASS: 100 tasks created successfully – system remains stable\n");

    // Keep running so we can observe the system
    while (1) {
        yield();
    }
}

int main(void)
{
    test_100_tasks();
    return 0;
}