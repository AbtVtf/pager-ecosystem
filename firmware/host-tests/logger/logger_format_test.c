// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for FW-004 logger formatting + rotation policy.
// The on-device part (mutex, fsync, ESP-IDF console proxy) is exercised
// by `idf.py monitor` smoke; this binary covers the bits that are pure
// arithmetic and pure-printf, since those are where rotation edge cases
// hide.

#include "logger_format.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

static size_t format_line(char *out, size_t out_len,
                          uint32_t uptime_ms,
                          pageros_log_fmt_level_t lvl,
                          const char *tag,
                          const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = pageros_logger_format_line(out, out_len, uptime_ms, lvl,
                                          tag, fmt, ap);
    va_end(ap);
    return n;
}

static void test_format_info_basic(void)
{
    char line[PAGEROS_LOG_LINE_MAX];
    size_t n = format_line(line, sizeof(line), 12345,
                           PAGEROS_LOG_FMT_INFO, "boot", "hello %d", 7);
    CHECK(n > 0, "format returned 0");
    CHECK(strcmp(line, "12345 I boot hello 7\n") == 0,
          "unexpected line: '%s'", line);
    CHECK(line[n - 1] == '\n', "no trailing newline");
    CHECK(line[n] == '\0', "no NUL terminator");
}

static void test_format_error_level(void)
{
    char line[PAGEROS_LOG_LINE_MAX];
    format_line(line, sizeof(line), 0, PAGEROS_LOG_FMT_ERROR, "wifi",
                "connect failed: %s", "timeout");
    CHECK(strcmp(line, "0 E wifi connect failed: timeout\n") == 0,
          "got: '%s'", line);
}

// Truncation: long body must be cut to the buffer and end with "...\n".
static void test_format_truncation(void)
{
    // Use a small buffer to force truncation.
    char line[64];
    char filler[200];
    memset(filler, 'a', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';

    size_t n = format_line(line, sizeof(line), 1, PAGEROS_LOG_FMT_INFO,
                           "t", "%s", filler);
    CHECK(n > 0 && n < sizeof(line), "n=%zu", n);
    CHECK(line[n - 1] == '\n', "no newline");
    CHECK(line[n] == '\0', "no NUL");

    // Last three body bytes (i.e. positions n-4, n-3, n-2) should be
    // "...", not 'a's.
    CHECK(line[n - 4] == '.' && line[n - 3] == '.' && line[n - 2] == '.',
          "expected '...\\n', got '%c%c%c%c'",
          line[n - 4], line[n - 3], line[n - 2], line[n - 1]);
}

// Buffers below the documented minimum return 0.
static void test_format_tiny_buffer(void)
{
    char tiny[8];
    size_t n = format_line(tiny, sizeof(tiny), 0, PAGEROS_LOG_FMT_INFO,
                           "t", "hello");
    CHECK(n == 0, "expected 0 for tiny buffer, got %zu", n);
}

// NULL tag/fmt fall back to safe defaults rather than crashing.
static void test_format_null_safety(void)
{
    char line[PAGEROS_LOG_LINE_MAX];
    size_t n = format_line(line, sizeof(line), 42, PAGEROS_LOG_FMT_INFO,
                           NULL, NULL);
    CHECK(n > 0, "n=%zu", n);
    CHECK(strstr(line, " I ? ") != NULL || strstr(line, "42 I") != NULL,
          "null-tag fallback missing: '%s'", line);
}

// Buffer larger than PAGEROS_LOG_LINE_MAX must still cap the produced
// line — otherwise a caller could nondeterministically write a longer
// line than the on-device path supports.
static void test_format_buffer_caps_at_line_max(void)
{
    char big[1024];
    char filler[800];
    memset(filler, 'x', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';

    size_t n = format_line(big, sizeof(big), 0, PAGEROS_LOG_FMT_INFO,
                           "t", "%s", filler);
    CHECK(n <= PAGEROS_LOG_LINE_MAX, "line exceeded LINE_MAX: %zu", n);
    CHECK(big[n - 1] == '\n', "no trailing newline");
}

// ---------------------------------------------------------------------------
// Rotation arithmetic.
// ---------------------------------------------------------------------------

static void test_rotate_under_threshold(void)
{
    CHECK(!pageros_logger_should_rotate(0, 100),
          "fresh file should not rotate");
    CHECK(!pageros_logger_should_rotate(500 * 1024, 1024),
          "500 KB + 1 KB should not rotate");
}

static void test_rotate_at_exact_threshold(void)
{
    // 1 MB exactly is fine; rotation triggers when the NEXT write would
    // exceed 1 MB.
    CHECK(!pageros_logger_should_rotate(1u * 1024u * 1024u - 1, 1),
          "1 MB - 1 + 1 should be the last write under threshold");
    CHECK(pageros_logger_should_rotate(1u * 1024u * 1024u, 1),
          "1 MB + 1 byte must rotate");
    CHECK(pageros_logger_should_rotate(1u * 1024u * 1024u - 10, 100),
          "would push past must rotate");
}

static void test_rotate_overflow_saturation(void)
{
    // A bogus huge pending value must still rotate, not wrap.
    CHECK(pageros_logger_should_rotate((size_t)-1 - 10, 100),
          "overflow must rotate");
}

int main(void)
{
    test_format_info_basic();
    test_format_error_level();
    test_format_truncation();
    test_format_tiny_buffer();
    test_format_null_safety();
    test_format_buffer_caps_at_line_max();
    test_rotate_under_threshold();
    test_rotate_at_exact_threshold();
    test_rotate_overflow_saturation();

    if (fail_count == 0) {
        printf("OK (9 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
