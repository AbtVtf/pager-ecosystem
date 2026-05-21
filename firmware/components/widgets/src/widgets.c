// SPDX-License-Identifier: Apache-2.0
//
// FW-020 widget renderer. See pageros_widgets.h.

#include "pageros_widgets.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

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
    size_t slen; const char *s = map_text(w, "s", &slen);
    if (!s) return 10;
    uint16_t color = style_color(&ctx->palette, w);
    // Heading style: render twice with a 1-px x-offset for fake bold.
    size_t style_len; const char *style = map_text(w, "style", &style_len);
    bool heading = style && style_len == 7 && memcmp(style, "heading", 7) == 0;
    int x = 4;
    pageros_fonts_draw_text(&ctx->canvas, x, y, s, (int)slen,
                            color, PAGEROS_WIDGETS_SCREEN_W - 8);
    if (heading) {
        pageros_fonts_draw_text(&ctx->canvas, x + 1, y, s, (int)slen,
                                color, PAGEROS_WIDGETS_SCREEN_W - 8);
    }
    return 12;  // 8 px glyph + 4 px line gap
}

static int draw_button(const pageros_widgets_ctx_t *ctx, int y,
                       const pgr_cbor_value_t *w, bool focused)
{
    size_t llen; const char *label = map_text(w, "label", &llen);
    if (!label) return 0;
    int btn_h = 18;
    int btn_w = 200;
    int x = (PAGEROS_WIDGETS_SCREEN_W - btn_w) / 2;
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
    size_t slen; const char *s = map_text(w, "s", &slen);
    if (!s) return 0;
    size_t llen; const char *lvl = map_text(w, "level", &llen);
    uint16_t bg = ctx->palette.info;
    if (lvl) {
        if (llen == 4 && memcmp(lvl, "warn", 4) == 0) bg = ctx->palette.warn;
        else if (llen == 5 && memcmp(lvl, "error", 5) == 0) bg = ctx->palette.error;
    }
    int h = 14;
    pageros_widgets_fill_rect(&ctx->canvas, 0, y, PAGEROS_WIDGETS_SCREEN_W, h, bg);
    pageros_fonts_draw_text(&ctx->canvas, 6, y + (h - 8) / 2, s, (int)slen,
                            ctx->palette.bg, PAGEROS_WIDGETS_SCREEN_W - 12);
    return h + 2;
}

static int draw_list(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w,
                     int widget_idx)
{
    const pgr_cbor_value_t *items = map_get(w, "items");
    if (!items || items->kind != PGR_CBOR_KIND_ARRAY) return 0;
    int consumed = 0;
    int row_h = 14;
    bool widget_focused = (ctx->focus.widget_index == widget_idx);

    int max_rows = (PAGEROS_WIDGETS_BODY_H - (y - PAGEROS_WIDGETS_BODY_Y)) / row_h;
    int count = (int)items->v.arr.len;
    if (max_rows > count) max_rows = count;

    for (int i = 0; i < max_rows; i++) {
        const pgr_cbor_value_t *it = &items->v.arr.items[i];
        if (it->kind != PGR_CBOR_KIND_MAP) continue;
        bool sel = widget_focused && (ctx->focus.item_index == i);
        if (sel) {
            pageros_widgets_fill_rect(&ctx->canvas, 0, y + consumed,
                                      PAGEROS_WIDGETS_SCREEN_W, row_h,
                                      ctx->palette.accent);
        }
        size_t llen; const char *label = map_text(it, "label", &llen);
        size_t sublen; const char *sub = map_text(it, "sub", &sublen);
        uint16_t fg = sel ? ctx->palette.bg : ctx->palette.fg;
        if (label) {
            // marker on selected row
            if (sel) {
                pageros_fonts_draw_text(&ctx->canvas, 2, y + consumed + 3,
                                        "►", 3, fg, 12);
            }
            pageros_fonts_draw_text(&ctx->canvas, 12, y + consumed + 3,
                                    label, (int)llen, fg,
                                    PAGEROS_WIDGETS_SCREEN_W - 20);
        }
        if (sub) {
            int x_sub = PAGEROS_WIDGETS_SCREEN_W - 8 -
                        pageros_fonts_measure_text(sub, (int)sublen);
            uint16_t sub_fg = sel ? ctx->palette.bg : ctx->palette.dim;
            pageros_fonts_draw_text(&ctx->canvas, x_sub, y + consumed + 3,
                                    sub, (int)sublen, sub_fg, 200);
        }
        consumed += row_h;
    }
    // Hint that there are more rows below
    if (count > max_rows) {
        pageros_fonts_draw_text(&ctx->canvas, PAGEROS_WIDGETS_SCREEN_W / 2 - 8,
                                y + consumed - 2,
                                "▼", 3, ctx->palette.dim, 16);
    }
    return consumed + 2;
}

static int draw_input(const pageros_widgets_ctx_t *ctx, int y,
                      const pgr_cbor_value_t *w, bool focused)
{
    size_t llen; const char *label = map_text(w, "label", &llen);
    size_t vlen; const char *value = map_text(w, "value", &vlen);
    int h = 28;
    if (label) {
        pageros_fonts_draw_text(&ctx->canvas, 4, y, label, (int)llen,
                                ctx->palette.dim, PAGEROS_WIDGETS_SCREEN_W - 8);
    }
    int box_y = y + 10;
    int box_h = 16;
    uint16_t border = focused ? ctx->palette.accent : ctx->palette.fg;
    pageros_widgets_outline_rect(&ctx->canvas, 4, box_y,
                                 PAGEROS_WIDGETS_SCREEN_W - 8, box_h, border);
    if (value) {
        pageros_fonts_draw_text(&ctx->canvas, 8, box_y + (box_h - 8) / 2,
                                value, (int)vlen, ctx->palette.fg,
                                PAGEROS_WIDGETS_SCREEN_W - 16);
    } else if (focused) {
        // blinking cursor placeholder — for now just a vertical bar
        pageros_widgets_fill_rect(&ctx->canvas, 8, box_y + 3, 1, box_h - 6,
                                  ctx->palette.accent);
    }
    return h + 2;
}

static int draw_form(const pageros_widgets_ctx_t *ctx, int y, const pgr_cbor_value_t *w,
                     int widget_idx)
{
    const pgr_cbor_value_t *fields = map_get(w, "fields");
    int cur_y = y;
    bool widget_focused = (ctx->focus.widget_index == widget_idx);
    if (fields && fields->kind == PGR_CBOR_KIND_ARRAY) {
        for (size_t i = 0; i < fields->v.arr.len; i++) {
            bool focused = widget_focused && (ctx->focus.item_index == (int)i);
            cur_y += draw_input(ctx, cur_y, &fields->v.arr.items[i], focused);
        }
    }
    // Submit button at the bottom — focus index = field_count
    size_t submit_len; const char *submit = map_text(w, "submit", &submit_len);
    if (!submit) { submit = "Submit"; submit_len = 6; }
    int submit_idx = fields && fields->kind == PGR_CBOR_KIND_ARRAY
                         ? (int)fields->v.arr.len
                         : 0;
    bool sf = widget_focused && (ctx->focus.item_index == submit_idx);
    // Reuse button draw with a synthetic widget — easier to inline:
    int btn_h = 18, btn_w = 160;
    int bx = (PAGEROS_WIDGETS_SCREEN_W - btn_w) / 2;
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

// ---- top/bottom chrome -------------------------------------------- //

static void draw_topbar(const pageros_widgets_ctx_t *ctx)
{
    pageros_widgets_fill_rect(&ctx->canvas, 0, 0,
                              PAGEROS_WIDGETS_SCREEN_W,
                              PAGEROS_WIDGETS_TOPBAR_H,
                              ctx->palette.topbar_bg);
    if (ctx->title) {
        int len = (int)strlen(ctx->title);
        int w = pageros_fonts_measure_text(ctx->title, len);
        int x = (PAGEROS_WIDGETS_SCREEN_W - w) / 2;
        int y = (PAGEROS_WIDGETS_TOPBAR_H - 8) / 2;
        pageros_fonts_draw_text(&ctx->canvas, x, y, ctx->title, len,
                                ctx->palette.topbar_fg, PAGEROS_WIDGETS_SCREEN_W);
    }
}

static void draw_botbar(const pageros_widgets_ctx_t *ctx)
{
    int y = PAGEROS_WIDGETS_SCREEN_H - PAGEROS_WIDGETS_BOTBAR_H;
    pageros_widgets_fill_rect(&ctx->canvas, 0, y,
                              PAGEROS_WIDGETS_SCREEN_W,
                              PAGEROS_WIDGETS_BOTBAR_H,
                              ctx->palette.topbar_bg);
    if (ctx->help) {
        int len = (int)strlen(ctx->help);
        int ty = y + (PAGEROS_WIDGETS_BOTBAR_H - 8) / 2;
        pageros_fonts_draw_text(&ctx->canvas, 4, ty, ctx->help, len,
                                ctx->palette.topbar_fg,
                                PAGEROS_WIDGETS_SCREEN_W - 8);
    }
}

// ---- public API ---------------------------------------------------- //

esp_err_t pageros_widgets_render_screen(const pageros_widgets_ctx_t *ctx,
                                        const pgr_cbor_value_t *body)
{
    if (!ctx || !ctx->canvas.pixels) return ESP_ERR_INVALID_ARG;

    // Clear body region only — top/bot bars get repainted by the chrome
    // helpers below so we never flash empty space.
    pageros_widgets_fill_rect(&ctx->canvas, 0, PAGEROS_WIDGETS_BODY_Y,
                              PAGEROS_WIDGETS_SCREEN_W,
                              PAGEROS_WIDGETS_BODY_H,
                              ctx->palette.bg);
    draw_topbar(ctx);
    draw_botbar(ctx);

    if (!body || body->kind != PGR_CBOR_KIND_ARRAY) return ESP_OK;

    int y = PAGEROS_WIDGETS_BODY_Y - ctx->focus.scroll;
    for (size_t i = 0; i < body->v.arr.len; i++) {
        const pgr_cbor_value_t *w = &body->v.arr.items[i];
        if (w->kind != PGR_CBOR_KIND_MAP) continue;
        if (y >= PAGEROS_WIDGETS_BODY_Y + PAGEROS_WIDGETS_BODY_H) break;

        bool widget_focused = (ctx->focus.widget_index == (int)i);
        int dy = 0;
        if      (tag_is(w, "text"))         dy = draw_text(ctx, y, w);
        else if (tag_is(w, "button"))       dy = draw_button(ctx, y, w, widget_focused);
        else if (tag_is(w, "notification")) dy = draw_notification(ctx, y, w);
        else if (tag_is(w, "list"))         dy = draw_list(ctx, y, w, (int)i);
        else if (tag_is(w, "input"))        dy = draw_input(ctx, y, w, widget_focused);
        else if (tag_is(w, "form"))         dy = draw_form(ctx, y, w, (int)i);
        else {
            // Unknown widget — render a placeholder per SPEC §5.3.
            char buf[40];
            size_t tlen; const char *t = widget_tag(w, &tlen);
            int n = snprintf(buf, sizeof(buf), "[unsupported: %.*s]",
                             (int)(tlen < 20 ? tlen : 20),
                             t ? t : "?");
            pageros_fonts_draw_text(&ctx->canvas, 4, y, buf, n,
                                    ctx->palette.dim,
                                    PAGEROS_WIDGETS_SCREEN_W - 8);
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
    return 12;
}

// Silence unused-static warnings on minimal builds.
_Static_assert(PAGEROS_WIDGETS_BODY_H > 0, "body must be positive");
__attribute__((unused)) static void _force_keep(void) {
    (void)TAG; (void)map_int;
}
