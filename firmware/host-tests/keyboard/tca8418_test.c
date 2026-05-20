// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the TCA8418 event-byte decoder.
//
// Covers:
//   - Empty FIFO byte (0x00) → !valid.
//   - Press / release bit (0x80) mapping.
//   - Row-major key index decode (key 1..80 → (row, col) in 0..7 / 0..9).
//   - The boundary keys: 1 → (0,0), 10 → (0,9), 11 → (1,0), 80 → (7,9).
//   - 4×10 matrix boundary: rows 0..3 / cols 0..9 are the values the
//     driver later filters to the public queue.

#include "tca8418.h"

#include <stdio.h>

static int fail_count = 0;
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

static void test_empty_fifo(void)
{
    tca8418_event_t e = tca8418_decode_event(0x00);
    CHECK(!e.valid, "empty FIFO byte must yield !valid");
}

static void test_press_release_bit(void)
{
    // key 0x01 = (0,0); 0x81 = press of (0,0); 0x01 = release of (0,0).
    tca8418_event_t up = tca8418_decode_event(0x01);
    tca8418_event_t dn = tca8418_decode_event(0x81);
    CHECK(up.valid && !up.pressed, "0x01 should decode as release");
    CHECK(dn.valid &&  dn.pressed, "0x81 should decode as press");
    CHECK(up.row == 0 && up.col == 0, "release at (0,0)");
    CHECK(dn.row == 0 && dn.col == 0, "press   at (0,0)");
}

static void test_first_row_last_col(void)
{
    // key 10 = (0,9)
    tca8418_event_t e = tca8418_decode_event(0x80 | 10);
    CHECK(e.valid && e.pressed, "key 10 valid + press");
    CHECK(e.row == 0 && e.col == 9, "(row,col) = (0,9), got (%u,%u)", e.row, e.col);
}

static void test_second_row_first_col(void)
{
    // key 11 = (1,0)
    tca8418_event_t e = tca8418_decode_event(0x80 | 11);
    CHECK(e.row == 1 && e.col == 0, "(row,col) = (1,0), got (%u,%u)", e.row, e.col);
}

static void test_full_8x10_top_corner(void)
{
    // The TCA8418 supports 8×10 = 80 keys; key 80 = (7,9).
    tca8418_event_t e = tca8418_decode_event(0x80 | 80);
    CHECK(e.row == 7 && e.col == 9, "(row,col) = (7,9), got (%u,%u)", e.row, e.col);
}

static void test_matrix_4x10_boundary(void)
{
    // Sweep keys 1..40 — every one should decode to (0..3, 0..9), which
    // is exactly the 4×10 region the driver routes to keypad scanning.
    for (uint8_t k = 1; k <= 40; k++) {
        tca8418_event_t e = tca8418_decode_event(0x80 | k);
        CHECK(e.valid && e.pressed, "key %u should be a valid press", k);
        CHECK(e.row < 4 && e.col < 10,
              "key %u out of 4×10 region: (%u,%u)", k, e.row, e.col);
    }
}

int main(void)
{
    test_empty_fifo();
    test_press_release_bit();
    test_first_row_last_col();
    test_second_row_first_col();
    test_full_8x10_top_corner();
    test_matrix_4x10_boundary();

    if (fail_count == 0) {
        printf("OK (6 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
