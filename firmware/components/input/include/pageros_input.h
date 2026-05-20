// SPDX-License-Identifier: Apache-2.0
//
// PagerOS rotary-encoder + ENTER + BACK driver — FW-007.
//
// LILYGO T-LoRa Pager wiring:
//   - Rotary A   = GPIO 40   (direct ESP32-S3 GPIO)
//   - Rotary B   = GPIO 41
//   - Rotary push (ENTER) = GPIO 7
//   - BOOT button (BACK)  = GPIO 0
//   All four are active-low with the built-in pull-ups.
//
// The PagerOS spec calls out "ENTER/BACK" as part of the input driver
// (TASKS.md §FW-007 / SPEC.md §4 Hardware capabilities). The T-LoRa Pager
// PCB has no dedicated BACK button, so BACK is bound to the BOOT button —
// the same key the bootloader uses for download mode, which also doubles
// as the front-panel "cancel" while user firmware is running. SPEC §7.3
// (Shell) requires a long-press distinguishment ("Hold BACK = force-quit"),
// so the driver also surfaces a `BACK_LONG` event after a ≥ 1 s hold.
//
// Threading: callers consume `pageros_input_event_t` values from
// `pageros_input_queue()` which is a standard FreeRTOS queue. The
// producer is a single internal task — safe to share among multiple
// consumers via xQueuePeek if needed.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGEROS_INPUT_NONE = 0,
    PAGEROS_INPUT_ROTARY_CW,   // one detent clockwise
    PAGEROS_INPUT_ROTARY_CCW,  // one detent counter-clockwise
    PAGEROS_INPUT_ENTER,       // rotary push released (after debounce)
    PAGEROS_INPUT_BACK,        // BOOT button released, short press (< 1 s)
    PAGEROS_INPUT_BACK_LONG,   // BOOT button held ≥ 1 s then released
} pageros_input_event_t;

// Bring up GPIO + the input task. Safe to call more than once (no-op after
// first success). Returns ESP_OK or an esp_err_t.
esp_err_t pageros_input_init(void);

// Tear down the task and free GPIO. Mostly for tests and low-power flows.
esp_err_t pageros_input_shutdown(void);

// Returns the FreeRTOS queue holding `pageros_input_event_t` values.
// NULL before `pageros_input_init` has succeeded.
QueueHandle_t pageros_input_queue(void);

// Convenience: block up to `timeout_ms` for the next event. Returns
// PAGEROS_INPUT_NONE on timeout / shutdown.
pageros_input_event_t pageros_input_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
