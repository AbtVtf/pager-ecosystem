// SPDX-License-Identifier: Apache-2.0
//
// PagerOS Wi-Fi + HTTPS client — FW-009.
//
// Brings up esp_wifi in station mode and exposes a minimal HTTPS client
// (`pageros_https_get` / `pageros_https_post`) on top of esp_http_client
// with mbedTLS, attached to ESP-IDF's bundled Mozilla CA roots
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
//
// Per SPEC §6.3 (Transport selection) Wi-Fi is the preferred path when
// reachable. The transport selector (FW-025) builds on this client.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_WIFI_SSID_MAX  32
#define PAGEROS_WIFI_PWD_MAX   64

// One-time global init for the lwIP stack, default event loop, and the
// esp_wifi station driver. Safe to call more than once. Does not start
// a connection — call `pageros_wifi_connect` for that.
esp_err_t pageros_wifi_init(void);

// Connect (or reconnect) to an SSID with WPA2-PSK credentials. Blocks up
// to `timeout_ms` for the IP_EVENT_STA_GOT_IP event. Returns ESP_OK once
// a DHCP lease is in hand, ESP_ERR_TIMEOUT if no lease before the
// deadline, or the underlying esp_wifi error.
esp_err_t pageros_wifi_connect(const char *ssid,
                               const char *pwd,
                               uint32_t    timeout_ms);

// True iff the station has an active IP lease. Updated by the underlying
// event handlers, so it can flip back to false on signal loss.
bool pageros_wifi_is_connected(void);

// Disconnect + stop the radio. Safe to call when not connected.
esp_err_t pageros_wifi_disconnect(void);

// -- HTTPS client ----------------------------------------------------------

typedef struct {
    int     status_code;   // populated on success and 4xx/5xx alike
    size_t  body_len;      // bytes written to `body_buf`
    bool    body_truncated;// true if response exceeded `body_cap`
} pageros_https_response_t;

// Perform an HTTPS GET. Returns ESP_OK on transport success (any HTTP
// status); inspect `out->status_code` for the result. Returns
// ESP_ERR_HTTPS_* / esp_err_t on TLS / DNS / socket errors. The
// response body is written into `body_buf` up to `body_cap`; if the
// server returns more, `body_truncated` is set and the excess is
// dropped (the caller decides whether to retry).
//
// Verifies the server certificate chain against the bundled Mozilla
// roots (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
esp_err_t pageros_https_get(const char              *url,
                            uint8_t                 *body_buf,
                            size_t                   body_cap,
                            pageros_https_response_t *out);

// Perform an HTTPS POST with a raw body and an explicit Content-Type.
// Same status / error semantics as `pageros_https_get`. `content_type`
// may be NULL — in which case the client sets `application/octet-stream`.
esp_err_t pageros_https_post(const char              *url,
                             const char              *content_type,
                             const uint8_t           *body,    size_t body_len,
                             uint8_t                 *resp_buf, size_t resp_cap,
                             pageros_https_response_t *out);

#ifdef __cplusplus
}
#endif
