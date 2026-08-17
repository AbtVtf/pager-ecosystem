// SPDX-License-Identifier: Apache-2.0
//
// Wi-Fi tools: scan, connect, disconnect, status.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "cJSON.h"

#include "pageros_network.h"

#include "agent_internal.h"

static const char *TAG = "tool_wifi";

#define WIFI_SCAN_MAX_APS 24

static const char *auth_mode_str(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
    default:                        return "UNKNOWN";
    }
}

static esp_err_t encode_err(const char *msg, char *out, size_t cap)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg ? msg : "unknown");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return ESP_ERR_NO_MEM;
    strncpy(out, s, cap - 1); out[cap - 1] = '\0';
    free(s);
    return ESP_OK;
}

static esp_err_t encode_ok(cJSON *obj, char *out, size_t cap)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return ESP_ERR_NO_MEM;
    strncpy(out, s, cap - 1); out[cap - 1] = '\0';
    free(s);
    return ESP_OK;
}

// --- wifi_scan --------------------------------------------------- //

static esp_err_t wifi_scan_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    wifi_scan_config_t cfg = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 250,
    };
    esp_err_t r = esp_wifi_scan_start(&cfg, true);
    if (r != ESP_OK) return encode_err(esp_err_to_name(r), out, cap);

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) { strncpy(out, "[]", cap - 1); out[cap - 1] = '\0'; return ESP_OK; }
    if (found > WIFI_SCAN_MAX_APS) found = WIFI_SCAN_MAX_APS;

    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (!recs) return encode_err("OOM", out, cap);
    r = esp_wifi_scan_get_ap_records(&found, recs);
    if (r != ESP_OK) { free(recs); return encode_err(esp_err_to_name(r), out, cap); }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < found; i++) {
        cJSON *ap = cJSON_CreateObject();
        char ssid[33];
        memcpy(ssid, recs[i].ssid, 32); ssid[32] = '\0';
        cJSON_AddStringToObject(ap, "ssid", ssid);
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                 recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
        cJSON_AddStringToObject(ap, "bssid",   bssid);
        cJSON_AddNumberToObject(ap, "rssi",    recs[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", recs[i].primary);
        cJSON_AddStringToObject(ap, "auth",    auth_mode_str(recs[i].authmode));
        cJSON_AddItemToArray(arr, ap);
    }
    free(recs);
    ESP_LOGI(TAG, "scan: %d APs", (int)found);
    return encode_ok(arr, out, cap);
}

// --- wifi_connect ------------------------------------------------ //

static esp_err_t wifi_connect_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *psk  = cJSON_GetObjectItemCaseSensitive(root, "psk");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || !ssid->valuestring[0]) {
        cJSON_Delete(root);
        return encode_err("ssid required", out, cap);
    }
    const char *psk_s = (cJSON_IsString(psk) && psk->valuestring) ? psk->valuestring : "";
    esp_err_t r = pageros_wifi_connect(ssid->valuestring, psk_s, 15000);
    if (r == ESP_OK) pageros_wifi_creds_save(ssid->valuestring, psk_s);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", r == ESP_OK);
    cJSON_AddStringToObject(o, "status",
                            r == ESP_OK ? "connected" : esp_err_to_name(r));
    return encode_ok(o, out, cap);
}

// --- wifi_disconnect --------------------------------------------- //

static esp_err_t wifi_disconnect_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    esp_err_t r = pageros_wifi_disconnect();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", r == ESP_OK);
    cJSON_AddStringToObject(o, "status", esp_err_to_name(r));
    return encode_ok(o, out, cap);
}

// --- wifi_status ------------------------------------------------- //

static esp_err_t wifi_status_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    bool connected = pageros_wifi_is_connected();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "connected", connected);
    if (connected) {
        wifi_ap_record_t info = {0};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            char ssid[33];
            memcpy(ssid, info.ssid, 32); ssid[32] = '\0';
            cJSON_AddStringToObject(o, "ssid",    ssid);
            cJSON_AddNumberToObject(o, "rssi",    info.rssi);
            cJSON_AddNumberToObject(o, "channel", info.primary);
            cJSON_AddStringToObject(o, "auth",    auth_mode_str(info.authmode));
        }
    }
    return encode_ok(o, out, cap);
}

void agent_tools_register_wifi(void)
{
    pageros_agent_tool_register(
        "wifi_scan",
        "Scan for nearby Wi-Fi access points (2.4 GHz). Returns a JSON "
        "array of {ssid, bssid, rssi, channel, auth}.",
        "{\"type\":\"object\",\"properties\":{}}",
        wifi_scan_fn, NULL);

    pageros_agent_tool_register(
        "wifi_connect",
        "Connect to a Wi-Fi network. Saves credentials to NVS for auto-"
        "rejoin on next boot. Returns {ok, status}.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"ssid\":{\"type\":\"string\",\"description\":\"network name\"},"
            "\"psk\":{\"type\":\"string\",\"description\":\"WPA passphrase; empty for open\"}"
         "},"
         "\"required\":[\"ssid\"]}",
        wifi_connect_fn, NULL);

    pageros_agent_tool_register(
        "wifi_disconnect",
        "Disconnect from the current Wi-Fi network. Returns {ok, status}.",
        "{\"type\":\"object\",\"properties\":{}}",
        wifi_disconnect_fn, NULL);

    pageros_agent_tool_register(
        "wifi_status",
        "Get current Wi-Fi connection state. Returns {connected, ssid, "
        "rssi, channel, auth} when connected, {connected:false} otherwise.",
        "{\"type\":\"object\",\"properties\":{}}",
        wifi_status_fn, NULL);
}
