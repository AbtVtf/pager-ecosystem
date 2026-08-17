// SPDX-License-Identifier: Apache-2.0
//
// NFC tools: status (chip presence + stats), wait for next tag, list
// saved tags.
//
// The driver is callback-driven, but the agent needs a *blocking* read.
// We install our own callback at registration time that captures the
// latest scan into a static slot and signals a binary semaphore. The
// `nfc_wait_tag` tool blocks on that semaphore with a timeout.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"

#include "pageros_nfc.h"

#include "agent_internal.h"

static SemaphoreHandle_t g_nfc_sem = NULL;
static pageros_nfc_scan_t g_last_scan;
static bool g_have_scan = false;

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

static void on_nfc_scan(const pageros_nfc_scan_t *scan, void *user)
{
    (void)user;
    if (!scan || scan->uid_len == 0) return;
    memcpy(&g_last_scan, scan, sizeof(g_last_scan));
    g_have_scan = true;
    if (g_nfc_sem) xSemaphoreGive(g_nfc_sem);

    // Also persist to /sd/nfc/<uid>.bin for the agent to retrieve later.
    char uid_hex[24];
    int copy = scan->uid_len > 10 ? 10 : scan->uid_len;
    for (int i = 0, off = 0; i < copy; i++) {
        off += snprintf(uid_hex + off, sizeof(uid_hex) - off,
                        "%02X", scan->uid[i]);
    }
    char path[64];
    snprintf(path, sizeof(path), "/sd/nfc/%s.bin", uid_hex);
    mkdir("/sd/nfc", 0777);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(scan->uid, 1, scan->uid_len, f);
        if (scan->ndef_len > 0) fwrite(scan->ndef, 1, scan->ndef_len, f);
        fclose(f);
    }
}

// --- nfc_status -------------------------------------------------- //

static esp_err_t nfc_status_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    pageros_nfc_stats_t st = {0};
    pageros_nfc_get_stats(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "present", st.chip_present);
    cJSON_AddNumberToObject(o, "polls",      (double)st.polls);
    cJSON_AddNumberToObject(o, "scans_ok",   (double)st.scans_ok);
    cJSON_AddNumberToObject(o, "spi_errors", (double)st.spi_errors);
    return encode_ok(o, out, cap);
}

// --- nfc_wait_tag ----------------------------------------------- //

static void uid_to_hex(const uint8_t *uid, int len, char *out)
{
    int off = 0;
    for (int i = 0; i < len; i++) {
        off += sprintf(out + off, "%02X%s", uid[i], (i == len - 1) ? "" : ":");
    }
}

static esp_err_t nfc_wait_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    int timeout_ms = 10000;
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

    if (!g_nfc_sem) return encode_err("NFC not initialized", out, cap);
    // Drain any stale signal so we only react to a new scan from now on.
    xSemaphoreTake(g_nfc_sem, 0);
    if (xSemaphoreTake(g_nfc_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return encode_err("timeout", out, cap);
    }
    if (!g_have_scan) return encode_err("no scan", out, cap);

    char uid[32]; uid_to_hex(g_last_scan.uid, g_last_scan.uid_len, uid);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "ok",      true);
    cJSON_AddStringToObject(o, "uid",     uid);
    cJSON_AddNumberToObject(o, "uid_len", g_last_scan.uid_len);
    if (g_last_scan.ndef_len > 0) {
        // Surface NDEF as a printable ASCII slice — full bytes are saved
        // to /sd/nfc/<uid>.bin for offline analysis.
        char ndef_txt[128];
        size_t n = g_last_scan.ndef_len < sizeof(ndef_txt) - 1
                   ? g_last_scan.ndef_len : sizeof(ndef_txt) - 1;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = g_last_scan.ndef[i];
            ndef_txt[i] = (b >= 0x20 && b < 0x7f) ? b : '.';
        }
        ndef_txt[n] = '\0';
        cJSON_AddStringToObject(o, "ndef_preview", ndef_txt);
        cJSON_AddNumberToObject(o, "ndef_bytes",   (int)g_last_scan.ndef_len);
    }
    return encode_ok(o, out, cap);
}

// --- nfc_list_saved --------------------------------------------- //

static esp_err_t nfc_list_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    DIR *d = opendir("/sd/nfc");
    cJSON *arr = cJSON_CreateArray();
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            cJSON_AddItemToArray(arr, cJSON_CreateString(e->d_name));
        }
        closedir(d);
    }
    return encode_ok(arr, out, cap);
}

void agent_tools_register_nfc(void)
{
    g_nfc_sem = xSemaphoreCreateBinary();
    // Replace any previously-installed callback (main.c installed NULL
    // at boot, meaning "use default"). Our callback fans out to the
    // semaphore for nfc_wait_tag + writes to /sd/nfc/.
    pageros_nfc_start(on_nfc_scan, NULL);

    pageros_agent_tool_register(
        "nfc_status",
        "Check the ST25R3916 NFC reader status. Returns {present, polls, "
        "scans_ok, spi_errors}.",
        "{\"type\":\"object\",\"properties\":{}}",
        nfc_status_fn, NULL);

    pageros_agent_tool_register(
        "nfc_wait_tag",
        "Block until an NFC tag is held near the device, up to timeout_ms "
        "(default 10000, max 30000). Returns {ok, uid, uid_len, "
        "ndef_preview, ndef_bytes} on success; full NDEF payload is "
        "saved to /sd/nfc/<uid>.bin.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":30000}"
         "}}",
        nfc_wait_fn, NULL);

    pageros_agent_tool_register(
        "nfc_list_saved",
        "List the filenames of every NFC tag previously scanned and saved "
        "to /sd/nfc/. Returns an array of strings.",
        "{\"type\":\"object\",\"properties\":{}}",
        nfc_list_fn, NULL);
}
