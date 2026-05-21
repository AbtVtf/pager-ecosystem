// SPDX-License-Identifier: Apache-2.0

#include "logger_format.h"

#include <stdio.h>
#include <string.h>

// Threshold used by `pageros_logger_should_rotate`. Defined locally so
// `logger_format.c` does not depend on the public header (which pulls
// in `esp_err.h`); the value is asserted to match the public macro in
// `logger.c`.
#define LOGGER_FMT_ROTATE_BYTES  (1u * 1024u * 1024u)

static char level_char(pageros_log_fmt_level_t lvl)
{
    switch (lvl) {
        case PAGEROS_LOG_FMT_ERROR: return 'E';
        case PAGEROS_LOG_FMT_INFO:
        default:                    return 'I';
    }
}

size_t pageros_logger_format_line(char *out, size_t out_len,
                                  uint32_t uptime_ms,
                                  pageros_log_fmt_level_t lvl,
                                  const char *tag,
                                  const char *fmt, va_list ap)
{
    if (out == NULL || out_len < PAGEROS_LOG_LINE_MIN_OUT) return 0;
    if (tag == NULL) tag = "?";
    if (fmt == NULL) fmt = "";

    // Cap the working buffer at PAGEROS_LOG_LINE_MAX so even if the
    // caller passes a 64 KB scratch we still produce the same lines
    // the on-device logger would have written.
    size_t cap = out_len;
    if (cap > PAGEROS_LOG_LINE_MAX) cap = PAGEROS_LOG_LINE_MAX;

    // Header. snprintf returns the count it *would* have written; if
    // the header alone exceeded the buffer something is deeply wrong
    // (very long tag) — fall back to the bare minimum.
    int hdr = snprintf(out, cap, "%lu %c %s ",
                       (unsigned long)uptime_ms,
                       level_char(lvl), tag);
    if (hdr < 0) return 0;
    if ((size_t)hdr >= cap - 2) {
        // Reserve 2 bytes for "\n\0". If even that doesn't fit, drop
        // the tag to keep the line well-formed.
        int short_hdr = snprintf(out, cap, "%lu %c ? ",
                                 (unsigned long)uptime_ms,
                                 level_char(lvl));
        if (short_hdr < 0 || (size_t)short_hdr >= cap - 2) {
            // Buffer is genuinely tiny — punt.
            return 0;
        }
        hdr = short_hdr;
    }

    // Body. We leave 1 byte for '\n' and 1 for '\0'.
    size_t body_room = cap - (size_t)hdr - 2;
    int body = vsnprintf(out + hdr, body_room + 1, fmt, ap);
    if (body < 0) body = 0;

    size_t body_written;
    if ((size_t)body > body_room) {
        // Truncation: vsnprintf already wrote `body_room` chars + NUL.
        // Replace the last three with "..." so the line stays
        // unambiguously truncated even when scrolling through a flat
        // text view.
        body_written = body_room;
        if (body_written >= 3) {
            out[hdr + body_written - 3] = '.';
            out[hdr + body_written - 2] = '.';
            out[hdr + body_written - 1] = '.';
        }
    } else {
        body_written = (size_t)body;
    }

    size_t total = (size_t)hdr + body_written;
    out[total++] = '\n';
    out[total]   = '\0';
    return total;
}

bool pageros_logger_should_rotate(size_t current_size, size_t bytes_pending)
{
    // Saturate addition to avoid an overflow miss that would silently
    // skip rotation on a runaway 4 GB+ ask.
    size_t total = current_size + bytes_pending;
    if (total < current_size) return true;  // overflow → rotate
    return total > LOGGER_FMT_ROTATE_BYTES;
}
