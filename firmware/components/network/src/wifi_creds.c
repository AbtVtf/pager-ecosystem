// SPDX-License-Identifier: Apache-2.0
//
// Persisted Wi-Fi credentials. Lives next to the rest of the network
// component so the auto-connect path can pull the saved SSID/PSK
// without dragging another header into main.c.

#include "pageros_network.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_creds";
#define NS  "wifi_creds"
#define K_SSID "ssid"
#define K_PSK  "psk"

esp_err_t pageros_wifi_creds_save(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_str(h, K_SSID, ssid);
    if (r == ESP_OK) {
        r = nvs_set_str(h, K_PSK, psk ? psk : "");
    }
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved (ssid=%.16s, psk=%s)", ssid, psk && *psk ? "set" : "open");
    return r;
}

esp_err_t pageros_wifi_creds_load(char *ssid, size_t ssid_cap,
                                  char *psk,  size_t psk_cap)
{
    if (!ssid || ssid_cap == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READONLY, &h);
    if (r != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t sz = ssid_cap;
    r = nvs_get_str(h, K_SSID, ssid, &sz);
    if (r != ESP_OK) { nvs_close(h); return ESP_ERR_NOT_FOUND; }
    if (psk && psk_cap > 0) {
        sz = psk_cap;
        if (nvs_get_str(h, K_PSK, psk, &sz) != ESP_OK) {
            psk[0] = '\0';
        }
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t pageros_wifi_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    nvs_erase_key(h, K_SSID);
    nvs_erase_key(h, K_PSK);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
