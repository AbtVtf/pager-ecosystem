// SPDX-License-Identifier: Apache-2.0
//
// Loads /sd/agent/config.json into the global agent_config_t. Falls
// back to defaults for any optional field.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cJSON.h"

#include "pageros_storage.h"

#include "agent_internal.h"

static const char *TAG = "agent_cfg";

#define AGENT_CONFIG_PATH "/sd/agent/config.json"

static const char *DEFAULT_BASE_URL =
    "https://openrouter.ai/api/v1";

static const char *DEFAULT_MODEL =
    "google/gemini-3.5-flash";

// Dev-mode default key so the device works out-of-the-box without an
// SD card config file. SD config (when present) still overrides. ROTATE
// THIS BEFORE COMMITTING — anyone with the firmware binary can read it.
static const char *DEFAULT_API_KEY =
    "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";

static const char *DEFAULT_SYSTEM_PROMPT =
    "You are the on-device agent of a PagerOS cyberdeck — a hand-held "
    "device with Wi-Fi, Bluetooth LE, LoRa, and GPS. The user 'jacks in' "
    "to interact with you. You have access to tools that drive these "
    "radios and sensors. Be terse — the screen is 480x222 pixels and the "
    "user reads everything on it. Call tools to act on the physical "
    "world; do not invent results. When a tool returns an error, say so "
    "plainly. End every reply with a one-line summary of what you did.";

static agent_config_t g_cfg = {0};

static void copy_string(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static char *read_file_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 8192) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

esp_err_t agent_config_load(void)
{
    // Seed defaults — including a baked-in dev key so the device works
    // without an SD card config file. SD config (if present) overrides
    // every field below.
    memset(&g_cfg, 0, sizeof(g_cfg));
    copy_string(g_cfg.api_key,  sizeof(g_cfg.api_key),  DEFAULT_API_KEY);
    copy_string(g_cfg.base_url, sizeof(g_cfg.base_url), DEFAULT_BASE_URL);
    copy_string(g_cfg.model,    sizeof(g_cfg.model),    DEFAULT_MODEL);
    copy_string(g_cfg.system_prompt, sizeof(g_cfg.system_prompt),
                DEFAULT_SYSTEM_PROMPT);
    g_cfg.max_iterations     = 6;
    g_cfg.request_timeout_ms = 30000;

    // Lazy SD mount — eager mount at boot fights the display on the
    // shared SPI bus. Mount here only if we're about to read a config.
    esp_err_t sd_err = pageros_storage_init();
    if (sd_err != ESP_OK) {
        ESP_LOGI(TAG, "SD unavailable (%s) — using baked defaults",
                 esp_err_to_name(sd_err));
        goto check_key;
    }

    size_t flen = 0;
    char *raw = read_file_all(AGENT_CONFIG_PATH, &flen);
    if (!raw) {
        ESP_LOGI(TAG, "no SD config at %s — using baked defaults",
                 AGENT_CONFIG_PATH);
        goto check_key;
    }

    cJSON *root = cJSON_ParseWithLength(raw, flen);
    free(raw);
    if (!root || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "config parse failed");
        if (root) cJSON_Delete(root);
        g_cfg.loaded = false;
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *key   = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON *base  = cJSON_GetObjectItemCaseSensitive(root, "base_url");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON *sys   = cJSON_GetObjectItemCaseSensitive(root, "system_prompt");
    const cJSON *maxi  = cJSON_GetObjectItemCaseSensitive(root, "max_iterations");
    const cJSON *tmo   = cJSON_GetObjectItemCaseSensitive(root, "request_timeout_ms");

    if (cJSON_IsString(key)) {
        copy_string(g_cfg.api_key, sizeof(g_cfg.api_key), key->valuestring);
    }
    if (cJSON_IsString(base) && base->valuestring && base->valuestring[0]) {
        copy_string(g_cfg.base_url, sizeof(g_cfg.base_url), base->valuestring);
    }
    if (cJSON_IsString(model) && model->valuestring && model->valuestring[0]) {
        copy_string(g_cfg.model, sizeof(g_cfg.model), model->valuestring);
    }
    if (cJSON_IsString(sys) && sys->valuestring && sys->valuestring[0]) {
        copy_string(g_cfg.system_prompt, sizeof(g_cfg.system_prompt),
                    sys->valuestring);
    }
    if (cJSON_IsNumber(maxi) && maxi->valueint > 0 && maxi->valueint < 20) {
        g_cfg.max_iterations = maxi->valueint;
    }
    if (cJSON_IsNumber(tmo) && tmo->valueint >= 5000) {
        g_cfg.request_timeout_ms = tmo->valueint;
    }

    cJSON_Delete(root);

check_key:
    if (g_cfg.api_key[0] == '\0') {
        ESP_LOGE(TAG, "no api_key in defaults or SD config");
        g_cfg.loaded = false;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "loaded: model=%s base=%s", g_cfg.model, g_cfg.base_url);
    g_cfg.loaded = true;
    return ESP_OK;
}

const agent_config_t *agent_config_get(void)
{
    return g_cfg.loaded ? &g_cfg : NULL;
}
