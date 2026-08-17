// SPDX-License-Identifier: Apache-2.0
// FW-033 — permissions.

#include "pageros_permissions.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "permissions";
#define NS "pageros_perm"

static const char *NAMES[PAGEROS_PERM__COUNT] = {
    [PAGEROS_PERM_LOCATION]      = "location",
    [PAGEROS_PERM_NFC]           = "nfc",
    [PAGEROS_PERM_NOTIFICATIONS] = "notifications",
    [PAGEROS_PERM_GROUPS]        = "groups",
    [PAGEROS_PERM_LORA_SEND]     = "lora_send",
    [PAGEROS_PERM_CONTACTS]      = "contacts",
};

const char *pageros_perm_name(pageros_perm_t p)
{
    if ((int)p < 0 || (int)p >= PAGEROS_PERM__COUNT) return "?";
    return NAMES[p];
}

bool pageros_perm_from_name(const char *name, pageros_perm_t *out)
{
    if (!name || !out) return false;
    for (int i = 0; i < PAGEROS_PERM__COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) { *out = (pageros_perm_t)i; return true; }
    }
    return false;
}

esp_err_t pageros_permissions_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

// NVS keys are limited to 15 characters. We hash the (app_id, perm)
// pair into a stable short key. App ids can be long ("notes.mafu.dev"),
// so we use a 4-byte FNV-1a + perm digit.
static void make_key(const char *app_id, pageros_perm_t perm, char out[16])
{
    uint32_t h = 0x811c9dc5u;
    for (const char *p = app_id; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }
    snprintf(out, 16, "%08x_%d", (unsigned)h, (int)perm);
}

esp_err_t pageros_permissions_get(const char *app_id, pageros_perm_t perm,
                                  pageros_perm_decision_t *out)
{
    if (!app_id || !out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    char key[16]; make_key(app_id, perm, key);
    uint8_t v = 0;
    esp_err_t e = nvs_get_u8(h, key, &v);
    nvs_close(h);
    if (e != ESP_OK) return ESP_ERR_NOT_FOUND;
    *out = (v ? PAGEROS_PERM_DECISION_ALLOW : PAGEROS_PERM_DECISION_DENY);
    return ESP_OK;
}

esp_err_t pageros_permissions_set(const char *app_id, pageros_perm_t perm,
                                  pageros_perm_decision_t decision)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    char key[16]; make_key(app_id, perm, key);
    nvs_set_u8(h, key, decision == PAGEROS_PERM_DECISION_ALLOW ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t pageros_permissions_revoke_all(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_OK;
    for (int i = 0; i < PAGEROS_PERM__COUNT; i++) {
        char key[16]; make_key(app_id, (pageros_perm_t)i, key);
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

size_t pageros_permissions_granted_for(const char *app_id,
                                       const char **out_names, size_t cap)
{
    if (!app_id || !out_names) return 0;
    size_t n = 0;
    for (int i = 0; i < PAGEROS_PERM__COUNT && n < cap; i++) {
        pageros_perm_decision_t d;
        if (pageros_permissions_get(app_id, (pageros_perm_t)i, &d) == ESP_OK &&
            d == PAGEROS_PERM_DECISION_ALLOW) {
            out_names[n++] = NAMES[i];
        }
    }
    return n;
}
