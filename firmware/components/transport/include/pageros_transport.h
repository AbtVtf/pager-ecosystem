// SPDX-License-Identifier: Apache-2.0
//
// PagerOS transport selector — FW-025.
//
// Per SPEC §6.3:
//
//   1. If Wi-Fi associated AND last successful HTTPS within 60 s → Wi-Fi.
//   2. Else try Wi-Fi with 3 s timeout.
//   3. Else fall back to LoRa if app's manifest has lora_compatible: true.
//   4. Else return ESP_ERR_NOT_SUPPORTED ("App requires internet").
//
// One call (`pageros_transport_request`) makes the decision, performs
// the call, returns the response + which transport served it.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGEROS_TRANSPORT_UNKNOWN = 0,
    PAGEROS_TRANSPORT_WIFI    = 1,
    PAGEROS_TRANSPORT_LORA    = 2,
    PAGEROS_TRANSPORT_PUSH    = 3,   // future: push relay pull
} pageros_transport_t;

typedef struct {
    const char *target_url;
    const uint8_t *body;
    size_t body_len;
    const char *content_type;        // NULL → application/cbor
    bool lora_compatible;            // from app manifest
    uint32_t timeout_ms;             // 0 → 30000
    const uint8_t *app_pubkey;       // 32 bytes; used for LoRa encrypt path
} pageros_transport_request_t;

typedef struct {
    pageros_transport_t used;
    int http_status;                 // 0 on LoRa or transport error
    uint8_t *body;                   // caller frees
    size_t   body_len;
} pageros_transport_response_t;

esp_err_t pageros_transport_init(void);

// Make the request per the SPEC §6.3 ladder. Caller frees response.body.
esp_err_t pageros_transport_request(const pageros_transport_request_t *req,
                                    pageros_transport_response_t *out);

// Stats hooks (for the diagnostics screen).
typedef struct {
    uint64_t wifi_success;
    uint64_t wifi_fail;
    uint64_t lora_success;
    uint64_t lora_fail;
    uint64_t no_transport;
    uint64_t last_wifi_success_unix;
} pageros_transport_stats_t;

void pageros_transport_get_stats(pageros_transport_stats_t *out);

#ifdef __cplusplus
}
#endif
