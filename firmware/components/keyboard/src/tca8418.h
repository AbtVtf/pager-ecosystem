// SPDX-License-Identifier: Apache-2.0
//
// TI TCA8418 keypad scanner — register addresses and FIFO-event decoder.
//
// Pure-C, no ESP-IDF symbols. Host tests under
// `firmware/host-tests/keyboard/` link this translation unit with
// native gcc.
//
// Datasheet: https://www.ti.com/lit/ds/symlink/tca8418.pdf (Aug 2015)

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default 7-bit I2C address. The TCA8418 only supports this address.
#define TCA8418_I2C_ADDR        0x34

// Register map (subset — only what the driver writes/reads).
#define TCA8418_REG_CFG         0x01
#define TCA8418_REG_INT_STAT    0x02
#define TCA8418_REG_KEY_LCK_EC  0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO_1   0x1D
#define TCA8418_REG_KP_GPIO_2   0x1E
#define TCA8418_REG_KP_GPIO_3   0x1F

// CFG bits.
#define TCA8418_CFG_AI          0x80   // auto-increment FIFO read pointer
#define TCA8418_CFG_GPI_E_CFG   0x40
#define TCA8418_CFG_OVR_FLOW_M  0x20
#define TCA8418_CFG_INT_CFG     0x10
#define TCA8418_CFG_OVR_FLOW_IEN 0x08
#define TCA8418_CFG_K_LCK_IEN   0x04
#define TCA8418_CFG_GPI_IEN     0x02
#define TCA8418_CFG_KE_IEN      0x01

// INT_STAT bits (write 1 to clear).
#define TCA8418_INT_K_INT       0x01
#define TCA8418_INT_GPI_INT     0x02
#define TCA8418_INT_K_LCK_INT   0x04
#define TCA8418_INT_OVR_FLOW_INT 0x08

// Decoded key-event byte.
//
// A TCA8418 KEY_EVENT_A read returns 0 when the FIFO is empty. Otherwise:
//   - bit 7   = 1 (press)  or 0 (release)
//   - bits6:0 = 1..80 = (row * 10) + col + 1, where row spans 0..7 and
//     col spans 0..9 in the full 8×10 matrix the chip supports.
//
// `valid` is false iff `raw == 0` (no event).
typedef struct {
    bool    valid;
    bool    pressed;
    uint8_t row;
    uint8_t col;
} tca8418_event_t;

tca8418_event_t tca8418_decode_event(uint8_t raw);

#ifdef __cplusplus
}
#endif
