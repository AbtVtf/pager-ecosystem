// SPDX-License-Identifier: Apache-2.0
//
// PagerOS QWERTY keyboard driver — see pageros_keyboard.h.
//
// Bring-up sequence:
//   1. Bring up the shared I2C bus if no one else has (idempotent).
//   2. Drive the XL9555 expander pin P1.2 high to power the TCA8418.
//   3. Configure the TCA8418's KP_GPIO_{1,2,3} registers so rows 0..3
//      and cols 0..9 are routed as the matrix. Enable CFG.KE_IEN +
//      CFG.AI so it asserts INT and we can stream-read KEY_EVENT_A.
//   4. Install an ANYEDGE GPIO ISR on the INT line (GPIO 6, active-low).
//      The ISR notifies a normal-priority task which drains the FIFO,
//      decodes each event byte through `tca8418_decode_event`, and posts
//      `pageros_kbd_event_t` values to the public queue.
//
// We keep the XL9555 helpers inline (same as gps.c) because no other
// driver has needed them yet; once FW-008/010/013 land they should all
// move into a shared `firmware/components/io_expander/` module.

#include "pageros_keyboard.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tca8418.h"

static const char *TAG = "kbd";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager)
// ---------------------------------------------------------------------------

#define KBD_INT_GPIO            6

#define KBD_I2C_PORT            I2C_NUM_0
#define KBD_I2C_SDA_GPIO        3
#define KBD_I2C_SCL_GPIO        2
#define KBD_I2C_FREQ_HZ         400000

#define XL9555_I2C_ADDR         0x20
#define XL9555_REG_OUTPUT_1     0x03    // P1.x output port
#define XL9555_REG_CONFIG_1     0x07    // P1.x direction (1 = input)
#define XL9555_BIT_KBD_PWR      (1u << 2)   // P1.2

#define PUBLIC_QUEUE_DEPTH      32

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

static struct {
    bool                  initialised;
    QueueHandle_t         pub_q;
    TaskHandle_t          task;
    SemaphoreHandle_t     int_signal;
    volatile bool         shutdown;
} s_state;

// ---------------------------------------------------------------------------
// I2C / XL9555 helpers
// ---------------------------------------------------------------------------

static esp_err_t init_i2c_once(void)
{
    static bool i2c_up = false;
    if (i2c_up) return ESP_OK;
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = KBD_I2C_SDA_GPIO,
        .scl_io_num       = KBD_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = KBD_I2C_FREQ_HZ,
    };
    // Try to install first. If another component (gps.c) beat us to it
    // ESP-IDF returns ESP_FAIL (v5.3 used to return INVALID_STATE; the
    // implementation flipped quietly). Either way the bus is up and we
    // skip param_config so we don't disturb the active configuration.
    esp_err_t err = i2c_driver_install(KBD_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_OK) {
        err = i2c_param_config(KBD_I2C_PORT, &cfg);
        if (err != ESP_OK) return err;
    } else if (err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        return err;
    }
    i2c_up = true;
    return ESP_OK;
}

static esp_err_t xl9555_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(KBD_I2C_PORT, XL9555_I2C_ADDR,
                                        &reg, 1, value, 1,
                                        pdMS_TO_TICKS(50));
}
static esp_err_t xl9555_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(KBD_I2C_PORT, XL9555_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(50));
}
static esp_err_t xl9555_set_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;
    esp_err_t err = xl9555_read(reg, &cur);
    if (err != ESP_OK) return err;
    cur = (uint8_t)((cur & ~mask) | (value & mask));
    return xl9555_write(reg, cur);
}

static esp_err_t xl9555_power_up_keyboard(void)
{
    esp_err_t err = xl9555_set_bits(XL9555_REG_CONFIG_1,
                                    XL9555_BIT_KBD_PWR, 0);
    if (err != ESP_OK) return err;
    err = xl9555_set_bits(XL9555_REG_OUTPUT_1,
                          XL9555_BIT_KBD_PWR, XL9555_BIT_KBD_PWR);
    if (err != ESP_OK) return err;
    // TCA8418 needs ~20 ms to settle after power-on per datasheet §8.4.
    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// TCA8418 I2C helpers
// ---------------------------------------------------------------------------

static esp_err_t tca_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(KBD_I2C_PORT, TCA8418_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(50));
}

static esp_err_t tca_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(KBD_I2C_PORT, TCA8418_I2C_ADDR,
                                        &reg, 1, value, 1,
                                        pdMS_TO_TICKS(50));
}

static esp_err_t tca_init_matrix(void)
{
    // Route rows 0..3 and cols 0..9 to the keypad scanner.
    esp_err_t err;
    if ((err = tca_write(TCA8418_REG_KP_GPIO_1, 0x0F)) != ESP_OK) return err;
    if ((err = tca_write(TCA8418_REG_KP_GPIO_2, 0xFF)) != ESP_OK) return err;
    if ((err = tca_write(TCA8418_REG_KP_GPIO_3, 0x03)) != ESP_OK) return err;

    // Clear any stale FIFO entries from a prior boot.
    for (int i = 0; i < 16; i++) {
        uint8_t raw = 0;
        if (tca_read(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) break;
        if (raw == 0) break;
    }

    // Clear stale interrupt flags.
    if ((err = tca_write(TCA8418_REG_INT_STAT, 0x0F)) != ESP_OK) return err;

    // Enable key-event interrupt + auto-increment FIFO read pointer.
    return tca_write(TCA8418_REG_CFG,
                     TCA8418_CFG_KE_IEN | TCA8418_CFG_AI);
}

// ---------------------------------------------------------------------------
// ISR + drain task
// ---------------------------------------------------------------------------

static void IRAM_ATTR int_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_state.int_signal, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

static void drain_fifo(void)
{
    // Read up to FIFO depth (10) events per pass; the chip serves zeros
    // for empty slots so this terminates naturally.
    for (int i = 0; i < 16; i++) {
        uint8_t raw = 0;
        if (tca_read(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) return;
        if (raw == 0) break;
        tca8418_event_t ev = tca8418_decode_event(raw);
        if (!ev.valid) continue;
        if (ev.row >= PAGEROS_KBD_ROWS || ev.col >= PAGEROS_KBD_COLS) {
            // Out-of-matrix events shouldn't happen with KP_GPIO_{1,2,3}
            // configured to a 4×10 region, but if they do we drop them
            // rather than push garbage at the upper layer.
            continue;
        }
        pageros_kbd_event_t out = {
            .row     = ev.row,
            .col     = ev.col,
            .pressed = ev.pressed,
            .ts_us   = esp_timer_get_time(),
        };
        (void)xQueueSend(s_state.pub_q, &out, 0);
    }
    // Clear K_INT so the INT line releases.
    (void)tca_write(TCA8418_REG_INT_STAT, TCA8418_INT_K_INT);
}

static void kbd_task(void *arg)
{
    (void)arg;
    // Safety drain on entry in case any events arrived between init and
    // task spawn.
    drain_fifo();

    while (!s_state.shutdown) {
        // 100 ms timeout doubles as a polling fallback: if the INT
        // somehow doesn't fire (e.g. we missed the falling edge during
        // init), we still drain on the next tick.
        if (xSemaphoreTake(s_state.int_signal, pdMS_TO_TICKS(100)) == pdTRUE
            || gpio_get_level(KBD_INT_GPIO) == 0) {
            drain_fifo();
        }
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t pageros_kbd_init(void)
{
    if (s_state.initialised) return ESP_OK;

    s_state.pub_q      = xQueueCreate(PUBLIC_QUEUE_DEPTH, sizeof(pageros_kbd_event_t));
    s_state.int_signal = xSemaphoreCreateBinary();
    if (!s_state.pub_q || !s_state.int_signal) return ESP_ERR_NO_MEM;

    esp_err_t err = init_i2c_once();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c: %s", esp_err_to_name(err)); return err; }

    err = xl9555_power_up_keyboard();
    if (err != ESP_OK) { ESP_LOGE(TAG, "kbd power-up: %s", esp_err_to_name(err)); return err; }

    err = tca_init_matrix();
    if (err != ESP_OK) { ESP_LOGE(TAG, "tca8418 init: %s", esp_err_to_name(err)); return err; }

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << KBD_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    if ((err = gpio_config(&int_cfg)) != ESP_OK) return err;

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if ((err = gpio_isr_handler_add(KBD_INT_GPIO, int_isr, NULL)) != ESP_OK) return err;

    s_state.shutdown = false;
    BaseType_t ok = xTaskCreate(kbd_task, "kbd", 3072, NULL, 6, &s_state.task);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    s_state.initialised = true;
    ESP_LOGI(TAG, "TCA8418 keyboard up: 4x10 matrix, INT on GPIO %d", KBD_INT_GPIO);
    return ESP_OK;
}

esp_err_t pageros_kbd_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;
    s_state.shutdown = true;
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_isr_handler_remove(KBD_INT_GPIO);
    (void)xl9555_set_bits(XL9555_REG_OUTPUT_1, XL9555_BIT_KBD_PWR, 0);
    vQueueDelete(s_state.pub_q);
    vSemaphoreDelete(s_state.int_signal);
    s_state.pub_q = NULL;
    s_state.int_signal = NULL;
    s_state.initialised = false;
    return ESP_OK;
}

QueueHandle_t pageros_kbd_queue(void)
{
    return s_state.pub_q;
}
