// SPDX-License-Identifier: Apache-2.0
//
// Pure-C quadrature decoder used by the PagerOS input driver (FW-007).
//
// No ESP-IDF symbols; host tests under `firmware/host-tests/input/` link
// this translation unit directly with native gcc.
//
// Algorithm: the classic 16-entry state-transition table indexed by
// `(prev_state << 2) | new_state`. Each table entry is +1 (CW), -1 (CCW),
// or 0 (idle or invalid transition — discarded as bounce / noise).
// Detents on the LILYGO encoder are 4 pulses per click, so we accumulate
// raw ticks and only emit a detent event when the accumulator hits ±4.
//
// This both eliminates spurious mid-detent reports (mandated by the
// FW-007 acceptance clause "no spurious events at rest") and gives the
// shell a clean "one event per physical click" cadence.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QUAD_NONE = 0,
    QUAD_CW,
    QUAD_CCW,
} quad_event_t;

typedef struct {
    uint8_t  prev_state;     // last (A<<1)|B sampled, 0..3
    int8_t   accumulator;    // running sub-detent tick counter
    int8_t   ticks_per_detent;
} quad_decoder_t;

// Initialise. `ticks_per_detent` is the number of valid quadrature
// transitions per physical click — 4 for the LILYGO encoder.
void quad_init(quad_decoder_t *d, int8_t ticks_per_detent);

// Feed a new (A, B) sample. Returns CW or CCW exactly when the
// accumulator crosses ±ticks_per_detent; otherwise NONE.
quad_event_t quad_step(quad_decoder_t *d, bool a, bool b);

#ifdef __cplusplus
}
#endif
