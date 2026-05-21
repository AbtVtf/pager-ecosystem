// SPDX-License-Identifier: Apache-2.0
//
// FW-019 — font system implementation.
//
// v0: ships one bundled fallback font, the public-domain 8x8 ASCII
// bitmap (`font8x8_basic`). Latin codepoints render directly; anything
// outside ASCII renders as the missing-glyph box. The API surface is
// the final one — adding more fonts is an internal-only change.

#include "pageros_fonts.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

// Vendored font: public-domain font8x8_basic by Daniel Hepper.
// Each glyph is 8 rows × 8 columns; bit 0 is the leftmost pixel.
#include "font8x8_basic.h"

static const char *TAG = "fonts";

#define BASELINE_HEIGHT  8
#define LINE_HEIGHT      10  // 8 px glyph + 2 px line-gap

static struct {
    bool inited;
    pageros_fonts_stats_t stats;
} g;

// -------- helpers --------------------------------------------------- //

static inline void put_pixel(const pageros_fonts_canvas_t *c, int x, int y, uint16_t rgb)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->pixels[y * c->w + x] = rgb;
}

// Draw an 8x8 glyph from font8x8_basic at (x, y). Each row is one
// byte; bit 0 is the leftmost pixel (font8x8's convention).
static void draw_glyph_8x8(const pageros_fonts_canvas_t *canvas,
                           int x, int y, const char glyph[8], uint16_t fg)
{
    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                put_pixel(canvas, x + col, y + row, fg);
            }
        }
    }
}

// "Missing glyph" — an outlined 6x8 square. Same look as Firefox's
// notdef and most embedded renderers.
static void draw_missing(const pageros_fonts_canvas_t *canvas, int x, int y, uint16_t fg)
{
    int w = 6, h = 8;
    for (int i = 0; i < w; i++) {
        put_pixel(canvas, x + i, y, fg);
        put_pixel(canvas, x + i, y + h - 1, fg);
    }
    for (int j = 0; j < h; j++) {
        put_pixel(canvas, x,         y + j, fg);
        put_pixel(canvas, x + w - 1, y + j, fg);
    }
}

// -------- public API ----------------------------------------------- //

esp_err_t pageros_fonts_init(const pageros_fonts_opts_t *opts)
{
    (void)opts;  // no runtime alloc yet — kept for forward-compat
    if (g.inited) return ESP_OK;
    memset(&g.stats, 0, sizeof(g.stats));
    g.inited = true;
    ESP_LOGI(TAG, "init: 8x8 ASCII fallback bundled (1/1 fonts ready)");
    return ESP_OK;
}

esp_err_t pageros_fonts_shutdown(void)
{
    g.inited = false;
    return ESP_OK;
}

void pageros_fonts_metrics_default(pageros_fonts_metrics_t *out)
{
    if (!out) return;
    out->width   = 8;
    out->height  = LINE_HEIGHT;
    out->advance = 8;
    out->ascent  = BASELINE_HEIGHT;
    out->descent = 2;
}

int pageros_fonts_measure_text(const char *text, int text_len)
{
    if (!text) return 0;
    const char *end = (text_len >= 0) ? (text + text_len) : NULL;
    const char *p = text;
    int width = 0;
    while (true) {
        if (end && p >= end) break;
        if (!end && *p == '\0') break;
        uint32_t cp = pageros_fonts_utf8_next(&p, end);
        if (cp == 0) break;
        width += 8;  // monospace — every codepoint advances 8 px
    }
    return width;
}

int pageros_fonts_draw_codepoint(const pageros_fonts_canvas_t *canvas,
                                 int x, int y,
                                 uint32_t codepoint,
                                 uint16_t fg_rgb565)
{
    if (!canvas || !canvas->pixels) return x;
    if (!g.inited) return x;

    if (codepoint < 0x80) {
        draw_glyph_8x8(canvas, x, y, font8x8_basic[codepoint], fg_rgb565);
        g.stats.hits++;
        return x + 8;
    }
    // Outside ASCII — fallback to missing-glyph for v0. The full Noto
    // bundle plugs in here without changing the public surface.
    draw_missing(canvas, x, y, fg_rgb565);
    g.stats.missing++;
    return x + 8;
}

int pageros_fonts_draw_text(const pageros_fonts_canvas_t *canvas,
                            int x, int y,
                            const char *text, int text_len,
                            uint16_t fg_rgb565,
                            int max_width_px)
{
    if (!canvas || !text) return x;
    const char *end = (text_len >= 0) ? (text + text_len) : NULL;
    const char *p = text;
    int cur_x = x;
    while (true) {
        if (end && p >= end) break;
        if (!end && *p == '\0') break;
        if (max_width_px > 0 && (cur_x - x) + 8 > max_width_px) break;
        uint32_t cp = pageros_fonts_utf8_next(&p, end);
        if (cp == 0) break;
        cur_x = pageros_fonts_draw_codepoint(canvas, cur_x, y, cp, fg_rgb565);
    }
    return cur_x;
}

void pageros_fonts_get_stats(pageros_fonts_stats_t *out)
{
    if (out) *out = g.stats;
}
