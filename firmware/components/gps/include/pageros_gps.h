// SPDX-License-Identifier: Apache-2.0
//
// PagerOS GPS driver — FW-011.
//
// Drives the LILYGO T-LoRa Pager's u-blox MIA-M10Q over UART1. The module
// streams NMEA-0183 (default 38400 / 8N1) on GPIO 4 (RX) / GPIO 12 (TX);
// power and reset are gated by an XL9555 I2C GPIO expander at I2C addr
// 0x20 (SDA = GPIO 3, SCL = GPIO 2). All of that wiring is encapsulated
// inside `pageros_gps_init`.
//
// Threading model: the driver spawns one FreeRTOS task that owns the UART
// and the parser; user code interacts only through `pageros_gps_get_last_fix`
// and the optional callback. The callback runs on the GPS task — keep it
// short (post to a queue, set a flag) per FreeRTOS rules.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool      valid;          // true once we've seen at least one fix
    double    latitude_deg;   // signed decimal degrees, +N / -S
    double    longitude_deg;  // signed decimal degrees, +E / -W
    float     altitude_m;     // metres above mean sea level (NaN if unknown)
    float     accuracy_m;     // horizontal accuracy estimate, metres
                              //   approximated as HDOP × UERE (UERE ≈ 5.0 m)
                              //   while we stay on NMEA only; a future
                              //   UBX-NAV-PVT path will surface u-blox's
                              //   own `hAcc` directly.
    uint8_t   satellites;     // satellites used in the fix
    uint64_t  utc_epoch_ms;   // UTC milliseconds since 1970, 0 if unknown
} pageros_gps_fix_t;

typedef void (*pageros_gps_fix_cb_t)(const pageros_gps_fix_t *fix, void *user_ctx);

// Power the GPS on, configure UART1, and start the receive task. Returns
// ESP_OK on success, an esp_err_t otherwise. Safe to call multiple times
// (no-op after first success).
esp_err_t pageros_gps_init(void);

// Power off the GPS, stop the task, and tear down UART1. Optional —
// mostly useful for low-power experiments.
esp_err_t pageros_gps_shutdown(void);

// Copy the most recent fix into `out`. Returns ESP_ERR_INVALID_STATE if
// no fix has been observed yet (`out->valid` will also be false).
esp_err_t pageros_gps_get_last_fix(pageros_gps_fix_t *out);

// Register a callback fired on every parsed *valid* fix (typically the
// MIA-M10Q's 1 Hz cadence). Pass `cb == NULL` to clear. The callback runs
// on the GPS task; do not block, do not call printf, do not call
// pageros_gps_shutdown from inside.
esp_err_t pageros_gps_set_fix_callback(pageros_gps_fix_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
