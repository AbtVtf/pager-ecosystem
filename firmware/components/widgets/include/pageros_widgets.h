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

#ifdef __cplusplus
}
#endif
