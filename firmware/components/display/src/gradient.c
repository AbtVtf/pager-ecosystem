// SPDX-License-Identifier: Apache-2.0

#include "gradient.h"

static inline void unpack(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // RGB565 → 8 bits per channel by replicating the high bits into the
    // low ones (standard RGB565 → RGB888 expansion).
    uint8_t r5 = (c >> 11) & 0x1f;
    uint8_t g6 = (c >> 5)  & 0x3f;
    uint8_t b5 = c         & 0x1f;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline uint16_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t pageros_lerp_rgb565(uint16_t top, uint16_t bottom,
                              int numer, int denom)
{
    if (denom <= 0) return top;
    if (numer < 0)      numer = 0;
    if (numer > denom)  numer = denom;

    uint8_t r0, g0, b0, r1, g1, b1;
    unpack(top,    &r0, &g0, &b0);
    unpack(bottom, &r1, &g1, &b1);

    int rr = r0 + ((r1 - r0) * numer) / denom;
    int gg = g0 + ((g1 - g0) * numer) / denom;
    int bb = b0 + ((b1 - b0) * numer) / denom;
    return pack((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
}

bool pageros_clip_rect(int canvas_w, int canvas_h,
                       int *x, int *y, int *w, int *h,
                       int *src_dx, int *src_dy)
{
    int rx = *x, ry = *y, rw = *w, rh = *h;
    int sdx = 0, sdy = 0;

    if (rw <= 0 || rh <= 0) return false;

    if (rx < 0) { sdx = -rx; rw += rx; rx = 0; }
    if (ry < 0) { sdy = -ry; rh += ry; ry = 0; }
    if (rx >= canvas_w || ry >= canvas_h) return false;

    if (rx + rw > canvas_w) rw = canvas_w - rx;
    if (ry + rh > canvas_h) rh = canvas_h - ry;

    if (rw <= 0 || rh <= 0) return false;

    *x = rx; *y = ry; *w = rw; *h = rh;
    *src_dx = sdx; *src_dy = sdy;
    return true;
}
