// SPDX-License-Identifier: Apache-2.0
//
// PagerOS cyberpunk fx engine.
//
// Procedural visual layer that sits on top of the regular widget render:
//
//   * matrix rain         — columns of falling katakana behind the
//                           desktop tiles and lock screen.
//   * glitch overlay      — random horizontal slice jitter + RGB chroma
//                           split. Fires every 3–6 seconds.
//   * focus sweep         — animated brackets + travelling light around
//                           a focused rect (typically a desktop tile).
//   * page transition     — short scanwipe + invert when the shell
//                           changes page.
//   * katakana glyph set  — 48 procedural 8×8 bitmaps that *evoke*
//                           katakana without shipping a CJK font.
//
// The engine owns a FreeRTOS task that ticks every ~40 ms and calls the
// installed render callback whenever an animation needs a redraw. The
// shell mutex-guards its render entry point so background ticks don't
// race the input-driven render.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pageros_fonts.h"
#include "pageros_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the engine: seed RNG, prep state, build the katakana table.
// Idempotent. Does NOT start the animation task — call pageros_cyber_fx_start()
// after the display + fonts are up.
void pageros_cyber_fx_init(void);

// Start the background animation task. Must only be called once.
// `render_cb` is invoked from the fx task to request a fresh render; it
// should be cheap and re-entrant with the shell's input-driven render.
void pageros_cyber_fx_start(void (*render_cb)(void));

// Page-enter notifier — call from each `mount_*` to trigger a transition
// scanwipe + reseed of matrix columns. `page_id` is a free-form tag.
void pageros_cyber_fx_on_page_enter(const char *page_id);

// Action notifier — called on ENTER/click events; spawns a quick
// foreground ripple ("hit") highlight that fades over 150 ms.
void pageros_cyber_fx_on_action(int x, int y);

// Glitch nudge — bias the next-glitch timer to fire within ~100 ms.
// Used by errors and boot-time wakeups to punch the user's eye.
void pageros_cyber_fx_kick_glitch(void);

// True when an animation needs a frame within the next ~40 ms — the
// shell can use this to decide whether to schedule an extra render.
bool pageros_cyber_fx_animating(void);

// --- paint primitives -------------------------------------------------- //

// Paint the matrix rain layer into (x,y,w,h). Call before painting the
// foreground content so tiles/text sit on top of the rain.
void pageros_cyber_fx_paint_matrix(const pageros_fonts_canvas_t *canvas,
                                   const pageros_widgets_palette_t *pal,
                                   int x, int y, int w, int h);

// Paint the focus-sweep brackets + travelling light around a focused
// rectangle. Call after the tile is painted so the sweep floats on top.
void pageros_cyber_fx_paint_focus(const pageros_fonts_canvas_t *canvas,
                                  const pageros_widgets_palette_t *pal,
                                  int x, int y, int w, int h);

// Per-element glitch — picks a horizontal slice inside the rect and
// shifts it by a few px for ~120 ms, then quiets for ~2.3 s. `seed`
// scatters the glitch events across elements so they don't all fire at
// once. Cheap when not actively glitching (early return). Call AFTER
// painting the element's content so we shift painted pixels.
void pageros_cyber_fx_paint_element_glitch(const pageros_fonts_canvas_t *canvas,
                                           const pageros_widgets_palette_t *pal,
                                           int x, int y, int w, int h,
                                           uint32_t seed);

// Paint the global glitch + scanline overlay on the full canvas. Call
// last in the render pipeline (after toast). Cheap: usually a no-op or
// a few short row shifts.
void pageros_cyber_fx_paint_overlay(const pageros_fonts_canvas_t *canvas,
                                    const pageros_widgets_palette_t *pal);

// Paint the active page-transition (scanwipe / invert). No-op when not
// in a transition. Call after content + chrome but before the toast.
void pageros_cyber_fx_paint_transition(const pageros_fonts_canvas_t *canvas,
                                       const pageros_widgets_palette_t *pal);

// --- katakana glyphs --------------------------------------------------- //

#define PAGEROS_CYBER_FX_KATAKANA_COUNT 48

// Draw one procedural katakana glyph (8x8 native, scaled by `scale`).
// `idx` is taken modulo PAGEROS_CYBER_FX_KATAKANA_COUNT.
void pageros_cyber_fx_draw_katakana(const pageros_fonts_canvas_t *canvas,
                                    int x, int y, int idx, int scale,
                                    uint16_t color);

// Draw a randomly-chosen katakana glyph; the index advances with the
// engine tick so successive calls in a render produce different shapes.
void pageros_cyber_fx_draw_katakana_random(const pageros_fonts_canvas_t *canvas,
                                           int x, int y, int scale,
                                           uint16_t color);

// Draw a decorative ideographic bracket pair — left + right — sized to
// (w, h). Looks like 「…」.
void pageros_cyber_fx_draw_bracket_pair(const pageros_fonts_canvas_t *canvas,
                                        int x, int y, int w, int h,
                                        uint16_t color);

// "Cyber tag": draws "[SYS//<label>·<status_glyph>]" with animated
// chroma jitter on the trailing glyph. Used by header rows.
void pageros_cyber_fx_draw_tag(const pageros_fonts_canvas_t *canvas,
                               const pageros_widgets_palette_t *pal,
                               int x, int y,
                               const char *prefix, const char *label);

#ifdef __cplusplus
}
#endif
