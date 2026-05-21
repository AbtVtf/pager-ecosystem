// SPDX-License-Identifier: Apache-2.0
//
// PagerOS push notification client — FW-030.
//
// Polls https://push.pageros.org/pull/<device_pubkey> on wake (and on a
// scheduled cadence while awake), decrypts the per-app envelopes, dedupes
// by notification id, queues into /notifications/inbox.cbor, and surfaces
// the next-unread on the topmost interaction.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *relay_url;     // default: https://push.pageros.org
} pageros_push_client_opts_t;

esp_err_t pageros_push_client_init(const pageros_push_client_opts_t *opts);
esp_err_t pageros_push_client_shutdown(void);

// Make one pull round-trip. Returns ESP_OK if the relay returned 2xx.
// `*out_drained` is set to the number of notifications received.
esp_err_t pageros_push_client_poll(int *out_drained);

// Surface a tone hint for the most recently queued notification, or
// NULL if there's nothing to surface. Caller uses this to drive FW-032.
const char *pageros_push_client_pending_tone(void);

// Pop the next pending notification body (binary CBOR). Caller frees.
esp_err_t pageros_push_client_next(uint8_t **out_body, size_t *out_len,
                                   char *out_app_id, size_t app_id_cap);

typedef struct {
    uint64_t polls;
    uint64_t pulls;
    uint64_t decode_fail;
    uint64_t decrypt_fail;
    uint64_t queued;
    uint64_t delivered;
} pageros_push_client_stats_t;

void pageros_push_client_get_stats(pageros_push_client_stats_t *out);

#ifdef __cplusplus
}
#endif
