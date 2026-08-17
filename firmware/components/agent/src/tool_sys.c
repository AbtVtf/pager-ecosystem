// SPDX-License-Identifier: Apache-2.0
//
// System tools: identity, uptime, power_status, imu_status, notify,
// tone, lock_device.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "cJSON.h"

#include "pageros_audio.h"
#include "pageros_identity.h"
#include "pageros_imu.h"
#include "pageros_power.h"
#include "pageros_widgets.h"

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

// --- identity ---------------------------------------------------- //

static esp_err_t identity_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    uint8_t pk[PAGEROS_IDENTITY_PUBKEY_LEN] = {0};
    esp_err_t r1 = pageros_identity_fingerprint(fp);
    esp_err_t r2 = pageros_identity_pubkey(pk);
    if (r1 != ESP_OK || r2 != ESP_OK) {
        return encode_err("identity not ready", out, cap);
    }
    char hex[2 * PAGEROS_IDENTITY_PUBKEY_LEN + 1];
    for (int i = 0; i < PAGEROS_IDENTITY_PUBKEY_LEN; i++) {
        sprintf(hex + i * 2, "%02x", pk[i]);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fingerprint", fp);
    cJSON_AddStringToObject(o, "pubkey_hex",  hex);
    return encode_ok(o, out, cap);
}

// --- uptime ------------------------------------------------------ //

static esp_err_t uptime_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    int64_t us = esp_timer_get_time();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "uptime_s",  (double)(us / 1000000));
    cJSON_AddNumberToObject(o, "uptime_us", (double)us);
    return encode_ok(o, out, cap);
}

// --- power_status ----------------------------------------------- //

static const char *power_state_name(pageros_power_state_t s)
{
    switch (s) {
    case PAGEROS_POWER_ACTIVE:      return "ACTIVE";
    case PAGEROS_POWER_DIM:         return "DIM";
    case PAGEROS_POWER_SCREEN_OFF:  return "SCREEN_OFF";
    case PAGEROS_POWER_LIGHT_SLEEP: return "LIGHT_SLEEP";
    case PAGEROS_POWER_DEEP_SLEEP:  return "DEEP_SLEEP";
    }
    return "?";
}

static esp_err_t power_status_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state",          power_state_name(pageros_power_state()));
    cJSON_AddNumberToObject(o, "ms_since_input", pageros_power_ms_since_kick());
    return encode_ok(o, out, cap);
}

// --- imu_status ------------------------------------------------- //

static esp_err_t imu_status_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "present", pageros_imu_present());
    cJSON_AddNumberToObject(o, "chip_id", pageros_imu_chip_id());
    cJSON_AddStringToObject(o, "note",
        "BHI260AP detected only; sensor fusion firmware upload is a v2 feature");
    return encode_ok(o, out, cap);
}

// --- notify ---------------------------------------------------- //

static esp_err_t notify_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *lvl = cJSON_GetObjectItemCaseSensitive(root, "level");
    if (!cJSON_IsString(txt) || !txt->valuestring) {
        cJSON_Delete(root);
        return encode_err("text required", out, cap);
    }
    pageros_toast_level_t level = PAGEROS_TOAST_INFO;
    if (cJSON_IsString(lvl)) {
        const char *s = lvl->valuestring;
        if      (!strcmp(s, "ok"))    level = PAGEROS_TOAST_OK;
        else if (!strcmp(s, "warn"))  level = PAGEROS_TOAST_WARN;
        else if (!strcmp(s, "error")) level = PAGEROS_TOAST_ERROR;
    }
    pageros_toast(txt->valuestring, level, 2500);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return encode_ok(o, out, cap);
}

// --- tone ------------------------------------------------------- //

static esp_err_t tone_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    pageros_tone_t t = PAGEROS_TONE_DEFAULT;
    if (root) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(n)) {
            int idx = pageros_tone_from_name(n->valuestring);
            if (idx >= 0) t = (pageros_tone_t)idx;
        }
        cJSON_Delete(root);
    }
    esp_err_t r = pageros_audio_play_tone(t);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", r == ESP_OK);
    cJSON_AddStringToObject(o, "tone", pageros_tone_name(t));
    return encode_ok(o, out, cap);
}

// --- lock_device ----------------------------------------------- //
//
// Lives in main.c — declared extern; tool calls it to drop straight
// back to the lock screen.

extern void agent_request_lock(void);

static esp_err_t lock_device_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    agent_request_lock();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "status", "lock requested");
    return encode_ok(o, out, cap);
}

void agent_tools_register_sys(void)
{
    pageros_agent_tool_register(
        "identity",
        "Get the device's Ed25519 identity. Returns {fingerprint, pubkey_hex}.",
        "{\"type\":\"object\",\"properties\":{}}",
        identity_fn, NULL);

    pageros_agent_tool_register(
        "uptime",
        "Get the device uptime in seconds and microseconds.",
        "{\"type\":\"object\",\"properties\":{}}",
        uptime_fn, NULL);

    pageros_agent_tool_register(
        "power_status",
        "Get the power state machine state and milliseconds since last "
        "user input.",
        "{\"type\":\"object\",\"properties\":{}}",
        power_status_fn, NULL);

    pageros_agent_tool_register(
        "imu_status",
        "Check the Bosch BHI260AP IMU presence. Sensor fusion is not "
        "wired in this firmware; only the chip-id probe is exposed.",
        "{\"type\":\"object\",\"properties\":{}}",
        imu_status_fn, NULL);

    pageros_agent_tool_register(
        "notify",
        "Show a transient on-screen toast notification. level ∈ "
        "{info, ok, warn, error}.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"text\":{\"type\":\"string\"},"
            "\"level\":{\"type\":\"string\",\"enum\":[\"info\",\"ok\",\"warn\",\"error\"]}"
         "},"
         "\"required\":[\"text\"]}",
        notify_fn, NULL);

    pageros_agent_tool_register(
        "tone",
        "Play a built-in notification tone through the ES8311 codec. "
        "name ∈ {default, low_priority, alert, success, error}.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"enum\":[\"default\",\"low_priority\",\"alert\",\"success\",\"error\"]}"
         "}}",
        tone_fn, NULL);

    pageros_agent_tool_register(
        "lock_device",
        "Lock the device — return to the jack-in lock screen. The user "
        "will have to press ENTER (and enter the PIN if set) to come back.",
        "{\"type\":\"object\",\"properties\":{}}",
        lock_device_fn, NULL);
}
