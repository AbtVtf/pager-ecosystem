// SPDX-License-Identifier: Apache-2.0
//
// Pure-C NMEA-0183 parser used by the PagerOS GPS driver (FW-011).
//
// No ESP-IDF symbols here — the host-side tests under
// `firmware/host-tests/gps/` link this translation unit directly with
// native gcc so the parser can be exercised against canned fixtures.
//
// Supported sentences:
//   - $G[PNALB]RMC — Recommended Minimum (used for fix validity,
//                    lat/lon, UTC time + date).
//   - $G[PNALB]GGA — Global positioning fix (used for altitude, satellite
//                    count, and HDOP → accuracy estimate).
//
// The "Gx" talker prefix is whatever NMEA talker the receiver uses:
//   GP = GPS, GN = combined GNSS, GL = GLONASS, GA = Galileo, GB = BeiDou.
// The MIA-M10Q emits GN when multi-constellation, GP when GPS-only.
//
// All other sentences are accepted but ignored — keeps the parser
// forward-compatible with future GNSS receivers.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One incremental fix update. Fields are only meaningful when the
// matching `has_*` flag is set; callers should accumulate updates and
// only treat the running aggregate as a "fix" once `valid` is true.
typedef struct {
    bool     valid;            // RMC reported A (active) — i.e. has a fix
    bool     has_position;     // lat/lon valid this sentence
    double   latitude_deg;
    double   longitude_deg;
    bool     has_altitude;
    float    altitude_m;
    bool     has_hdop;
    float    hdop;
    bool     has_satellites;
    uint8_t  satellites;
    bool     has_utc;
    uint64_t utc_epoch_ms;     // unix-epoch milliseconds (RMC-derived)
} nmea_update_t;

// Parse a single NMEA sentence in `line` (length `len`, no trailing CRLF).
// Leading '$' is optional. Returns true if the sentence was recognised
// AND its checksum (if present) validated. Unsupported sentences return
// false without touching `out`.
//
// Required output policy: every field whose `has_*` is left false MUST be
// ignored by the caller. The function never partially-fills numbers.
bool nmea_parse_sentence(const char *line, size_t len, nmea_update_t *out);

// Verify an NMEA checksum suffix of the form "...*HH" where HH is the
// XOR of all bytes between the leading '$' and the '*'. Returns true if
// the sentence has no `*` (some receivers omit it on PMTK/etc) OR if it
// matches. False on a mismatch.
bool nmea_checksum_ok(const char *line, size_t len);

// ---------------------------------------------------------------------------
// Streaming wrapper — feed raw UART bytes, get sentences out.
// ---------------------------------------------------------------------------

#ifndef NMEA_MAX_LINE
#define NMEA_MAX_LINE 96    // NMEA caps at 82 chars; round up for safety.
#endif

typedef void (*nmea_sentence_cb_t)(const char *line, size_t len, void *user);

typedef struct {
    char   buf[NMEA_MAX_LINE];
    size_t len;
    bool   overflow;          // current line exceeded MAX_LINE; drop it.
} nmea_stream_t;

void nmea_stream_init(nmea_stream_t *s);

// Push `n` bytes into the stream parser. For each complete line
// (terminated by LF), `cb` is invoked with the line contents (CR stripped,
// no terminating LF). Lines longer than NMEA_MAX_LINE are dropped silently.
void nmea_stream_push(nmea_stream_t *s,
                      const uint8_t *data, size_t n,
                      nmea_sentence_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
