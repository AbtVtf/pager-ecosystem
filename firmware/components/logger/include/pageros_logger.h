// SPDX-License-Identifier: Apache-2.0
//
// PagerOS rotating SD logger — FW-004.
//
// Provides `LOG_INFO` / `LOG_ERROR` macros that append a single line to
// `/sd/logs/pageros.log`. Once the file reaches 1 MB the current log is
// renamed to `/sd/logs/pageros.log.1` (replacing any prior backup) and a
// fresh log file is opened. We keep exactly one rotated backup; older
// data is discarded — a pager is not a black box, the log is for the
// human + the QA agent to triage the last few minutes of runtime, not
// for forensic archival.
//
// Line format:
//   `<uptime_ms> <LVL> <tag> <message>\n`
// where `<LVL>` is `I` (info) or `E` (error). Uptime millis come from
// `esp_log_timestamp()` so log lines line up 1:1 with what ESP-IDF
// prints to the console.
//
// Every line is also printed via the ESP-IDF console logger so a
// developer watching `idf.py monitor` sees the same stream — the SD
// copy is the only durable surface, the UART one is convenience.
//
// Concurrency: a single FreeRTOS mutex guards the file handle. The
// logger holds the file open between writes for throughput; we flush
// (`fflush` + `fsync`) on every line so a crash never costs more than
// the last in-flight write.
//
// Failure model: if the SD card is not mounted (FW-003 reported
// `ESP_ERR_NOT_FOUND` or shutdown) the logger silently keeps printing
// to the console only. Re-running `pageros_logger_init` after a later
// `pageros_storage_init` succeeds will start writing to SD from the
// next call.

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Severity passed to `pageros_logger_log`. `INFO` and `ERROR` are the
// only levels v1 needs — Wi-Fi-noisy levels like DEBUG/VERBOSE belong on
// the console, not on a 1 MB rolling SD file.
typedef enum {
    PAGEROS_LOG_INFO  = 0,
    PAGEROS_LOG_ERROR = 1,
} pageros_log_level_t;

// Hard rotation threshold. 1 MB matches SPEC §7.4 (`/logs/pageros.log
// — rotating, 1 MB`).
#define PAGEROS_LOG_ROTATE_BYTES   (1u * 1024u * 1024u)

// Active log file and the single rotated backup. Both live under the
// `/logs/` directory created by `pageros_storage_init`.
#define PAGEROS_LOG_PATH           "/sd/logs/pageros.log"
#define PAGEROS_LOG_PATH_ROTATED   "/sd/logs/pageros.log.1"

// Bring the logger online. Creates the mutex, opens (or creates) the
// log file, and rotates any oversized leftover from a prior boot.
//
// Safe to call before `pageros_storage_init` has succeeded — when the
// SD mount is absent the logger keeps a mutex and falls back to console
// only. Re-call after a successful mount to start appending to SD.
//
// Returns:
//   ESP_OK              — logger ready (with or without SD)
//   ESP_ERR_NO_MEM      — mutex allocation failed
esp_err_t pageros_logger_init(void);

// Flush and close the SD file handle, drop the mutex. Subsequent
// `LOG_*` calls become no-ops on SD and fall back to console only.
esp_err_t pageros_logger_shutdown(void);

// True if the logger has an open file handle to the SD log.
bool pageros_logger_is_writing_to_sd(void);

// Emit one log line. Thread-safe. Always prints to console; appends to
// SD when the mount is up. Rotates `pageros.log` → `pageros.log.1` when
// the active file reaches `PAGEROS_LOG_ROTATE_BYTES`.
//
// `tag` and `fmt` must be non-NULL. Variadic arguments follow printf(3)
// conventions. The full formatted line (excluding the trailing newline)
// is truncated to 256 bytes before write — long widget dumps end with
// "..." rather than corrupting the file.
void pageros_logger_log(pageros_log_level_t lvl,
                        const char *tag,
                        const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Variadic-args form for wrappers that take their own `va_list`.
void pageros_logger_vlog(pageros_log_level_t lvl,
                         const char *tag,
                         const char *fmt, va_list ap);

// Convenience macros. The acceptance criterion in TASKS.md calls these
// out by name — `LOG_INFO(tag, fmt, ...)` / `LOG_ERROR(tag, fmt, ...)`.
#define LOG_INFO(tag, fmt, ...) \
    pageros_logger_log(PAGEROS_LOG_INFO, (tag), (fmt), ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) \
    pageros_logger_log(PAGEROS_LOG_ERROR, (tag), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
