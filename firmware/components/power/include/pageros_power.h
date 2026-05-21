// SPDX-License-Identifier: Apache-2.0
//
// PagerOS power management — FW-035.
//
// State machine per SPEC §7.5:
//
//   ACTIVE    full UI, full backlight (display backlight = 100%)
//      ↓  30 s no input
//   DIM       reduce backlight (~25%), CPU stays awake
//      ↓  60 s no input
//   SCREEN_OFF  backlight 0, panel still powered, CPU awake
//      ↓  5 min no input
//   LIGHT_SLEEP  Wi-Fi off, LoRa duty-cycled (1 s on / 9 s off), CPU sleeps
//      ↓  user request (Settings → Sleep)
//   DEEP_SLEEP   only RTC alarm + power button wakes
//
// Any of {keyboard/encoder/nav input, push-relay tick, GPS fix arriving}
// resets the inactivity timer and bumps the state back toward ACTIVE.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGEROS_POWER_ACTIVE      = 0,
    PAGEROS_POWER_DIM         = 1,
    PAGEROS_POWER_SCREEN_OFF  = 2,
    PAGEROS_POWER_LIGHT_SLEEP = 3,
    PAGEROS_POWER_DEEP_SLEEP  = 4,
} pageros_power_state_t;

typedef struct {
    uint32_t dim_after_ms;          // default 30000
    uint32_t screen_off_after_ms;   // default 60000
    uint32_t light_sleep_after_ms;  // default 300000

    // Backlight duty (0..255) for ACTIVE and DIM states. Defaults
    // 255 / 64.
    uint8_t  bl_active;
    uint8_t  bl_dim;
} pageros_power_opts_t;

esp_err_t pageros_power_init(const pageros_power_opts_t *opts);

// Notify the power manager that user activity just occurred. Resets
// the inactivity timer and bumps the state back to ACTIVE (turning
// the backlight back on if it had been dimmed/off).
void pageros_power_kick(void);

pageros_power_state_t pageros_power_state(void);

// Force a transition. Used by Settings → Sleep to go straight to
// deep sleep. Caller should expect the function not to return when
// going to deep sleep.
esp_err_t pageros_power_set_state(pageros_power_state_t state);

// Diagnostics — how long since the last kick?
uint32_t pageros_power_ms_since_kick(void);

#ifdef __cplusplus
}
#endif
