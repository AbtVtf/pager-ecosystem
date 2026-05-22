// SPDX-License-Identifier: Apache-2.0
//
// PagerOS Wi-Fi + HTTPS client — see `pageros_network.h`.

#include "pageros_network.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "url.h"

static const char *TAG = "net";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_RECONNECT_MAX  5

static struct {
    bool                initialised;
    bool                got_ip;
    EventGroupHandle_t  events;
    esp_netif_t        *sta_netif;
    int                 retries;
} s_state;

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state.got_ip = false;
        if (s_state.retries < WIFI_RECONNECT_MAX) {
            s_state.retries++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "wifi reconnect attempt %d", s_state.retries);
        } else {
            xEventGroupSetBits(s_state.events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_state.retries = 0;
        s_state.got_ip  = true;
        xEventGroupSetBits(s_state.events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t pageros_wifi_init(void)
{
    if (s_state.initialised) return ESP_OK;

    // Required by esp_wifi — caller should have already initialised NVS
    // (main.c does, for FW-014). If for some reason it didn't, surface
    // an error rather than panic.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_state.sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_state.sta_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    s_state.events = xEventGroupCreate();
    if (!s_state.events) return ESP_ERR_NO_MEM;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    s_state.initialised = true;
    ESP_LOGI(TAG, "wifi station initialised");
    return ESP_OK;
}

esp_err_t pageros_wifi_connect(const char *ssid, const char *pwd,
                                uint32_t timeout_ms)
{
    if (!s_state.initialised) {
        esp_err_t err = pageros_wifi_init();
        if (err != ESP_OK) return err;
    }
    if (!ssid || strlen(ssid) == 0
        || strlen(ssid) >= PAGEROS_WIFI_SSID_MAX
        || (pwd && strlen(pwd) >= PAGEROS_WIFI_PWD_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pwd) {
        strncpy((char *)cfg.sta.password, pwd,
                sizeof(cfg.sta.password) - 1);
    }
    // Open network if no password supplied.
    cfg.sta.threshold.authmode =
        (pwd && pwd[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    s_state.retries = 0;
    s_state.got_ip  = false;
    xEventGroupClearBits(s_state.events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) return err;

    EventBits_t bits = xEventGroupWaitBits(s_state.events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAIL_BIT)      return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

bool pageros_wifi_is_connected(void)
{
    return s_state.got_ip;
}

esp_err_t pageros_wifi_disconnect(void)
{
    if (!s_state.initialised) return ESP_OK;
    s_state.got_ip = false;
    (void)esp_wifi_disconnect();
    return esp_wifi_stop();
}

// ---------------------------------------------------------------------------
// HTTPS client
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     truncated;
} http_sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_sink_t *sink = (http_sink_t *)evt->user_data;
    if (!sink || !sink->buf || sink->cap == 0) {
        sink->truncated = true;
        return ESP_OK;
    }
    int n = evt->data_len;
    size_t space = sink->cap - sink->len;
    if ((size_t)n > space) {
        sink->truncated = true;
        n = (int)space;
    }
    if (n > 0) {
        memcpy(sink->buf + sink->len, evt->data, (size_t)n);
        sink->len += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t perform_request(esp_http_client_method_t method,
                                  const char              *url,
                                  const char              *content_type,
                                  const uint8_t           *body,
                                  size_t                   body_len,
                                  uint8_t                 *out_buf,
                                  size_t                   out_cap,
                                  pageros_https_response_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    // Allow plain http:// for local development (sideload from a
    // laptop on the same LAN). Marketplace apps go through https://
    // in practice; this just removes the hard refusal so dev URLs
    // like http://192.168.x.y:8000 actually fire.
    if (!pageros_url_is_https(url)) {
        ESP_LOGW(TAG, "non-HTTPS URL accepted (dev): %s", url);
    }
    if (!s_state.got_ip) return ESP_ERR_INVALID_STATE;

    http_sink_t sink = { .buf = out_buf, .cap = out_cap };

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = method,
        .timeout_ms     = 15000,
        .event_handler  = http_event,
        .user_data      = &sink,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (method == HTTP_METHOD_POST && body && body_len > 0) {
        esp_http_client_set_header(client, "Content-Type",
                                   content_type ? content_type
                                                : "application/octet-stream");
        esp_http_client_set_post_field(client, (const char *)body,
                                       (int)body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status_code    = esp_http_client_get_status_code(client);
        out->body_len       = sink.len;
        out->body_truncated = sink.truncated;
    } else {
        ESP_LOGW(TAG, "http perform: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t pageros_https_get(const char *url,
                            uint8_t    *body_buf, size_t body_cap,
                            pageros_https_response_t *out)
{
    return perform_request(HTTP_METHOD_GET, url, NULL, NULL, 0,
                            body_buf, body_cap, out);
}

esp_err_t pageros_https_post(const char    *url,
                             const char    *content_type,
                             const uint8_t *body,    size_t body_len,
                             uint8_t       *resp_buf, size_t resp_cap,
                             pageros_https_response_t *out)
{
    return perform_request(HTTP_METHOD_POST, url, content_type,
                            body, body_len, resp_buf, resp_cap, out);
}
