// SPDX-License-Identifier: Apache-2.0
//
// PagerOS rotating SD logger — see `pageros_logger.h`.

#include "pageros_logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "logger_format.h"

// Mirror the public threshold over the host-test helper's local copy.
// The two must stay in lockstep — flag the mismatch at compile time
// rather than waiting for a 1 MB log to behave wrong on real hardware.
_Static_assert(PAGEROS_LOG_ROTATE_BYTES == (1u * 1024u * 1024u),
               "rotate threshold drifted from logger_format.c");
_Static_assert((int)PAGEROS_LOG_INFO  == (int)PAGEROS_LOG_FMT_INFO,
               "log level enum drift");
_Static_assert((int)PAGEROS_LOG_ERROR == (int)PAGEROS_LOG_FMT_ERROR,
               "log level enum drift");

static const char *TAG = "logger";

typedef struct {
    SemaphoreHandle_t lock;
    FILE             *fp;
    size_t            size;       // bytes in `fp` (matches `ftell(fp)`)
    bool              initialised;
} logger_state_t;

static logger_state_t s_state = {0};

// ---------------------------------------------------------------------------
// SD file plumbing
// ---------------------------------------------------------------------------

// File size on disk, regardless of whether we currently hold a handle.
// Returns 0 if the file does not exist (errno = ENOENT is fine — that's
// just "fresh boot, no log yet").
static size_t file_size(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return 0;
    if (st.st_size < 0) return 0;
    return (size_t)st.st_size;
}

// Move the active log aside, dropping any previous rotation. We rename
// rather than copy so a 1 MB rotate is O(1) on FAT — and so a crash
// mid-rotate at worst loses the new file's last write, never both files
// simultaneously.
static void rotate_files(void)
{
    // remove() of a non-existent file is fine on FAT — we don't care
    // about the return value, only the post-condition that the path
    // is gone before we rename onto it.
    remove(PAGEROS_LOG_PATH_ROTATED);
    if (rename(PAGEROS_LOG_PATH, PAGEROS_LOG_PATH_ROTATED) != 0) {
        // The active log might not exist yet (first boot with a brand
        // new SD card). Anything else is a real error — log to console
        // and continue; the next open() will create a fresh file.
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "rotate rename failed: %s", strerror(errno));
        }
    }
}

// Open (or create) the active log file for appending. Caller already
// holds the mutex. Closes any previous handle first. Resets the cached
// size to the on-disk value so a re-open after rotate starts from 0.
static void open_active(void)
{
    if (s_state.fp != NULL) {
        fclose(s_state.fp);
        s_state.fp = NULL;
    }
    s_state.fp = fopen(PAGEROS_LOG_PATH, "ab");
    if (s_state.fp == NULL) {
        // ENOENT is common before storage mounts — quiet in that case.
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "open %s: %s", PAGEROS_LOG_PATH, strerror(errno));
        }
        s_state.size = 0;
        return;
    }
    s_state.size = file_size(PAGEROS_LOG_PATH);

    // Stream-level buffering is fine but we drive flushes ourselves on
    // every line; turn off the libc buffer so `ftell` and our cached
    // size agree without an extra `fflush`.
    setvbuf(s_state.fp, NULL, _IONBF, 0);
}

// Caller already holds the mutex. Performs rotation if the pending
// write would push the file past the threshold.
static void maybe_rotate_locked(size_t pending_bytes)
{
    if (!pageros_logger_should_rotate(s_state.size, pending_bytes)) return;
    if (s_state.fp != NULL) {
        fclose(s_state.fp);
        s_state.fp = NULL;
    }
    rotate_files();
    open_active();
}

// ---------------------------------------------------------------------------
// Console proxy — keep `idf.py monitor` watchers in sync.
// ---------------------------------------------------------------------------

static void console_emit(pageros_log_level_t lvl,
                         const char *tag,
                         const char *fmt, va_list ap)
{
    // `esp_log_writev` is the formatted entry point ESP_LOGx eventually
    // calls. We hand-format here because we want our own '%s' line — but
    // we still want it to look like a normal ESP-IDF console line so
    // existing log filters keep working.
    esp_log_level_t esp_lvl = (lvl == PAGEROS_LOG_ERROR)
                                  ? ESP_LOG_ERROR
                                  : ESP_LOG_INFO;
    char body[PAGEROS_LOG_LINE_MAX];
    vsnprintf(body, sizeof(body), fmt, ap);
    // ESP_LOG_LEVEL handles colorization + the standard prefix.
    ESP_LOG_LEVEL(esp_lvl, tag, "%s", body);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t pageros_logger_init(void)
{
    if (!s_state.initialised) {
        s_state.lock = xSemaphoreCreateMutex();
        if (s_state.lock == NULL) return ESP_ERR_NO_MEM;
        s_state.initialised = true;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    // A leftover file from a prior boot might already be over the
    // threshold (the previous run crashed mid-line). Rotate inline so
    // the first write of this boot stays under 1 MB.
    size_t leftover = file_size(PAGEROS_LOG_PATH);
    if (pageros_logger_should_rotate(leftover, 0) || leftover >= PAGEROS_LOG_ROTATE_BYTES) {
        rotate_files();
    }

    open_active();

    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t pageros_logger_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.fp != NULL) {
        fflush(s_state.fp);
        fclose(s_state.fp);
        s_state.fp = NULL;
    }
    s_state.size = 0;
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

bool pageros_logger_is_writing_to_sd(void)
{
    if (!s_state.initialised) return false;
    return s_state.fp != NULL;
}

void pageros_logger_vlog(pageros_log_level_t lvl,
                         const char *tag,
                         const char *fmt, va_list ap)
{
    // Always copy the va_list — we walk it twice (console + SD), and
    // a va_list is single-use per ISO C.
    va_list ap_console;
    va_copy(ap_console, ap);
    console_emit(lvl, tag, fmt, ap_console);
    va_end(ap_console);

    if (!s_state.initialised) return;

    char line[PAGEROS_LOG_LINE_MAX];
    uint32_t uptime = esp_log_timestamp();
    size_t n = pageros_logger_format_line(line, sizeof(line),
                                          uptime,
                                          (pageros_log_fmt_level_t)lvl,
                                          tag, fmt, ap);
    if (n == 0) return;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    if (s_state.fp == NULL) {
        // SD wasn't ready at init — try once more so the first write
        // after a successful mount picks up automatically.
        open_active();
    }
    if (s_state.fp == NULL) {
        xSemaphoreGive(s_state.lock);
        return;
    }

    maybe_rotate_locked(n);
    if (s_state.fp == NULL) {
        // Rotation reopened a fresh file but open_active() failed (SD
        // popped between init and now). Give up quietly.
        xSemaphoreGive(s_state.lock);
        return;
    }

    size_t w = fwrite(line, 1, n, s_state.fp);
    if (w != n) {
        // Short write usually means the card filled or disappeared.
        // Drop the handle so subsequent calls fall back to console only.
        fclose(s_state.fp);
        s_state.fp = NULL;
        s_state.size = 0;
        xSemaphoreGive(s_state.lock);
        return;
    }
    s_state.size += w;

    // Per-line durability: cost is ~1 ms on a microSD class 10, and a
    // pager that crashes between writes is exactly the case we want
    // these logs for.
    fflush(s_state.fp);
    int fd = fileno(s_state.fp);
    if (fd >= 0) fsync(fd);

    xSemaphoreGive(s_state.lock);
}

void pageros_logger_log(pageros_log_level_t lvl,
                        const char *tag,
                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pageros_logger_vlog(lvl, tag, fmt, ap);
    va_end(ap);
}
