// SPDX-License-Identifier: Apache-2.0
//
// Pure-C colour math used by the display driver. Split out so the host
// tests can exercise the gradient and clip logic without ESP-IDF.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Linear-interpolated row colour at fractional position `numer/denom`
// between `top` and `bottom` (both RGB565). Channel-wise interpolation
// happens in 8-bit space and is re-packed to RGB565 to avoid the
// chunky banding you get from interpolating in packed form.
//
// `denom` must be > 0; `numer` is clamped to [0, denom].
uint16_t pageros_lerp_rgb565(uint16_t top, uint16_t bottom,
                              int numer, int denom);

// Clip a (x,y,w,h) rectangle to the canvas bounds (0,0,canvas_w,canvas_h)
// **and** the source bitmap bounds (the source is anchored at the
// possibly negative (x,y) corner before clipping). On entry `*x`, `*y`,
// `*w`, `*h` describe the destination rectangle; `*src_dx`, `*src_dy`
// receive the source-bitmap offset to add after clipping.
//
// Returns false if the rectangle is fully off-screen (caller should
// skip the blit) and true if any pixels remain.
bool pageros_clip_rect(int canvas_w, int canvas_h,
                       int *x, int *y, int *w, int *h,
                       int *src_dx, int *src_dy);

#ifdef __cplusplus
}
#endif
