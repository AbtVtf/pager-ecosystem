// SPDX-License-Identifier: Apache-2.0
//
// FW-020 widget renderer. See pageros_widgets.h.

#include "pageros_widgets.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

// Image widget (FW-021) consumes the image_cache; declared weakly here
// so the renderer compiles even on host builds that don't link it.
// At link time on-device the real symbol from `image_cache` wins.
extern esp_err_t pageros_image_cache_decode(const char *sha256_hex,
                                            uint8_t **out_rgba,
                                            int *out_w, int *out_h)
    __attribute__((weak));

static const char *TAG = "widgets";

// ---- palette ------------------------------------------------------- //

void pageros_widgets_default_palette(pageros_widgets_palette_t *out)
{
    if (!out) return;
    out->bg         = 0x0000;  // black
    out->fg         = 0xFFFF;  // white
    out->dim        = 0x8410;  // mid gray
    out->accent     = 0xFD20;  // orange
    out->info       = 0x07FF;  // cyan
    out->warn       = 0xFFE0;  // yellow
    out->error      = 0xF800;  // red
    out->topbar_bg  = 0x18E3;  // dark slate
    out->topbar_fg  = 0xFFFF;
}

void pageros_widgets_cyberpunk_palette(pageros_widgets_palette_t *out)
{
    if (!out) return;
    // RGB565. Chosen for high contrast on the ST7796 IPS panel with the
    // BGR element order we drive (see pageros_display init).
    out->bg         = 0x0000;  // pure black
    out->fg         = 0xFFFF;  // white body text
    out->dim        = 0x4208;  // dim cyan-gray (unfocused chrome, hairlines)
    out->accent     = 0xF81F;  // neon magenta (focus, activations)
    out->info       = 0x07FF;  // neon cyan (secondary chrome, status ●)
    out->warn       = 0xFFE0;  // neon yellow
    out->error      = 0xF800;  // red
    out->topbar_bg  = 0x0000;  // black chrome — bars are full-bleed
    out->topbar_fg  = 0x07FF;  // cyan text in chrome
}

// ---- viewport helpers -------------------------------------------- //
//
// All widget rendering happens inside a viewport rect. When the ctx
// has vw == 0 we default to the legacy full-screen 480x222 region —
// which is also the full canvas for the LILYGO panel.

typedef struct {
    int x, y, w, h;         // outer viewport (chrome + body)
    int body_y, body_h;     // inner body region (chrome stripped)
} vp_t;

static inline vp_t vp_of(const pageros_widgets_ctx_t *ctx)
{
    vp_t v;
    if (ctx->vw > 0 && ctx->vh > 0) {
        v.x = ctx->vx; v.y = ctx->vy; v.w = ctx->vw; v.h = ctx->vh;
    } else {
        v.x = 0; v.y = 0;
        v.w = PAGEROS_WIDGETS_SCREEN_W;
        v.h = PAGEROS_WIDGETS_SCREEN_H;
    }
    int chrome_top = ctx->skip_chrome ? 0 : PAGEROS_WIDGETS_TOPBAR_H;
    int chrome_bot = ctx->skip_chrome ? 0 : PAGEROS_WIDGETS_BOTBAR_H;
    v.body_y = v.y + chrome_top;
    v.body_h = v.h - chrome_top - chrome_bot;
    if (v.body_h < 0) v.body_h = 0;
    return v;
}

// ---- canvas primitives ------------------------------------------- //

static inline void pix(const pageros_fonts_canvas_t *c, int x, int y, uint16_t rgb)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->pixels[y * c->w + x] = rgb;
}

void pageros_widgets_fill_rect(const pageros_fonts_canvas_t *c,
                               int x, int y, int w, int h, uint16_t rgb)
{
    if (!c || !c->pixels) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = c->pixels + (y + j) * c->w + x;
        for (int i = 0; i < w; i++) row[i] = rgb;
    }
}

void pageros_widgets_outline_rect(const pageros_fonts_canvas_t *c,
                                  int x, int y, int w, int h, uint16_t rgb)
{
    if (w <= 0 || h <= 0) return;
    pageros_widgets_fill_rect(c, x,         y,         w, 1, rgb);
    pageros_widgets_fill_rect(c, x,         y + h - 1, w, 1, rgb);
    pageros_widgets_fill_rect(c, x,         y,         1, h, rgb);
    pageros_widgets_fill_rect(c, x + w - 1, y,         1, h, rgb);
}

// ---- CBOR field helpers ------------------------------------------ //

static const pgr_cbor_value_t *map_get(const pgr_cbor_value_t *map, const char *key)
{
    if (!map || map->kind != PGR_CBOR_KIND_MAP) return NULL;
    size_t klen = strlen(key);
    for (size_t i = 0; i < map->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &map->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == klen &&
            memcmp(p->key.v.bytes.data, key, klen) == 0) {
            return &p->val;
        }
    }
    return NULL;
}

static const char *map_text(const pgr_cbor_value_t *map, const char *key, size_t *out_len)
{
    const pgr_cbor_value_t *v = map_get(map, key);
    if (!v || v->kind != PGR_CBOR_KIND_TEXT) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = v->v.bytes.len;
    return (const char *)v->v.bytes.data;
}

static int64_t map_int(const pgr_cbor_value_t *map, const char *key, int64_t fallback)
{
    const pgr_cbor_value_t *v = map_get(map, key);
    if (!v) return fallback;
    if (v->kind == PGR_CBOR_KIND_UINT) return (int64_t)v->v.u64;
    if (v->kind == PGR_CBOR_KIND_NEGINT) return -(int64_t)v->v.u64 - 1;
    return fallback;
}

static const char *widget_tag(const pgr_cbor_value_t *w, size_t *out_len)
{
    return map_text(w, "t", out_len);
}

static bool tag_is(const pgr_cbor_value_t *w, const char *want)
{
    size_t len; const char *t = widget_tag(w, &len);
    return t && len == strlen(want) && memcmp(t, want, len) == 0;
}

// ---- per-widget renderers ---------------------------------------- //
//
// Each returns the y-cursor delta (px consumed in the body).

static uint16_t style_color(const pageros_widgets_palette_t *pal,
                            const pgr_cbor_value_t *widget)
{
    size_t len; const char *style = map_text(widget, "style", &len);
    if (style && len == 4 && memcmp(style, "heading", 4) == 0) return pal->fg;  // bold == fg; same color
    if (style && len == 3 && memcmp(style, "dim", 3) == 0)     return pal->dim;
    if (style && len == 4 && memcmp(style, "mono", 4) == 0)    return pal->fg;
    return pal->fg;
}

static int draw_text(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    size_t slen; const char *s = map_text(w, "s", &slen);
    if (!s) return 10;
    uint16_t color = style_color(&ctx->palette, w);
    size_t style_len; const char *style = map_text(w, "style", &style_len);
    bool heading = style && style_len == 7 && memcmp(style, "heading", 7) == 0;
    int x = vp.x + 4;
    pageros_fonts_draw_text(&ctx->canvas, x, y, s, (int)slen,
                            color, vp.w - 8);
    if (heading) {
        pageros_fonts_draw_text(&ctx->canvas, x + 1, y, s, (int)slen,
                                color, vp.w - 8);
    }
    return 12;
}

static int draw_button(const pageros_widgets_ctx_t *ctx, int y,
                       const pgr_cbor_value_t *w, bool focused)
{
    vp_t vp = vp_of(ctx);
    size_t llen; const char *label = map_text(w, "label", &llen);
    if (!label) return 0;
    int btn_h = 18;
    int btn_w = vp.w - 40;
    if (btn_w > 200) btn_w = 200;
    int x = vp.x + (vp.w - btn_w) / 2;
    uint16_t border = focused ? ctx->palette.accent : ctx->palette.fg;
    if (focused) {
        pageros_widgets_fill_rect(&ctx->canvas, x, y, btn_w, btn_h, ctx->palette.accent);
    }
    pageros_widgets_outline_rect(&ctx->canvas, x, y, btn_w, btn_h, border);
    int text_w = pageros_fonts_measure_text(label, (int)llen);
    int tx = x + (btn_w - text_w) / 2;
    int ty = y + (btn_h - 8) / 2;
    pageros_fonts_draw_text(&ctx->canvas, tx, ty, label, (int)llen,
                            focused ? ctx->palette.bg : ctx->palette.fg, btn_w - 8);
    return btn_h + 4;
}

static int draw_notification(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    size_t slen; const char *s = map_text(w, "s", &slen);
    if (!s) return 0;
    size_t llen; const char *lvl = map_text(w, "level", &llen);
    uint16_t bg = ctx->palette.info;
    if (lvl) {
        if (llen == 4 && memcmp(lvl, "warn", 4) == 0) bg = ctx->palette.warn;
        else if (llen == 5 && memcmp(lvl, "error", 5) == 0) bg = ctx->palette.error;
    }
    int h = 14;
    pageros_widgets_fill_rect(&ctx->canvas, vp.x, y, vp.w, h, bg);
    pageros_fonts_draw_text(&ctx->canvas, vp.x + 6, y + (h - 8) / 2, s, (int)slen,
                            ctx->palette.bg, vp.w - 12);
    return h + 2;
}

static int draw_list(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w,
                     int widget_idx)
{
    vp_t vp = vp_of(ctx);
    const pgr_cbor_value_t *items = map_get(w, "items");
    if (!items || items->kind != PGR_CBOR_KIND_ARRAY) return 0;
    int consumed = 0;
    int row_h = 14;
    bool widget_focused = (ctx->focus.widget_index == widget_idx);

    int max_rows = (vp.body_h - (y - vp.body_y)) / row_h;
    int count = (int)items->v.arr.len;
    if (max_rows > count) max_rows = count;

    for (int i = 0; i < max_rows; i++) {
        const pgr_cbor_value_t *it = &items->v.arr.items[i];
        if (it->kind != PGR_CBOR_KIND_MAP) continue;
        bool sel = widget_focused && (ctx->focus.item_index == i);
        if (sel) {
            pageros_widgets_fill_rect(&ctx->canvas, vp.x, y + consumed,
                                      vp.w, row_h,
                                      ctx->palette.accent);
        }
        size_t llen; const char *label = map_text(it, "label", &llen);
        size_t sublen; const char *sub = map_text(it, "sub", &sublen);
        uint16_t fg = sel ? ctx->palette.bg : ctx->palette.fg;
        if (label) {
            if (sel) {
                pageros_fonts_draw_text(&ctx->canvas, vp.x + 2, y + consumed + 3,
                                        "►", 3, fg, 12);
            }
            pageros_fonts_draw_text(&ctx->canvas, vp.x + 12, y + consumed + 3,
                                    label, (int)llen, fg,
                                    vp.w - 20);
        }
        if (sub) {
            int x_sub = vp.x + vp.w - 8 -
                        pageros_fonts_measure_text(sub, (int)sublen);
            uint16_t sub_fg = sel ? ctx->palette.bg : ctx->palette.dim;
            pageros_fonts_draw_text(&ctx->canvas, x_sub, y + consumed + 3,
                                    sub, (int)sublen, sub_fg, 200);
        }
        consumed += row_h;
    }
    if (count > max_rows) {
        pageros_fonts_draw_text(&ctx->canvas, vp.x + vp.w / 2 - 8,
                                y + consumed - 2,
                                "▼", 3, ctx->palette.dim, 16);
    }
    return consumed + 2;
}

static int draw_input(const pageros_widgets_ctx_t *ctx, int y,
                      const pgr_cbor_value_t *w, bool focused)
{
    vp_t vp = vp_of(ctx);
    size_t llen; const char *label = map_text(w, "label", &llen);
    size_t vlen; const char *value = map_text(w, "value", &vlen);
    int h = 28;
    if (label) {
        pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, y, label, (int)llen,
                                ctx->palette.dim, vp.w - 8);
    }
    int box_y = y + 10;
    int box_h = 16;
    uint16_t border = focused ? ctx->palette.accent : ctx->palette.fg;
    pageros_widgets_outline_rect(&ctx->canvas, vp.x + 4, box_y,
                                 vp.w - 8, box_h, border);
    if (value) {
        pageros_fonts_draw_text(&ctx->canvas, vp.x + 8, box_y + (box_h - 8) / 2,
                                value, (int)vlen, ctx->palette.fg,
                                vp.w - 16);
    } else if (focused) {
        pageros_widgets_fill_rect(&ctx->canvas, vp.x + 8, box_y + 3, 1, box_h - 6,
                                  ctx->palette.accent);
    }
    return h + 2;
}

static int draw_form(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w,
                     int widget_idx)
{
    vp_t vp = vp_of(ctx);
    const pgr_cbor_value_t *fields = map_get(w, "fields");
    int cur_y = y;
    bool widget_focused = (ctx->focus.widget_index == widget_idx);
    if (fields && fields->kind == PGR_CBOR_KIND_ARRAY) {
        for (size_t i = 0; i < fields->v.arr.len; i++) {
            bool focused = widget_focused && (ctx->focus.item_index == (int)i);
            cur_y += draw_input(ctx, cur_y, &fields->v.arr.items[i], focused);
        }
    }
    size_t submit_len; const char *submit = map_text(w, "submit", &submit_len);
    if (!submit) { submit = "Submit"; submit_len = 6; }
    int submit_idx = fields && fields->kind == PGR_CBOR_KIND_ARRAY
                         ? (int)fields->v.arr.len
                         : 0;
    bool sf = widget_focused && (ctx->focus.item_index == submit_idx);
    int btn_h = 18, btn_w = 160;
    if (btn_w > vp.w - 40) btn_w = vp.w - 40;
    int bx = vp.x + (vp.w - btn_w) / 2;
    if (sf) pageros_widgets_fill_rect(&ctx->canvas, bx, cur_y, btn_w, btn_h, ctx->palette.accent);
    pageros_widgets_outline_rect(&ctx->canvas, bx, cur_y, btn_w, btn_h,
                                 sf ? ctx->palette.accent : ctx->palette.fg);
    int tw = pageros_fonts_measure_text(submit, (int)submit_len);
    pageros_fonts_draw_text(&ctx->canvas, bx + (btn_w - tw) / 2, cur_y + (btn_h - 8) / 2,
                            submit, (int)submit_len,
                            sf ? ctx->palette.bg : ctx->palette.fg, btn_w - 8);
    cur_y += btn_h + 4;
    return cur_y - y;
}

// ---- image widget (FW-021) ---------------------------------------- //
//
// src is "img:<sha256_hex>"; we look it up in the image cache and blit
// the decoded RGB565 into the framebuffer. Cache misses render a
// labelled placeholder so the screen doesn't reflow when the image
// later arrives.

static inline uint16_t rgba_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int draw_image(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    size_t slen; const char *src = map_text(w, "src", &slen);
    int target_w = (int)map_int(w, "w", 96);
    int target_h = (int)map_int(w, "h", 96);
    if (target_w <= 0 || target_h <= 0 || target_w > vp.w) {
        target_w = 96; target_h = 96;
    }
    int x = vp.x + (vp.w - target_w) / 2;

    // Empty / non-img: placeholder.
    if (!src || slen < 5 || memcmp(src, "img:", 4) != 0) {
        pageros_widgets_outline_rect(&ctx->canvas, x, y, target_w, target_h, ctx->palette.dim);
        return target_h + 4;
    }
    char hex[65];
    size_t hex_len = slen - 4;
    if (hex_len > 64) hex_len = 64;
    memcpy(hex, src + 4, hex_len);
    hex[hex_len] = '\0';

    if (!pageros_image_cache_decode || hex_len != 64) {
        pageros_widgets_outline_rect(&ctx->canvas, x, y, target_w, target_h, ctx->palette.dim);
        pageros_fonts_draw_text(&ctx->canvas, x + 4, y + target_h / 2 - 4,
                                "[img]", 5, ctx->palette.dim, target_w - 8);
        return target_h + 4;
    }
    uint8_t *rgba = NULL; int iw = 0, ih = 0;
    esp_err_t r = pageros_image_cache_decode(hex, &rgba, &iw, &ih);
    if (r != ESP_OK || !rgba) {
        pageros_widgets_outline_rect(&ctx->canvas, x, y, target_w, target_h, ctx->palette.dim);
        pageros_fonts_draw_text(&ctx->canvas, x + 4, y + target_h / 2 - 4,
                                "[load]", 6, ctx->palette.dim, target_w - 8);
        if (rgba) free(rgba);
        return target_h + 4;
    }

    // Simple nearest-neighbour blit, clamped to the declared box.
    int draw_w = iw < target_w ? iw : target_w;
    int draw_h = ih < target_h ? ih : target_h;
    int ox = x + (target_w - draw_w) / 2;
    int oy = y + (target_h - draw_h) / 2;
    for (int j = 0; j < draw_h; j++) {
        for (int i = 0; i < draw_w; i++) {
            const uint8_t *px = rgba + (j * iw + i) * 4;
            if (px[3] < 0x20) continue;  // skip transparent
            pix(&ctx->canvas, ox + i, oy + j, rgba_to_565(px[0], px[1], px[2]));
        }
    }
    free(rgba);
    return target_h + 4;
}

// ---- map widget (FW-022) v0 --------------------------------------- //
//
// Renders a placeholder map background plus a centered GPS marker.
// Real OSM tile fetching + rendering lives behind the SD tile cache
// (see firmware/components/storage's /cache/tiles/) and the network
// HTTPS client; the wiring is staged for a follow-up so the widget
// renderer can ship now without an OSM dependency at compile time.

static int draw_map(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    int box_h = 96;
    int box_y = y;
    pageros_widgets_fill_rect(&ctx->canvas, vp.x, box_y, vp.w, box_h,
                              ctx->palette.bg);
    for (int gy = box_y; gy < box_y + box_h; gy += 16) {
        pageros_widgets_fill_rect(&ctx->canvas, vp.x, gy, vp.w, 1,
                                  ctx->palette.dim);
    }
    for (int gx = 0; gx < vp.w; gx += 16) {
        pageros_widgets_fill_rect(&ctx->canvas, vp.x + gx, box_y, 1, box_h,
                                  ctx->palette.dim);
    }

    int cx = vp.x + vp.w / 2;
    int cy = box_y + box_h / 2;
    for (int j = -3; j <= 3; j++) {
        for (int i = -3; i <= 3; i++) {
            if (i * i + j * j <= 9) pix(&ctx->canvas, cx + i, cy + j, ctx->palette.accent);
        }
    }
    pageros_widgets_outline_rect(&ctx->canvas, cx - 6, cy - 6, 13, 13, ctx->palette.fg);

    const pgr_cbor_value_t *lat = map_get(w, "lat");
    const pgr_cbor_value_t *lon = map_get(w, "lon");
    char caption[64];
    int n = -1;
    if (lat && lon && lat->kind == PGR_CBOR_KIND_FLOAT && lon->kind == PGR_CBOR_KIND_FLOAT) {
        n = snprintf(caption, sizeof(caption), "%.4f, %.4f", lat->v.dbl, lon->v.dbl);
    } else {
        n = snprintf(caption, sizeof(caption), "no fix");
    }
    pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, box_y + box_h - 10, caption, n,
                            ctx->palette.dim, vp.w - 8);
    return box_h + 4;
}

// ---- presence_list (FW-023) --------------------------------------- //

static int draw_presence(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    const pgr_cbor_value_t *members = map_get(w, "members");
    if (!members || members->kind != PGR_CBOR_KIND_ARRAY) return 0;
    int consumed = 0;
    int row_h = 12;
    int max_rows = 6;
    int count = (int)members->v.arr.len;
    if (count > max_rows) count = max_rows;
    pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, y, "Presence", 8, ctx->palette.dim, 100);
    consumed += 10;
    for (int i = 0; i < count; i++) {
        const pgr_cbor_value_t *m = &members->v.arr.items[i];
        if (m->kind != PGR_CBOR_KIND_MAP) continue;
        size_t nlen; const char *name = map_text(m, "name", &nlen);
        const pgr_cbor_value_t *online_v = map_get(m, "online");
        bool online = online_v && online_v->kind == PGR_CBOR_KIND_BOOL && online_v->v.boolean;
        if (online) {
            pageros_widgets_fill_rect(&ctx->canvas, vp.x + 6, y + consumed + 2, 6, 6, ctx->palette.accent);
        } else {
            pageros_widgets_outline_rect(&ctx->canvas, vp.x + 6, y + consumed + 2, 6, 6, ctx->palette.dim);
        }
        if (name) {
            pageros_fonts_draw_text(&ctx->canvas, vp.x + 16, y + consumed + 1,
                                    name, (int)nlen, ctx->palette.fg, vp.w - 24);
        }
        consumed += row_h;
    }
    return consumed + 4;
}

// ---- chat (FW-023) ------------------------------------------------ //

static int draw_chat(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w)
{
    vp_t vp = vp_of(ctx);
    const pgr_cbor_value_t *msgs = map_get(w, "messages");
    int avail = vp.body_y + vp.body_h - y - 22;
    int row_h = 12;
    int max_rows = avail / row_h;
    int total = (msgs && msgs->kind == PGR_CBOR_KIND_ARRAY) ? (int)msgs->v.arr.len : 0;
    int start = total > max_rows ? total - max_rows : 0;
    int cy = y;
    for (int i = start; i < total; i++) {
        const pgr_cbor_value_t *m = &msgs->v.arr.items[i];
        if (m->kind != PGR_CBOR_KIND_MAP) continue;
        size_t flen; const char *from = map_text(m, "from", &flen);
        size_t slen; const char *s = map_text(m, "s", &slen);
        char line[160];
        int n = snprintf(line, sizeof(line), "%.*s: %.*s",
                         (int)(flen < 16 ? flen : 16), from ? from : "?",
                         (int)(slen < 80 ? slen : 80), s ? s : "");
        if (n < 0) n = 0;
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, cy, line, n,
                                ctx->palette.fg, vp.w - 8);
        cy += row_h;
    }
    int composer_y = y + avail + 4;
    pageros_widgets_outline_rect(&ctx->canvas, vp.x + 4, composer_y,
                                 vp.w - 8, 14, ctx->palette.dim);
    pageros_fonts_draw_text(&ctx->canvas, vp.x + 8, composer_y + 3, "Type…", 5,
                            ctx->palette.dim, vp.w - 16);
    return composer_y + 18 - y;
}

// ---- top/bottom chrome -------------------------------------------- //

static void draw_topbar(const pageros_widgets_ctx_t *ctx)
{
    vp_t vp = vp_of(ctx);
    pageros_widgets_fill_rect(&ctx->canvas, vp.x, vp.y,
                              vp.w, PAGEROS_WIDGETS_TOPBAR_H,
                              ctx->palette.topbar_bg);
    if (ctx->title) {
        int len = (int)strlen(ctx->title);
        int w = pageros_fonts_measure_text(ctx->title, len);
        int x = vp.x + (vp.w - w) / 2;
        int y = vp.y + (PAGEROS_WIDGETS_TOPBAR_H - 8) / 2;
        pageros_fonts_draw_text(&ctx->canvas, x, y, ctx->title, len,
                                ctx->palette.topbar_fg, vp.w);
    }
}

static void draw_botbar(const pageros_widgets_ctx_t *ctx)
{
    vp_t vp = vp_of(ctx);
    int y = vp.y + vp.h - PAGEROS_WIDGETS_BOTBAR_H;
    pageros_widgets_fill_rect(&ctx->canvas, vp.x, y,
                              vp.w, PAGEROS_WIDGETS_BOTBAR_H,
                              ctx->palette.topbar_bg);
    if (ctx->help) {
        int len = (int)strlen(ctx->help);
        int ty = y + (PAGEROS_WIDGETS_BOTBAR_H - 8) / 2;
        pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, ty, ctx->help, len,
                                ctx->palette.topbar_fg,
                                vp.w - 8);
    }
}

// ---- public API ---------------------------------------------------- //

esp_err_t pageros_widgets_render_screen(const pageros_widgets_ctx_t *ctx,
                                        const pgr_cbor_value_t *body)
{
    if (!ctx || !ctx->canvas.pixels) return ESP_ERR_INVALID_ARG;
    vp_t vp = vp_of(ctx);

    // Clear body region only — the chrome (if any) repaints its strip.
    pageros_widgets_fill_rect(&ctx->canvas, vp.x, vp.body_y,
                              vp.w, vp.body_h,
                              ctx->palette.bg);
    if (!ctx->skip_chrome) {
        draw_topbar(ctx);
        draw_botbar(ctx);
    }

    if (!body || body->kind != PGR_CBOR_KIND_ARRAY) return ESP_OK;

    int y = vp.body_y - ctx->focus.scroll;
    for (size_t i = 0; i < body->v.arr.len; i++) {
        const pgr_cbor_value_t *w = &body->v.arr.items[i];
        if (w->kind != PGR_CBOR_KIND_MAP) continue;
        if (y >= vp.body_y + vp.body_h) break;

        bool widget_focused = (ctx->focus.widget_index == (int)i);
        int dy = 0;
        if      (tag_is(w, "text"))         dy = draw_text(ctx, y, w);
        else if (tag_is(w, "button"))       dy = draw_button(ctx, y, w, widget_focused);
        else if (tag_is(w, "notification")) dy = draw_notification(ctx, y, w);
        else if (tag_is(w, "list"))         dy = draw_list(ctx, y, w, (int)i);
        else if (tag_is(w, "input"))        dy = draw_input(ctx, y, w, widget_focused);
        else if (tag_is(w, "form"))         dy = draw_form(ctx, y, w, (int)i);
        else if (tag_is(w, "image"))        dy = draw_image(ctx, y, w);
        else if (tag_is(w, "map"))          dy = draw_map(ctx, y, w);
        else if (tag_is(w, "presence_list"))dy = draw_presence(ctx, y, w);
        else if (tag_is(w, "chat"))         dy = draw_chat(ctx, y, w);
        else {
            char buf[40];
            size_t tlen; const char *t = widget_tag(w, &tlen);
            int n = snprintf(buf, sizeof(buf), "[unsupported: %.*s]",
                             (int)(tlen < 20 ? tlen : 20),
                             t ? t : "?");
            pageros_fonts_draw_text(&ctx->canvas, vp.x + 4, y, buf, n,
                                    ctx->palette.dim,
                                    vp.w - 8);
            dy = 12;
        }
        y += dy;
    }
    return ESP_OK;
}

int pageros_widgets_measure(const pgr_cbor_value_t *w)
{
    if (!w || w->kind != PGR_CBOR_KIND_MAP) return 0;
    if (tag_is(w, "text")) return 12;
    if (tag_is(w, "button")) return 22;
    if (tag_is(w, "notification")) return 16;
    if (tag_is(w, "input")) return 30;
    if (tag_is(w, "list")) {
        const pgr_cbor_value_t *items = map_get(w, "items");
        int n = (items && items->kind == PGR_CBOR_KIND_ARRAY) ? (int)items->v.arr.len : 0;
        if (n > 12) n = 12;
        return n * 14 + 4;
    }
    if (tag_is(w, "form")) {
        const pgr_cbor_value_t *fields = map_get(w, "fields");
        int n = (fields && fields->kind == PGR_CBOR_KIND_ARRAY) ? (int)fields->v.arr.len : 0;
        return n * 30 + 22 + 4;
    }
    if (tag_is(w, "image")) {
        int h = (int)map_int(w, "h", 96);
        return h + 4;
    }
    if (tag_is(w, "map")) return 96 + 4;
    if (tag_is(w, "presence_list")) {
        const pgr_cbor_value_t *m = map_get(w, "members");
        int n = (m && m->kind == PGR_CBOR_KIND_ARRAY) ? (int)m->v.arr.len : 0;
        if (n > 6) n = 6;
        return 10 + n * 12 + 4;
    }
    if (tag_is(w, "chat")) return PAGEROS_WIDGETS_BODY_H;  // takes remaining body
    return 12;
}

// ---- desktop chrome ---------------------------------------------- //

static void chrome_draw_indicator(const pageros_fonts_canvas_t *c,
                                  int x, int y, const char *label,
                                  int state, uint16_t on_color,
                                  uint16_t off_color, uint16_t fg_color)
{
    // "LABEL●" — fixed-width label + dot. Used by botbar.
    int lx = pageros_fonts_draw_text(c, x, y + 4, label, -1, fg_color, 80);
    uint16_t dot = state ? on_color : off_color;
    // Filled 8x8 circle-ish (just a 5x5 block, cheap).
    for (int j = 1; j <= 5; j++) {
        for (int i = 1; i <= 5; i++) {
            pix(c, lx + i, y + 4 + j, dot);
        }
    }
}

void pageros_widgets_chrome_topbar(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   const pageros_chrome_state_t *state)
{
    if (!canvas || !pal) return;
    int w = canvas->w;
    pageros_widgets_fill_rect(canvas, 0, 0, w, PAGEROS_CHROME_BAR_H, pal->bg);
    // Thin underline in cyan to delimit the bar from the body.
    pageros_widgets_fill_rect(canvas, 0, PAGEROS_CHROME_BAR_H - 1, w, 1, pal->info);

    // Left: HH:MM (or --:-- if unknown)
    char clock[8];
    if (state && state->hh >= 0 && state->mm >= 0) {
        int hh = state->hh % 100;
        int mm = state->mm % 100;
        snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }
    pageros_fonts_draw_text(canvas, 4, 4, clock, -1, pal->info, 80);

    // Mid: PAGEROS title (always)
    const char *title = "PAGEROS";
    int title_w = pageros_fonts_measure_text(title, -1);
    pageros_fonts_draw_text(canvas, (w - title_w) / 2, 4, title, -1,
                            pal->info, w);

    // Right: identity hash
    if (state && state->identity_short) {
        int id_w = pageros_fonts_measure_text(state->identity_short, -1);
        pageros_fonts_draw_text(canvas, w - id_w - 4, 4,
                                state->identity_short, -1,
                                pal->dim, 200);
    }
}

void pageros_widgets_chrome_botbar(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   const pageros_chrome_state_t *state)
{
    if (!canvas || !pal) return;
    int w = canvas->w;
    int h = canvas->h;
    int y = h - PAGEROS_CHROME_BAR_H;
    pageros_widgets_fill_rect(canvas, 0, y, w, PAGEROS_CHROME_BAR_H, pal->bg);
    pageros_widgets_fill_rect(canvas, 0, y, w, 1, pal->info);

    // Battery: BAT▓▓▓░
    char bat[16];
    int pct = state ? state->battery_pct : -1;
    int bars = (pct < 0) ? 0 : (pct >= 75 ? 4 : pct >= 50 ? 3 : pct >= 25 ? 2 : pct > 0 ? 1 : 0);
    // 4-cell block bar, filled vs empty.
    snprintf(bat, sizeof(bat), "BAT");
    int x = 4;
    x = pageros_fonts_draw_text(canvas, x, y + 4, bat, -1, pal->info, 40);
    // Draw 4 small cells.
    for (int i = 0; i < 4; i++) {
        uint16_t col = (i < bars) ? pal->info : pal->dim;
        pageros_widgets_fill_rect(canvas, x + 1 + i * 6, y + 5, 4, 6, col);
    }
    x += 4 * 6 + 8;

    // WIFI ●
    int wifi = state ? state->wifi_state : 0;
    chrome_draw_indicator(canvas, x, y, "WIFI", wifi >= 2, pal->info, pal->dim, pal->info);
    x += 50;

    // LORA ●
    int lora = state ? state->lora_state : 0;
    chrome_draw_indicator(canvas, x, y, "LORA", lora >= 1, pal->info, pal->dim, pal->info);
    x += 50;

    // GPS ●
    int gps = state ? state->gps_state : 0;
    chrome_draw_indicator(canvas, x, y, "GPS", gps >= 1, pal->info, pal->dim, pal->info);
    x += 42;

    // PWR:MODE on the right edge.
    if (state && state->power_mode) {
        char pm[16];
        snprintf(pm, sizeof(pm), "PWR:%s", state->power_mode);
        int pmw = pageros_fonts_measure_text(pm, -1);
        pageros_fonts_draw_text(canvas, w - pmw - 4, y + 4, pm, -1,
                                pal->dim, 120);
    }
}

void pageros_widgets_chrome_rail(const pageros_fonts_canvas_t *canvas,
                                 const pageros_widgets_palette_t *pal,
                                 const pageros_chrome_rail_t *rail)
{
    if (!canvas || !pal || !rail) return;
    int rail_x = 0;
    int rail_y = PAGEROS_CHROME_BAR_H;
    int rail_w = PAGEROS_CHROME_RAIL_W;
    int rail_h = canvas->h - 2 * PAGEROS_CHROME_BAR_H;
    pageros_widgets_fill_rect(canvas, rail_x, rail_y, rail_w, rail_h, pal->bg);
    // Vertical divider on the right edge.
    pageros_widgets_fill_rect(canvas, rail_x + rail_w - 1, rail_y, 1, rail_h, pal->info);

    int n = 1 + (rail->n_items > 0 ? rail->n_items : 0);
    if (n > 1 + PAGEROS_CHROME_RAIL_MAX_APPS) n = 1 + PAGEROS_CHROME_RAIL_MAX_APPS;

    int row_h = 24;
    int total_h = n * row_h;
    int start_y = rail_y + (rail_h - total_h) / 2;
    if (start_y < rail_y + 4) start_y = rail_y + 4;

    for (int i = 0; i < n; i++) {
        int row_y = start_y + i * row_h;
        bool focused = (rail->focus_index == i);
        uint16_t border = focused
                              ? (rail->rail_active ? pal->accent : pal->info)
                              : pal->dim;

        if (focused) {
            // Filled background tint.
            uint16_t tint = rail->rail_active ? pal->accent : pal->info;
            // Composite alpha-ish via a darker variant — we don't have
            // blend so just outline + small fill chunk.
            pageros_widgets_outline_rect(canvas, rail_x + 4, row_y,
                                         rail_w - 10, row_h - 4, border);
            // Left-edge accent strip.
            pageros_widgets_fill_rect(canvas, rail_x + 2, row_y, 2, row_h - 4, tint);
        }

        const char *label = (i == 0) ? "HOME" : rail->items[i - 1];
        if (!label) label = "?";
        // Truncate name to fit ~4 chars at 16x16.
        char short_label[6];
        size_t L = strlen(label);
        size_t copy = L > 5 ? 5 : L;
        memcpy(short_label, label, copy);
        short_label[copy] = '\0';
        // Upper-case for cyberpunk feel — many labels are mixed case.
        for (size_t k = 0; k < copy; k++) {
            if (short_label[k] >= 'a' && short_label[k] <= 'z') {
                short_label[k] = (char)(short_label[k] - 32);
            }
        }
        uint16_t label_color = focused ? pal->fg : pal->info;
        int label_w = pageros_fonts_measure_text_16(short_label, -1);
        int label_x = rail_x + (rail_w - 4 - label_w) / 2;
        if (label_x < rail_x + 6) label_x = rail_x + 6;
        int label_y = row_y + (row_h - 4 - 16) / 2;
        pageros_fonts_draw_text_16(canvas, label_x, label_y,
                                   short_label, -1, label_color,
                                   rail_w - 10);
    }
}

void pageros_widgets_chrome_tile_grid(const pageros_fonts_canvas_t *canvas,
                                      const pageros_widgets_palette_t *pal,
                                      const pageros_chrome_tile_t *tiles,
                                      int n_tiles,
                                      int focus_index,
                                      int x, int y, int w, int h)
{
    if (!canvas || !pal || !tiles) return;
    pageros_widgets_fill_rect(canvas, x, y, w, h, pal->bg);
    if (n_tiles <= 0) {
        const char *msg = "NO APPS";
        int mw = pageros_fonts_measure_text_16(msg, -1);
        pageros_fonts_draw_text_16(canvas, x + (w - mw) / 2,
                                   y + (h - 16) / 2, msg, -1,
                                   pal->dim, w);
        return;
    }

    // 3 columns × 2 rows = 6 tiles. Adjust if fewer.
    int cols = 3;
    int rows = (n_tiles + cols - 1) / cols;
    if (rows > 2) rows = 2;
    int total = cols * rows;
    if (total > n_tiles) total = n_tiles;

    int pad = 8;
    int tile_w = (w - pad * (cols + 1)) / cols;
    int tile_h = (h - pad * (rows + 1)) / rows;

    for (int i = 0; i < total; i++) {
        int r = i / cols;
        int c = i % cols;
        int tx = x + pad + c * (tile_w + pad);
        int ty = y + pad + r * (tile_h + pad);
        bool focused = (i == focus_index);
        uint16_t border = focused ? pal->accent : pal->info;
        // Tile outline
        pageros_widgets_outline_rect(canvas, tx, ty, tile_w, tile_h, border);
        if (focused) {
            // Inner highlight ring.
            pageros_widgets_outline_rect(canvas, tx + 2, ty + 2,
                                         tile_w - 4, tile_h - 4, border);
        }
        // Tile name centered, 16x16 font.
        const char *name = tiles[i].name ? tiles[i].name : "?";
        // Upper-case the name for the chunky look.
        char buf[16];
        size_t nl = strlen(name);
        if (nl > sizeof(buf) - 1) nl = sizeof(buf) - 1;
        for (size_t k = 0; k < nl; k++) {
            char ch = name[k];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            buf[k] = ch;
        }
        buf[nl] = '\0';
        int nw = pageros_fonts_measure_text_16(buf, -1);
        int nx = tx + (tile_w - nw) / 2;
        int ny = ty + (tile_h - 16) / 2;
        pageros_fonts_draw_text_16(canvas, nx, ny, buf, -1,
                                   focused ? pal->fg : pal->info,
                                   tile_w - 8);
        // Unread badge in the top-right corner if non-zero.
        if (tiles[i].unread > 0) {
            char badge[8];
            int n = tiles[i].unread;
            if (n > 99) n = 99;
            snprintf(badge, sizeof(badge), "[%d]", n);
            int bw = pageros_fonts_measure_text(badge, -1);
            pageros_fonts_draw_text(canvas, tx + tile_w - bw - 4, ty + 4,
                                    badge, -1, pal->accent, tile_w);
        }
    }
}

void pageros_widgets_chrome_scanlines(const pageros_fonts_canvas_t *canvas,
                                      int x, int y, int w, int h,
                                      int every_n_rows)
{
    if (!canvas || !canvas->pixels || every_n_rows <= 1) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas->w) w = canvas->w - x;
    if (y + h > canvas->h) h = canvas->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j += every_n_rows) {
        uint16_t *row = canvas->pixels + j * canvas->w + x;
        for (int i = 0; i < w; i++) {
            uint16_t p = row[i];
            // Halve each RGB channel — cheap CRT scanline tint.
            uint16_t r = (p >> 11) & 0x1F;
            uint16_t g = (p >> 5)  & 0x3F;
            uint16_t b = p         & 0x1F;
            row[i] = (uint16_t)(((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1));
        }
    }
}

_Static_assert(PAGEROS_WIDGETS_BODY_H > 0, "body must be positive");
__attribute__((unused)) static void _force_keep(void) {
    (void)TAG; (void)map_int; (void)vp_of;
}
