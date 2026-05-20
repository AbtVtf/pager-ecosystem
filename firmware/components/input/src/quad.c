// SPDX-License-Identifier: Apache-2.0
//
// Quadrature decoder — see quad.h.

#include "quad.h"

// Transition table from (prev_state << 2) | new_state. Convention here:
// state = (A << 1) | B, so state 0=00, 1=01, 2=10, 3=11.
//
// The canonical Gray-code sequence for one CW detent is
//   00 → 01 → 11 → 10 → 00     (transitions: 00→01, 01→11, 11→10, 10→00)
// For CCW the sequence reverses
//   00 → 10 → 11 → 01 → 00     (transitions: 00→10, 10→11, 11→01, 01→00)
//
// Any other transition (same-state, or a skip like 00→11) is invalid —
// either contact bounce or a sampling miss — and contributes 0.
static const int8_t TRANSITION[16] = {
    /* prev=00 */  0, +1, -1,  0,    /* new=00,01,10,11 */
    /* prev=01 */ -1,  0,  0, +1,
    /* prev=10 */ +1,  0,  0, -1,
    /* prev=11 */  0, -1, +1,  0,
};

void quad_init(quad_decoder_t *d, int8_t ticks_per_detent)
{
    d->prev_state       = 0;
    d->accumulator      = 0;
    d->ticks_per_detent = ticks_per_detent > 0 ? ticks_per_detent : 1;
}

quad_event_t quad_step(quad_decoder_t *d, bool a, bool b)
{
    uint8_t st = (uint8_t)((a ? 2u : 0u) | (b ? 1u : 0u));
    uint8_t idx = (uint8_t)((d->prev_state << 2) | st);
    d->prev_state = st;
    int8_t delta = TRANSITION[idx & 0x0F];
    if (delta == 0) return QUAD_NONE;

    int next = d->accumulator + delta;
    if (next >= d->ticks_per_detent) {
        d->accumulator = 0;
        return QUAD_CW;
    }
    if (next <= -d->ticks_per_detent) {
        d->accumulator = 0;
        return QUAD_CCW;
    }
    d->accumulator = (int8_t)next;
    return QUAD_NONE;
}
