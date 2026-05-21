// SPDX-License-Identifier: Apache-2.0
//
// FW-019 — UTF-8 decoder. Split out so the host tests can exercise
// edge cases (truncated sequences, overlong encodings, surrogate halves)
// without dragging in any other component.

#include "pageros_fonts.h"

#include <stddef.h>
#include <stdint.h>

#define REPLACEMENT 0xFFFDu

uint32_t pageros_fonts_utf8_next(const char **p, const char *end)
{
    if (!p || !*p) return REPLACEMENT;
    const unsigned char *s = (const unsigned char *)*p;
    if (end && s >= (const unsigned char *)end) return REPLACEMENT;

    unsigned char c = *s;

    if (c < 0x80) {            // ASCII
        *p = (const char *)(s + 1);
        return c;
    }

    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else {
        // Stray continuation byte or 5/6-byte lead — replace and skip 1.
        *p = (const char *)(s + 1);
        return REPLACEMENT;
    }

    // Check the continuation bytes exist + are 0b10xxxxxx.
    for (int i = 1; i <= extra; i++) {
        if (end && s + i >= (const unsigned char *)end) {
            *p = (const char *)(s + 1);
            return REPLACEMENT;
        }
        unsigned char cc = s[i];
        if ((cc & 0xC0) != 0x80) {
            *p = (const char *)(s + 1);
            return REPLACEMENT;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }

    // Overlong encodings are illegal — replacement char.
    if ((extra == 1 && cp < 0x80) ||
        (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000)) {
        *p = (const char *)(s + 1);
        return REPLACEMENT;
    }

    // Surrogate halves are illegal in UTF-8.
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        *p = (const char *)(s + 1);
        return REPLACEMENT;
    }

    // Above Unicode max → replacement.
    if (cp > 0x10FFFF) {
        *p = (const char *)(s + 1);
        return REPLACEMENT;
    }

    *p = (const char *)(s + 1 + extra);
    return cp;
}
