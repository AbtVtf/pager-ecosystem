// SPDX-License-Identifier: Apache-2.0

#include "pageros_i2c_bus.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2c_bus";
static bool g_installed = false;

esp_err_t pageros_i2c_bus_init(void)
{
    if (g_installed) return ESP_OK;
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PAGEROS_I2C_SDA_GPIO,
        .scl_io_num = PAGEROS_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PAGEROS_I2C_FREQ_HZ,
    };
    esp_err_t r = i2c_param_config(PAGEROS_I2C_PORT, &cfg);
    if (r != ESP_OK) return r;
    r = i2c_driver_install(PAGEROS_I2C_PORT, cfg.mode, 0, 0, 0);
    if (r == ESP_ERR_INVALID_STATE) {
        // Already installed by someone else — accept it.
        g_installed = true;
        return ESP_OK;
    }
    if (r != ESP_OK) return r;
    g_installed = true;
    ESP_LOGI(TAG, "I2C0 up: SDA=%d SCL=%d %u Hz",
             PAGEROS_I2C_SDA_GPIO, PAGEROS_I2C_SCL_GPIO,
             (unsigned)PAGEROS_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t pageros_i2c_write_read(uint8_t addr,
                                 const uint8_t *write_buf, size_t write_len,
                                 uint8_t *read_buf, size_t read_len,
                                 uint32_t timeout_ms)
{
    if (write_len == 0 && read_len == 0) return ESP_ERR_INVALID_ARG;
    TickType_t to = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 100);
    if (write_len > 0 && read_len > 0) {
        return i2c_master_write_read_device(PAGEROS_I2C_PORT, addr,
                                            write_buf, write_len,
                                            read_buf, read_len, to);
    }
    if (write_len > 0) {
        return i2c_master_write_to_device(PAGEROS_I2C_PORT, addr,
                                          write_buf, write_len, to);
    }
    return i2c_master_read_from_device(PAGEROS_I2C_PORT, addr,
                                       read_buf, read_len, to);
}

esp_err_t pageros_i2c_write(uint8_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{ return pageros_i2c_write_read(addr, buf, len, NULL, 0, timeout_ms); }

esp_err_t pageros_i2c_read(uint8_t addr, uint8_t *buf, size_t len, uint32_t timeout_ms)
{ return pageros_i2c_write_read(addr, NULL, 0, buf, len, timeout_ms); }

esp_err_t pageros_i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{ return pageros_i2c_write_read(addr, &reg, 1, buf, len, 100); }

esp_err_t pageros_i2c_reg_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    if (len > 32) return ESP_ERR_INVALID_SIZE;
    uint8_t tmp[33];
    tmp[0] = reg;
    for (size_t i = 0; i < len; i++) tmp[1 + i] = buf[i];
    return pageros_i2c_write(addr, tmp, 1 + len, 100);
}

esp_err_t pageros_i2c_reg_write8(uint8_t addr, uint8_t reg, uint8_t value)
{ return pageros_i2c_reg_write(addr, reg, &value, 1); }
