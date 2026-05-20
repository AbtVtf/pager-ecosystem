// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the PagerOS NMEA parser (FW-011).
//
// Runs against canned u-blox-style sentences — covers:
//   - Valid fix (RMC + GGA together): produces lat/lon/sat/alt/time.
//   - No-fix RMC ("V" status, empty fields): does not flip valid=true.
//   - Bad checksum: rejected.
//   - Unsupported sentence (GSV): returns false, no state change.
//   - Streaming wrapper splits on '\n' and tolerates CRLF + chunk
//     boundaries that straddle a line.

#include "nmea.h"

#include <math.h>
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

static int close_to(double a, double b, double eps)
{
    double d = a - b;
    if (d < 0) d = -d;
    return d <= eps;
}

// Test 1 — RMC sentence with a valid fix (Brno airport, Czechia).
static void test_rmc_valid_fix(void)
{
    // Known-good NMEA from a u-blox receiver. The "065545" / "200526"
    // give 2026-05-20T06:55:45Z = epoch ms 1779260145000.
    const char *s = "$GPRMC,065545.00,A,4910.20000,N,01640.30000,E,0.05,123.4,200526,,,A*65";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(ok, "RMC parse returned false");
    CHECK(u.valid, "RMC status A should set valid");
    CHECK(u.has_position, "RMC should report position");
    CHECK(close_to(u.latitude_deg, 49 + 10.20000 / 60.0, 1e-6),
          "lat = %.6f", u.latitude_deg);
    CHECK(close_to(u.longitude_deg, 16 + 40.30000 / 60.0, 1e-6),
          "lon = %.6f", u.longitude_deg);
    CHECK(u.has_utc, "RMC with time+date should set utc");
    CHECK(u.utc_epoch_ms == 1779260145000ULL,
          "utc = %llu (expected 1779260145000 for 2026-05-20T06:55:45Z)",
          (unsigned long long)u.utc_epoch_ms);
}

// Test 2 — GGA sentence supplies altitude + satellites + HDOP.
static void test_gga_full(void)
{
    const char *s = "$GPGGA,065545.00,4910.20000,N,01640.30000,E,1,08,0.94,289.0,M,45.9,M,,*5B";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(ok, "GGA parse returned false");
    CHECK(u.valid, "GGA fix_qual=1 should set valid");
    CHECK(u.has_position, "GGA should report position");
    CHECK(u.has_altitude && close_to(u.altitude_m, 289.0, 1e-3),
          "alt = %.3f", u.altitude_m);
    CHECK(u.has_satellites && u.satellites == 8, "sats = %u", u.satellites);
    CHECK(u.has_hdop && close_to(u.hdop, 0.94, 1e-6), "hdop = %.3f", u.hdop);
}

// Test 3 — RMC with "V" status (searching for fix) leaves valid=false.
static void test_rmc_no_fix(void)
{
    const char *s = "$GPRMC,,V,,,,,,,,,,N*53";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(ok, "RMC-no-fix parse returned false");
    CHECK(!u.valid, "RMC status V must NOT set valid");
    CHECK(!u.has_position, "RMC-no-fix must not report a position");
}

// Test 4 — corrupted checksum is rejected.
static void test_bad_checksum(void)
{
    // Wrong checksum (real one is *65).
    const char *s = "$GPRMC,065545.00,A,4910.20000,N,01640.30000,E,0.05,123.4,200526,,,A*00";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(!ok, "bad-checksum sentence must be rejected");
}

// Test 5 — unsupported sentence type returns false (caller ignores).
static void test_unsupported_sentence(void)
{
    const char *s = "$GPGSV,3,1,11,02,55,070,42,05,40,150,38,07,28,300,35,09,18,250,32*7F";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(!ok, "GSV must not be claimed by parser");
}

// Test 6 — multi-constellation talker (GN) is treated identically.
static void test_gn_prefix(void)
{
    const char *s = "$GNRMC,065545.00,A,4910.20000,N,01640.30000,E,0.05,123.4,200526,,,A*7B";
    nmea_update_t u;
    int ok = nmea_parse_sentence(s, strlen(s), &u);
    CHECK(ok, "GNRMC parse returned false");
    CHECK(u.valid, "GNRMC A should set valid");
}

// Test 7 — streaming wrapper splits on '\n' and strips '\r'.
typedef struct { int count; char last[NMEA_MAX_LINE]; size_t last_len; } stream_capture_t;
static void capture_cb(const char *line, size_t len, void *user)
{
    stream_capture_t *c = (stream_capture_t *)user;
    c->count++;
    c->last_len = len;
    if (len > sizeof(c->last)) len = sizeof(c->last);
    memcpy(c->last, line, len);
}

static void test_stream_split(void)
{
    nmea_stream_t s;
    nmea_stream_init(&s);
    stream_capture_t cap = {0};

    // Feed two complete sentences, the second split across two pushes.
    // Checksums are real (computed XOR of body) so the streaming wrapper
    // doesn't accidentally swallow valid frames in CI.
    const char *part1 = "$GPRMC,xx*67\r\n$GP";
    const char *part2 = "GGA,yy*7A\r\n";
    nmea_stream_push(&s, (const uint8_t *)part1, strlen(part1), capture_cb, &cap);
    nmea_stream_push(&s, (const uint8_t *)part2, strlen(part2), capture_cb, &cap);
    CHECK(cap.count == 2, "expected 2 lines, got %d", cap.count);
    CHECK(cap.last_len == strlen("$GPGGA,yy*7A"),
          "expected reassembled GGA, got len %zu", cap.last_len);
    CHECK(memcmp(cap.last, "$GPGGA,yy*7A", cap.last_len) == 0,
          "second sentence content mismatch");
}

// Test 8 — sentences longer than NMEA_MAX_LINE are dropped silently.
static void test_stream_overflow(void)
{
    nmea_stream_t s;
    nmea_stream_init(&s);
    stream_capture_t cap = {0};

    char big[NMEA_MAX_LINE * 2];
    memset(big, 'X', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\n';

    nmea_stream_push(&s, (const uint8_t *)big, sizeof(big), capture_cb, &cap);
    CHECK(cap.count == 0, "oversized line must be suppressed (got %d)", cap.count);

    // After the LF the parser must be back in sync — a normal sentence
    // immediately after should still be emitted.
    const char *normal = "$GPRMC,xx*67\n";
    nmea_stream_push(&s, (const uint8_t *)normal, strlen(normal), capture_cb, &cap);
    CHECK(cap.count == 1, "post-overflow recovery failed (got %d)", cap.count);
}

int main(void)
{
    test_rmc_valid_fix();
    test_gga_full();
    test_rmc_no_fix();
    test_bad_checksum();
    test_unsupported_sentence();
    test_gn_prefix();
    test_stream_split();
    test_stream_overflow();

    if (fail_count == 0) {
        printf("OK (8 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
