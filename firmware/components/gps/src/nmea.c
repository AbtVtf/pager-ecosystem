// SPDX-License-Identifier: Apache-2.0
//
// NMEA-0183 parser — see nmea.h for the public contract.
//
// Decisions worth flagging:
//
//   * No floating-point format-strings (no sscanf "%f"). We parse digits
//     manually because TinyUSB-CDC + newlib-nano (the firmware build's
//     default) ships without `*f` scanf support, and we want the same
//     translation unit to work on host and target.
//
//   * The parser is strict about the field SHAPE but lenient about
//     missing OPTIONAL fields. RMC and GGA both carry many fields whose
//     value is empty while the receiver is searching for a fix; we
//     surface that as `valid = false` rather than refusing the sentence.

#include "nmea.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

bool nmea_checksum_ok(const char *line, size_t len)
{
    // Skip optional leading '$'.
    size_t i = 0;
    if (len > 0 && line[0] == '$') i = 1;

    // Find '*'.
    size_t star = (size_t)-1;
    for (size_t j = i; j < len; j++) {
        if (line[j] == '*') { star = j; break; }
    }
    if (star == (size_t)-1) return true;          // no checksum present
    if (star + 2 >= len)    return false;         // truncated

    uint8_t sum = 0;
    for (size_t j = i; j < star; j++) sum ^= (uint8_t)line[j];

    int hi = hex_val(line[star + 1]);
    int lo = hex_val(line[star + 2]);
    if (hi < 0 || lo < 0) return false;
    return sum == (uint8_t)((hi << 4) | lo);
}

// Splits the comma-separated body of an NMEA sentence in-place into
// pointers. Returns the number of fields. Trailing checksum (*XX) is
// not included in the last field. The supplied buffer is mutated:
// commas overwritten with NULs and the '*' (if present) also NUL'd.
//
// NOTE: caller owns the buffer; this is intentionally a destructive parse
// because everything in this file is short-lived per-sentence work.
static size_t split_csv(char *buf, size_t len, const char **fields, size_t max_fields)
{
    size_t nf = 0;
    if (max_fields == 0) return 0;
    fields[nf++] = buf;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ',') {
            buf[i] = '\0';
            if (nf < max_fields) fields[nf++] = &buf[i + 1];
        } else if (buf[i] == '*') {
            buf[i] = '\0';
            len = i;       // ignore checksum tail in remaining iterations
            break;
        }
    }
    return nf;
}

// Parse a decimal integer from a NUL-terminated string. Returns false on
// any non-digit, on empty input, or on overflow. `*out` is unmodified on
// failure.
static bool parse_uint(const char *s, uint64_t *out)
{
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        uint64_t next = v * 10 + (uint64_t)(*p - '0');
        if (next < v) return false;
        v = next;
    }
    *out = v;
    return true;
}

// Parse a decimal floating-point number from a NUL-terminated string.
// Accepts an optional leading '-' and an optional decimal point.
static bool parse_double(const char *s, double *out)
{
    if (!s || !*s) return false;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;

    bool any_digit = false;
    double whole = 0.0;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10.0 + (double)(*s - '0');
        any_digit = true;
        s++;
    }
    double frac = 0.0;
    double scale = 1.0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            frac = frac * 10.0 + (double)(*s - '0');
            scale *= 10.0;
            any_digit = true;
            s++;
        }
    }
    if (!any_digit) return false;
    if (*s != '\0') return false;
    *out = sign * (whole + frac / scale);
    return true;
}

// Parse an NMEA latitude/longitude field of the form "ddmm.mmmm" or
// "dddmm.mmmm" plus a hemisphere indicator ('N'/'S' for lat, 'E'/'W' for
// lon). Sign is applied per hemisphere. `deg_digits` is 2 for latitude,
// 3 for longitude.
static bool parse_coord(const char *field, const char *hemi,
                        int deg_digits, double *out)
{
    if (!field || !*field || !hemi || !*hemi) return false;
    size_t flen = strlen(field);
    if (flen < (size_t)(deg_digits + 2)) return false;  // "dd" + "m."

    // Degrees portion.
    double deg = 0.0;
    for (int i = 0; i < deg_digits; i++) {
        if (field[i] < '0' || field[i] > '9') return false;
        deg = deg * 10.0 + (double)(field[i] - '0');
    }

    // Minutes portion (the rest of the field; may contain a decimal point).
    double minutes = 0.0;
    if (!parse_double(field + deg_digits, &minutes)) return false;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    double v = deg + minutes / 60.0;
    if (*hemi == 'S' || *hemi == 'W') v = -v;
    else if (*hemi != 'N' && *hemi != 'E') return false;
    *out = v;
    return true;
}

// Parse hhmmss(.sss) into 24h components. The fractional seconds part is
// rounded to milliseconds.
static bool parse_time_hms(const char *s, uint8_t *hh, uint8_t *mm,
                           uint8_t *ss, uint16_t *ms)
{
    if (!s || strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *hh = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
    *mm = (uint8_t)((s[2] - '0') * 10 + (s[3] - '0'));
    *ss = (uint8_t)((s[4] - '0') * 10 + (s[5] - '0'));
    *ms = 0;
    if (s[6] == '.') {
        // Up to 3 fractional digits; ignore further precision.
        int mul = 100;
        for (int i = 7; i < 10 && s[i] >= '0' && s[i] <= '9'; i++) {
            *ms = (uint16_t)(*ms + (s[i] - '0') * mul);
            mul /= 10;
        }
    }
    return *hh < 24 && *mm < 60 && *ss < 60;
}

static bool parse_date_dmy(const char *s, uint8_t *dd, uint8_t *mo, uint8_t *yy)
{
    if (!s || strlen(s) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *dd = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
    *mo = (uint8_t)((s[2] - '0') * 10 + (s[3] - '0'));
    *yy = (uint8_t)((s[4] - '0') * 10 + (s[5] - '0'));
    return *dd >= 1 && *dd <= 31 && *mo >= 1 && *mo <= 12;
}

// Convert YYYY/MM/DD hh:mm:ss.mmm to milliseconds since 1970-01-01T00:00Z.
// Pure integer civil-from-days formula (Howard Hinnant, "date" library).
// Avoids `mktime` (which uses TZ) and `timegm` (POSIX-but-not-portable).
static uint64_t epoch_ms_from_components(int year, int month, int day,
                                         int hour, int min, int sec, int ms)
{
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u
                   + (unsigned)day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days_since_epoch = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t secs = days_since_epoch * 86400
                   + (int64_t)hour * 3600
                   + (int64_t)min * 60
                   + (int64_t)sec;
    return (uint64_t)(secs * 1000 + ms);
}

// ---------------------------------------------------------------------------
// Sentence parsers
// ---------------------------------------------------------------------------

// $GxRMC,time,status,lat,N/S,lon,E/W,sog,cog,date,magvar,magvar_dir[,mode]*HH
static bool parse_rmc(char *fields_buf, size_t flen, nmea_update_t *out)
{
    const char *f[14] = {0};
    size_t nf = split_csv(fields_buf, flen, f, 14);
    if (nf < 10) return false;

    // f[0] = "GxRMC", f[1] = time, f[2] = status, f[3..6] = lat/lon,
    // f[9] = date.
    out->valid = (f[2] && f[2][0] == 'A');

    double lat, lon;
    if (parse_coord(f[3], f[4], 2, &lat)
        && parse_coord(f[5], f[6], 3, &lon)) {
        out->has_position   = true;
        out->latitude_deg   = lat;
        out->longitude_deg  = lon;
    }

    uint8_t hh = 0, mm = 0, ss = 0;
    uint16_t ms = 0;
    uint8_t dd = 0, mo = 0, yy = 0;
    if (parse_time_hms(f[1], &hh, &mm, &ss, &ms)
        && parse_date_dmy(f[9], &dd, &mo, &yy)) {
        int year = 2000 + (int)yy;        // NMEA-0183 uses 2-digit years
        out->utc_epoch_ms = epoch_ms_from_components(year, mo, dd,
                                                     hh, mm, ss, ms);
        out->has_utc = true;
    }
    return true;
}

// $GxGGA,time,lat,N/S,lon,E/W,fix_qual,sats,hdop,alt,M,...*HH
static bool parse_gga(char *fields_buf, size_t flen, nmea_update_t *out)
{
    const char *f[16] = {0};
    size_t nf = split_csv(fields_buf, flen, f, 16);
    if (nf < 10) return false;

    // Fix quality: 0 = invalid, 1+ = some sort of fix.
    if (f[6] && f[6][0] >= '0' && f[6][0] <= '9' && f[6][0] != '0') {
        out->valid = true;
    }

    double lat, lon;
    if (parse_coord(f[2], f[3], 2, &lat)
        && parse_coord(f[4], f[5], 3, &lon)) {
        out->has_position  = true;
        out->latitude_deg  = lat;
        out->longitude_deg = lon;
    }

    uint64_t sats;
    if (parse_uint(f[7], &sats) && sats <= 64) {
        out->has_satellites = true;
        out->satellites     = (uint8_t)sats;
    }

    double hdop, alt;
    if (parse_double(f[8], &hdop)) {
        out->has_hdop = true;
        out->hdop     = (float)hdop;
    }
    if (parse_double(f[9], &alt)) {
        out->has_altitude = true;
        out->altitude_m   = (float)alt;
    }
    return true;
}

bool nmea_parse_sentence(const char *line, size_t len, nmea_update_t *out)
{
    if (!line || !out || len == 0) return false;

    // Skip optional leading '$'.
    size_t i = 0;
    if (line[0] == '$') i = 1;

    if (!nmea_checksum_ok(line, len)) return false;

    // Talker(2) + Sentence(3) = 5 chars at minimum, followed by ','.
    if (len - i < 6) return false;
    if (line[i + 5] != ',') return false;

    // Identify sentence type by characters 2..4 of the talker (positions
    // i+2 .. i+4). e.g. "GPRMC" → "RMC".
    char id[4] = { line[i + 2], line[i + 3], line[i + 4], '\0' };

    // Work on a mutable copy because split_csv mutates in place. Bound
    // by NMEA_MAX_LINE which is plenty for any spec-compliant sentence.
    char scratch[NMEA_MAX_LINE + 1];
    size_t flen = len - i;
    if (flen > NMEA_MAX_LINE) return false;
    memcpy(scratch, line + i, flen);
    scratch[flen] = '\0';

    memset(out, 0, sizeof(*out));

    if (strcmp(id, "RMC") == 0) return parse_rmc(scratch, flen, out);
    if (strcmp(id, "GGA") == 0) return parse_gga(scratch, flen, out);
    return false;
}

// ---------------------------------------------------------------------------
// Streaming wrapper
// ---------------------------------------------------------------------------

void nmea_stream_init(nmea_stream_t *s)
{
    s->len      = 0;
    s->overflow = false;
}

static void emit_line(nmea_stream_t *s, nmea_sentence_cb_t cb, void *user)
{
    if (!s->overflow && s->len > 0 && cb) {
        // Strip a trailing '\r' if the receiver sent CRLF.
        size_t n = s->len;
        if (s->buf[n - 1] == '\r') n--;
        if (n > 0) cb(s->buf, n, user);
    }
    s->len      = 0;
    s->overflow = false;
}

void nmea_stream_push(nmea_stream_t *s,
                      const uint8_t *data, size_t n,
                      nmea_sentence_cb_t cb, void *user)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (b == '\n') {
            emit_line(s, cb, user);
            continue;
        }
        if (s->len < NMEA_MAX_LINE) {
            s->buf[s->len++] = (char)b;
        } else {
            s->overflow = true;
        }
    }
}
