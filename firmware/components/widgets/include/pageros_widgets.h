// SPDX-License-Identifier: Apache-2.0
//
// PagerOS widget renderer — FW-020 .. FW-023.
//
// Walks a decoded Frame (a `pgr_cbor_value_t *` tree per FW-016) and
// draws every widget into an RGB565 canvas. Each widget type in
// SPEC §5.3 has a renderer:
//
//   FW-020:  text · list · input · form · button · notification
//   FW-021:  image       (delegates to FW-018 image_cache for decode)
//   FW-022:  map         (renders cached OSM tiles + GPS marker)
//   FW-023:  presence_list · chat
//
// Layout model (v0):
//
//   - Screen is 480x222. The renderer reserves a 14 px top bar for the
//     Frame's `title` (centered) and a 12 px bottom bar for the
//     status/help line. The body region is 480x196 starting at y=14.
//
//   - Body is a vertical stack of widgets. Each widget gets its
//     natural height; the renderer skips remaining widgets if the body
//     region is full. Per-widget scrolling (long lists) is owned by the
//     widget itself via a `pageros_widgets_set_scroll` knob.
//
//   - The renderer is stateless w.r.t. focus — callers pass the
//     currently focused widget+item index in. Drawing is deterministic
//     given the same (frame, focus, scroll) triple.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pageros_cbor.h"
#include "pageros_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_WIDGETS_SCREEN_W      480
#define PAGEROS_WIDGETS_SCREEN_H      222
#define PAGEROS_WIDGETS_TOPBAR_H       14
#define PAGEROS_WIDGETS_BOTBAR_H       12
#define PAGEROS_WIDGETS_BODY_Y        PAGEROS_WIDGETS_TOPBAR_H
#define PAGEROS_WIDGETS_BODY_H        (PAGEROS_WIDGETS_SCREEN_H - \
                                       PAGEROS_WIDGETS_TOPBAR_H - \
                                       PAGEROS_WIDGETS_BOTBAR_H)

// One palette per render — themed once at boot from the user's settings.
typedef struct {
    uint16_t bg;
    uint16_t fg;
    uint16_t dim;
    uint16_t accent;
    uint16_t info;
    uint16_t warn;
    uint16_t error;
    uint16_t topbar_bg;
    uint16_t topbar_fg;
} pageros_widgets_palette_t;

// Sensible default palette: dark theme, white text, orange accent.
void pageros_widgets_default_palette(pageros_widgets_palette_t *out);

// Cyberpunk palette: black bg, magenta accent (focus / activations), cyan
// secondary (links, status indicators), dim cyan for unfocused chrome.
// Used by the desktop shell to give the system its Blade Runner look.
void pageros_widgets_cyberpunk_palette(pageros_widgets_palette_t *out);

// Focus state passed to every render call. The renderer doesn't own
// focus — the input router (FW-024) does — but it needs to draw the
// highlight in the right place.
typedef struct {
    int widget_index;     // index into Frame.body that owns the focus
    int item_index;       // index within that widget (list rows, form fields)
    int scroll;           // body-region vertical scroll offset, in px
} pageros_widgets_focus_t;

typedef struct {
    pageros_fonts_canvas_t canvas;            // RGB565 framebuffer + size
    pageros_widgets_palette_t palette;
    pageros_widgets_focus_t focus;
    const char *title;                        // top-bar title, or NULL
    const char *help;                         // bottom-bar help text, or NULL

    // Optional render viewport — rectangle of the canvas the renderer
    // is allowed to write into. When `vw == 0` the renderer falls back
    // to (0, 0, canvas.w, canvas.h) for backward compat. Used by the
    // desktop shell to reserve space for status bars + sidebar rail.
    int vx, vy, vw, vh;

    // Skip the internal top/bot chrome (title + help). The desktop draws
    // its own persistent status bars, so for in-viewport app rendering
    // we don't want a second set of bars eating viewport rows.
    bool skip_chrome;
} pageros_widgets_ctx_t;

// Draw the chrome (top bar with title, bottom bar with help text) +
// every widget in `body`. `body` MUST be a CBOR array; non-array inputs
// render the empty body. Returns ESP_OK on success.
esp_err_t pageros_widgets_render_screen(const pageros_widgets_ctx_t *ctx,
                                        const pgr_cbor_value_t *body);

// Compute the natural height (px) of a single widget — used by layout
// to figure out scroll bounds + how many items fit. Returns 0 if the
// widget is unknown or malformed.
int pageros_widgets_measure(const pgr_cbor_value_t *widget);

// Direct primitives — exposed for the Shell + test rigs that want to
// paint outside of a Frame.
void pageros_widgets_fill_rect(const pageros_fonts_canvas_t *canvas,
                               int x, int y, int w, int h, uint16_t rgb);
void pageros_widgets_outline_rect(const pageros_fonts_canvas_t *canvas,
                                  int x, int y, int w, int h, uint16_t rgb);

// --- desktop chrome ----------------------------------------------- //
//
// Persistent UI furniture rendered by the shell around the foreground
// app's viewport. The chrome layer lives in widgets so it can share
// font + rect primitives with the rest of the renderer.

#define PAGEROS_CHROME_BAR_H        16
#define PAGEROS_CHROME_RAIL_W       80
#define PAGEROS_CHROME_RAIL_MAX_APPS 6

// Liveness state collected by the shell each render.
typedef struct {
    int  hh, mm;                   // wall clock; -1, -1 if unknown
    const char *identity_short;    // short hex like "7B11.A9F2", or NULL

    int  battery_pct;              // 0..100, -1 if unknown
    int  wifi_state;               // 0=off, 1=connecting, 2=connected
    int  lora_state;               // 0=off, 1=ready
    int  gps_state;                // 0=no fix, 1=fix
    const char *power_mode;        // "ACT" / "DIM" / "OFF" / "SLP"
} pageros_chrome_state_t;

// 16 px-tall top bar across the full canvas. Renders:
//   left  — HH:MM
//   mid   — PAGEROS title
//   right — identity hash
void pageros_widgets_chrome_topbar(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   const pageros_chrome_state_t *state);

// 16 px-tall bottom bar with subsystem indicators. Renders:
//   BAT▓▓▓░  WIFI●  LORA●  GPS●  PWR:ACT
void pageros_widgets_chrome_botbar(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   const pageros_chrome_state_t *state);

// Sidebar rail state.
typedef struct {
    // Always-present HOME entry at the top. The "items" array is the
    // open-apps stack (most-recent first).
    const char *items[PAGEROS_CHROME_RAIL_MAX_APPS];
    int   n_items;

    // Focus on the rail. 0 = HOME, 1..n_items = open app entries.
    // -1 means the rail isn't focused right now (the viewport is).
    int   focus_index;
    bool  rail_active;            // true: rail focus highlights tinted accent
} pageros_chrome_rail_t;

// 80 px-wide left rail spanning the body region (between top/bot bars).
void pageros_widgets_chrome_rail(const pageros_fonts_canvas_t *canvas,
                                 const pageros_widgets_palette_t *pal,
                                 const pageros_chrome_rail_t *rail);

// Desktop tile launcher. Renders an N-tile grid inside the rectangle
// (x, y, w, h). `focus_index` highlights the focused tile (or -1).
typedef struct {
    const char *name;       // displayed in 16x16
    int unread;             // shown as "[N]" badge if > 0
} pageros_chrome_tile_t;

void pageros_widgets_chrome_tile_grid(const pageros_fonts_canvas_t *canvas,
                                      const pageros_widgets_palette_t *pal,
                                      const pageros_chrome_tile_t *tiles,
                                      int n_tiles,
                                      int focus_index,
                                      int x, int y, int w, int h);

// Optional scanline overlay — dims every Nth row by halving each pixel.
// Pure cosmetic, very cheap. Skip for app viewports if it hurts legibility.
void pageros_widgets_chrome_scanlines(const pageros_fonts_canvas_t *canvas,
                                      int x, int y, int w, int h,
                                      int every_n_rows);

#ifdef __cplusplus
}
#endif
