// SPDX-License-Identifier: Apache-2.0
//
// XL9555 16-bit I2C port expander.
//
// On the LILYGO T-LoRa Pager the XL9555 (default I2C address 0x20)
// owns the enable lines for most peripherals — the LoRa radio, GPS,
// NFC, SD card, keyboard, speaker amp, etc. are POWERED OFF at boot
// until the expander asserts their enable bit. Without flipping these
// bits the SPI/I2C/UART probes for those chips all fail with
// timeouts or wrong chip IDs (which is what we saw on first flash).
//
// Bit assignments come from the LILYGO arduino-esp32 variant header
// (EXPANDS_* defines in `lilygo_tlora_pager/pins_arduino.h`):
//
//   bit  0  EXPANDS_DRV_EN        DRV2605L haptic enable
//   bit  1  EXPANDS_AMP_EN        Speaker amp enable
//   bit  2  EXPANDS_KB_RST        Keyboard reset (active low — drive HIGH to release)
//   bit  3  EXPANDS_LORA_EN       LoRa power enable
//   bit  4  EXPANDS_GPS_EN        GPS power enable
//   bit  5  EXPANDS_NFC_EN        NFC power enable
//   bit  7  EXPANDS_GPS_RST       GPS reset (active low — drive HIGH to run)
//   bit  8  EXPANDS_KB_EN         Keyboard backlight enable
//   bit  9  EXPANDS_GPIO_EN       Generic GPIO enable
//   bit 10  EXPANDS_SD_DET        SD card detect (input)
//   bit 11  EXPANDS_SD_PULLEN     SD card data line pull-up enable
//   bit 12  EXPANDS_SD_EN         SD card power enable

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_XL9555_I2C_ADDR  0x20

// Enable-line bit indices. Names mirror the LILYGO variant header.
typedef enum {
    PAGEROS_XL_DRV_EN     = 0,
    PAGEROS_XL_AMP_EN     = 1,
    PAGEROS_XL_KB_RST     = 2,
    PAGEROS_XL_LORA_EN    = 3,
    PAGEROS_XL_GPS_EN     = 4,
    PAGEROS_XL_NFC_EN     = 5,
    PAGEROS_XL_GPS_RST    = 7,
    PAGEROS_XL_KB_EN      = 8,
    PAGEROS_XL_GPIO_EN    = 9,
    PAGEROS_XL_SD_DET     = 10,
    PAGEROS_XL_SD_PULLEN  = 11,
    PAGEROS_XL_SD_EN      = 12,
} pageros_xl_pin_t;

esp_err_t pageros_xl9555_init(void);

// Set/clear/read one expander pin (0..15). The expander remembers
// the full output register state across calls.
esp_err_t pageros_xl9555_set(pageros_xl_pin_t pin, bool level);
esp_err_t pageros_xl9555_get(pageros_xl_pin_t pin, bool *out_level);

// Mark a pin as input or output. Inputs read the external level via
// pageros_xl9555_get(); outputs drive the level set by _set().
esp_err_t pageros_xl9555_set_dir(pageros_xl_pin_t pin, bool is_input);

// True iff init() succeeded — useful for soft-degrade paths.
bool pageros_xl9555_present(void);

#ifdef __cplusplus
}
#endif
