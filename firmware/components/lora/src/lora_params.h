// SPDX-License-Identifier: Apache-2.0
//
// Pure helpers shared by `lora.c` and the host-test harness. No
// FreeRTOS / ESP-IDF dependencies — only stdint + the public enum
// types. The pattern mirrors `path.h` (storage) and `logger_format.h`
// (logger).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pageros_lora_types.h"

// SX1262 center frequencies (Hz) for the two supported bands. 868.0 MHz
// is the canonical EU868 channel; 915.0 MHz is the AU915 / US915 dial
// (the higher-level mesh code does proper channel hopping later).
#define PAGEROS_LORA_FREQ_868_HZ  868000000u
#define PAGEROS_LORA_FREQ_915_HZ  915000000u

// SX1262 reference oscillator. Used by `freq_to_pll`.
#define PAGEROS_LORA_XTAL_HZ      32000000u

// TX power rails — SX1262 PA can go from -9 dBm to +22 dBm.
#define PAGEROS_LORA_TX_MIN_DBM   (-9)
#define PAGEROS_LORA_TX_MAX_DBM   (22)

// Internal helpers (also re-declared in `pageros_lora.h` for host
// tests; declared here too so `lora.c` doesn't need the public header
// to reach them).
uint32_t pageros_lora_freq_to_pll(uint32_t freq_hz);
uint8_t  pageros_lora_bw_to_reg(pageros_lora_bw_t bw);
bool     pageros_lora_config_is_valid(const pageros_lora_config_t *cfg);

// Translate the band enum to a Hz constant. Returns 0 for unknown.
uint32_t pageros_lora_band_to_hz(pageros_lora_band_t band);

// Clamp `dbm` to the SX1262 PA range.
int8_t pageros_lora_clamp_power(int8_t dbm);
