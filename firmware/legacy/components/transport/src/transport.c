// SPDX-License-Identifier: Apache-2.0
// FW-025 — transport selector.

#include "pageros_transport.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "pageros_lora_client.h"
#include "pageros_network.h"

static const char *TAG = "transport";

#define HTTPS_RECENT_S      60
#define HTTPS_FAST_TIMEOUT  3000
#define DEFAULT_TIMEOUT     30000
#define RESP_BUF_BYTES      (4 * 1024)

static pageros_transport_stats_t g_stats;

esp_err_t pageros_transport_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    return ESP_OK;
}

static bool wifi_recent(void)
{
    if (g_stats.last_wifi_success_unix == 0) return false;
    uint64_t now = (uint64_t)time(NULL);
    return (now - g_stats.last_wifi_success_unix) < HTTPS_RECENT_S;
}

static esp_err_t try_wifi(const pageros_transport_request_t *req,
                          uint32_t timeout_ms,
                          pageros_transport_response_t *out)
{
    uint8_t *resp = (uint8_t *)malloc(RESP_BUF_BYTES);
    if (!resp) return ESP_ERR_NO_MEM;
    pageros_https_response_t r = {0};
    esp_err_t e = pageros_https_post(req->target_url,
                                     req->content_type ? req->content_type : "application/cbor",
                                     req->body, req->body_len,
                                     resp, RESP_BUF_BYTES, &r);
    if (e != ESP_OK) {
        free(resp);
        g_stats.wifi_fail++;
        return e;
    }
    out->used = PAGEROS_TRANSPORT_WIFI;
    out->http_status = r.status_code;
    // Trim to actual body length (https_response_t carries it).
    out->body = resp;
    out->body_len = r.body_len < RESP_BUF_BYTES ? r.body_len : RESP_BUF_BYTES;
    g_stats.wifi_success++;
    g_stats.last_wifi_success_unix = (uint64_t)time(NULL);
    (void)timeout_ms;  // pageros_https_post uses its own internal timeout
    return ESP_OK;
}

static esp_err_t try_lora(const pageros_transport_request_t *req,
                          pageros_transport_response_t *out)
{
    uint8_t *resp = NULL; size_t resp_len = 0;
    esp_err_t e = pageros_lora_client_request(req->target_url,
                                              req->app_pubkey,
                                              req->body, req->body_len,
                                              &resp, &resp_len,
                                              req->timeout_ms ? req->timeout_ms : DEFAULT_TIMEOUT);
    if (e != ESP_OK) { g_stats.lora_fail++; return e; }
    out->used = PAGEROS_TRANSPORT_LORA;
    out->http_status = 0;
    out->body = resp;
    out->body_len = resp_len;
    g_stats.lora_success++;
    return ESP_OK;
}

esp_err_t pageros_transport_request(const pageros_transport_request_t *req,
                                    pageros_transport_response_t *out)
{
    if (!req || !out || !req->target_url || !req->body) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    // Step 1: known-good Wi-Fi window
    if (wifi_recent()) {
        if (try_wifi(req, req->timeout_ms ? req->timeout_ms : DEFAULT_TIMEOUT, out) == ESP_OK) {
            return ESP_OK;
        }
        // fall through
    }
    // Step 2: try Wi-Fi with a fast timeout to probe association
    if (try_wifi(req, HTTPS_FAST_TIMEOUT, out) == ESP_OK) return ESP_OK;

    // Step 3: LoRa if allowed
    if (req->lora_compatible) {
        if (try_lora(req, out) == ESP_OK) return ESP_OK;
    }

    g_stats.no_transport++;
    ESP_LOGW(TAG, "no transport available for %s (lora_compatible=%d)",
             req->target_url, req->lora_compatible);
    return ESP_ERR_NOT_SUPPORTED;
}

void pageros_transport_get_stats(pageros_transport_stats_t *out)
{
    if (out) *out = g_stats;
}
