// SPDX-License-Identifier: Apache-2.0
//
// PagerOS LoRa driver wrapper (Semtech SX1262) — FW-008.
//
// Thin synchronous wrapper around the SX1262 transceiver on the LILYGO
// T-LoRa Pager. Provides:
//
//   - TX/RX of arbitrary byte buffers (up to 255 bytes per packet — the
//     hardware ceiling), at either 868 MHz (EU) or 915 MHz (NA/AU).
//   - Configurable LoRa modulation params: spreading factor (SF 5..12),
//     bandwidth (125/250/500 kHz), and coding rate (4/5..4/8).
//   - Per-packet RSSI (dBm) and SNR (dB) read-back on receive.
//
// What this driver is *not*:
//   - It is not a mesh router. LORA-* tasks own the routing envelope
//     (SPEC §6.2.1) and fragmentation (SPEC §6.2.3); this driver only
//     pushes opaque bytes across the air.
//   - It is not interrupt-driven. RX/TX use polling on the DIO1 IRQ
//     line through `gpio_get_level` with a small `vTaskDelay` to keep
//     the CPU available. A later task can wire DIO1 to a GPIO ISR if
//     latency matters; the synchronous shape is what every immediate
//     consumer (selftest probe, future LORA-001 envelope sender) needs.
//
// Pinout (LILYGO T-LoRa Pager / ESP32-S3, matches the arduino-esp32
// variant `lilygo_tlora_pager/pins_arduino.h`):
//
//   MOSI / MISO / SCK : 34 / 33 / 35  (shared SPI2 with SD, display, NFC)
//   LORA_CS           : 36
//   LORA_RST          : 47
//   LORA_BUSY         : 48
//   LORA_DIO1 / IRQ   : 14
//   Power gate        : XL9555 P0.3 (EXPANDS_LORA_EN)

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pageros_lora_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Power up the radio, reset, configure modulation, and leave it in
// standby. Idempotent. Pins the shared SPI bus up if no other peripheral
// has yet, then adds the SX1262 as a SPI device.
//
// Returns:
//   ESP_OK                 — radio ready in standby
//   ESP_ERR_INVALID_ARG    — config out of range (e.g. SF=13)
//   ESP_ERR_INVALID_STATE  — XL9555 / SPI bring-up failed
//   ESP_ERR_NOT_FOUND      — SX1262 did not respond on the bus
//   any esp_err_t          — surfaced from the SPI / GPIO drivers
esp_err_t pageros_lora_init(const pageros_lora_config_t *cfg);

// Drop the radio back to sleep and detach the SPI device. Safe to call
// when never initialised.
esp_err_t pageros_lora_shutdown(void);

// True if `pageros_lora_init` succeeded since the last shutdown.
bool pageros_lora_is_ready(void);

// ---------------------------------------------------------------------------
// Configuration tweaks (after init)
// ---------------------------------------------------------------------------

// Switch operating band. Re-applies frequency + sync-word (SX1262 has
// no atomic "set band" command — under the hood this is a SetStandby
// + SetRfFrequency pair).
esp_err_t pageros_lora_set_band(pageros_lora_band_t band);

// Adjust modulation. Frequency stays at whatever the last `set_band`
// (or init) selected.
esp_err_t pageros_lora_set_modulation(pageros_lora_bw_t bw,
                                      uint8_t spreading_factor,
                                      pageros_lora_cr_t cr);

// Adjust TX power. Clamped to the SX1262 PA range (-9..+22 dBm).
esp_err_t pageros_lora_set_tx_power(int8_t dbm);

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

// Transmit `len` bytes. Blocks until TX completes (DIO1 IRQ TxDone) or
// `timeout_ms` elapses. Returns:
//   ESP_OK                — packet sent
//   ESP_ERR_INVALID_ARG   — len > PAGEROS_LORA_MAX_PACKET, NULL data
//   ESP_ERR_INVALID_STATE — radio not initialised
//   ESP_ERR_TIMEOUT       — radio did not report TxDone in time
esp_err_t pageros_lora_tx(const uint8_t *data, size_t len, uint32_t timeout_ms);

// Receive into `buf` (capacity `buf_capacity`). Blocks until a packet
// arrives or `timeout_ms` elapses. `out_len`, `out_rssi_dbm`, and
// `out_snr_db_q4` are filled on success; `out_snr_db_q4` is signed
// fixed-point with 4 fractional bits (i.e. raw SNR in 0.25 dB units —
// kept as q4 so the caller can decide how to display it).
//
// Returns:
//   ESP_OK              — packet received
//   ESP_ERR_TIMEOUT     — no packet within window
//   ESP_ERR_INVALID_SIZE — packet larger than `buf_capacity` (data
//                          discarded; out_len = received length)
esp_err_t pageros_lora_rx(uint8_t *buf, size_t buf_capacity,
                          size_t *out_len,
                          int16_t *out_rssi_dbm,
                          int16_t *out_snr_db_q4,
                          uint32_t timeout_ms);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Read the SX1262 status byte (datasheet §13.5.1). Useful as a quick
// presence probe: a healthy radio returns a chip-mode + command-status
// nibble pair that is *not* 0x00 / 0xFF. Returned verbatim — the caller
// decides how to interpret it.
esp_err_t pageros_lora_get_status(uint8_t *out_status);

// Convenience: is the radio detectable on the bus? Equivalent to
// reading the status byte and checking it isn't a stuck 0x00/0xFF.
bool pageros_lora_probe(void);

// ---------------------------------------------------------------------------
// Pure-C helpers exposed for host tests. Application code should call
// the API above. See `lora_params.h` for the rationale.
// ---------------------------------------------------------------------------

// Encode a frequency in Hz as the 32-bit RF_FREQUENCY value the SX1262
// expects: `floor(freq_hz * 2^25 / 32_000_000)`. Exposed so the test
// harness can pin the exact bytes that go on the wire for each band.
uint32_t pageros_lora_freq_to_pll(uint32_t freq_hz);

// Map the public BW enum to the SX1262 register byte. Returns 0xFF on
// unknown input (the driver itself rejects this earlier; the host test
// uses the value to assert the mapping).
uint8_t pageros_lora_bw_to_reg(pageros_lora_bw_t bw);

// True if `cfg` is internally consistent (SF in range, BW recognised,
// power within rails, preamble sane). Performs the same checks
// `pageros_lora_init` does before touching hardware.
bool pageros_lora_config_is_valid(const pageros_lora_config_t *cfg);

#ifdef __cplusplus
}
#endif
