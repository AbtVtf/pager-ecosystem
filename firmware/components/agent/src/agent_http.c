// SPDX-License-Identifier: Apache-2.0
//
// OpenRouter / OpenAI-compatible /chat/completions client.
//
// pageros_https_post() in the network component doesn't accept custom
// headers, but OpenRouter needs `Authorization: Bearer <key>` (and
// nice-to-have `HTTP-Referer` + `X-Title`). So this file talks to
// esp_http_client directly — same underlying stack as the network
// component's wrapper, just with the headers we need. TLS still goes
// through the bundled Mozilla CA roots via esp_crt_bundle_attach.
//
// Request shape:
//   {
//     "model": "<model>",
//     "messages": [ ... ],
//     "tools":    [ ... ],   // only when tools are registered
//     "tool_choice": "auto",
//     "temperature": 0.5
//   }
//
// Response shape (we only need a slice):
//   {
//     "choices": [{
//       "message": {
//         "role": "assistant",
//         "content": "...",            // may be empty / null
//         "tool_calls": [               // present when the model wants tools
//           { "id": "...", "type": "function",
//             "function": { "name": "...", "arguments": "<json-string>" }}
//         ]
//       }
//     }]
//   }

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"

#include "agent_internal.h"

static const char *TAG = "agent_http";

#define REQ_BUF_MAX  (16 * 1024)
#define RESP_BUF_MAX (16 * 1024)

// --- response capture --------------------------------------------- //

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;
} resp_capture_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    resp_capture_t *rc = (resp_capture_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !rc || !rc->buf) {
        return ESP_OK;
    }
    int len = evt->data_len;
    if (rc->len + (size_t)len > rc->cap - 1) {
        // Keep what fits, drop the rest.
        int keep = (int)(rc->cap - 1 - rc->len);
        if (keep > 0) {
            memcpy(rc->buf + rc->len, evt->data, keep);
            rc->len += keep;
        }
        rc->truncated = true;
        return ESP_OK;
    }
    memcpy(rc->buf + rc->len, evt->data, len);
    rc->len += len;
    rc->buf[rc->len] = '\0';
    return ESP_OK;
}

// --- request body building ---------------------------------------- //

static char *build_request_body(size_t *out_len)
{
    const agent_config_t *cfg = agent_config_get();
    if (!cfg) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg->model);

    cJSON *msgs = agent_messages_to_json_array();
    if (msgs) {
        cJSON_AddItemToObject(root, "messages", msgs);
    } else {
        cJSON_AddItemToObject(root, "messages", cJSON_CreateArray());
    }

    cJSON *tools = agent_tools_to_json_array();
    if (tools) {
        cJSON_AddItemToObject(root, "tools", tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    cJSON_AddNumberToObject(root, "temperature", 0.5);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return NULL;
    if (out_len) *out_len = strlen(body);
    return body;
}

// --- response parsing --------------------------------------------- //

static void set_error(agent_http_result_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, ap);
    va_end(ap);
}

static esp_err_t parse_response_into_result(const char *json,
                                            agent_http_result_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_error(out, "response parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Surface API-level error messages (auth failures, model not found,
    // rate limit, etc.) so the user sees something actionable.
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(err)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            set_error(out, "API: %.140s", msg->valuestring);
        } else {
            set_error(out, "API error (no message)");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        set_error(out, "no choices in response");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *choice0  = cJSON_GetArrayItem(choices, 0);
    cJSON *message  = cJSON_GetObjectItemCaseSensitive(choice0, "message");
    if (!cJSON_IsObject(message)) {
        set_error(out, "no message in choice");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring &&
        content->valuestring[0]) {
        out->content = strdup(content->valuestring);
    }

    cJSON *tcs = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs) > 0) {
        // Re-serialise the array so we can stash it as a string in
        // the assistant message slot — agent_messages_to_json_array
        // turns it back into structured JSON for the next request.
        out->tool_calls_json = cJSON_PrintUnformatted(tcs);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// --- public ------------------------------------------------------- //

void agent_http_result_free(agent_http_result_t *r)
{
    if (!r) return;
    free(r->content);          r->content = NULL;
    free(r->tool_calls_json);  r->tool_calls_json = NULL;
}

esp_err_t agent_http_complete(agent_http_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const agent_config_t *cfg = agent_config_get();
    if (!cfg) {
        set_error(out, "no config loaded");
        return ESP_ERR_INVALID_STATE;
    }

    size_t body_len = 0;
    char *body = build_request_body(&body_len);
    if (!body) {
        set_error(out, "OOM building request");
        return ESP_ERR_NO_MEM;
    }
    if (body_len > REQ_BUF_MAX) {
        ESP_LOGW(TAG, "request body %u bytes (cap %u) — sending anyway",
                 (unsigned)body_len, REQ_BUF_MAX);
    }
    ESP_LOGI(TAG, "POST %s (%u bytes)", cfg->base_url, (unsigned)body_len);

    // Build the URL: base_url + "/chat/completions"
    char url[192];
    int n = snprintf(url, sizeof(url), "%s/chat/completions", cfg->base_url);
    if (n < 0 || n >= (int)sizeof(url)) {
        free(body);
        set_error(out, "URL too long");
        return ESP_ERR_INVALID_ARG;
    }

    resp_capture_t rc = {
        .buf = (char *)calloc(1, RESP_BUF_MAX),
        .cap = RESP_BUF_MAX,
    };
    if (!rc.buf) {
        free(body);
        set_error(out, "OOM response buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t hcfg = {
        .url             = url,
        .method          = HTTP_METHOD_POST,
        .timeout_ms      = cfg->request_timeout_ms,
        .event_handler   = http_event_cb,
        .user_data       = &rc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        free(body); free(rc.buf);
        set_error(out, "http_client_init failed");
        return ESP_FAIL;
    }

    char auth[320];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    // OpenRouter likes these — harmless on other backends.
    esp_http_client_set_header(client, "HTTP-Referer", "https://pageros.dev");
    esp_http_client_set_header(client, "X-Title",      "PagerOS Cyberdeck");

    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t e = esp_http_client_perform(client);
    out->status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(body);

    if (e != ESP_OK) {
        free(rc.buf);
        set_error(out, "transport: %s", esp_err_to_name(e));
        return e;
    }

    if (out->status_code / 100 != 2) {
        // Surface a hint of the response body so 400/401/429 errors are
        // diagnosable from the screen instead of needing the UART.
        const char *body_hint = rc.buf;
        size_t hint_len = rc.len < 120 ? rc.len : 120;
        set_error(out, "HTTP %d: %.*s",
                  out->status_code, (int)hint_len, body_hint);
        free(rc.buf);
        return ESP_FAIL;
    }

    if (rc.truncated) {
        ESP_LOGW(TAG, "response truncated to %u bytes", (unsigned)rc.cap);
    }

    esp_err_t pe = parse_response_into_result(rc.buf, out);
    free(rc.buf);
    return pe;
}
