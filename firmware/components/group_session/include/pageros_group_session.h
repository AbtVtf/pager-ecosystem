// SPDX-License-Identifier: Apache-2.0
//
// PagerOS group session client — FW-031.
//
// When a Frame declares `subscribe_groups: [...]` (SPEC §5.4.2), this
// component holds the list and dispatches incoming group events
// (`group_message`, `presence_update`, `member_joined`, `member_left`)
// to a caller-supplied callback that mutates the live widget tree
// (FW-023). Events arrive via the push client (offline) or the app
// server's long-poll endpoint (online).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_GS_MAX_GROUPS    8
#define PAGEROS_GS_MAX_GROUP_ID  64

typedef enum {
    PAGEROS_GS_GROUP_MESSAGE   = 1,
    PAGEROS_GS_PRESENCE_UPDATE = 2,
    PAGEROS_GS_MEMBER_JOINED   = 3,
    PAGEROS_GS_MEMBER_LEFT     = 4,
} pageros_gs_event_kind_t;

typedef struct {
    pageros_gs_event_kind_t kind;
    const char *group_id;
    const uint8_t *payload;       // CBOR payload — opaque to runtime
    size_t        payload_len;
} pageros_gs_event_t;

typedef void (*pageros_gs_callback_t)(const pageros_gs_event_t *event, void *user);

esp_err_t pageros_group_session_init(void);
esp_err_t pageros_group_session_shutdown(void);

// Replace the current subscription set. Pass a list of group ids and
// the count. The runtime makes its own copies.
esp_err_t pageros_group_session_subscribe(const char * const *group_ids, size_t n);

bool pageros_group_session_has(const char *group_id);

// Register the dispatch callback. The callback runs on the caller's
// task — it MUST be reentrant and short.
void pageros_group_session_set_callback(pageros_gs_callback_t cb, void *user);

// Called by the transport layer when it receives a group event packet
// (push client or app long-poll). The runtime filters by subscription.
esp_err_t pageros_group_session_dispatch(const pageros_gs_event_t *event);

typedef struct {
    size_t subscribed;
    uint64_t delivered;
    uint64_t dropped_not_subscribed;
} pageros_gs_stats_t;

void pageros_group_session_get_stats(pageros_gs_stats_t *out);

#ifdef __cplusplus
}
#endif
