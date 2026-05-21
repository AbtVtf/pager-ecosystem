// SPDX-License-Identifier: Apache-2.0
//
// Shared I2C bus for the LILYGO T-LoRa Pager.
//
// Per the variant header (BHI260, PCF85063, BQ25896, DRV2605L, ES8311
// share an I2C bus): SDA=3, SCL=2. The legacy ESP-IDF i2c driver only
// lets one component call `i2c_driver_install` per port; this thin
// wrapper centralises the install so every consumer (XL9555 expander,
// IMU, ES8311 codec, RTC, charger, haptics) gets a working handle
// without fighting over the install slot.

#pragma once

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_I2C_PORT       I2C_NUM_0
#define PAGEROS_I2C_SDA_GPIO   3
#define PAGEROS_I2C_SCL_GPIO   2
#define PAGEROS_I2C_FREQ_HZ    400000

// Idempotent — safe to call from every consumer; only the first call
// actually installs the driver.
esp_err_t pageros_i2c_bus_init(void);

// Convenience: write `len` bytes to `addr`, then read `read_len` back.
// Either half may be omitted with len/read_len == 0.
esp_err_t pageros_i2c_write_read(uint8_t addr,
                                 const uint8_t *write_buf, size_t write_len,
                                 uint8_t *read_buf, size_t read_len,
                                 uint32_t timeout_ms);

esp_err_t pageros_i2c_write(uint8_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms);
esp_err_t pageros_i2c_read(uint8_t addr, uint8_t *buf, size_t len, uint32_t timeout_ms);

// Register-level helpers (common pattern for these chips):
//   write reg id, then read N bytes back.
esp_err_t pageros_i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t pageros_i2c_reg_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);
esp_err_t pageros_i2c_reg_write8(uint8_t addr, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif
