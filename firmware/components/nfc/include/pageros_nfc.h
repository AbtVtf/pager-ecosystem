// SPDX-License-Identifier: Apache-2.0
//
// PagerOS NFC driver — FW-010.
//
// Drives the ST25R3916 NFC reader IC over SPI on the LILYGO T-LoRa
// Pager. v0 scope: SPI bring-up, chip-id read sanity check, polling
// loop scaffold. Full ISO 14443-A anticollision + NDEF parsing lands
// behind the same API once we have card hardware on the bench.
//
// On detection, the driver emits a `nfc_scan` event (SPEC §5.4.1) to
// the input router carrying:
//
//   - uid: variable-length UID bytes
//   - records: zero or more NDEF text/URI records
//
// Apps subscribe via the Frame's `subscribe: ["nfc_scan"]` and the
// router POSTs the event to the foreground app's handler.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_NFC_MAX_UID_BYTES   10
#define PAGEROS_NFC_MAX_NDEF_BYTES  256

typedef struct {
    uint8_t uid[PAGEROS_NFC_MAX_UID_BYTES];
    uint8_t uid_len;
    uint8_t ndef[PAGEROS_NFC_MAX_NDEF_BYTES];
    size_t  ndef_len;
} pageros_nfc_scan_t;

// Callback fired from the NFC poll task when a tag is detected.
typedef void (*pageros_nfc_callback_t)(const pageros_nfc_scan_t *scan, void *user);

esp_err_t pageros_nfc_init(void);
esp_err_t pageros_nfc_shutdown(void);

// Returns true if the ST25R3916 responded with the expected chip ID
// on initialisation. False means either the IC isn't present (wrong
// board variant) or the SPI wiring isn't right.
bool pageros_nfc_present(void);

// Start the polling loop. Calls `cb` on each successful scan; pass
// NULL to use the default "log + push event" behavior.
esp_err_t pageros_nfc_start(pageros_nfc_callback_t cb, void *user);
esp_err_t pageros_nfc_stop(void);

typedef struct {
    uint64_t polls;
    uint64_t scans_ok;
    uint64_t spi_errors;
    bool     chip_present;
} pageros_nfc_stats_t;

void pageros_nfc_get_stats(pageros_nfc_stats_t *out);

#ifdef __cplusplus
}
#endif
