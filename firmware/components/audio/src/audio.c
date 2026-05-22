// SPDX-License-Identifier: Apache-2.0
//
// FW-013 / FW-032 — audio driver + tones.
//
// v0 implementation: I2S (legacy std driver) at 8 kHz/16-bit/mono with
// an ES8311 init sequence. The ES8311 register writes are the minimal
// playback subset from Espressif's `esp_codec_dev` reference; the full
// codec component is intentionally not pulled in so this firmware
// stays lean. The bundled tones are short sine wave envelopes
// procedurally generated at first init — see `gen_tones()` below.
// Replacing them with curated PCM samples is straightforward (drop
// .pcm files into firmware/tones/, embed via `EMBED_FILES` in
// CMakeLists, and lookup by id).

#include "pageros_audio.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pageros_i2c_bus.h"

static const char *TAG = "audio";

// LilyGO T-LoRa Pager — verified against arduino-esp32 variant header.
// The ES8311 codec sits on the shared I2C bus (SDA=3, SCL=2) at 0x18.
// I2S pins per the variant: WS=18, SCK=11, MCLK=10, SDOUT=45, SDIN=17.
// The amp is gated by XL9555 EXPANDS_AMP_EN — main.c asserts that bit
// during XL9555 init so the speaker can sound.
#define ES8311_I2C_ADDR        0x18
#define I2S_PORT               I2S_NUM_0
#define I2S_MCLK_GPIO          10
#define I2S_BCLK_GPIO          11
#define I2S_LRCK_GPIO          18
#define I2S_DOUT_GPIO          45
#define I2S_DIN_GPIO           17

#define TONE_DURATION_MS       250
#define TONE_SAMPLES_PER_MS    (PAGEROS_AUDIO_SAMPLE_RATE / 1000)

static void ui_audio_task(void *arg);

static struct {
    bool inited;
    bool muted;
    uint8_t volume;
    i2s_chan_handle_t tx_chan;
    int16_t *tones[PAGEROS_TONE__COUNT];
    size_t   tone_lens[PAGEROS_TONE__COUNT];

    // Async UI sound worker — the shell calls play_ui_click/scroll
    // from input-handler context where any blocking I2S write would
    // freeze the UI for the clip duration. The worker drains the
    // queue, dropping older requests so a burst of encoder ticks
    // doesn't pile up dozens of overlapping plays.
    QueueHandle_t ui_queue;
    TaskHandle_t  ui_task;
} g = { .volume = 240 };

// --- ES8311 minimal init -------------------------------------------- //

static esp_err_t es8311_w(uint8_t reg, uint8_t val)
{
    return pageros_i2c_reg_write8(ES8311_I2C_ADDR, reg, val);
}

static esp_err_t es8311_init(void)
{
    // Power on + clock setup. Values match the reference 8 kHz mono
    // playback sequence from esp_codec_dev/es8311.
    esp_err_t r;
    if ((r = es8311_w(0x00, 0x1F)) != ESP_OK) return r;  // reset
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((r = es8311_w(0x00, 0x00)) != ESP_OK) return r;  // out of reset

    if ((r = es8311_w(0x01, 0x30)) != ESP_OK) return r;  // clk on, BCLK provider
    if ((r = es8311_w(0x02, 0x10)) != ESP_OK) return r;  // 8k
    if ((r = es8311_w(0x03, 0x10)) != ESP_OK) return r;  // 256x oversample
    if ((r = es8311_w(0x16, 0x24)) != ESP_OK) return r;  // dac soft ramp on
    if ((r = es8311_w(0x44, 0x08)) != ESP_OK) return r;  // dac mute off
    if ((r = es8311_w(0x32, 0xFF)) != ESP_OK) return r;  // dac volume = +5 dB (max)
    if ((r = es8311_w(0x37, 0x08)) != ESP_OK) return r;  // headphone path on
    return ESP_OK;
}

// --- Tone generation ----------------------------------------------- //

static int16_t *gen_envelope_tone(int freq_hz, int duration_ms, size_t *out_n)
{
    size_t n = (size_t)(PAGEROS_AUDIO_SAMPLE_RATE * duration_ms / 1000);
    int16_t *buf = (int16_t *)malloc(n * sizeof(int16_t));
    if (!buf) { *out_n = 0; return NULL; }
    double w = 2.0 * 3.14159265358979323846 * (double)freq_hz / PAGEROS_AUDIO_SAMPLE_RATE;
    for (size_t i = 0; i < n; i++) {
        // 5% attack, 80% sustain, 15% release linear envelope.
        double env;
        double t = (double)i / n;
        if      (t < 0.05) env = t / 0.05;
        else if (t > 0.85) env = (1.0 - t) / 0.15;
        else               env = 1.0;
        double s = sin(w * (double)i) * env * 0.45;
        buf[i] = (int16_t)(s * 32767.0);
    }
    *out_n = n;
    return buf;
}

static void gen_tones(void)
{
    // Frequencies picked for distinct character. Total memory ~10 KB.
    static const int specs[PAGEROS_TONE__COUNT][2] = {
        [PAGEROS_TONE_DEFAULT]      = { 880,  220 },
        [PAGEROS_TONE_LOW_PRIORITY] = { 440,  120 },
        [PAGEROS_TONE_ALERT]        = { 1320, 320 },
        [PAGEROS_TONE_SUCCESS]      = { 660,  180 },
        [PAGEROS_TONE_ERROR]        = { 220,  400 },
    };
    for (int i = 0; i < PAGEROS_TONE__COUNT; i++) {
        if (g.tones[i]) continue;
        g.tones[i] = gen_envelope_tone(specs[i][0], specs[i][1], &g.tone_lens[i]);
    }
}

// --- API ----------------------------------------------------------- //

const char *pageros_tone_name(pageros_tone_t t)
{
    switch (t) {
    case PAGEROS_TONE_DEFAULT:      return "default";
    case PAGEROS_TONE_LOW_PRIORITY: return "low_priority";
    case PAGEROS_TONE_ALERT:        return "alert";
    case PAGEROS_TONE_SUCCESS:      return "success";
    case PAGEROS_TONE_ERROR:        return "error";
    default:                        return "?";
    }
}

int pageros_tone_from_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < PAGEROS_TONE__COUNT; i++) {
        if (strcmp(name, pageros_tone_name((pageros_tone_t)i)) == 0) return i;
    }
    return -1;
}

esp_err_t pageros_audio_init(void)
{
    if (g.inited) return ESP_OK;

    // The codec sits on the shared I2C bus (init owned by i2c_bus).
    // Calling the shared init is idempotent and safe regardless of who
    // got there first.
    esp_err_t r = pageros_i2c_bus_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "shared I2C bus init: %s", esp_err_to_name(r));
    }

    // ES8311 register init — non-fatal so we still expose the I2S TX
    // path even if the codec isn't there (e.g. on dev boards without
    // the speaker mod).
    if (es8311_init() != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed — playback path may be silent");
    }

    // I2S TX channel — pins per LILYGO authoritative variant.
    i2s_chan_config_t ch_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    r = i2s_new_channel(&ch_cfg, &g.tx_chan, NULL);
    if (r != ESP_OK) return r;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(PAGEROS_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = { 0 },
        },
    };
    r = i2s_channel_init_std_mode(g.tx_chan, &std_cfg);
    if (r != ESP_OK) return r;
    r = i2s_channel_enable(g.tx_chan);
    if (r != ESP_OK) return r;

    gen_tones();

    // UI sound worker — single-slot queue with drain-on-receive so a
    // fast input burst collapses to one play instead of stacking up.
    if (!g.ui_queue) {
        g.ui_queue = xQueueCreate(8, sizeof(uint8_t));
        if (g.ui_queue) {
            BaseType_t ok = xTaskCreate(ui_audio_task, "ui_audio",
                                        2048, NULL, 5, &g.ui_task);
            if (ok != pdPASS) {
                vQueueDelete(g.ui_queue);
                g.ui_queue = NULL;
                ESP_LOGW(TAG, "ui audio task spawn failed; UI sounds disabled");
            }
        }
    }

    g.inited = true;
    ESP_LOGI(TAG, "init done (i2s + es8311 + ui sounds)");
    return ESP_OK;
}

esp_err_t pageros_audio_shutdown(void)
{
    if (!g.inited) return ESP_OK;
    i2s_channel_disable(g.tx_chan);
    i2s_del_channel(g.tx_chan);
    for (int i = 0; i < PAGEROS_TONE__COUNT; i++) {
        if (g.tones[i]) { free(g.tones[i]); g.tones[i] = NULL; }
        g.tone_lens[i] = 0;
    }
    g.inited = false;
    return ESP_OK;
}

esp_err_t pageros_audio_play_pcm(const int16_t *samples, size_t n_samples)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!samples || n_samples == 0) return ESP_ERR_INVALID_ARG;
    if (g.muted) return ESP_OK;
    // Apply volume by software scaling — codec volume changes need an
    // I2C round-trip per tone which is overkill.
    int16_t *scratch = (int16_t *)malloc(n_samples * sizeof(int16_t));
    if (!scratch) return ESP_ERR_NO_MEM;
    int vol = g.volume;
    for (size_t i = 0; i < n_samples; i++) {
        int32_t s = (int32_t)samples[i] * vol / 255;
        if (s > INT16_MAX) s = INT16_MAX;
        if (s < INT16_MIN) s = INT16_MIN;
        scratch[i] = (int16_t)s;
    }
    size_t written = 0;
    esp_err_t r = i2s_channel_write(g.tx_chan, scratch,
                                    n_samples * sizeof(int16_t),
                                    &written, pdMS_TO_TICKS(2000));
    free(scratch);
    return r;
}

esp_err_t pageros_audio_set_volume(uint8_t vol)
{
    g.volume = vol;
    return ESP_OK;
}

esp_err_t pageros_audio_play_tone(pageros_tone_t t)
{
    if ((int)t < 0 || (int)t >= PAGEROS_TONE__COUNT) return ESP_ERR_INVALID_ARG;
    if (!g.tones[t]) return ESP_ERR_NOT_FOUND;
    return pageros_audio_play_pcm(g.tones[t], g.tone_lens[t]);
}

void pageros_audio_set_muted(bool muted) { g.muted = muted; }
bool pageros_audio_is_muted(void) { return g.muted; }

// --- UI sound effects (embedded via EMBED_FILES) ------------------- //
//
// ESP-IDF's EMBED_FILES drops a pair of symbols around each file:
//   _binary_<safe_name>_start  / _binary_<safe_name>_end
// where `<safe_name>` is the file path with non-alphanumeric chars
// replaced by '_'. For tones/click.pcm the symbol is `click_pcm`.

extern const uint8_t _binary_click_pcm_start[]  asm("_binary_click_pcm_start");
extern const uint8_t _binary_click_pcm_end[]    asm("_binary_click_pcm_end");
extern const uint8_t _binary_scroll_pcm_start[] asm("_binary_scroll_pcm_start");
extern const uint8_t _binary_scroll_pcm_end[]   asm("_binary_scroll_pcm_end");

typedef enum { UI_SOUND_CLICK = 0, UI_SOUND_SCROLL = 1 } ui_sound_t;

static void ui_audio_task(void *arg)
{
    (void)arg;
    uint8_t kind;
    for (;;) {
        if (xQueueReceive(g.ui_queue, &kind, portMAX_DELAY) != pdTRUE) continue;
        // Drain backlog — last request wins so encoder bursts don't
        // queue dozens of overlapping plays.
        uint8_t newer;
        while (xQueueReceive(g.ui_queue, &newer, 0) == pdTRUE) kind = newer;

        const uint8_t *start = NULL, *end = NULL;
        if (kind == UI_SOUND_CLICK) {
            start = _binary_click_pcm_start;
            end   = _binary_click_pcm_end;
        } else if (kind == UI_SOUND_SCROLL) {
            start = _binary_scroll_pcm_start;
            end   = _binary_scroll_pcm_end;
        }
        if (!start || !end || end <= start) continue;
        if (g.muted) continue;
        size_t bytes = (size_t)(end - start);
        // Blocking play is fine on this task — only this task ever
        // blocks on i2s_channel_write, callers stay snappy.
        pageros_audio_play_pcm((const int16_t *)start, bytes / 2);
    }
}

static esp_err_t enqueue_ui_sound(ui_sound_t kind)
{
    if (!g.inited || !g.ui_queue) return ESP_ERR_INVALID_STATE;
    uint8_t v = (uint8_t)kind;
    // Non-blocking enqueue — if the queue is full we drop, which is
    // exactly what we want for crisp UI feedback.
    if (xQueueSend(g.ui_queue, &v, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

esp_err_t pageros_audio_play_ui_click(void)
{
    return enqueue_ui_sound(UI_SOUND_CLICK);
}

esp_err_t pageros_audio_play_ui_scroll(void)
{
    return enqueue_ui_sound(UI_SOUND_SCROLL);
}
