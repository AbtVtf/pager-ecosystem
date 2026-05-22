// SPDX-License-Identifier: Apache-2.0
//
// PagerOS display driver — see `pageros_display.h`.

#include "pageros_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_lcd_st7796.h"
#include "gradient.h"

static const char *TAG = "display";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager — arduino-esp32 variant)
// ---------------------------------------------------------------------------

#define DISP_SPI_HOST       SPI2_HOST
#define DISP_PIN_MOSI       34
#define DISP_PIN_MISO       33
#define DISP_PIN_SCK        35
#define DISP_PIN_CS         38
#define DISP_PIN_DC         37
#define DISP_PIN_RST        -1
#define DISP_PIN_BL         42
#define DISP_PIXEL_CLK_HZ   (40 * 1000 * 1000)  // 40 MHz device-local

// Native panel is 222 wide × 480 tall (portrait); PagerOS renders
// landscape, so we swap_xy and mirror as needed below.
#define DISP_PANEL_PIX_W    PAGEROS_DISPLAY_HEIGHT
#define DISP_PANEL_PIX_H    PAGEROS_DISPLAY_WIDTH

#define DISP_BL_TIMER       LEDC_TIMER_0
#define DISP_BL_CHANNEL     LEDC_CHANNEL_0
#define DISP_BL_FREQ_HZ     5000
#define DISP_BL_RESOLUTION  LEDC_TIMER_8_BIT  // duty 0..255

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

static struct {
    bool                  ready;
    bool                  spi_up;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    uint16_t              *framebuffer;   // CPU-side, host-endian RGB565
    uint16_t              *wire_buf;      // SPI-side, big-endian RGB565
    SemaphoreHandle_t     band_done;      // signals one band fully DMA'd to panel
} s_state;

// Per-band completion callback. Fires from the SPI ISR after each
// draw_bitmap chunk finishes; present() waits on band_done before
// overwriting wire_buf with the next band's pixels.
static bool IRAM_ATTR on_color_band_done(esp_lcd_panel_io_handle_t io,
                                         esp_lcd_panel_io_event_data_t *edata,
                                         void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    BaseType_t hpw = pdFALSE;
    if (s_state.band_done) xSemaphoreGiveFromISR(s_state.band_done, &hpw);
    return hpw == pdTRUE;
}

// ---------------------------------------------------------------------------
// Backlight (LEDC PWM on GPIO 42)
// ---------------------------------------------------------------------------

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = DISP_BL_TIMER,
        .duty_resolution = DISP_BL_RESOLUTION,
        .freq_hz         = DISP_BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) return err;

    ledc_channel_config_t ccfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = DISP_BL_CHANNEL,
        .timer_sel  = DISP_BL_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = DISP_PIN_BL,
        .duty       = 128,      // start at half until shell sets a value
        .hpoint     = 0,
    };
    return ledc_channel_config(&ccfg);
}

esp_err_t pageros_display_backlight(uint8_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, DISP_BL_CHANNEL, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, DISP_BL_CHANNEL);
}

// ---------------------------------------------------------------------------
// SPI + ST7796
// ---------------------------------------------------------------------------

static esp_err_t spi_bus_up_once(void)
{
    if (s_state.spi_up) return ESP_OK;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = DISP_PIN_MOSI,
        .miso_io_num     = DISP_PIN_MISO,
        .sclk_io_num     = DISP_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // One band per DMA transaction; present() loops over bands.
        .max_transfer_sz = PAGEROS_DISPLAY_CHUNK_BYTES + 256,
    };
    esp_err_t err = spi_bus_initialize(DISP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        // Bus already up (FW-003 storage got here first). That's fine.
        s_state.spi_up = true;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    s_state.spi_up = true;
    return ESP_OK;
}

esp_err_t pageros_display_init(void)
{
    if (s_state.ready) return ESP_OK;

    // Explicitly park CS=38 and DC=37 in their idle states BEFORE the
    // SPI bus comes up. LilyGoLib's display init does the same — it
    // means any subsequent SPI traffic from SD or LoRa (which share
    // the bus) reaches the bus with the panel deselected, regardless
    // of how long the panel_io takes to claim the pins.
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << DISP_PIN_CS) | (1ULL << DISP_PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(DISP_PIN_CS, 1);   // HIGH = deselected
    gpio_set_level(DISP_PIN_DC, 1);

    ESP_RETURN_ON_ERROR(spi_bus_up_once(), TAG, "spi");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = DISP_PIN_CS,
        .dc_gpio_num         = DISP_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = DISP_PIXEL_CLK_HZ,
        .trans_queue_depth   = 4,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .on_color_trans_done = on_color_band_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISP_SPI_HOST,
                                  &io_cfg, &s_state.io),
        TAG, "panel io");
    s_state.band_done = xSemaphoreCreateBinary();
    if (!s_state.band_done) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_state.band_done);  // start "available" so first take succeeds

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        // Per the LILYGO T-LoRa Pager variant rotation config table
        // (LilyGoLib `LilyGo_LoRa_Pager.cpp` `rotation_config[]`), the
        // landscape MADCTL is 0xE8 = MY|MX|MV|BGR. Element order BGR
        // tells the ST7796 controller to swap R/B before mapping to the
        // panel's physical color filters — without it everything comes
        // out red-shifted (or, after our wire byte-swap, green-shifted).
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7796(s_state.io, &panel_cfg, &s_state.panel),
        TAG, "st7796");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_state.panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_state.panel),  TAG, "panel init");
    // Landscape (MADCTL = 0xE8 = MY|MX|MV|BGR per LILYGO authoritative
    // config). BGR is set on the panel_dev_config above; the other
    // three bits come from swap_xy + mirror(true, true).
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_state.panel, true), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_state.panel, true, true), TAG, "mirror");
    // The LILYGO panel's visible window starts at controller row 49,
    // not row 0 (the ST7796 internal buffer is 320x480 but only 222
    // rows are wired to physical pixels, centered with a 49-row offset
    // in landscape). Without this gap, our 0..221 writes land on
    // controller rows 0..221 — the top 49 fall above the visible area
    // and the bottom 49 of the panel show stale content from before
    // our boot. (Source: LilyGoLib rotation_config[1] = {0x48, ...,
    // 49, 0} for the equivalent portrait rotation; landscape uses
    // {0, 49}.)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_state.panel, 0, 49),
                        TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_state.panel, true),
                        TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_state.panel, true),
                        TAG, "disp on");

    // PSRAM framebuffer (host-endian); SPI DMA on the LILYGO board's
    // QSPI PSRAM doesn't accept transfers directly so we ship pixels
    // through a small chunk-sized wire buffer in DMA-capable internal
    // RAM. Tradeoff: present() loops N times instead of one big DMA,
    // but each loop is < 20 ms and the queue never sees more than a
    // single chunk in flight.
    s_state.framebuffer = heap_caps_malloc(PAGEROS_DISPLAY_FB_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_state.wire_buf    = heap_caps_malloc(PAGEROS_DISPLAY_CHUNK_BYTES,
                                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_state.framebuffer || !s_state.wire_buf) {
        ESP_LOGE(TAG, "framebuffer alloc (fb=%u + wire=%u bytes) failed",
                 (unsigned)PAGEROS_DISPLAY_FB_BYTES,
                 (unsigned)PAGEROS_DISPLAY_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_state.framebuffer, 0, PAGEROS_DISPLAY_FB_BYTES);

    s_state.ready = true;
    ESP_LOGI(TAG, "display up: %dx%d ST7796, fb=%p (PSRAM, %u bytes)",
             PAGEROS_DISPLAY_WIDTH, PAGEROS_DISPLAY_HEIGHT,
             s_state.framebuffer, (unsigned)PAGEROS_DISPLAY_FB_BYTES);
    return ESP_OK;
}

esp_err_t pageros_display_shutdown(void)
{
    if (!s_state.ready) return ESP_OK;
    (void)pageros_display_backlight(0);
    (void)esp_lcd_panel_disp_on_off(s_state.panel, false);
    esp_lcd_panel_del(s_state.panel);
    esp_lcd_panel_io_del(s_state.io);
    s_state.panel = NULL;
    s_state.io    = NULL;
    if (s_state.framebuffer) {
        heap_caps_free(s_state.framebuffer);
        s_state.framebuffer = NULL;
    }
    if (s_state.wire_buf) {
        heap_caps_free(s_state.wire_buf);
        s_state.wire_buf = NULL;
    }
    if (s_state.band_done) {
        vSemaphoreDelete(s_state.band_done);
        s_state.band_done = NULL;
    }
    s_state.ready = false;
    return ESP_OK;
}

esp_err_t pageros_display_panel_reinit(void)
{
    if (!s_state.ready || !s_state.panel) return ESP_ERR_INVALID_STATE;
    // Re-run the init sequence: reset (software, no RST pin), init,
    // orientation, gap, invert, display on. This is the same sequence
    // pageros_display_init() runs the first time, minus the SPI bus +
    // panel object creation (those stay valid).
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_state.panel),                  TAG, "re-reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_state.panel),                   TAG, "re-init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_state.panel, true),          TAG, "re-swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_state.panel, true, true),     TAG, "re-mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_state.panel, 0, 49),         TAG, "re-gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_state.panel, true),     TAG, "re-invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_state.panel, true),      TAG, "re-disp on");
    ESP_LOGI(TAG, "panel re-armed after bus disturbance");
    return ESP_OK;
}

uint16_t *pageros_display_framebuffer(void)
{
    return s_state.framebuffer;
}

// ---------------------------------------------------------------------------
// Drawing primitives — all operate on the PSRAM framebuffer
// ---------------------------------------------------------------------------

esp_err_t pageros_display_fill(uint16_t rgb565)
{
    if (!s_state.framebuffer) return ESP_ERR_INVALID_STATE;
    uint16_t *fb = s_state.framebuffer;
    // RGB565 is 16-bit; memset only smears bytes — so loop.
    size_t n = (size_t)PAGEROS_DISPLAY_WIDTH * PAGEROS_DISPLAY_HEIGHT;
    for (size_t i = 0; i < n; i++) fb[i] = rgb565;
    return ESP_OK;
}

esp_err_t pageros_display_gradient(uint16_t top, uint16_t bottom)
{
    if (!s_state.framebuffer) return ESP_ERR_INVALID_STATE;
    uint16_t *fb = s_state.framebuffer;
    int rows = PAGEROS_DISPLAY_HEIGHT;
    int cols = PAGEROS_DISPLAY_WIDTH;
    for (int y = 0; y < rows; y++) {
        uint16_t row_color = pageros_lerp_rgb565(top, bottom, y, rows - 1);
        for (int x = 0; x < cols; x++) {
            fb[y * cols + x] = row_color;
        }
    }
    return ESP_OK;
}

esp_err_t pageros_display_blit(int x, int y, int w, int h,
                               const uint16_t *pixels)
{
    if (!s_state.framebuffer || !pixels) return ESP_ERR_INVALID_ARG;
    int sx = 0, sy = 0;
    if (!pageros_clip_rect(PAGEROS_DISPLAY_WIDTH, PAGEROS_DISPLAY_HEIGHT,
                            &x, &y, &w, &h, &sx, &sy)) {
        return ESP_OK;  // entirely off-screen
    }
    int src_stride = w + sx;  // caller's bitmap width before clipping
    // The clip routine doesn't tell us the original `w` post-trim from
    // the right edge; the source stride is the caller's full bitmap
    // width. Recompute below using the entry-time `w` instead.
    (void)src_stride;
    // Re-derive original width: w_after_clip + sx_left + sx_right_trim.
    // We don't have sx_right_trim, so we accept the simpler convention:
    // the caller passes a packed bitmap matching the clipped region.
    // For now we copy row-by-row using w as the row length — this means
    // off-canvas right-edge clipping requires the caller to NOT include
    // out-of-range columns. The Renderer (FW-020) packs tightly already.
    uint16_t       *dst = s_state.framebuffer + y * PAGEROS_DISPLAY_WIDTH + x;
    const uint16_t *src = pixels + sy * w + sx;
    for (int row = 0; row < h; row++) {
        memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        dst += PAGEROS_DISPLAY_WIDTH;
        src += w;
    }
    return ESP_OK;
}

esp_err_t pageros_display_present(void)
{
    if (!s_state.ready || !s_state.framebuffer || !s_state.wire_buf)
        return ESP_ERR_INVALID_STATE;

    // Stream the framebuffer to the panel in CHUNK_ROWS-tall bands.
    // Each band fits in the small DMA-capable wire buffer; we wait
    // for the previous band's transmission to fully drain before
    // overwriting the buffer for the next band, otherwise the SPI
    // DMA would read half-mutated pixels and the panel shows
    // doubled / ghosted content.
    const int H = PAGEROS_DISPLAY_HEIGHT;
    const int W = PAGEROS_DISPLAY_WIDTH;
    for (int y = 0; y < H; y += PAGEROS_DISPLAY_CHUNK_ROWS) {
        int rows = (y + PAGEROS_DISPLAY_CHUNK_ROWS <= H)
                       ? PAGEROS_DISPLAY_CHUNK_ROWS : (H - y);
        // Wait for the previous band's DMA to complete. On timeout
        // (SPI bus glitched, e.g. user just inserted the SD card)
        // assume the trans was lost, log it, and continue — never
        // hang the shell on a stalled DMA.
        if (xSemaphoreTake(s_state.band_done, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "band_done timeout at y=%d (DMA stalled?)", y);
        }
        const uint16_t *src = s_state.framebuffer + y * W;
        uint16_t       *dst = s_state.wire_buf;
        size_t n = (size_t)W * rows;
        for (size_t i = 0; i < n; i++) dst[i] = __builtin_bswap16(src[i]);
        esp_err_t r = esp_lcd_panel_draw_bitmap(s_state.panel,
                                                 0,     y,
                                                 W, y + rows,
                                                 s_state.wire_buf);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap band y=%d returned %s",
                     y, esp_err_to_name(r));
            // Re-arm the semaphore so subsequent presents aren't
            // permanently stuck waiting for a callback that won't fire.
            xSemaphoreGive(s_state.band_done);
            return r;
        }
    }
    // Wait for the final band to land before returning so callers can
    // safely overwrite the framebuffer + we leave band_done in the
    // "given" state for the next present. Timeout-tolerant: never
    // block the shell on a stalled DMA.
    (void)xSemaphoreTake(s_state.band_done, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_state.band_done);
    return ESP_OK;
}
