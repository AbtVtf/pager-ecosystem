// SPDX-License-Identifier: Apache-2.0
//
// Network tools: http_get. Uses the pageros_https_get wrapper so the
// CA bundle, timeouts, etc. are consistent with the rest of the OS.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "pageros_network.h"

#include "agent_internal.h"

#define HTTP_RESPONSE_MAX 4096

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

static esp_err_t http_get_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(u) || !u->valuestring || !u->valuestring[0]) {
        cJSON_Delete(root);
        return encode_err("url required", out, cap);
    }

    if (!pageros_wifi_is_connected()) {
        cJSON_Delete(root);
        return encode_err("wifi not connected", out, cap);
    }

    uint8_t *body = (uint8_t *)malloc(HTTP_RESPONSE_MAX);
    if (!body) { cJSON_Delete(root); return encode_err("OOM", out, cap); }
    pageros_https_response_t resp = {0};
    esp_err_t r = pageros_https_get(u->valuestring, body, HTTP_RESPONSE_MAX, &resp);
    cJSON_Delete(root);

    if (r != ESP_OK) {
        free(body);
        return encode_err(esp_err_to_name(r), out, cap);
    }

    // Sanitize for transport — non-printable bytes become '.'.
    size_t n = resp.body_len < HTTP_RESPONSE_MAX - 1 ? resp.body_len : HTTP_RESPONSE_MAX - 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = body[i];
        if (b != '\n' && b != '\r' && b != '\t' && (b < 0x20 || b >= 0x7f)) {
            body[i] = '.';
        }
    }
    body[n] = '\0';

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status",    resp.status_code);
    cJSON_AddNumberToObject(o, "bytes",     (int)resp.body_len);
    cJSON_AddBoolToObject(o,   "truncated", resp.body_truncated);
    cJSON_AddStringToObject(o, "body",      (const char *)body);
    free(body);
    return encode_ok(o, out, cap);
}

void agent_tools_register_net(void)
{
    pageros_agent_tool_register(
        "http_get",
        "Perform an HTTPS GET against a URL. Returns {status, bytes, "
        "truncated, body}. Response body is capped at 4096 bytes. TLS "
        "is verified against the bundled Mozilla CA roots. Requires Wi-Fi.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"https://… URL\"}"
         "},"
         "\"required\":[\"url\"]}",
        http_get_fn, NULL);
}
