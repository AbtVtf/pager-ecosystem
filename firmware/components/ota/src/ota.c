// SPDX-License-Identifier: Apache-2.0
// FW-036 — OTA client.
//
// v0 scope: API surface + esp_https_ota integration with the standard
// signed-image flow (ESP-IDF's bundled secure boot v2 / app signature
// is the trust anchor). The manifest fetch + version compare is
// implemented; the actual `pageros_ota_apply` call delegates to
// `esp_https_ota` which already does verify→write→set-boot→reboot.

#include "pageros_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

#define DEFAULT_MANIFEST_URL "https://updates.pageros.org/firmware.json"

static struct {
    bool inited;
    char manifest_url[256];
    char latest_version[32];
    char latest_url[256];
    pageros_ota_stats_t stats;
} g;

esp_err_t pageros_ota_init(const pageros_ota_opts_t *opts)
{
    if (g.inited) return ESP_OK;
    const char *u = (opts && opts->manifest_url) ? opts->manifest_url : DEFAULT_MANIFEST_URL;
    strncpy(g.manifest_url, u, sizeof(g.manifest_url) - 1);
    g.manifest_url[sizeof(g.manifest_url) - 1] = '\0';
    g.inited = true;
    memset(&g.stats, 0, sizeof(g.stats));

    // Mark current partition as good so we don't roll back on next boot
    // because we never validated. Safe to call repeatedly.
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            if (st == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_ota_mark_app_valid_cancel_rollback();
                g.stats.rollbacks = 0;  // we just confirmed
            }
        }
    }
    return ESP_OK;
}

// Tiny JSON scanner — looks for "version": "...", "url": "..." top-
// level fields. Keeps us from pulling cJSON into the firmware for a
// 2-field manifest.
static bool extract_quoted(const char *body, const char *key, char *out, size_t cap)
{
    if (!body || !key || !out || cap == 0) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    while (*p && *p != '"') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

esp_err_t pageros_ota_check(bool *out_available, char *out_version, size_t cap)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    g.stats.checks++;
    if (out_available) *out_available = false;

    esp_http_client_config_t cfg = {
        .url = g.manifest_url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_err_t r = esp_http_client_open(c, 0);
    if (r != ESP_OK) { esp_http_client_cleanup(c); return r; }
    int total = esp_http_client_fetch_headers(c);
    if (total <= 0 || total > 4096) { esp_http_client_cleanup(c); return ESP_FAIL; }
    char *buf = (char *)malloc(total + 1);
    if (!buf) { esp_http_client_cleanup(c); return ESP_ERR_NO_MEM; }
    int n = esp_http_client_read(c, buf, total);
    esp_http_client_cleanup(c);
    if (n <= 0) { free(buf); return ESP_FAIL; }
    buf[n] = '\0';

    bool ok_v = extract_quoted(buf, "version", g.latest_version, sizeof(g.latest_version));
    bool ok_u = extract_quoted(buf, "url",     g.latest_url,     sizeof(g.latest_url));
    free(buf);
    if (!ok_v || !ok_u) return ESP_ERR_INVALID_RESPONSE;

    const esp_app_desc_t *cur = esp_app_get_description();
    if (cur && strcmp(cur->version, g.latest_version) != 0) {
        if (out_available) *out_available = true;
        if (out_version && cap) {
            strncpy(out_version, g.latest_version, cap - 1);
            out_version[cap - 1] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t pageros_ota_apply(void)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (g.latest_url[0] == '\0') return ESP_ERR_INVALID_STATE;

    esp_http_client_config_t cfg = {
        .url = g.latest_url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &cfg };
    ESP_LOGI(TAG, "applying %s", g.latest_url);
    esp_err_t r = esp_https_ota(&ota_cfg);
    if (r != ESP_OK) {
        g.stats.apply_failures++;
        return r;
    }
    g.stats.applies++;
    // Caller reboots.
    return ESP_OK;
}

void pageros_ota_get_stats(pageros_ota_stats_t *out)
{
    if (out) *out = g.stats;
}
