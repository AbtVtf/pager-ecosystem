// SPDX-License-Identifier: Apache-2.0
//
// PagerOS font system — FW-019.
//
// Per SPEC §7.7 the firmware ships with Noto Sans subsets covering Latin,
// CJK, Arabic, Cyrillic, Greek, Hebrew, Devanagari + a curated emoji
// subset, with a per-codepoint fallback chain and an LRU glyph cache in
// PSRAM.
//
// This v0 implementation lands the API surface + a bundled compact ASCII
// bitmap font (the public-domain `font8x8_basic`). The fallback chain has
// exactly one font for now; non-Latin codepoints render as the
// missing-glyph box (U+25A1 → outlined square). Loading additional
// Noto Sans subsets from `/system/fonts/` (FreeType / stb_truetype) is a
// follow-up tracked in TASKS.md §FW-019 notes — the public API in this
// header doesn't change when that lands; only the implementation files
// grow.
//
// Renderer contract:
//
//   - Glyphs are drawn into a caller-supplied RGB565 canvas of arbitrary
//     dimensions. The display component (FW-005) hands its framebuffer
//     in directly. The renderer never reaches outside the canvas bounds.
//
//   - Text rendering is left-to-right only in v0. Bidi (Arabic, Hebrew)
//     and complex script shaping (Devanagari conjuncts) land with the
//     .ttf engine; for now those codepoints render but stack visually.
//
//   - All routines are safe to call from any task; the glyph cache uses
//     a mutex internally.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Public glyph cache budget. PSRAM-allocated; tunable per
// `pageros_fonts_init`.
#define PAGEROS_FONTS_DEFAULT_CACHE_ENTRIES   256

// One canvas in the renderer's expected pixel format.
typedef struct {
    uint16_t *pixels;    // RGB565 (display native)
    int       w;
    int       h;
} pageros_fonts_canvas_t;

// A rendered glyph's bounding box, returned by measure helpers so
// callers can lay out runs without a second pass.
typedef struct {
    int width;
    int height;
    int advance;        // x-cursor advance after rendering this glyph
    int ascent;         // pixels above baseline
    int descent;        // pixels below baseline (positive)
} pageros_fonts_metrics_t;

typedef struct {
    size_t cache_max_entries;
} pageros_fonts_opts_t;

esp_err_t pageros_fonts_init(const pageros_fonts_opts_t *opts);
esp_err_t pageros_fonts_shutdown(void);

// Metrics for the bundled fallback font. Used by layout code (FW-020)
// to size widgets before drawing.
void pageros_fonts_metrics_default(pageros_fonts_metrics_t *out);

// Pixel width of `text` (UTF-8) when rendered with the default font.
// `text_len` may be -1 (NUL-terminated) or an explicit byte length.
int pageros_fonts_measure_text(const char *text, int text_len);

// Render a single Unicode codepoint at (x, y) (top-left of the glyph
// bounding box). Returns the new x cursor (x + advance), or `x` if the
// glyph couldn't be rendered.
int pageros_fonts_draw_codepoint(const pageros_fonts_canvas_t *canvas,
                                 int x, int y,
                                 uint32_t codepoint,
                                 uint16_t fg_rgb565);

// Render a UTF-8 text run at (x, y). `text_len` may be -1
// (NUL-terminated) or an explicit byte length. If `max_width_px > 0`
// the run is truncated mid-run (no ellipsis in v0). Returns the new x
// cursor.
int pageros_fonts_draw_text(const pageros_fonts_canvas_t *canvas,
                            int x, int y,
                            const char *text, int text_len,
                            uint16_t fg_rgb565,
                            int max_width_px);

// --- 16x16 path ----------------------------------------------------- //
//
// Large glyphs for the desktop shell chrome (status bars, tile labels,
// titles). Codepoints with a designed glyph use it; others fall back to
// the 8x8 bitmap rendered at (x + 4, y + 4) — every ASCII codepoint
// renders, just less prettily.

#define PAGEROS_FONTS_GLYPH16_W   16
#define PAGEROS_FONTS_GLYPH16_H   16

int pageros_fonts_measure_text_16(const char *text, int text_len);

int pageros_fonts_draw_codepoint_16(const pageros_fonts_canvas_t *canvas,
                                    int x, int y,
                                    uint32_t codepoint,
                                    uint16_t fg_rgb565);

int pageros_fonts_draw_text_16(const pageros_fonts_canvas_t *canvas,
                               int x, int y,
                               const char *text, int text_len,
                               uint16_t fg_rgb565,
                               int max_width_px);

// Decode the next UTF-8 codepoint at `*p`. On success `*p` advances
// past the consumed bytes and the codepoint is returned. On a malformed
// or truncated sequence the function returns 0xFFFD (replacement char)
// and `*p` advances one byte to keep the run moving forward.
uint32_t pageros_fonts_utf8_next(const char **p, const char *end);

// Cache stats — exposed for the Settings → diagnostics screen.
typedef struct {
    size_t cache_entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t fallbacks;        // codepoints not in primary; drew via fallback
    uint64_t missing;          // codepoints in no font; drew the box glyph
} pageros_fonts_stats_t;

void pageros_fonts_get_stats(pageros_fonts_stats_t *out);

#ifdef __cplusplus
}
#endif
