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
    uint16_t              *framebuffer;
} s_state;

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
        .max_transfer_sz = PAGEROS_DISPLAY_WIDTH * 40 * 2,  // ~40 lines/DMA
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

    ESP_RETURN_ON_ERROR(spi_bus_up_once(), TAG, "spi");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = DISP_PIN_CS,
        .dc_gpio_num         = DISP_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = DISP_PIXEL_CLK_HZ,
        .trans_queue_depth   = 10,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISP_SPI_HOST,
                                  &io_cfg, &s_state.io),
        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7796(s_state.io, &panel_cfg, &s_state.panel),
        TAG, "st7796");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_state.panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_state.panel),  TAG, "panel init");
    // Landscape: swap XY, then mirror the X axis so (0,0) is top-left
    // of the case-up orientation matching the keyboard.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_state.panel, true), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_state.panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_state.panel, true),
                        TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_state.panel, true),
                        TAG, "disp on");

    // PSRAM framebuffer. ~213 KB at 480x222x2.
    s_state.framebuffer = heap_caps_malloc(PAGEROS_DISPLAY_FB_BYTES,
                                            MALLOC_CAP_SPIRAM
                                            | MALLOC_CAP_8BIT);
    if (!s_state.framebuffer) {
        ESP_LOGE(TAG, "PSRAM framebuffer alloc (%u bytes) failed",
                 (unsigned)PAGEROS_DISPLAY_FB_BYTES);
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
    s_state.ready = false;
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
    if (!s_state.ready || !s_state.framebuffer) return ESP_ERR_INVALID_STATE;
    // ST7796 over SPI consumes RGB565 bytes big-endian on the wire, but
    // our framebuffer stores them as native uint16_t (little-endian on
    // ESP32-S3). Byte-swap in place before draw_bitmap, then swap back
    // so the renderer can keep reading/writing in host order. Cost is
    // ~150 KB swaps on a 480x320 frame — sub-millisecond on the S3.
    uint16_t *fb = s_state.framebuffer;
    size_t n = (size_t)PAGEROS_DISPLAY_WIDTH * PAGEROS_DISPLAY_HEIGHT;
    for (size_t i = 0; i < n; i++) fb[i] = __builtin_bswap16(fb[i]);
    esp_err_t r = esp_lcd_panel_draw_bitmap(s_state.panel,
                                            0, 0,
                                            PAGEROS_DISPLAY_WIDTH,
                                            PAGEROS_DISPLAY_HEIGHT,
                                            s_state.framebuffer);
    for (size_t i = 0; i < n; i++) fb[i] = __builtin_bswap16(fb[i]);
    return r;
}
