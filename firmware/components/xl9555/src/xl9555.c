// SPDX-License-Identifier: Apache-2.0
//
// XL9555 driver. Register map (XL9555 / PCA9555-compatible):
//
//   0x00, 0x01  Input port 0/1     (read)
//   0x02, 0x03  Output port 0/1    (read/write — last driven value)
//   0x04, 0x05  Polarity inversion 0/1
//   0x06, 0x07  Configuration 0/1  (1 = input, 0 = output)

#include "pageros_xl9555.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pageros_i2c_bus.h"

static const char *TAG = "xl9555";

#define REG_INPUT_0    0x00
#define REG_OUTPUT_0   0x02
#define REG_CONFIG_0   0x06

static struct {
    bool ok;
    uint16_t output;     // shadow of OUTPUT registers
    uint16_t config;     // shadow of CONFIG (1 bit = input)
} g;

static esp_err_t write16(uint8_t base_reg, uint16_t v)
{
    uint8_t buf[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    return pageros_i2c_reg_write(PAGEROS_XL9555_I2C_ADDR, base_reg, buf, 2);
}

static esp_err_t read16(uint8_t base_reg, uint16_t *out)
{
    uint8_t buf[2] = {0, 0};
    esp_err_t r = pageros_i2c_reg_read(PAGEROS_XL9555_I2C_ADDR, base_reg, buf, 2);
    if (r == ESP_OK) *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return r;
}

esp_err_t pageros_xl9555_init(void)
{
    if (g.ok) return ESP_OK;
    esp_err_t r = pageros_i2c_bus_init();
    if (r != ESP_OK) return r;

    // Verify the chip is there by reading the input port (any value is
    // fine — we just need the bus ACK).
    uint16_t probe = 0;
    r = read16(REG_INPUT_0, &probe);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "XL9555 not detected at 0x%02X: %s",
                 PAGEROS_XL9555_I2C_ADDR, esp_err_to_name(r));
        return r;
    }

    // Default state: everything an OUTPUT and driven LOW (peripherals off).
    // The SD_DET bit (10) we leave as input — it's a strap from the card slot.
    g.config = (uint16_t)(1u << PAGEROS_XL_SD_DET);
    g.output = 0;
    r = write16(REG_OUTPUT_0, g.output);
    if (r != ESP_OK) return r;
    r = write16(REG_CONFIG_0, g.config);
    if (r != ESP_OK) return r;

    g.ok = true;
    ESP_LOGI(TAG, "XL9555 up at 0x%02X (input=0x%04X)", PAGEROS_XL9555_I2C_ADDR, probe);
    return ESP_OK;
}

bool pageros_xl9555_present(void) { return g.ok; }

esp_err_t pageros_xl9555_set(pageros_xl_pin_t pin, bool level)
{
    if (!g.ok) return ESP_ERR_INVALID_STATE;
    uint16_t bit = 1u << (int)pin;
    if (level) g.output |= bit; else g.output &= ~bit;
    return write16(REG_OUTPUT_0, g.output);
}

esp_err_t pageros_xl9555_get(pageros_xl_pin_t pin, bool *out_level)
{
    if (!g.ok) return ESP_ERR_INVALID_STATE;
    if (!out_level) return ESP_ERR_INVALID_ARG;
    uint16_t v = 0;
    esp_err_t r = read16(REG_INPUT_0, &v);
    if (r != ESP_OK) return r;
    *out_level = (v & (1u << (int)pin)) != 0;
    return ESP_OK;
}

esp_err_t pageros_xl9555_set_dir(pageros_xl_pin_t pin, bool is_input)
{
    if (!g.ok) return ESP_ERR_INVALID_STATE;
    uint16_t bit = 1u << (int)pin;
    if (is_input) g.config |= bit; else g.config &= ~bit;
    return write16(REG_CONFIG_0, g.config);
}
