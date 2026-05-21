// SPDX-License-Identifier: Apache-2.0
//
// PagerOS IMU driver stub — FW-012.
//
// The LILYGO T-LoRa Pager ships a Bosch BHI260AP "Smart Sensor Hub" on
// the shared I2C bus (SDA=3, SCL=2, address 0x28). The BHI260AP requires
// a multi-tens-of-kB firmware blob to be uploaded over I2C before it
// emits sensor events; PagerOS v1 does not exercise that path. Per
// TASKS.md FW-012 "No event dispatch in v1 (reserved for v2)" we only
// need to:
//   * confirm the chip is present (chip-id read), and
//   * expose a raw register-read API so future v2 work — fusion firmware
//     upload, step counter, orientation events — can land without
//     touching the bus plumbing.
//
// On hardware without an IMU populated, `pageros_imu_init` returns
// ESP_ERR_NOT_FOUND and `pageros_imu_present()` returns false; callers
// degrade gracefully.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// BHI260AP product identifier observed at register 0x2B. Documented in
// the Bosch BHy 1.x datasheet §5 "Chip Identification".
#define PAGEROS_IMU_CHIP_ID_BHI260AP  0x89

// Probe the chip over I2C. Bus init is idempotent (matches the
// keyboard / GPS / storage pattern). Returns:
//   ESP_OK                 — chip present, chip-id matches BHI260AP
//   ESP_ERR_NOT_FOUND      — no I2C ACK at 0x28
//   ESP_ERR_INVALID_RESPONSE — chip ACKs but reports unexpected ID
//   other esp_err_t         — surfaced I2C bring-up errors
esp_err_t pageros_imu_init(void);

// True iff a successful `pageros_imu_init` call has happened in this
// boot. Used by the shell to grey out the "Orientation" settings group.
bool pageros_imu_present(void);

// Cached chip ID from the most recent successful init. 0 before init.
uint8_t pageros_imu_chip_id(void);

// Raw I2C register read passthrough. Reads `len` bytes starting at
// `reg` into `buf`. Returns ESP_ERR_INVALID_STATE if the IMU was not
// detected at init. Used by future firmware-upload + sensor-event
// code; in v1 no other component should call this.
esp_err_t pageros_imu_read(uint8_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
