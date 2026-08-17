// SPDX-License-Identifier: Apache-2.0
//
// PagerOS sideload-by-URL — FW-034.
//
// Settings → "Add app by URL" → fetches `<url>/.pageros/manifest.cbor`,
// writes it to `/apps/<id>/manifest.cbor`, tags as `sideloaded`. The
// device's shell drawer then renders the app.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_SIDELOAD_MAX_MANIFEST   2048
#define PAGEROS_SIDELOAD_MAX_APP_ID     128

esp_err_t pageros_sideload_install_from_url(const char *url,
                                            char *out_app_id, size_t cap);

// Extract a top-level text-string field from a CBOR-encoded manifest.
// Returns the field length on success, -1 if missing / non-text / over cap.
// Used by the shell desktop to read `name` from a stored manifest.cbor.
int pageros_sideload_extract_field(const uint8_t *cbor, size_t cbor_len,
                                   const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
