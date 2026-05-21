// SPDX-License-Identifier: Apache-2.0

#include "lora_params.h"

uint32_t pageros_lora_freq_to_pll(uint32_t freq_hz)
{
    // SX1262 §13.4.1: RF_FREQUENCY = floor(freq * 2^25 / Fxtal).
    // freq * 2^25 overflows 32 bits at ~128 MHz, so do the math in 64.
    uint64_t scaled = (uint64_t)freq_hz << 25;
    return (uint32_t)(scaled / PAGEROS_LORA_XTAL_HZ);
}

uint8_t pageros_lora_bw_to_reg(pageros_lora_bw_t bw)
{
    // SX1262 §13.4.5 LoRa BW codes. Only the three we expose are
    // mapped; the driver rejects anything else before getting here.
    switch (bw) {
        case PAGEROS_LORA_BW_125_KHZ: return 0x04;
        case PAGEROS_LORA_BW_250_KHZ: return 0x05;
        case PAGEROS_LORA_BW_500_KHZ: return 0x06;
    }
    return 0xFF;
}

uint32_t pageros_lora_band_to_hz(pageros_lora_band_t band)
{
    switch (band) {
        case PAGEROS_LORA_BAND_868_MHZ: return PAGEROS_LORA_FREQ_868_HZ;
        case PAGEROS_LORA_BAND_915_MHZ: return PAGEROS_LORA_FREQ_915_HZ;
    }
    return 0;
}

int8_t pageros_lora_clamp_power(int8_t dbm)
{
    if (dbm < PAGEROS_LORA_TX_MIN_DBM) return PAGEROS_LORA_TX_MIN_DBM;
    if (dbm > PAGEROS_LORA_TX_MAX_DBM) return PAGEROS_LORA_TX_MAX_DBM;
    return dbm;
}

bool pageros_lora_config_is_valid(const pageros_lora_config_t *cfg)
{
    if (cfg == NULL) return false;
    if (pageros_lora_band_to_hz(cfg->band) == 0) return false;
    if (pageros_lora_bw_to_reg(cfg->bandwidth) == 0xFF) return false;
    if (cfg->spreading_factor < PAGEROS_LORA_SF_MIN
            || cfg->spreading_factor > PAGEROS_LORA_SF_MAX) return false;
    if ((int)cfg->coding_rate < PAGEROS_LORA_CR_4_5
            || (int)cfg->coding_rate > PAGEROS_LORA_CR_4_8) return false;
    if (cfg->preamble_symbols < 4) return false;   // SX1262 min
    return true;
}
