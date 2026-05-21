// SPDX-License-Identifier: Apache-2.0
//
// PagerOS permissions — FW-033.
//
// Per-(app, permission) grant state, persisted to NVS. SPEC §9.4 lists
// the v1 permissions: location, nfc, notifications, groups, lora_send,
// contacts. First request from an app surfaces a prompt; user picks
// allow/deny/always. The "always" branch persists. Revocation lives in
// Settings (FW-029).

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGEROS_PERM_LOCATION = 0,
    PAGEROS_PERM_NFC,
    PAGEROS_PERM_NOTIFICATIONS,
    PAGEROS_PERM_GROUPS,
    PAGEROS_PERM_LORA_SEND,
    PAGEROS_PERM_CONTACTS,
    PAGEROS_PERM__COUNT,
} pageros_perm_t;

typedef enum {
    PAGEROS_PERM_DECISION_DENY  = 0,
    PAGEROS_PERM_DECISION_ALLOW = 1,
} pageros_perm_decision_t;

esp_err_t pageros_permissions_init(void);

// Stable string id used in NVS keys and across the wire.
const char *pageros_perm_name(pageros_perm_t p);
bool pageros_perm_from_name(const char *name, pageros_perm_t *out);

// Read the current grant for (app_id, perm). Returns ESP_ERR_NOT_FOUND
// when no decision has been made yet — caller surfaces the prompt.
esp_err_t pageros_permissions_get(const char *app_id, pageros_perm_t perm,
                                  pageros_perm_decision_t *out);

// Persist a decision.
esp_err_t pageros_permissions_set(const char *app_id, pageros_perm_t perm,
                                  pageros_perm_decision_t decision);

// Wipe all grants for an app — call on uninstall.
esp_err_t pageros_permissions_revoke_all(const char *app_id);

// Convenience for the SDK's PagerOS-Granted header emission.
// out_count is the number of permission names written into out_names
// (each pointer is a static string from pageros_perm_name).
size_t pageros_permissions_granted_for(const char *app_id,
                                       const char **out_names, size_t cap);

#ifdef __cplusplus
}
#endif
