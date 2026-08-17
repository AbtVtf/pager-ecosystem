// SPDX-License-Identifier: Apache-2.0
//
// LoRa tools: send arbitrary text packets, listen for incoming, status.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "pageros_lora.h"

#include "agent_internal.h"

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

// --- lora_send --------------------------------------------------- //

static esp_err_t lora_send_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    if (!pageros_lora_is_ready()) return encode_err("LoRa not ready", out, cap);

    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON_Delete(root);
        return encode_err("text required", out, cap);
    }
    const char *payload = text->valuestring;
    size_t plen = strlen(payload);
    if (plen > 250) plen = 250;

    esp_err_t r = pageros_lora_tx((const uint8_t *)payload, plen, 5000);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,  "ok",     r == ESP_OK);
    cJSON_AddNumberToObject(o, "bytes", (int)plen);
    cJSON_AddStringToObject(o, "status", esp_err_to_name(r));
    return encode_ok(o, out, cap);
}

// --- lora_listen ------------------------------------------------- //

static esp_err_t lora_listen_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    if (!pageros_lora_is_ready()) return encode_err("LoRa not ready", out, cap);

    int timeout_ms = 10000;  // default 10s
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (root) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
        if (cJSON_IsNumber(t)) {
            timeout_ms = t->valueint;
            if (timeout_ms < 1000)  timeout_ms = 1000;
            if (timeout_ms > 30000) timeout_ms = 30000;
        }
        cJSON_Delete(root);
    }

    uint8_t buf[256];
    size_t  rxlen = 0;
    int16_t rssi  = 0;
    int16_t snr_q4 = 0;
    esp_err_t r = pageros_lora_rx(buf, sizeof(buf) - 1, &rxlen,
                                  &rssi, &snr_q4, (uint32_t)timeout_ms);
    if (r == ESP_ERR_TIMEOUT) return encode_err("timeout", out, cap);
    if (r != ESP_OK)          return encode_err(esp_err_to_name(r), out, cap);
    buf[rxlen] = '\0';

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "ok",       true);
    cJSON_AddNumberToObject(o, "bytes",    (int)rxlen);
    cJSON_AddStringToObject(o, "text",     (const char *)buf);
    cJSON_AddNumberToObject(o, "rssi_dbm", rssi);
    cJSON_AddNumberToObject(o, "snr_db",   snr_q4 / 4.0);
    return encode_ok(o, out, cap);
}

// --- lora_status ------------------------------------------------- //

static esp_err_t lora_status_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ready", pageros_lora_is_ready());
    uint8_t st = 0;
    if (pageros_lora_get_status(&st) == ESP_OK) {
        cJSON_AddNumberToObject(o, "chip_status", st);
    }
    cJSON_AddStringToObject(o, "band",  "868MHz");
    cJSON_AddStringToObject(o, "modulation", "SF7/BW125");
    return encode_ok(o, out, cap);
}

void agent_tools_register_lora(void)
{
    pageros_agent_tool_register(
        "lora_send",
        "Transmit a text packet over LoRa (Semtech SX1262, 868 MHz, SF7, "
        "125 kHz BW). Up to 250 bytes per packet. Returns {ok, bytes, status}.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"text\":{\"type\":\"string\",\"description\":\"payload, up to 250 bytes UTF-8\"}"
         "},"
         "\"required\":[\"text\"]}",
        lora_send_fn, NULL);

    pageros_agent_tool_register(
        "lora_listen",
        "Wait for one incoming LoRa packet, up to timeout_ms (default "
        "10000, max 30000). Blocks the agent until a packet arrives or "
        "the timeout elapses. Returns {ok, bytes, text, rssi_dbm, snr_db} "
        "on success, {error:'timeout'} otherwise.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":30000}"
         "}}",
        lora_listen_fn, NULL);

    pageros_agent_tool_register(
        "lora_status",
        "Get the current LoRa radio status. Returns {ready, chip_status, "
        "band, modulation}.",
        "{\"type\":\"object\",\"properties\":{}}",
        lora_status_fn, NULL);
}
