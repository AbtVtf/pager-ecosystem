// PagerOS bootloader self-test (FW-002).
//
// Runs immediately after the ESP-IDF bootloader hands control to the app,
// before identity load / storage mount / shell launch (see SPEC.md §7.2).
//
// Validates the four hardware subsystems listed in the spec:
//   flash, PSRAM, display, radios
//
// Each check returns a structured result. Required checks that hard-fail
// cause the boot to halt with a clear log; soft / deferred checks log a
// warning but allow boot to continue.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SELFTEST_PASS = 0,        // check ran and passed
    SELFTEST_FAIL_HARD = 1,   // check failed; required subsystem; halt boot
    SELFTEST_FAIL_SOFT = 2,   // check failed but boot may continue (logged WARN)
    SELFTEST_DEFERRED = 3,    // driver not yet wired; logged INFO
} selftest_status_t;

typedef enum {
    SELFTEST_SUBSYS_FLASH = 0,
    SELFTEST_SUBSYS_PSRAM,
    SELFTEST_SUBSYS_DISPLAY,
    SELFTEST_SUBSYS_RADIOS,
    SELFTEST_SUBSYS_COUNT,
} selftest_subsys_t;

typedef struct {
    selftest_subsys_t subsys;
    selftest_status_t status;
    // Short human-readable detail, e.g. "16 MB, 9 partitions" or "psram alloc failed".
    // Static / arena-owned; do not free.
    const char *detail;
} selftest_entry_t;

typedef struct {
    selftest_entry_t entries[SELFTEST_SUBSYS_COUNT];
    bool any_hard_fail;
} selftest_result_t;

// Run all self-test checks. Always returns; caller decides what to do with hard fails.
selftest_result_t selftest_run(void);

// Log a one-line summary per subsystem in the order they were checked.
void selftest_log_result(const selftest_result_t *r);

// If any check hard-failed, log an ERROR and enter a non-returning halt loop.
// Otherwise returns normally.
void selftest_halt_if_hard_fail(const selftest_result_t *r);

// Human-readable name for a subsystem id (for logs / tests).
const char *selftest_subsys_name(selftest_subsys_t s);
const char *selftest_status_name(selftest_status_t s);

#ifdef __cplusplus
}
#endif
