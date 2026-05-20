// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the quadrature decoder used by FW-007.
//
// We replay the canonical 4-step Gray sequence for both CW and CCW
// rotations and assert exactly one detent event lands on the closing
// transition. We also verify the "no spurious events at rest" clause
// from the FW-007 acceptance: holding A/B steady, or feeding invalid
// transitions, produces zero events.

#include "quad.h"

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

// Helper: feed a sequence of (a,b) samples and tally events.
typedef struct { int cw, ccw, none; } counts_t;

static counts_t feed(quad_decoder_t *d, const uint8_t (*pairs)[2], size_t n)
{
    counts_t c = {0};
    for (size_t i = 0; i < n; i++) {
        quad_event_t e = quad_step(d, pairs[i][0], pairs[i][1]);
        if (e == QUAD_CW)  c.cw++;
        else if (e == QUAD_CCW) c.ccw++;
        else c.none++;
    }
    return c;
}

// Test 1 — one clean clockwise detent emits exactly one CW.
static void test_one_cw_detent(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    // CW Gray-code sequence: 00 -> 01 -> 11 -> 10 -> 00
    const uint8_t seq[][2] = { {0,0}, {0,1}, {1,1}, {1,0}, {0,0} };
    counts_t c = feed(&d, seq, sizeof(seq)/sizeof(seq[0]));
    CHECK(c.cw == 1, "expected 1 CW, got %d", c.cw);
    CHECK(c.ccw == 0, "expected 0 CCW, got %d", c.ccw);
}

// Test 2 — one clean counter-clockwise detent emits exactly one CCW.
static void test_one_ccw_detent(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    // CCW Gray-code sequence: 00 -> 10 -> 11 -> 01 -> 00
    const uint8_t seq[][2] = { {0,0}, {1,0}, {1,1}, {0,1}, {0,0} };
    counts_t c = feed(&d, seq, sizeof(seq)/sizeof(seq[0]));
    CHECK(c.ccw == 1, "expected 1 CCW, got %d", c.ccw);
    CHECK(c.cw == 0, "expected 0 CW, got %d", c.cw);
}

// Test 3 — three CW detents in a row emit three CW events.
static void test_multiple_cw(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    const uint8_t step[][2] = { {0,1}, {1,1}, {1,0}, {0,0} };  // one detent from 00
    counts_t c = {0};
    for (int i = 0; i < 3; i++) {
        counts_t r = feed(&d, step, sizeof(step)/sizeof(step[0]));
        c.cw += r.cw; c.ccw += r.ccw;
    }
    CHECK(c.cw == 3, "expected 3 CW, got %d", c.cw);
    CHECK(c.ccw == 0, "expected 0 CCW, got %d", c.ccw);
}

// Test 4 — invalid (skip) transitions are discarded as bounce.
static void test_invalid_transitions(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    // 00 -> 11 (skip), 11 -> 00 (skip): all zero deltas → no events.
    const uint8_t seq[][2] = { {0,0}, {1,1}, {0,0}, {1,1}, {0,0} };
    counts_t c = feed(&d, seq, sizeof(seq)/sizeof(seq[0]));
    CHECK(c.cw == 0 && c.ccw == 0,
          "skip transitions must yield 0 events (CW=%d, CCW=%d)", c.cw, c.ccw);
}

// Test 5 — same-state samples (no rotation) emit nothing. Critical for
// the FW-007 acceptance: "no spurious events at rest".
static void test_idle(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    counts_t c = {0};
    for (int i = 0; i < 100; i++) {
        quad_event_t e = quad_step(&d, 0, 0);
        if (e != QUAD_NONE) c.cw++;
    }
    CHECK(c.cw == 0, "idle produced %d events", c.cw);
}

// Test 6 — sub-detent oscillation (single tick CW then CCW) does NOT
// generate a detent. Simulates the user resting their finger on the
// encoder while it wobbles at a partial click.
static void test_partial_detent_bounce(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    // 00 -> 01 (CW +1) -> 00 (CCW -1) -> 01 (CW +1) -> 00 (CCW -1) ...
    const uint8_t bounce[][2] = { {0,1}, {0,0} };
    counts_t c = {0};
    for (int i = 0; i < 20; i++) {
        counts_t r = feed(&d, bounce, 2);
        c.cw += r.cw; c.ccw += r.ccw;
    }
    CHECK(c.cw == 0 && c.ccw == 0,
          "partial-detent bounce produced events (CW=%d, CCW=%d)", c.cw, c.ccw);
}

// Test 7 — direction reversal mid-rotation is recognised.
static void test_direction_reversal(void)
{
    quad_decoder_t d;
    quad_init(&d, 4);
    // One full CW detent...
    const uint8_t cw[][2]  = { {0,1}, {1,1}, {1,0}, {0,0} };
    feed(&d, cw, 4);
    // ...then one full CCW detent.
    const uint8_t ccw[][2] = { {1,0}, {1,1}, {0,1}, {0,0} };
    counts_t c = feed(&d, ccw, 4);
    CHECK(c.ccw == 1, "expected 1 CCW after reversal, got %d", c.ccw);
}

int main(void)
{
    test_one_cw_detent();
    test_one_ccw_detent();
    test_multiple_cw();
    test_invalid_transitions();
    test_idle();
    test_partial_detent_bounce();
    test_direction_reversal();

    if (fail_count == 0) {
        printf("OK (7 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
