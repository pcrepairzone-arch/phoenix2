#!/usr/bin/env python3
"""
run_all.py – Automated Compatibility Test Suite Runner for RISC OS Phoenix
Tests Wimp cooperative behavior, memory protection, multi-tasking, and more.
Author: R Andrews Grok 4 – 06 Feb 2026
"""

import subprocess
import os
import time
import sys

# List of all tests to run
tests = [
    "wimp_poll_test",
    "event_order_test",
    "shared_global_test",
    "100_task_stress",
    "memory_fault_test"
]

def compile_test(test_name):
    """Compile a single C test"""
    print(f"Compiling {test_name}...")
    result = subprocess.run([
        "aarch64-none-elf-gcc",
        "-o", test_name,
        f"{test_name}.c",
        "-I../kernel",
        "-ffreestanding",
        "-nostdlib",
        "-static"
    ], capture_output=True, text=True)

    if result.returncode != 0:
        print(f"COMPILE FAILED: {test_name}")
        print(result.stderr)
        return False
    return True

def run_test(test_name):
    """Run a compiled test"""
    print(f"Running {test_name}...")
    start_time = time.time()

    try:
        result = subprocess.run(
            [f"./{test_name}"],
            capture_output=True,
            text=True,
            timeout=30
        )
        duration = time.time() - start_time

        output = result.stdout + result.stderr

        if "PASS" in output.upper():
            print(f"PASS {test_name} ({duration:.2f}s)")
            return True
        else:
            print(f"FAIL {test_name}")
            print(output)
            return False

    except subprocess.TimeoutExpired:
        print(f"TIMEOUT {test_name}")
        return False
    except FileNotFoundError:
        print(f"NOT FOUND {test_name}")
        return False

def main():
    print("=" * 60)
    print("RISC OS Phoenix Compatibility Test Suite")
    print("Date: 06 Feb 2026")
    print("=" * 60)
    print()

    passed = 0
    total = len(tests)

    for test in tests:
        if compile_test(test):
            if run_test(test):
                passed += 1
        print("-" * 40)

    print("\n" + "=" * 60)
    print(f"Test Summary: {passed}/{total} tests passed")
    if passed == total:
        print("ALL TESTS PASSED - Compatibility confirmed!")
        print("RISC OS Phoenix is ready for release.")
    else:
        print("Some tests failed. Please review the output.")
    print("=" * 60)

    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())