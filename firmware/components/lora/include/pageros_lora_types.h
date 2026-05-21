// SPDX-License-Identifier: Apache-2.0
//
// Plain-data types for the PagerOS LoRa wrapper. Split out of
// `pageros_lora.h` so they can also be included from the host-test
// harness without pulling in `esp_err.h`. The driver and the host
// helpers both consume these definitions.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Operating band. Selected at init; can be switched at runtime via
// `pageros_lora_set_band`.
typedef enum {
    PAGEROS_LORA_BAND_868_MHZ = 0,
    PAGEROS_LORA_BAND_915_MHZ = 1,
} pageros_lora_band_t;

// LoRa channel bandwidth. Wire values match SX1262 register encoding
// but consumers should use the enum names — the driver translates.
typedef enum {
    PAGEROS_LORA_BW_125_KHZ = 0,
    PAGEROS_LORA_BW_250_KHZ = 1,
    PAGEROS_LORA_BW_500_KHZ = 2,
} pageros_lora_bw_t;

// LoRa coding rate. 4/5 is the spec default for low-overhead links; the
// mesh tasks can pick a slower one for resilience.
typedef enum {
    PAGEROS_LORA_CR_4_5 = 1,
    PAGEROS_LORA_CR_4_6 = 2,
    PAGEROS_LORA_CR_4_7 = 3,
    PAGEROS_LORA_CR_4_8 = 4,
} pageros_lora_cr_t;

// Spreading factor bounds. SX1262 supports SF5..SF12 in LoRa mode.
#define PAGEROS_LORA_SF_MIN  5
#define PAGEROS_LORA_SF_MAX  12

// Hardware maximum packet payload, in bytes.
#define PAGEROS_LORA_MAX_PACKET  255

// Driver configuration. All fields are required — there are no
// implicit defaults, because a wrong default LoRa link is worse than a
// compile error (silent off-band TX violates regulations).
typedef struct {
    pageros_lora_band_t band;             // 868 vs 915 MHz center
    pageros_lora_bw_t   bandwidth;        // 125/250/500 kHz
    uint8_t             spreading_factor; // 5..12
    pageros_lora_cr_t   coding_rate;      // 4/5..4/8
    int8_t              tx_power_dbm;     // -9..+22 (clamped on apply)
    uint16_t            preamble_symbols; // typical 8 or 12
    bool                explicit_header;  // true = header w/ length+CRC bits
    bool                crc_on;
    bool                iq_inverted;
} pageros_lora_config_t;

#ifdef __cplusplus
}
#endif
