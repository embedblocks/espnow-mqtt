/*
 * test_app_main.c — Unity test runner entry point.
 *
 * Presents the interactive Unity test selection menu on UART.
 * Flash test_app/ to the target and open a serial terminal to select
 * and run individual test cases or groups.
 *
 * Usage:
 *   idf.py -C test_app build flash monitor
 *
 * In the terminal, press Enter to show the test menu. Type a test group
 * tag (e.g. "[wire_protocol]") or a test name to run matching tests.
 * Type 'all' to run every registered test case.
 */

#include "unity.h"
#include "unity_test_runner.h"

void app_main(void)
{
    unity_run_menu();
}
