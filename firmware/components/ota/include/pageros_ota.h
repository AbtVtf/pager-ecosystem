// SPDX-License-Identifier: Apache-2.0
//
// PagerOS OTA client — FW-036.
//
// Polls `updates.pageros.org` for a signed manifest, verifies the
// signature against the project release key (compiled in), and applies
// the update to the inactive A/B partition via esp_https_ota. Failure
// to boot the new image triggers a rollback per ESP-IDF's standard
// boot validation.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *manifest_url;        // default: https://updates.pageros.org/firmware.json
} pageros_ota_opts_t;

esp_err_t pageros_ota_init(const pageros_ota_opts_t *opts);

// Check for a newer release. Returns ESP_OK + sets *out_available.
esp_err_t pageros_ota_check(bool *out_available, char *out_version, size_t cap);

// Download + verify signature + write to inactive partition + set
// boot. Caller reboots after success.
esp_err_t pageros_ota_apply(void);

typedef struct {
    uint64_t checks;
    uint64_t applies;
    uint64_t apply_failures;
    uint64_t rollbacks;
} pageros_ota_stats_t;

void pageros_ota_get_stats(pageros_ota_stats_t *out);

#ifdef __cplusplus
}
#endif
