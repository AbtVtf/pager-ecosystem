// SPDX-License-Identifier: Apache-2.0
//
// PagerOS QWERTY keyboard driver — FW-006.
//
// Hardware (LILYGO T-LoRa Pager):
//   - Controller: TI TCA8418 I2C keypad scanner at addr 0x34, on the
//     shared I2C bus (SDA = GPIO 3 / SCL = GPIO 2).
//   - INT line:   GPIO 6, active-low, asserted while events sit in the
//     TCA8418's 10-entry FIFO.
//   - Power gate: XL9555 expander pin P1.2 (bit 10), needs to be driven
//     high before the TCA8418 will respond on I2C.
//   - Matrix:     4 rows × 10 columns (40 cells; the physical PCB
//     populates fewer than 40 — unpopulated cells simply never fire).
//
// The driver surfaces raw (row, col, pressed) events on a FreeRTOS queue.
// Translating those into characters / modifier handling / repeat rate is
// the job of the upper layer (input router FW-024 + shell), exactly as
// the rotary-encoder driver (FW-007) hands off raw `pageros_input_event_t`
// values without baking in shell semantics.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  row;        // 0..PAGEROS_KBD_ROWS-1
    uint8_t  col;        // 0..PAGEROS_KBD_COLS-1
    bool     pressed;    // true on key-down, false on key-up
    int64_t  ts_us;      // esp_timer_get_time() at the moment the FIFO
                         // entry was drained (monotonic device time)
} pageros_kbd_event_t;

#define PAGEROS_KBD_ROWS 4
#define PAGEROS_KBD_COLS 10

// Bring up I2C (shared, idempotent), power the TCA8418, configure the
// 4×10 matrix, install the INT-line ISR, and spawn the drain task.
esp_err_t pageros_kbd_init(void);

// Power off the TCA8418 and tear the task down.
esp_err_t pageros_kbd_shutdown(void);

// FreeRTOS queue of `pageros_kbd_event_t`. NULL before init.
QueueHandle_t pageros_kbd_queue(void);

#ifdef __cplusplus
}
#endif
