// SPDX-License-Identifier: Apache-2.0
//
// TCA8418 helpers — see tca8418.h.

#include "tca8418.h"

tca8418_event_t tca8418_decode_event(uint8_t raw)
{
    tca8418_event_t e = {0};
    if (raw == 0) return e;                   // empty FIFO

    uint8_t key = raw & 0x7F;                 // 1..80
    if (key == 0) return e;                   // defensive: malformed

    e.valid   = true;
    e.pressed = (raw & 0x80) != 0;
    // The TCA8418 indexes (row, col) 1-based, row-major across 10 columns.
    uint8_t idx = (uint8_t)(key - 1);
    e.row = (uint8_t)(idx / 10);
    e.col = (uint8_t)(idx % 10);
    return e;
}
