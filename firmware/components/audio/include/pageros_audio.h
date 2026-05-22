// SPDX-License-Identifier: Apache-2.0
//
// PagerOS audio driver — FW-013 / FW-032.
//
// Wraps the I2S peripheral hooked to the ES8311 codec on the LILYGO
// T-LoRa Pager. Exposes:
//
//   - `pageros_audio_init` — bring up I2S in 16-bit/8 kHz mono.
//   - `pageros_audio_play_pcm` — play a buffered PCM clip end-to-end.
//   - `pageros_audio_play_tone` (FW-032) — play a bundled tone by id.
//
// ES8311 register init follows the reference sequence from Espressif's
// `esp_codec_dev` examples — minimal: power-on, set 8 kHz, mono path,
// speaker amp on. Reset is via GPIO 21 on the LilyGO board.
//
// Bundled tone PCM data lives in `firmware/tones/`; the audio
// component compiles them into program memory as `const uint8_t[]`
// blobs so we don't need an SD read on every notification.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sample format the audio driver expects from callers.
// 22.05 kHz is a sweet spot for the ES8311 + this speaker — telephone-
// grade 8 kHz cut off everything above 4 kHz which made clicks sound
// muddy. 22.05 kHz captures the full audible band of short FX without
// pushing the codec into rates it sometimes glitches on.
#define PAGEROS_AUDIO_SAMPLE_RATE  22050
#define PAGEROS_AUDIO_BIT_DEPTH    16
#define PAGEROS_AUDIO_CHANNELS     1

esp_err_t pageros_audio_init(void);
esp_err_t pageros_audio_shutdown(void);

// Play a PCM clip (signed 16-bit, mono, 8 kHz). Blocking until done.
esp_err_t pageros_audio_play_pcm(const int16_t *samples, size_t n_samples);

// Set master volume 0–255. 0 = mute, 255 = full.
esp_err_t pageros_audio_set_volume(uint8_t vol);

// --- Notification tones (FW-032) ----------------------------------- //

typedef enum {
    PAGEROS_TONE_DEFAULT      = 0,
    PAGEROS_TONE_LOW_PRIORITY = 1,
    PAGEROS_TONE_ALERT        = 2,
    PAGEROS_TONE_SUCCESS      = 3,
    PAGEROS_TONE_ERROR        = 4,
    PAGEROS_TONE__COUNT       = 5,
} pageros_tone_t;

// Lookup by name (matches the push payload's `tone:` field).
const char *pageros_tone_name(pageros_tone_t t);
int pageros_tone_from_name(const char *name);   // -1 on unknown

esp_err_t pageros_audio_play_tone(pageros_tone_t t);

// Mute all tones (Settings → Silent).
void pageros_audio_set_muted(bool muted);
bool pageros_audio_is_muted(void);

// --- UI sound effects (cyberpunk shell) ---------------------------- //
//
// click.pcm + scroll.pcm are transcoded from /audio/*.mp3 to 8 kHz
// mono s16le at build time and embedded in flash. The shell calls
// these on every interaction:
//
//   - `click` on button activations (ENTER on a button or focused
//     list item).
//   - `scroll` on cursor / focus movement (encoder tick, sidebar
//     focus change).
//
// Both are non-blocking-friendly: they ship a few hundred samples to
// the I2S channel and return. Calls overlap by truncating the prior
// effect — short and crisp wins over interleaved.

esp_err_t pageros_audio_play_ui_click(void);
esp_err_t pageros_audio_play_ui_scroll(void);

#ifdef __cplusplus
}
#endif
