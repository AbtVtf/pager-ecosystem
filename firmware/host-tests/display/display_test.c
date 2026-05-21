// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the FW-005 display driver's pure-C bits —
// the RGB565 gradient interpolation and the blit clipping rectangle.
// The ST7796 / SPI / LEDC paths are hardware-only and validated on the
// device.

#include "gradient.h"

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

// Helper to inspect rgb565 channel values.
static void unpack565(uint16_t c, int *r, int *g, int *b)
{
    *r = (c >> 11) & 0x1f;
    *g = (c >> 5)  & 0x3f;
    *b = c         & 0x1f;
}

// At t=0 the lerp should return exactly `top`; at t=denom it should
// return exactly `bottom`. (No floating-point drift since the math is
// in integer space and the endpoints round-trip cleanly through the
// 565 ↔ 888 expansion.)
static void test_lerp_endpoints(void)
{
    uint16_t top    = 0xF800;  // pure red
    uint16_t bottom = 0x001F;  // pure blue
    CHECK(pageros_lerp_rgb565(top, bottom, 0, 100) == top,
          "t=0 should return top exactly");
    CHECK(pageros_lerp_rgb565(top, bottom, 100, 100) == bottom,
          "t=denom should return bottom exactly");
}

// Mid-point sanity: lerp(red, blue, 50%) should have ~half red and
// ~half blue channels, with green near zero.
static void test_lerp_midpoint(void)
{
    uint16_t red  = 0xF800;  // (R=255, G=0, B=0)
    uint16_t blue = 0x001F;  // (R=0,   G=0, B=255)
    uint16_t mid  = pageros_lerp_rgb565(red, blue, 1, 2);
    int r, g, b;
    unpack565(mid, &r, &g, &b);
    CHECK(r >= 14 && r <= 17, "mid red ~ half-of-31: got %d", r);
    CHECK(b >= 14 && b <= 17, "mid blue ~ half-of-31: got %d", b);
    CHECK(g <= 2, "mid green should be ~0: got %d", g);
}

// Negative or out-of-range t is clamped, not crashed.
static void test_lerp_clamps(void)
{
    uint16_t top = 0xF800, bottom = 0x001F;
    CHECK(pageros_lerp_rgb565(top, bottom, -1, 100) == top,
          "t<0 should clamp to top");
    CHECK(pageros_lerp_rgb565(top, bottom, 999, 100) == bottom,
          "t>denom should clamp to bottom");
    // denom=0 → no-op, returns top.
    CHECK(pageros_lerp_rgb565(top, bottom, 0, 0) == top,
          "denom=0 should return top");
}

// Identity lerp: top == bottom must return that colour regardless of t.
static void test_lerp_identity(void)
{
    uint16_t c = 0x07E0;  // pure green
    for (int i = 0; i <= 10; i++) {
        CHECK(pageros_lerp_rgb565(c, c, i, 10) == c,
              "identity at t=%d", i);
    }
}

// -- Clip --------------------------------------------------------------

static void test_clip_fully_visible(void)
{
    int x = 10, y = 20, w = 100, h = 50, sdx, sdy;
    bool ok = pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy);
    CHECK(ok, "fully-visible rect should clip true");
    CHECK(x == 10 && y == 20 && w == 100 && h == 50,
          "unchanged: %d %d %d %d", x, y, w, h);
    CHECK(sdx == 0 && sdy == 0, "no source offset: %d %d", sdx, sdy);
}

static void test_clip_off_left_top(void)
{
    int x = -10, y = -5, w = 30, h = 20, sdx, sdy;
    bool ok = pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy);
    CHECK(ok, "partially-on rect should clip true");
    CHECK(x == 0 && y == 0, "moved to origin: %d %d", x, y);
    CHECK(w == 20 && h == 15, "shortened: %d %d", w, h);
    CHECK(sdx == 10 && sdy == 5, "src offset: %d %d", sdx, sdy);
}

static void test_clip_off_right_bottom(void)
{
    int x = 470, y = 215, w = 30, h = 20, sdx, sdy;
    bool ok = pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy);
    CHECK(ok, "rect off right/bottom should clip true");
    CHECK(x == 470 && y == 215, "unchanged origin: %d %d", x, y);
    CHECK(w == 10 && h == 7, "trimmed: %d %d", w, h);
}

static void test_clip_fully_off(void)
{
    int x, y, w, h, sdx, sdy;

    x = 500; y = 50; w = 10; h = 10;
    CHECK(!pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy),
          "off the right edge");

    x = -50; y = -50; w = 30; h = 30;
    CHECK(!pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy),
          "off the top-left");

    x = 0; y = 0; w = 0; h = 100;
    CHECK(!pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy),
          "zero width");
    x = 0; y = 0; w = 100; h = -1;
    CHECK(!pageros_clip_rect(480, 222, &x, &y, &w, &h, &sdx, &sdy),
          "negative height");
}

int main(void)
{
    test_lerp_endpoints();
    test_lerp_midpoint();
    test_lerp_clamps();
    test_lerp_identity();
    test_clip_fully_visible();
    test_clip_off_left_top();
    test_clip_off_right_bottom();
    test_clip_fully_off();

    if (fail_count == 0) {
        printf("OK (8 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
