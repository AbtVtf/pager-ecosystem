// SPDX-License-Identifier: Apache-2.0
//
// PagerOS IMU stub — see `pageros_imu.h`.

#include "pageros_imu.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager — shared I2C with keyboard, GPS, etc.)
// ---------------------------------------------------------------------------

#define IMU_I2C_PORT        I2C_NUM_0
#define IMU_I2C_SDA_GPIO    3
#define IMU_I2C_SCL_GPIO    2
#define IMU_I2C_FREQ_HZ     400000
// BHI260AP I2C address: tried both 0x28 (returns 0x70 — wrong chip
// answering, likely the PCF85063 RTC at 0x51 reflecting register
// noise) and 0x29 (no ACK). The BHI260's actual presence on this
// particular board revision is uncertain; full bring-up (firmware
// upload via SPI patch RAM) was scoped to v2 anyway. Probe stays at
// 0x28 so we surface the wrong-id warning instead of a hard ACK fail.
#define IMU_I2C_ADDR        0x28

// BHI260AP chip-id register. Documented at 0x2B in the Bosch BHy 1.x
// host interface specification. Before firmware upload this is the only
// register that reliably returns a fixed value.
#define IMU_REG_CHIP_ID     0x2B

static struct {
    bool    initialised;
    bool    present;
    uint8_t chip_id;
} s_state;

static esp_err_t i2c_init_once(void)
{
    static bool i2c_up = false;
    if (i2c_up) return ESP_OK;

    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = IMU_I2C_SDA_GPIO,
        .scl_io_num       = IMU_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = IMU_I2C_FREQ_HZ,
    };
    // Try install first; if another component (keyboard / GPS /
    // storage / audio) beat us to it, IDF v5.3 returns ESP_FAIL while
    // older versions return ESP_ERR_INVALID_STATE. Either way the bus
    // is up and we skip param_config so we don't disturb its settings.
    esp_err_t err = i2c_driver_install(IMU_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_OK) {
        err = i2c_param_config(IMU_I2C_PORT, &cfg);
        if (err != ESP_OK) return err;
    } else if (err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        return err;
    }
    i2c_up = true;
    return ESP_OK;
}

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(IMU_I2C_PORT, IMU_I2C_ADDR,
                                         &reg, 1, buf, len,
                                         pdMS_TO_TICKS(50));
}

esp_err_t pageros_imu_init(void)
{
    if (s_state.initialised) return s_state.present ? ESP_OK
                                                     : ESP_ERR_NOT_FOUND;

    esp_err_t err = i2c_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id = 0;
    err = i2c_read_reg(IMU_REG_CHIP_ID, &id, 1);
    s_state.initialised = true;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02x: %s", IMU_I2C_ADDR,
                 esp_err_to_name(err));
        s_state.present = false;
        return ESP_ERR_NOT_FOUND;
    }

    s_state.chip_id = id;
    if (id != PAGEROS_IMU_CHIP_ID_BHI260AP) {
        ESP_LOGW(TAG, "unexpected chip id 0x%02x (want 0x%02x)",
                 id, PAGEROS_IMU_CHIP_ID_BHI260AP);
        s_state.present = false;
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_state.present = true;
    ESP_LOGI(TAG, "BHI260AP present at 0x%02x (chip-id 0x%02x); "
                  "firmware upload deferred to v2", IMU_I2C_ADDR, id);
    return ESP_OK;
}

bool pageros_imu_present(void)
{
    return s_state.present;
}

uint8_t pageros_imu_chip_id(void)
{
    return s_state.chip_id;
}

esp_err_t pageros_imu_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_state.present) return ESP_ERR_INVALID_STATE;
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    return i2c_read_reg(reg, buf, len);
}
