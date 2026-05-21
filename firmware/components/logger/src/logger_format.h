// SPDX-License-Identifier: Apache-2.0
//
// Pure-C helpers shared by the on-device logger and its host tests.
// No ESP-IDF or FreeRTOS dependencies — line formatting and rotation
// arithmetic are the only pieces of the logger worth unit-testing on
// the host, and that's only viable if they're free of platform headers.
// See `path.h` in the storage component for the same pattern.

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mirror of `pageros_log_level_t` in the public header, repeated here
// so this file does not have to pull in `esp_err.h`. Both enums share
// the same numeric values by construction.
typedef enum {
    PAGEROS_LOG_FMT_INFO  = 0,
    PAGEROS_LOG_FMT_ERROR = 1,
} pageros_log_fmt_level_t;

// Maximum length, including the trailing newline and NUL, of any single
// log line written by the logger. Lines longer than this are truncated
// with "..." before the newline.
#define PAGEROS_LOG_LINE_MAX  256

// Format a single log line into `out`. The output always ends with a
// trailing '\n' followed by '\0'. Returns the number of bytes written
// (excluding the NUL).
//
// Header layout: `<uptime_ms> <LVL> <tag> ` then the formatted body,
// then '\n'. On truncation the last three body bytes are replaced with
// "..." so the line stays parseable. Always leaves at least the header
// + newline + NUL intact, even when `fmt` is empty.
//
// `out_len` must be at least `PAGEROS_LOG_LINE_MIN_OUT` (a couple dozen
// bytes — enough for the header alone). Smaller buffers return 0.
size_t pageros_logger_format_line(char *out, size_t out_len,
                                  uint32_t uptime_ms,
                                  pageros_log_fmt_level_t lvl,
                                  const char *tag,
                                  const char *fmt, va_list ap)
    __attribute__((format(printf, 6, 0)));

// Decide whether a write of `bytes_pending` more bytes to a file
// currently sized `current_size` would push it past
// `PAGEROS_LOG_ROTATE_BYTES`. Returns true if the caller should rotate
// **before** performing the write. Saturates on overflow so a runaway
// `bytes_pending` near SIZE_MAX still answers "rotate now".
bool pageros_logger_should_rotate(size_t current_size, size_t bytes_pending);

// Minimum buffer size accepted by `pageros_logger_format_line`. Smaller
// than this and the function bails — the timestamp header alone is on
// the order of 16 bytes, and there is no value in producing a line that
// can hold "...\n" but no real content.
#define PAGEROS_LOG_LINE_MIN_OUT  32
