// SPDX-License-Identifier: Apache-2.0
//
// PagerOS device identity — FW-014.
//
// On first boot, generates a 32-byte Ed25519 seed via the hardware RNG
// (`esp_fill_random`) and persists it to NVS under the `pageros_id`
// namespace. The Ed25519 public key (32 bytes, RFC 8032 / SHA-512
// variant — see `pageros_crypto`) is derived from that seed at boot and
// cached in RAM for the lifetime of the firmware run.
//
// Per SPEC §9.1:
//   - Ed25519 keypair, generated on first boot.
//   - Private key stored in ESP32-S3 secure storage (NVS, with flash
//     encryption enabled at production build time).
//   - Public key is the device's stable identity. Displayable as a
//     12-char base32 fingerprint for user readability.
//
// FW-015 (crypto primitives wrapper) will provide signing on top of the
// seed exposed by `pageros_identity_seed()`.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_IDENTITY_SEED_LEN    32
#define PAGEROS_IDENTITY_PUBKEY_LEN  32

// 12 chars of RFC 4648 base32 (no padding) + NUL — the "12-char base32
// fingerprint" called out in SPEC §9.1. Derived from the first 60 bits
// of the public key.
#define PAGEROS_IDENTITY_FP_LEN      13

// Initialise the identity subsystem. Loads the device seed from NVS, or
// generates and persists one on first boot. Derives and caches the
// public key. Idempotent — subsequent calls are no-ops once the in-RAM
// cache is populated.
//
// Returns:
//   ESP_OK                — identity ready
//   ESP_ERR_INVALID_STATE — NVS not initialised (caller forgot
//                           `nvs_flash_init`)
//   any nvs_*/esp_random error code on persistence/RNG failure
esp_err_t pageros_identity_init(void);

// Copy the 32-byte device public key into `out`. Fails with
// ESP_ERR_INVALID_STATE if `pageros_identity_init` hasn't succeeded.
esp_err_t pageros_identity_pubkey(uint8_t out[PAGEROS_IDENTITY_PUBKEY_LEN]);

// Copy the raw 32-byte Ed25519 seed. **Sensitive material.** Consumers
// (e.g. the FW-015 signer) must zero their copy immediately after use.
esp_err_t pageros_identity_seed(uint8_t out[PAGEROS_IDENTITY_SEED_LEN]);

// Write a 12-character base32 fingerprint (NUL-terminated, total 13
// bytes) into `out`. The fingerprint is the RFC 4648 base32 encoding of
// the first 60 bits of the device public key — see SPEC §9.1.
esp_err_t pageros_identity_fingerprint(char out[PAGEROS_IDENTITY_FP_LEN]);

// Compute a fingerprint from an arbitrary public key. Exposed so the
// shell can render fingerprints for peer pubkeys (e.g. group-member
// rendering, sideload-by-URL trust confirmation). No state required.
void pageros_identity_pubkey_fingerprint(
        char out[PAGEROS_IDENTITY_FP_LEN],
        const uint8_t pubkey[PAGEROS_IDENTITY_PUBKEY_LEN]);

#ifdef __cplusplus
}
#endif
