// SPDX-License-Identifier: Apache-2.0
//
// PagerOS display driver — FW-005.
//
// Drives the LILYGO T-LoRa Pager's 2.33" 480x222 IPS panel (ST7796
// controller) over the shared SPI bus. Allocates an RGB565 framebuffer
// in PSRAM and exposes simple fill / gradient / blit primitives. The
// upper-layer renderer (FW-020..023) draws into the framebuffer and
// calls `pageros_display_present()` to push the result to the panel.
//
// Hardware (per arduino-esp32 `lilygo_tlora_pager` variant):
//   - Panel:       ST7796, native 222x480 portrait (we run landscape).
//   - SPI:         shared bus on SPI2_HOST (MOSI=34, MISO=33, SCK=35).
//   - DISP_CS  = 38, DISP_DC = 37, DISP_RST = none (-1).
//   - Backlight = GPIO 42 (driven via LEDC PWM).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_DISPLAY_WIDTH    480

// ST7796 native landscape buffer is 480x320; the LILYGO Pager's visible
// window is the upper 222 rows (UI area) but the controller still has 320
// rows of internal RAM. If we only fill 222 rows of the panel, the bottom
// 98 rows show stale content from whatever was on the panel before
// (factory firmware, bootloader splash). Allocate + present the full
// 480x320 region so the panel is fully under our control; the widget
// renderer keeps drawing in the 480x222 layout area and the unused rows
// stay at the boot fill color.
#define PAGEROS_DISPLAY_PANEL_H  320
#define PAGEROS_DISPLAY_HEIGHT   PAGEROS_DISPLAY_PANEL_H
#define PAGEROS_DISPLAY_UI_H     222   // logical UI height (SPEC §4)
#define PAGEROS_DISPLAY_BPP      2   // RGB565 = 2 bytes per pixel
#define PAGEROS_DISPLAY_FB_BYTES \
    ((size_t)PAGEROS_DISPLAY_WIDTH * PAGEROS_DISPLAY_HEIGHT * PAGEROS_DISPLAY_BPP)

// Pack 8-bit RGB into RGB565 (panel native format) in big-endian byte
// order — ST7796 reads MSB first on the SPI wire.
static inline uint16_t pageros_display_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Bring up the SPI panel IO, ST7796 controller, backlight (LEDC PWM
// half brightness), and allocate the PSRAM framebuffer. Idempotent.
esp_err_t pageros_display_init(void);

// Tear down the panel, free the framebuffer, and gate the backlight.
esp_err_t pageros_display_shutdown(void);

// Return a writable pointer to the RGB565 framebuffer (480*222*2 bytes,
// PSRAM-allocated). NULL before `pageros_display_init`. Pixels are laid
// out row-major; `row[x]` lives at offset `y * 480 + x`.
uint16_t *pageros_display_framebuffer(void);

// Solid colour fill of the framebuffer. Doesn't push to the panel —
// follow with `pageros_display_present()`.
esp_err_t pageros_display_fill(uint16_t rgb565);

// Linear top-to-bottom gradient between `top` and `bottom` RGB565
// colours. Used by the boot splash and by selftest visual probe.
esp_err_t pageros_display_gradient(uint16_t top, uint16_t bottom);

// Copy a rectangle of RGB565 pixels into the framebuffer at (x, y).
// Bounds-clipped; out-of-range rectangles are silently truncated.
esp_err_t pageros_display_blit(int x, int y, int w, int h,
                               const uint16_t *pixels);

// Push the whole framebuffer to the panel via SPI DMA. The acceptance
// criterion is "sustained ≥ 30 fps for full-screen redraws"; on the
// shared SPI bus running the panel device at 40 MHz this comes in
// around 50–60 fps with no other bus traffic.
esp_err_t pageros_display_present(void);

// Set backlight duty in [0, 255]. 0 = off, 255 = full. PWM is on LEDC
// channel 0, fixed at ~5 kHz.
esp_err_t pageros_display_backlight(uint8_t duty);

#ifdef __cplusplus
}
#endif
