// SPDX-License-Identifier: Apache-2.0
//
// PagerOS cyberpunk fx engine — implementation.

#include "pageros_cyber_fx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cyber_fx";

// ============================================================== //
// Procedural katakana table (48 × 8×8).
//
// Hand-designed dense bitmaps that *evoke* katakana without shipping a
// CJK font. Each entry is 8 rows, row[0] = top, bit 0 = leftmost pixel.
// Variety matters more than orthographic accuracy — these scroll past
// at 25 fps in the matrix rain.
// ============================================================== //

static const uint8_t kata_glyphs[PAGEROS_CYBER_FX_KATAKANA_COUNT][8] = {
    // ア (a-ish)
    { 0x7E, 0x40, 0x40, 0x20, 0x10, 0x12, 0x0A, 0x06 },
    // イ
    { 0x10, 0x18, 0x14, 0x12, 0x10, 0x10, 0x10, 0x10 },
    // ウ
    { 0x7C, 0x44, 0x00, 0x7E, 0x42, 0x02, 0x02, 0x02 },
    // エ
    { 0x7E, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7E },
    // オ
    { 0x7E, 0x10, 0x10, 0x7E, 0x14, 0x12, 0x12, 0x10 },
    // カ
    { 0x42, 0x42, 0x7E, 0x42, 0x42, 0x44, 0x44, 0x18 },
    // キ
    { 0x10, 0x10, 0x7E, 0x10, 0x7E, 0x10, 0x10, 0x10 },
    // ク
    { 0x7E, 0x42, 0x42, 0x42, 0x44, 0x44, 0x28, 0x10 },
    // ケ
    { 0x10, 0x7E, 0x14, 0x12, 0x10, 0x10, 0x10, 0x10 },
    // コ
    { 0x7E, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E },
    // サ
    { 0x24, 0x24, 0x7E, 0x24, 0x24, 0x24, 0x24, 0x44 },
    // シ
    { 0x42, 0x00, 0x42, 0x00, 0x40, 0x40, 0x40, 0x3E },
    // ス
    { 0x7E, 0x42, 0x40, 0x20, 0x10, 0x28, 0x44, 0x42 },
    // セ
    { 0x12, 0x12, 0x7E, 0x12, 0x12, 0x12, 0x14, 0x18 },
    // ソ
    { 0x42, 0x42, 0x02, 0x04, 0x04, 0x08, 0x10, 0x20 },
    // タ
    { 0x7E, 0x42, 0x42, 0x7E, 0x4A, 0x52, 0x22, 0x20 },
    // チ
    { 0x7E, 0x08, 0x7E, 0x10, 0x20, 0x20, 0x20, 0x20 },
    // ツ
    { 0x42, 0x42, 0x42, 0x00, 0x40, 0x40, 0x40, 0x3E },
    // テ
    { 0x7E, 0x00, 0x7E, 0x10, 0x10, 0x10, 0x10, 0x10 },
    // ト
    { 0x10, 0x10, 0x10, 0x14, 0x12, 0x10, 0x10, 0x10 },
    // ナ
    { 0x10, 0x10, 0x7E, 0x10, 0x10, 0x10, 0x10, 0x10 },
    // ニ
    { 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E },
    // ヌ
    { 0x7E, 0x42, 0x40, 0x20, 0x10, 0x28, 0x44, 0x40 },
    // ネ
    { 0x10, 0x10, 0x7E, 0x42, 0x10, 0x28, 0x44, 0x42 },
    // ノ
    { 0x40, 0x40, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08 },
    // ハ
    { 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42 },
    // ヒ
    { 0x40, 0x44, 0x48, 0x50, 0x60, 0x40, 0x40, 0x7E },
    // フ
    { 0x7E, 0x42, 0x40, 0x20, 0x10, 0x10, 0x08, 0x08 },
    // ヘ
    { 0x00, 0x00, 0x00, 0x18, 0x24, 0x42, 0x00, 0x00 },
    // ホ
    { 0x10, 0x10, 0x7E, 0x14, 0x52, 0x52, 0x10, 0x10 },
    // マ
    { 0x7E, 0x40, 0x20, 0x10, 0x28, 0x44, 0x42, 0x00 },
    // ミ
    { 0x7E, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x7E },
    // ム
    { 0x10, 0x10, 0x10, 0x20, 0x40, 0x40, 0x42, 0x7E },
    // メ
    { 0x40, 0x20, 0x10, 0x7E, 0x18, 0x28, 0x44, 0x42 },
    // モ
    { 0x10, 0x7E, 0x10, 0x10, 0x7E, 0x10, 0x10, 0x7E },
    // ヤ
    { 0x10, 0x10, 0x52, 0x54, 0x10, 0x10, 0x10, 0x10 },
    // ユ
    { 0x7E, 0x40, 0x40, 0x40, 0x7E, 0x40, 0x40, 0x7E },
    // ヨ
    { 0x7E, 0x40, 0x40, 0x7E, 0x40, 0x40, 0x40, 0x7E },
    // ラ
    { 0x7E, 0x00, 0x7E, 0x40, 0x20, 0x10, 0x08, 0x04 },
    // リ
    { 0x44, 0x44, 0x44, 0x44, 0x44, 0x42, 0x42, 0x40 },
    // ル
    { 0x42, 0x42, 0x42, 0x42, 0x52, 0x52, 0x42, 0x46 },
    // レ
    { 0x40, 0x40, 0x40, 0x40, 0x42, 0x44, 0x48, 0x30 },
    // ロ
    { 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E },
    // ワ
    { 0x7E, 0x42, 0x42, 0x42, 0x44, 0x40, 0x20, 0x10 },
    // ヲ
    { 0x7E, 0x40, 0x40, 0x7E, 0x20, 0x10, 0x08, 0x04 },
    // ン
    { 0x02, 0x04, 0x00, 0x00, 0x40, 0x40, 0x40, 0x3E },
    // ヰ-ish
    { 0x18, 0x18, 0x7E, 0x18, 0x18, 0x7E, 0x18, 0x18 },
    // ヱ-ish
    { 0x7E, 0x10, 0x10, 0x7E, 0x10, 0x10, 0x10, 0x7E },
};

// ============================================================== //
// State.
// ============================================================== //

#define MATRIX_MAX_COLS  60      // covers 480 px wide @ 8 px per col
#define MATRIX_GRID_PX    8      // glyph step
#define MATRIX_ROWS_MAX  64

typedef struct {
    int16_t head_row;     // current head position in rows (can exceed h_rows)
    int8_t  speed;        // rows per "speed_div" ticks
    uint8_t trail;        // trail length in rows
    uint8_t accum;        // accumulator for speed-divider stepping
    uint8_t glyph_seed;   // base glyph index for this column
} matrix_col_t;

static struct {
    bool inited;
    bool task_running;
    SemaphoreHandle_t mu;
    void (*render_cb)(void);

    // Matrix rain ----------------------------------------------------
    matrix_col_t cols[MATRIX_MAX_COLS];
    int          col_count;             // active number (depends on viewport)
    int          col_viewport_w;        // last w used, triggers reseed if changed
    int64_t      matrix_last_step_us;

    // Glitch overlay -------------------------------------------------
    int64_t glitch_next_us;             // when next glitch starts
    int64_t glitch_until_us;            // when current glitch ends
    int     glitch_y;                   // top row of glitched slice
    int     glitch_h;                   // glitched slice height
    int     glitch_dx;                  // x-shift in px (-12..+12)

    // Transition wipe ------------------------------------------------
    int64_t transition_until_us;
    int64_t transition_started_us;

    // Action ripple --------------------------------------------------
    int     ripple_x, ripple_y;
    int64_t ripple_until_us;

    // Tick counter ---------------------------------------------------
    uint32_t tick;

    // Matrix visibility timer — extended on each paint_matrix call.
    // While the wall-clock is below this we keep ticking + requesting
    // renders so the rain animates. After it expires we stop calling
    // render_cb, which lets non-animated pages idle the SPI bus.
    int64_t matrix_visible_until_us;
} g;

// Time helpers --------------------------------------------------------
static inline int64_t now_us(void) { return esp_timer_get_time(); }

static inline uint32_t rng32(void) { return esp_random(); }

static inline int rng_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(rng32() % (uint32_t)(hi - lo));
}

// ============================================================== //
// 565 helpers for tint / blend.
// ============================================================== //

static inline uint16_t rgb_scale(uint16_t c, int num, int den)
{
    if (den <= 0) return 0;
    int r = (c >> 11) & 0x1F;
    int gn = (c >> 5)  & 0x3F;
    int b = c         & 0x1F;
    r = (r * num) / den;
    gn = (gn * num) / den;
    b = (b * num) / den;
    if (r > 31) r = 31;
    if (gn > 63) gn = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (gn << 5) | b);
}

static inline void pix(const pageros_fonts_canvas_t *c, int x, int y, uint16_t rgb)
{
    if (!c || !c->pixels) return;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->pixels[y * c->w + x] = rgb;
}

// ============================================================== //
// Matrix rain.
// ============================================================== //

static void matrix_reseed(int viewport_w)
{
    int cols = viewport_w / MATRIX_GRID_PX;
    if (cols < 1)  cols = 1;
    if (cols > MATRIX_MAX_COLS) cols = MATRIX_MAX_COLS;
    g.col_count        = cols;
    g.col_viewport_w   = viewport_w;
    for (int i = 0; i < cols; i++) {
        g.cols[i].head_row    = (int16_t)(-(rng_range(0, 40)));
        g.cols[i].speed       = (int8_t)rng_range(1, 4);
        g.cols[i].trail       = (uint8_t)rng_range(6, 18);
        g.cols[i].accum       = 0;
        g.cols[i].glyph_seed  = (uint8_t)rng_range(0, 256);
    }
    g.matrix_last_step_us = now_us();
}

// Draw one 8x8 katakana glyph at (x,y). Bit 0 of each row byte is the
// leftmost pixel.
static void draw_kata_8(const pageros_fonts_canvas_t *c,
                        int x, int y, int idx, uint16_t color)
{
    if (idx < 0) idx = -idx;
    idx %= PAGEROS_CYBER_FX_KATAKANA_COUNT;
    const uint8_t *rows = kata_glyphs[idx];
    for (int j = 0; j < 8; j++) {
        uint8_t r = rows[j];
        if (!r) continue;
        for (int i = 0; i < 8; i++) {
            if (r & (1u << i)) pix(c, x + i, y + j, color);
        }
    }
}

void pageros_cyber_fx_draw_katakana(const pageros_fonts_canvas_t *canvas,
                                    int x, int y, int idx, int scale,
                                    uint16_t color)
{
    if (scale < 1) scale = 1;
    if (idx < 0) idx = -idx;
    idx %= PAGEROS_CYBER_FX_KATAKANA_COUNT;
    const uint8_t *rows = kata_glyphs[idx];
    for (int j = 0; j < 8; j++) {
        uint8_t r = rows[j];
        if (!r) continue;
        for (int i = 0; i < 8; i++) {
            if (!(r & (1u << i))) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    pix(canvas, x + i * scale + sx, y + j * scale + sy, color);
                }
            }
        }
    }
}

void pageros_cyber_fx_draw_katakana_random(const pageros_fonts_canvas_t *canvas,
                                           int x, int y, int scale,
                                           uint16_t color)
{
    pageros_cyber_fx_draw_katakana(canvas, x, y,
                                   (int)(g.tick + (uint32_t)(x * 13 + y * 7)),
                                   scale, color);
}

// Tick matrix columns: each column has an accumulator; if it reaches its
// speed threshold the head advances one row and the column may mutate
// its glyph_seed so the trail "shimmers". Called once per fx tick.
static void matrix_step(int rows_max)
{
    for (int i = 0; i < g.col_count; i++) {
        matrix_col_t *col = &g.cols[i];
        col->accum++;
        // speed 1 → step every 4 ticks, speed 3 → step every ~1 tick.
        if (col->accum < (uint8_t)(5 - col->speed)) continue;
        col->accum = 0;
        col->head_row++;
        // Mutate seed so trail glyphs shimmer.
        col->glyph_seed = (uint8_t)(col->glyph_seed * 5u + 23u);
        if (col->head_row > rows_max + (int16_t)col->trail) {
            col->head_row = (int16_t)(-(rng_range(0, 30)));
            col->speed    = (int8_t)rng_range(1, 4);
            col->trail    = (uint8_t)rng_range(6, 18);
        }
    }
}

void pageros_cyber_fx_paint_matrix(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   int x, int y, int w, int h)
{
    if (!g.inited) return;
    if (!canvas || !pal || w <= 0 || h <= 0) return;
    if (w != g.col_viewport_w) matrix_reseed(w);
    // Keep the engine ticking as long as something paints the rain.
    g.matrix_visible_until_us = now_us() + 250 * 1000;

    int rows = h / MATRIX_GRID_PX;
    if (rows < 1) return;
    if (rows > MATRIX_ROWS_MAX) rows = MATRIX_ROWS_MAX;

    // Head colour: a near-white shade. Trail uses pal->info scaled down.
    uint16_t head_white = 0xFFFF;
    uint16_t trail_full = pal->info;

    for (int i = 0; i < g.col_count; i++) {
        const matrix_col_t *col = &g.cols[i];
        int cx = x + i * MATRIX_GRID_PX;
        int head = col->head_row;
        int trail = col->trail;

        // Walk trail from head upward.
        for (int r = 0; r <= trail; r++) {
            int row = head - r;
            if (row < 0 || row >= rows) continue;
            int py = y + row * MATRIX_GRID_PX;
            int gidx = (col->glyph_seed + i * 7 + row * 3) & 0x7F;
            uint16_t color;
            if (r == 0) {
                color = head_white;
            } else if (r == 1) {
                color = rgb_scale(trail_full, 13, 16);
            } else {
                // Fade from full → 1/8 across the trail.
                int num = 14 - r;
                if (num < 1) num = 1;
                color = rgb_scale(trail_full, num, 16);
            }
            draw_kata_8(canvas, cx, py, gidx, color);
        }
    }
}

// ============================================================== //
// Focus sweep.
// ============================================================== //

void pageros_cyber_fx_paint_focus(const pageros_fonts_canvas_t *canvas,
                                  const pageros_widgets_palette_t *pal,
                                  int x, int y, int w, int h)
{
    if (!g.inited) return;
    if (!canvas || !pal || w <= 8 || h <= 8) return;

    uint16_t accent = pal->accent;
    uint16_t info   = pal->info;

    // Animated corner bracket length: 6..16 px oscillating over ~1s.
    uint32_t t = g.tick;
    int phase  = (int)(t % 50);
    int base   = 6 + (phase < 25 ? phase * 10 / 25 : (50 - phase) * 10 / 25);

    // Four corners.
    int b = base;
    for (int i = 0; i < b; i++) {
        // top-left
        pix(canvas, x + i, y,         accent);
        pix(canvas, x,     y + i,     accent);
        // top-right
        pix(canvas, x + w - 1 - i, y,         accent);
        pix(canvas, x + w - 1,     y + i,     accent);
        // bot-left
        pix(canvas, x + i, y + h - 1, accent);
        pix(canvas, x,     y + h - 1 - i, accent);
        // bot-right
        pix(canvas, x + w - 1 - i, y + h - 1, accent);
        pix(canvas, x + w - 1,     y + h - 1 - i, accent);
    }

    // Travelling light along the top edge — a 12 px bright dot that
    // walks left→right and wraps. Speed: 1 px / tick.
    int sweep = (int)(t * 2 % (uint32_t)(w + 24)) - 12;
    for (int i = 0; i < 12; i++) {
        int sx = x + sweep + i;
        if (sx < x + b + 2 || sx > x + w - b - 3) continue;
        int alpha = (i < 6) ? i : (11 - i);
        uint16_t c = rgb_scale(info, alpha + 1, 7);
        pix(canvas, sx, y, c);
        pix(canvas, sx, y + 1, c);
    }

    // Outer "data-burst" ticks on the left edge — every 4 rows, small
    // accent dots that pulse with tick.
    for (int j = b + 4; j < h - b - 4; j += 5) {
        if (((t / 4) + j) % 3 != 0) continue;
        pix(canvas, x - 2, y + j, accent);
        pix(canvas, x - 3, y + j, accent);
    }
}

// ============================================================== //
// Per-element glitch (per-tile / per-rail-row jitter).
// ============================================================== //

// Each element has its own glitch schedule keyed by `seed`. Within a
// 60-tick window (~2.4 s) the element glitches for 3 ticks (~120 ms),
// then sits quiet. Different seeds offset the active window so
// neighbouring tiles glitch out of phase.
void pageros_cyber_fx_paint_element_glitch(const pageros_fonts_canvas_t *canvas,
                                           const pageros_widgets_palette_t *pal,
                                           int x, int y, int w, int h,
                                           uint32_t seed)
{
    if (!g.inited) return;
    if (!canvas || !canvas->pixels || !pal) return;
    if (w < 6 || h < 4) return;

    const uint32_t cycle  = 60;
    const uint32_t active = 3;
    uint32_t phase = (g.tick + seed * 17u) % cycle;
    if (phase >= active) return;          // quiet — early-out hot path

    // Deterministic per-event RNG so the same glitch frame is consistent
    // across consecutive ticks (no per-pixel strobe within the slice).
    uint32_t r = (g.tick / active + seed) * 1103515245u + 12345u;
    int max_slice = h / 2;
    if (max_slice < 2) max_slice = 2;
    int slice_h = 2 + (int)((r >> 4) & 0x7);
    if (slice_h > max_slice) slice_h = max_slice;
    int slice_y = y + (int)((r >> 7) % (uint32_t)(h - slice_h));
    int dx = ((int)((r >> 14) & 0xF)) - 7;   // -7..+8
    if (dx == 0) dx = (seed & 1) ? 3 : -3;
    if (dx > 6)  dx = 6;
    if (dx < -6) dx = -6;

    int adx = dx < 0 ? -dx : dx;
    int x0 = x;
    int x1 = x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > canvas->w) x1 = canvas->w;

    for (int yy = slice_y; yy < slice_y + slice_h; yy++) {
        if (yy < 0 || yy >= canvas->h) continue;
        uint16_t *row = canvas->pixels + (size_t)yy * canvas->w;
        if (dx > 0) {
            // Shift slice right; fill leftmost cells with accent fringe.
            for (int xx = x1 - 1; xx >= x0 + adx; xx--) row[xx] = row[xx - adx];
            for (int xx = x0; xx < x0 + adx && xx < x1; xx++) {
                row[xx] = pal->accent;
            }
        } else {
            // Shift slice left; fill rightmost cells with info fringe.
            for (int xx = x0; xx < x1 - adx; xx++) row[xx] = row[xx + adx];
            for (int xx = x1 - adx; xx < x1; xx++) {
                row[xx] = pal->info;
            }
        }
    }

    // Chroma bleed on the rows just above + below the shifted slice —
    // halve all channels so the edges read as the "scan tear".
    int bleed_top = slice_y - 1;
    int bleed_bot = slice_y + slice_h;
    if (bleed_top >= y && bleed_top < canvas->h) {
        uint16_t *row = canvas->pixels + (size_t)bleed_top * canvas->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = rgb_scale(row[xx], 5, 8);
        }
    }
    if (bleed_bot < y + h && bleed_bot < canvas->h) {
        uint16_t *row = canvas->pixels + (size_t)bleed_bot * canvas->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = rgb_scale(row[xx], 5, 8);
        }
    }
}

// ============================================================== //
// Glitch overlay (slice jitter + chroma split).
// ============================================================== //

static void glitch_pick_next(void)
{
    // Fire again in 2.4–5.6 s.
    g.glitch_next_us = now_us() + (int64_t)rng_range(2400, 5600) * 1000;
}

void pageros_cyber_fx_kick_glitch(void)
{
    if (!g.inited) return;
    g.glitch_next_us = now_us() + 80 * 1000;
}

void pageros_cyber_fx_paint_overlay(const pageros_fonts_canvas_t *canvas,
                                    const pageros_widgets_palette_t *pal)
{
    if (!g.inited) return;
    if (!canvas || !canvas->pixels || !pal) return;
    int64_t t = now_us();

    // Start a new glitch?
    if (g.glitch_until_us <= t && t >= g.glitch_next_us) {
        g.glitch_until_us = t + (int64_t)rng_range(60, 160) * 1000;
        g.glitch_y  = rng_range(14, canvas->h - 30);
        g.glitch_h  = rng_range(3, 14);
        g.glitch_dx = rng_range(-10, 11);
        glitch_pick_next();
    }

    // Active glitch — shift a horizontal slice.
    if (g.glitch_until_us > t && g.glitch_h > 0 && g.glitch_dx != 0) {
        int y0 = g.glitch_y;
        int y1 = y0 + g.glitch_h;
        if (y0 < 0) y0 = 0;
        if (y1 > canvas->h) y1 = canvas->h;
        int dx = g.glitch_dx;
        for (int y = y0; y < y1; y++) {
            uint16_t *row = canvas->pixels + y * canvas->w;
            if (dx > 0) {
                for (int x = canvas->w - 1; x >= dx; x--) row[x] = row[x - dx];
                // RGB chroma fringe along the new left edge.
                for (int x = 0; x < dx && x < canvas->w; x++) {
                    row[x] = pal->error;
                }
            } else {
                int adx = -dx;
                for (int x = 0; x < canvas->w - adx; x++) row[x] = row[x + adx];
                for (int x = canvas->w - adx; x < canvas->w; x++) {
                    row[x] = pal->info;
                }
            }
        }
        // Adjacent chroma offset row above + below.
        if (y0 - 1 >= 0) {
            uint16_t *row = canvas->pixels + (y0 - 1) * canvas->w;
            for (int x = 0; x < canvas->w; x++) row[x] = rgb_scale(row[x], 5, 8);
        }
        if (y1 < canvas->h) {
            uint16_t *row = canvas->pixels + y1 * canvas->w;
            for (int x = 0; x < canvas->w; x++) row[x] = rgb_scale(row[x], 5, 8);
        }
    }

    // Action ripple — bright accent dot fading near the click point.
    if (g.ripple_until_us > t) {
        int64_t left = g.ripple_until_us - t;
        int r = (int)(8 + (180 - (left / 1000)) / 8);
        if (r > 28) r = 28;
        uint16_t c = pal->accent;
        for (int ang = 0; ang < 32; ang++) {
            // Cheap circle: precomputed isn't needed for 32 samples.
            int dx = (int)(r * (int)(((ang & 7) - 4)) / 4);
            int dy = (int)(r * (int)((((ang >> 3) & 3) - 1)) / 2);
            pix(canvas, g.ripple_x + dx,     g.ripple_y + dy, c);
            pix(canvas, g.ripple_x + dx + 1, g.ripple_y + dy, c);
        }
    }

    // Subtle vignette on the four edges (1 line at edges, faded).
    int w = canvas->w, h = canvas->h;
    for (int x = 0; x < w; x++) {
        canvas->pixels[0 * w + x]       = rgb_scale(canvas->pixels[0 * w + x],       5, 8);
        canvas->pixels[(h - 1) * w + x] = rgb_scale(canvas->pixels[(h - 1) * w + x], 5, 8);
    }
}

// ============================================================== //
// Page transition wipe.
// ============================================================== //

void pageros_cyber_fx_on_page_enter(const char *page_id)
{
    (void)page_id;
    if (!g.inited) return;
    int64_t t = now_us();
    g.transition_started_us = t;
    g.transition_until_us   = t + 350 * 1000;   // hologram boot reveal, 350 ms
    // Reseed matrix so the new page starts crisp.
    if (g.col_count > 0) matrix_reseed(g.col_viewport_w);
    // NB: no glitch kick here — the CRT collapse already owns the eye.
    // NB: do NOT call render_cb from here — the shell triggers the next
    // render itself, and we can be called from inside the render path.
}

void pageros_cyber_fx_on_action(int x, int y)
{
    g.ripple_x = x;
    g.ripple_y = y;
    g.ripple_until_us = now_us() + 180 * 1000;
}

// Hologram boot reveal. 350 ms top→bottom scan:
//
//   - Above the scan line: the new page is left visible (already painted
//     by the page renderer); we overlay a subtle cyan scanline pattern
//     so it reads as "freshly rendered" by the electron gun.
//   - The scan line itself: a 4 px neon stripe — magenta core + white
//     hot line + cyan trailing edge, with a half-row magenta glow above.
//   - Below the scan line: black with sparse falling katakana, like the
//     CPU is still computing the bottom of the frame. Glyphs are seeded
//     by (column, tick) so they shimmer but don't strobe randomly.
//
// All paint operations are over-paint on a fresh framebuffer (each fx
// tick re-renders the new page before this runs), so the reveal looks
// like the new page is being drawn in place.
void pageros_cyber_fx_paint_transition(const pageros_fonts_canvas_t *canvas,
                                       const pageros_widgets_palette_t *pal)
{
    if (!g.inited) return;
    if (!canvas || !canvas->pixels || !pal) return;
    int64_t t = now_us();
    if (t >= g.transition_until_us) return;

    int64_t total = g.transition_until_us - g.transition_started_us;
    if (total < 1) return;
    int64_t elapsed = t - g.transition_started_us;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > total) elapsed = total;

    int w = canvas->w, h = canvas->h;

    // Scan line position: travels from y=-4 (just above top) to y=h+4
    // (off the bottom), so it always enters/leaves smoothly.
    int scan_y = (int)(((elapsed * (int64_t)(h + 8)) / total)) - 4;

    // 1) Subtle "freshly drawn" scanline pattern over the revealed region
    //    above the scan head (only top 30 px of revealed area to avoid
    //    overwhelming the rest of the new page).
    int reveal_top = scan_y - 32;
    if (reveal_top < 0) reveal_top = 0;
    for (int y = reveal_top; y < scan_y - 2 && y < h; y++) {
        if (y < 0) continue;
        if ((y & 1) == 0) continue;     // every other row
        int dist = scan_y - y;           // distance from scan head
        if (dist > 32) continue;
        // Brightness ramps with proximity to the scan head.
        int num = 8 - (dist / 4);
        if (num < 1) num = 1;
        uint16_t *row = canvas->pixels + y * w;
        for (int x = 0; x < w; x++) {
            row[x] = rgb_scale(row[x], 16 - num, 16);
        }
    }

    // 2) The scan head itself — 4 px neon stripe with glow.
    //
    //    layout (top→bottom):
    //      scan_y - 1   : magenta glow (blended onto revealed page)
    //      scan_y       : magenta core
    //      scan_y + 1   : pure white hot line
    //      scan_y + 2   : cyan trailing edge
    //      scan_y + 3   : dim cyan after-glow

    if (scan_y - 1 >= 0 && scan_y - 1 < h) {
        uint16_t *row = canvas->pixels + (scan_y - 1) * w;
        for (int x = 0; x < w; x++) {
            row[x] = rgb_scale(pal->accent, 5, 8);
        }
    }
    if (scan_y >= 0 && scan_y < h) {
        uint16_t *row = canvas->pixels + scan_y * w;
        for (int x = 0; x < w; x++) row[x] = pal->accent;
    }
    if (scan_y + 1 >= 0 && scan_y + 1 < h) {
        uint16_t *row = canvas->pixels + (scan_y + 1) * w;
        for (int x = 0; x < w; x++) row[x] = 0xFFFF;
    }
    if (scan_y + 2 >= 0 && scan_y + 2 < h) {
        uint16_t *row = canvas->pixels + (scan_y + 2) * w;
        for (int x = 0; x < w; x++) row[x] = pal->info;
    }
    if (scan_y + 3 >= 0 && scan_y + 3 < h) {
        uint16_t *row = canvas->pixels + (scan_y + 3) * w;
        for (int x = 0; x < w; x++) row[x] = rgb_scale(pal->info, 5, 8);
    }

    // 3) Below the scan head: blank to black, then sprinkle stable
    //    "compute" katakana so the unrendered region feels active.
    int below_y = scan_y + 4;
    if (below_y < 0) below_y = 0;
    if (below_y < h) {
        size_t bytes = (size_t)(h - below_y) * (size_t)w * sizeof(uint16_t);
        memset(canvas->pixels + (size_t)below_y * (size_t)w, 0, bytes);

        // Falling katakana on a 16-px grid. Each cell is seeded by its
        // grid coords so it stays in place per-frame (no strobe).
        int cell = 16;
        uint32_t seed = g.tick / 3;       // shift glyphs every ~120 ms
        for (int gy = below_y; gy < h; gy += cell) {
            for (int gx = 0; gx < w; gx += cell) {
                uint32_t k = ((uint32_t)gx * 73856093u)
                           ^ ((uint32_t)gy * 19349663u)
                           ^ (seed * 83492791u);
                // Sparse: only ~1 in 4 cells draws a glyph.
                if ((k & 3) != 0) continue;
                int idx = (int)((k >> 4) & 0x7F);
                int dim = (int)((k >> 11) & 0x7);
                uint16_t base = (k & 0x10) ? pal->info : pal->accent;
                uint16_t col = rgb_scale(base, 3 + dim, 12);
                draw_kata_8(canvas, gx + 4, gy + 4, idx, col);
            }
        }
    }
}

// ============================================================== //
// Decorative drawing helpers.
// ============================================================== //

void pageros_cyber_fx_draw_bracket_pair(const pageros_fonts_canvas_t *canvas,
                                        int x, int y, int w, int h,
                                        uint16_t color)
{
    if (!canvas || w < 8 || h < 6) return;
    int arm = h / 3;
    if (arm < 2) arm = 2;
    // Left bracket  「
    for (int i = 0; i < arm; i++) {
        pix(canvas, x,     y + i, color);
        pix(canvas, x + 1, y + i, color);
    }
    for (int i = 0; i < arm; i++) {
        pix(canvas, x + i, y, color);
        pix(canvas, x + i, y + 1, color);
    }
    // Right bracket  」
    int rx = x + w - 1;
    int ry = y + h - 1;
    for (int i = 0; i < arm; i++) {
        pix(canvas, rx,     ry - i, color);
        pix(canvas, rx - 1, ry - i, color);
    }
    for (int i = 0; i < arm; i++) {
        pix(canvas, rx - i, ry,     color);
        pix(canvas, rx - i, ry - 1, color);
    }
}

void pageros_cyber_fx_draw_tag(const pageros_fonts_canvas_t *canvas,
                               const pageros_widgets_palette_t *pal,
                               int x, int y,
                               const char *prefix, const char *label)
{
    if (!canvas || !pal) return;
    // [<prefix>//<label>·▓] — last char shimmers between blocks.
    char buf[40];
    const uint8_t shim[] = { 0xB0, 0xB1, 0xB2, 0xDB };  // ascii shade-ish stand-in
    char ch = (char)('!' + (g.tick & 0x1F));
    if (g.tick & 1) ch = '#';
    snprintf(buf, sizeof(buf), "[%s//%s%c%c]",
             prefix ? prefix : "SYS",
             label  ? label  : "",
             '~', (int)ch);
    (void)shim;
    pageros_fonts_draw_text(canvas, x, y, buf, -1, pal->info, 240);
}

// ============================================================== //
// Animation task.
// ============================================================== //

static void cyber_fx_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(40);  // ~25 fps
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, period);
        if (g.mu) xSemaphoreTake(g.mu, portMAX_DELAY);
        g.tick++;

        // Step the matrix columns. We don't know the exact viewport h here
        // — paint walks with rows_max derived from h passed in. Step using
        // a generous bound that covers any page.
        matrix_step(MATRIX_ROWS_MAX);

        bool needs = pageros_cyber_fx_animating();
        if (g.mu) xSemaphoreGive(g.mu);

        if (needs && g.render_cb) g.render_cb();
    }
}

bool pageros_cyber_fx_animating(void)
{
    int64_t t = now_us();
    if (g.glitch_until_us > t)        return true;
    if (g.transition_until_us > t)    return true;
    if (g.ripple_until_us > t)        return true;
    if (g.matrix_visible_until_us > t) return true;
    return false;
}

// ============================================================== //
// Init / start.
// ============================================================== //

void pageros_cyber_fx_init(void)
{
    if (g.inited) return;
    memset(&g, 0, sizeof(g));
    g.mu = xSemaphoreCreateMutex();
    g.glitch_next_us = now_us() + 1500 * 1000;
    g.inited = true;
    ESP_LOGI(TAG, "init: %d katakana glyphs ready", PAGEROS_CYBER_FX_KATAKANA_COUNT);
}

void pageros_cyber_fx_start(void (*render_cb)(void))
{
    if (!g.inited) pageros_cyber_fx_init();
    if (g.task_running) return;
    g.render_cb = render_cb;
    // Reseed for a sensible default viewport before the first paint.
    matrix_reseed(400);
    g.task_running = true;
    xTaskCreate(cyber_fx_task, "cyber_fx", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "fx task started (25 fps)");
}
