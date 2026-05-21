// SPDX-License-Identifier: Apache-2.0
//
// PagerOS GPS driver — see pageros_gps.h for the public contract.
//
// Responsibilities:
//   1. Power the GPS via the XL9555 I2C GPIO expander (LILYGO T-LoRa Pager
//      ships the MIA-M10Q gated through expander GPIOs 4 (EN) and 7 (RST)).
//   2. Open UART1 on RX = GPIO 4 / TX = GPIO 12 at 38400 8N1 (the M10
//      datasheet default).
//   3. Spawn a task that pumps UART bytes through the NMEA parser and
//      publishes complete fixes to a mutex-protected last-fix slot and
//      the optional user callback.
//
// The XL9555 helper is intentionally tiny and inlined here — the rest of
// the driver-bring-up tasks (FW-008 LoRa, FW-010 NFC, FW-013 audio,
// FW-014 secure element) will need the same primitive, at which point
// this should be lifted to a shared `firmware/components/io_expander/`
// module. For now FW-011 is the first consumer, so keeping it local
// avoids designing the shared API on speculation.

#include "pageros_gps.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nmea.h"

static const char *TAG = "gps";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager, ESP32-S3)
// ---------------------------------------------------------------------------

#define GPS_UART_NUM           UART_NUM_1
#define GPS_UART_RX_GPIO       4
#define GPS_UART_TX_GPIO       12
#define GPS_UART_BAUD          38400
#define GPS_UART_BUF_BYTES     1024

#define GPS_I2C_PORT           I2C_NUM_0
#define GPS_I2C_SDA_GPIO       3
#define GPS_I2C_SCL_GPIO       2
#define GPS_I2C_FREQ_HZ        400000

#define XL9555_I2C_ADDR        0x20
#define XL9555_REG_OUTPUT_0    0x02   // output port 0 (P0_0..P0_7)
#define XL9555_REG_CONFIG_0    0x06   // direction port 0 (1 = input)
#define XL9555_BIT_GPS_EN      (1u << 4)   // expander P0.4 — power gate
#define XL9555_BIT_GPS_RST     (1u << 7)   // expander P0.7 — active-low reset

// Approximate user equivalent range error (m) for a consumer GNSS receiver
// under open sky. Multiplied by HDOP this gives a usable horizontal
// accuracy estimate while we stay on NMEA only; a future UBX-NAV-PVT
// path will surface u-blox's own `hAcc`.
#define GPS_UERE_M             5.0f

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

static struct {
    bool                  initialised;
    TaskHandle_t          task;
    volatile bool         shutdown_requested;
    SemaphoreHandle_t     fix_lock;
    pageros_gps_fix_t     last_fix;
    pageros_gps_fix_cb_t  user_cb;
    void                 *user_ctx;
} s_state;

// ---------------------------------------------------------------------------
// XL9555 helpers (minimal — single-byte register reads/writes)
// ---------------------------------------------------------------------------

static esp_err_t xl9555_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(GPS_I2C_PORT, XL9555_I2C_ADDR,
                                        &reg, 1, value, 1,
                                        pdMS_TO_TICKS(50));
}

static esp_err_t xl9555_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(GPS_I2C_PORT, XL9555_I2C_ADDR,
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

// Bring the GPS out of reset and feed it power. Sequence per u-blox M10
// datasheet §1.4 — assert RST low, drop power, wait, raise power, release
// reset, wait for the receiver firmware to come up before opening UART.
static esp_err_t xl9555_power_up_gps(void)
{
    // Configure both bits as outputs (XL9555 reset state: all inputs).
    esp_err_t err = xl9555_set_bits(XL9555_REG_CONFIG_0,
                                    XL9555_BIT_GPS_EN | XL9555_BIT_GPS_RST,
                                    0);
    if (err != ESP_OK) return err;

    // Drop EN and assert RST (active-low) to fully reset the module.
    err = xl9555_set_bits(XL9555_REG_OUTPUT_0,
                          XL9555_BIT_GPS_EN | XL9555_BIT_GPS_RST,
                          0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Power the module.
    err = xl9555_set_bits(XL9555_REG_OUTPUT_0, XL9555_BIT_GPS_EN,
                          XL9555_BIT_GPS_EN);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Release reset.
    err = xl9555_set_bits(XL9555_REG_OUTPUT_0, XL9555_BIT_GPS_RST,
                          XL9555_BIT_GPS_RST);
    if (err != ESP_OK) return err;

    // u-blox boot to "NMEA streaming" takes ~150 ms after reset release.
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

static esp_err_t xl9555_power_down_gps(void)
{
    return xl9555_set_bits(XL9555_REG_OUTPUT_0, XL9555_BIT_GPS_EN, 0);
}

// ---------------------------------------------------------------------------
// I2C + UART bring-up
// ---------------------------------------------------------------------------

static esp_err_t init_i2c_once(void)
{
    static bool i2c_up = false;
    if (i2c_up) return ESP_OK;

    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = GPS_I2C_SDA_GPIO,
        .scl_io_num       = GPS_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = GPS_I2C_FREQ_HZ,
    };
    // Try install first. ESP_OK = we installed; ESP_ERR_INVALID_STATE
    // and ESP_FAIL both mean the driver is already up via another
    // component (i2c_bus has installed it for us in IDF v5.3, which
    // returns ESP_FAIL on legacy re-install attempts rather than the
    // older ESP_ERR_INVALID_STATE). Accept either; the GPS driver
    // really uses I2C only to bring the XL9555 enable line up, which
    // main.c already did before us.
    esp_err_t err = i2c_driver_install(GPS_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_OK) {
        err = i2c_param_config(GPS_I2C_PORT, &cfg);
        if (err != ESP_OK) return err;
    } else if (err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        return err;
    }
    i2c_up = true;
    return ESP_OK;
}

static esp_err_t init_uart(void)
{
    uart_config_t cfg = {
        .baud_rate           = GPS_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(GPS_UART_NUM, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(GPS_UART_NUM,
                       GPS_UART_TX_GPIO, GPS_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    return uart_driver_install(GPS_UART_NUM,
                               GPS_UART_BUF_BYTES, 0,
                               0, NULL, 0);
}

// ---------------------------------------------------------------------------
// Reader task — pumps UART → parser → last_fix slot + callback.
// ---------------------------------------------------------------------------

typedef struct {
    pageros_gps_fix_t  acc;
    bool               dirty;
    uint32_t           sentences_total;   // raw NMEA lines emitted by stream
    uint32_t           sentences_used;    // RMC/GGA we actually parsed
} fix_accum_t;

static void on_sentence(const char *line, size_t len, void *user)
{
    fix_accum_t *acc = (fix_accum_t *)user;
    acc->sentences_total++;
    nmea_update_t up;
    if (!nmea_parse_sentence(line, len, &up)) return;
    acc->sentences_used++;

    if (up.has_position) {
        acc->acc.latitude_deg  = up.latitude_deg;
        acc->acc.longitude_deg = up.longitude_deg;
    }
    if (up.has_altitude)   acc->acc.altitude_m   = up.altitude_m;
    if (up.has_satellites) acc->acc.satellites   = up.satellites;
    if (up.has_utc)        acc->acc.utc_epoch_ms = up.utc_epoch_ms;
    if (up.has_hdop)       acc->acc.accuracy_m   = up.hdop * GPS_UERE_M;

    // Only flip `valid` true on the back of an explicit RMC/GGA validity
    // signal — never on stale residual data.
    if (up.valid && up.has_position) {
        acc->acc.valid = true;
        acc->dirty     = true;
    } else if (acc->acc.valid && up.has_position) {
        // already valid; treat further fixes with position as updates
        acc->dirty = true;
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    nmea_stream_t stream;
    nmea_stream_init(&stream);

    fix_accum_t acc = {0};

    // Heartbeat log: every 10s emit a one-line "still alive" stat with
    // bytes received and last-fix-age so an operator can tell at a glance
    // whether the UART is wired up but the receiver hasn't locked yet
    // (a chronic source of mystery in field deployments).
    uint64_t bytes_total = 0;
    TickType_t last_log_tick = xTaskGetTickCount();

    uint8_t rx[256];
    while (!s_state.shutdown_requested) {
        int n = uart_read_bytes(GPS_UART_NUM, rx, sizeof(rx),
                                pdMS_TO_TICKS(100));
        if (n > 0) {
            bytes_total += (uint64_t)n;
            nmea_stream_push(&stream, rx, (size_t)n, on_sentence, &acc);
        }
        if (acc.dirty) {
            acc.dirty = false;
            if (xSemaphoreTake(s_state.fix_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_state.last_fix = acc.acc;
                xSemaphoreGive(s_state.fix_lock);
            }
            if (s_state.user_cb) {
                s_state.user_cb(&acc.acc, s_state.user_ctx);
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(10000)) {
            last_log_tick = now;
            ESP_LOGI(TAG,
                     "stats: rx=%llu bytes, lines=%lu, rmc/gga=%lu, fix=%s",
                     (unsigned long long)bytes_total,
                     (unsigned long)acc.sentences_total,
                     (unsigned long)acc.sentences_used,
                     acc.acc.valid ? "yes" : "no");
        }
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t pageros_gps_init(void)
{
    if (s_state.initialised) return ESP_OK;

    s_state.fix_lock = xSemaphoreCreateMutex();
    if (!s_state.fix_lock) return ESP_ERR_NO_MEM;
    memset(&s_state.last_fix, 0, sizeof(s_state.last_fix));
    s_state.last_fix.altitude_m = NAN;
    s_state.last_fix.accuracy_m = NAN;

    esp_err_t err = init_i2c_once();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c init: %s", esp_err_to_name(err)); return err; }

    err = xl9555_power_up_gps();
    if (err != ESP_OK) { ESP_LOGE(TAG, "gps power-up: %s", esp_err_to_name(err)); return err; }

    err = init_uart();
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart init: %s", esp_err_to_name(err)); return err; }

    s_state.shutdown_requested = false;
    BaseType_t ok = xTaskCreate(gps_task, "gps_rx", 4096, NULL, 5, &s_state.task);
    if (ok != pdPASS) { ESP_LOGE(TAG, "task create failed"); return ESP_ERR_NO_MEM; }

    s_state.initialised = true;
    ESP_LOGI(TAG, "GPS up (MIA-M10Q @ %d baud on UART%d)", GPS_UART_BAUD, GPS_UART_NUM);
    return ESP_OK;
}

esp_err_t pageros_gps_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;
    s_state.shutdown_requested = true;
    // The task self-deletes; wait a couple of poll periods then tear down.
    vTaskDelay(pdMS_TO_TICKS(250));
    uart_driver_delete(GPS_UART_NUM);
    xl9555_power_down_gps();
    if (s_state.fix_lock) {
        vSemaphoreDelete(s_state.fix_lock);
        s_state.fix_lock = NULL;
    }
    s_state.initialised = false;
    return ESP_OK;
}

esp_err_t pageros_gps_get_last_fix(pageros_gps_fix_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialised || !s_state.fix_lock) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state.fix_lock, portMAX_DELAY);
    *out = s_state.last_fix;
    xSemaphoreGive(s_state.fix_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t pageros_gps_set_fix_callback(pageros_gps_fix_cb_t cb, void *user_ctx)
{
    s_state.user_cb  = cb;
    s_state.user_ctx = user_ctx;
    return ESP_OK;
}
