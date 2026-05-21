// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the pure-C bits of the FW-008 LoRa driver:
// frequency → PLL encoding, bandwidth enum mapping, config validation
// and power clamping. The hardware-bound paths (SPI/IRQ/SX1262 state
// machine) live in `lora.c` and are exercised on real hardware via the
// selftest probe + a future LORA-001 echo test.

#include "lora_params.h"
#include "pageros_lora_types.h"

#include <stdio.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

static pageros_lora_config_t base_cfg(void)
{
    pageros_lora_config_t c = {
        .band             = PAGEROS_LORA_BAND_868_MHZ,
        .bandwidth        = PAGEROS_LORA_BW_125_KHZ,
        .spreading_factor = 7,
        .coding_rate      = PAGEROS_LORA_CR_4_5,
        .tx_power_dbm     = 14,
        .preamble_symbols = 8,
        .explicit_header  = true,
        .crc_on           = true,
        .iq_inverted      = false,
    };
    return c;
}

// ---------------------------------------------------------------------------
// Frequency encoding
// ---------------------------------------------------------------------------

static void test_freq_868_round_trip(void)
{
    // SX1262 §13.4.1: PLL = floor(freq * 2^25 / 32 MHz)
    //                    = 868 * 2^20 = 910,163,968 = 0x36400000.
    uint32_t pll = pageros_lora_freq_to_pll(868000000u);
    CHECK(pll == 0x36400000u, "expected 0x36400000, got 0x%08x", pll);
}

static void test_freq_915_round_trip(void)
{
    // 915 * 2^20 = 959,447,040 = 0x39300000.
    uint32_t pll = pageros_lora_freq_to_pll(915000000u);
    CHECK(pll == 0x39300000u, "expected 0x39300000, got 0x%08x", pll);
}

static void test_freq_monotonic(void)
{
    // Sanity: bigger Hz → bigger PLL value.
    CHECK(pageros_lora_freq_to_pll(868000000u)
              < pageros_lora_freq_to_pll(915000000u),
          "freq encoding not monotonic");
}

// ---------------------------------------------------------------------------
// Bandwidth mapping
// ---------------------------------------------------------------------------

static void test_bw_mapping(void)
{
    CHECK(pageros_lora_bw_to_reg(PAGEROS_LORA_BW_125_KHZ) == 0x04, "125 mismap");
    CHECK(pageros_lora_bw_to_reg(PAGEROS_LORA_BW_250_KHZ) == 0x05, "250 mismap");
    CHECK(pageros_lora_bw_to_reg(PAGEROS_LORA_BW_500_KHZ) == 0x06, "500 mismap");
    CHECK(pageros_lora_bw_to_reg((pageros_lora_bw_t)99) == 0xFF, "unknown should map to 0xFF");
}

// ---------------------------------------------------------------------------
// Band → Hz mapping
// ---------------------------------------------------------------------------

static void test_band_to_hz(void)
{
    CHECK(pageros_lora_band_to_hz(PAGEROS_LORA_BAND_868_MHZ) == 868000000u, "868 mismap");
    CHECK(pageros_lora_band_to_hz(PAGEROS_LORA_BAND_915_MHZ) == 915000000u, "915 mismap");
    CHECK(pageros_lora_band_to_hz((pageros_lora_band_t)42) == 0, "unknown band must be 0");
}

// ---------------------------------------------------------------------------
// Power clamp
// ---------------------------------------------------------------------------

static void test_power_clamp(void)
{
    CHECK(pageros_lora_clamp_power(0)   == 0,   "0 passes through");
    CHECK(pageros_lora_clamp_power(22)  == 22,  "+22 stays");
    CHECK(pageros_lora_clamp_power(-9)  == -9,  "-9 stays");
    CHECK(pageros_lora_clamp_power(50)  == 22,  "over → 22");
    CHECK(pageros_lora_clamp_power(-50) == -9,  "under → -9");
}

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

static void test_config_valid(void)
{
    pageros_lora_config_t c = base_cfg();
    CHECK(pageros_lora_config_is_valid(&c), "base config should be valid");
}

static void test_config_null_rejected(void)
{
    CHECK(!pageros_lora_config_is_valid(NULL), "NULL must be invalid");
}

static void test_config_sf_bounds(void)
{
    pageros_lora_config_t c = base_cfg();
    c.spreading_factor = 4;
    CHECK(!pageros_lora_config_is_valid(&c), "SF4 must be invalid");
    c.spreading_factor = 13;
    CHECK(!pageros_lora_config_is_valid(&c), "SF13 must be invalid");
    c.spreading_factor = 12;
    CHECK(pageros_lora_config_is_valid(&c), "SF12 valid");
    c.spreading_factor = 5;
    CHECK(pageros_lora_config_is_valid(&c), "SF5 valid");
}

static void test_config_preamble_min(void)
{
    pageros_lora_config_t c = base_cfg();
    c.preamble_symbols = 3;
    CHECK(!pageros_lora_config_is_valid(&c), "preamble < 4 must be invalid");
    c.preamble_symbols = 4;
    CHECK(pageros_lora_config_is_valid(&c), "preamble = 4 valid");
}

static void test_config_unknown_band(void)
{
    pageros_lora_config_t c = base_cfg();
    c.band = (pageros_lora_band_t)5;
    CHECK(!pageros_lora_config_is_valid(&c), "unknown band must be invalid");
}

int main(void)
{
    test_freq_868_round_trip();
    test_freq_915_round_trip();
    test_freq_monotonic();
    test_bw_mapping();
    test_band_to_hz();
    test_power_clamp();
    test_config_valid();
    test_config_null_rejected();
    test_config_sf_bounds();
    test_config_preamble_min();
    test_config_unknown_band();

    if (fail_count == 0) {
        printf("OK (11 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
