// SPDX-License-Identifier: Apache-2.0
//
// FW-028 — app runtime implementation.

#include "pageros_apprt.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "apprt";

#define APPRT_ARENA_BYTES (16 * 1024)  // per-app CBOR working area

static struct {
    bool inited;
    pageros_apprt_opts_t opts;
    pageros_apprt_state_t state;

    char     fg_app_id[PAGEROS_APPRT_MAX_APP_ID];
    uint8_t *fg_arena_backing;
    pgr_cbor_arena_t fg_arena;
    pgr_cbor_value_t *fg_frame;
    uint8_t *fg_raw;
    size_t   fg_raw_len;

    // Recents — fixed-size MRU.
    char recents[PAGEROS_APPRT_RECENTS_MAX][PAGEROS_APPRT_MAX_APP_ID];
    size_t recents_len;

    // Open-apps stack — distinct from recents; persists across switches
    // and reboots. Most-recent first.
    char open_apps[PAGEROS_APPRT_OPEN_MAX][PAGEROS_APPRT_MAX_APP_ID];
    size_t open_apps_len;

    pageros_apprt_stats_t stats;
} g;

static uint64_t now_default(void) { return (uint64_t)time(NULL); }
static uint64_t now_unix(void) { return g.opts.now_unix ? g.opts.now_unix() : now_default(); }

// -------- recents persistence -------------------------------------- //

static const char *ns_name(void)
{
    return g.opts.nvs_namespace ? g.opts.nvs_namespace : "pageros_apprt";
}

static void recents_save(void)
{
    nvs_handle_t h;
    if (nvs_open(ns_name(), NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", (uint8_t)g.recents_len);
    for (size_t i = 0; i < g.recents_len; i++) {
        char key[16]; snprintf(key, sizeof(key), "r%u", (unsigned)i);
        nvs_set_str(h, key, g.recents[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void recents_load(void)
{
    nvs_handle_t h;
    if (nvs_open(ns_name(), NVS_READONLY, &h) != ESP_OK) {
        g.recents_len = 0;
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK) count = 0;
    if (count > PAGEROS_APPRT_RECENTS_MAX) count = PAGEROS_APPRT_RECENTS_MAX;
    g.recents_len = 0;
    for (uint8_t i = 0; i < count; i++) {
        char key[16]; snprintf(key, sizeof(key), "r%u", i);
        size_t sz = sizeof(g.recents[g.recents_len]);
        if (nvs_get_str(h, key, g.recents[g.recents_len], &sz) == ESP_OK &&
            g.recents[g.recents_len][0] != '\0') {
            g.recents_len++;
        }
    }
    nvs_close(h);
}

static void recents_touch(const char *app_id)
{
    // Promote app_id to MRU; cap at MAX.
    int existing = -1;
    for (size_t i = 0; i < g.recents_len; i++) {
        if (strncmp(g.recents[i], app_id, sizeof(g.recents[0])) == 0) {
            existing = (int)i; break;
        }
    }
    if (existing == 0) return;  // already at front
    if (existing > 0) {
        char tmp[PAGEROS_APPRT_MAX_APP_ID];
        strncpy(tmp, g.recents[existing], sizeof(tmp));
        for (int i = existing; i > 0; i--) {
            memcpy(g.recents[i], g.recents[i - 1], sizeof(g.recents[0]));
        }
        memcpy(g.recents[0], tmp, sizeof(g.recents[0]));
    } else {
        // Shift down, insert at front.
        size_t move = g.recents_len < PAGEROS_APPRT_RECENTS_MAX
                          ? g.recents_len : PAGEROS_APPRT_RECENTS_MAX - 1;
        for (size_t i = move; i > 0; i--) {
            memcpy(g.recents[i], g.recents[i - 1], sizeof(g.recents[0]));
        }
        strncpy(g.recents[0], app_id, sizeof(g.recents[0]) - 1);
        g.recents[0][sizeof(g.recents[0]) - 1] = '\0';
        if (g.recents_len < PAGEROS_APPRT_RECENTS_MAX) g.recents_len++;
    }
    recents_save();
}

static void recents_drop(const char *app_id)
{
    for (size_t i = 0; i < g.recents_len; i++) {
        if (strncmp(g.recents[i], app_id, sizeof(g.recents[0])) == 0) {
            for (size_t j = i + 1; j < g.recents_len; j++) {
                memcpy(g.recents[j - 1], g.recents[j], sizeof(g.recents[0]));
            }
            g.recents_len--;
            recents_save();
            return;
        }
    }
}

// -------- open-apps stack ----------------------------------------- //

static void open_apps_save(void)
{
    nvs_handle_t h;
    if (nvs_open(ns_name(), NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ocount", (uint8_t)g.open_apps_len);
    for (size_t i = 0; i < g.open_apps_len; i++) {
        char key[16]; snprintf(key, sizeof(key), "o%u", (unsigned)i);
        nvs_set_str(h, key, g.open_apps[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void open_apps_load(void)
{
    nvs_handle_t h;
    if (nvs_open(ns_name(), NVS_READONLY, &h) != ESP_OK) {
        g.open_apps_len = 0;
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(h, "ocount", &count) != ESP_OK) count = 0;
    if (count > PAGEROS_APPRT_OPEN_MAX) count = PAGEROS_APPRT_OPEN_MAX;
    g.open_apps_len = 0;
    for (uint8_t i = 0; i < count; i++) {
        char key[16]; snprintf(key, sizeof(key), "o%u", i);
        size_t sz = sizeof(g.open_apps[g.open_apps_len]);
        if (nvs_get_str(h, key, g.open_apps[g.open_apps_len], &sz) == ESP_OK
                && g.open_apps[g.open_apps_len][0] != '\0') {
            g.open_apps_len++;
        }
    }
    nvs_close(h);
}

esp_err_t pageros_apprt_open_mark(const char *app_id)
{
    if (!g.inited || !app_id || !*app_id) return ESP_ERR_INVALID_ARG;
    if (strlen(app_id) >= PAGEROS_APPRT_MAX_APP_ID) return ESP_ERR_INVALID_ARG;
    // The shell is the desktop itself — never goes in the open list.
    if (strcmp(app_id, "pageros.shell") == 0) return ESP_OK;

    int existing = -1;
    for (size_t i = 0; i < g.open_apps_len; i++) {
        if (strncmp(g.open_apps[i], app_id, sizeof(g.open_apps[0])) == 0) {
            existing = (int)i; break;
        }
    }
    if (existing == 0) return ESP_OK;  // already front
    if (existing > 0) {
        char tmp[PAGEROS_APPRT_MAX_APP_ID];
        strncpy(tmp, g.open_apps[existing], sizeof(tmp));
        for (int i = existing; i > 0; i--) {
            memcpy(g.open_apps[i], g.open_apps[i - 1], sizeof(g.open_apps[0]));
        }
        memcpy(g.open_apps[0], tmp, sizeof(g.open_apps[0]));
    } else {
        size_t move = g.open_apps_len < PAGEROS_APPRT_OPEN_MAX
                          ? g.open_apps_len : PAGEROS_APPRT_OPEN_MAX - 1;
        for (size_t i = move; i > 0; i--) {
            memcpy(g.open_apps[i], g.open_apps[i - 1], sizeof(g.open_apps[0]));
        }
        strncpy(g.open_apps[0], app_id, sizeof(g.open_apps[0]) - 1);
        g.open_apps[0][sizeof(g.open_apps[0]) - 1] = '\0';
        if (g.open_apps_len < PAGEROS_APPRT_OPEN_MAX) g.open_apps_len++;
    }
    open_apps_save();
    return ESP_OK;
}

esp_err_t pageros_apprt_open_close(const char *app_id)
{
    if (!g.inited || !app_id || !*app_id) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < g.open_apps_len; i++) {
        if (strncmp(g.open_apps[i], app_id, sizeof(g.open_apps[0])) == 0) {
            for (size_t j = i + 1; j < g.open_apps_len; j++) {
                memcpy(g.open_apps[j - 1], g.open_apps[j], sizeof(g.open_apps[0]));
            }
            g.open_apps_len--;
            open_apps_save();
            return ESP_OK;
        }
    }
    return ESP_OK;  // idempotent
}

size_t pageros_apprt_open_list(const char *out_app_ids[], size_t cap)
{
    if (!g.inited || !out_app_ids) return 0;
    size_t n = g.open_apps_len < cap ? g.open_apps_len : cap;
    for (size_t i = 0; i < n; i++) out_app_ids[i] = g.open_apps[i];
    return n;
}

// -------- lifecycle ------------------------------------------------ //

static void fg_release(void)
{
    if (g.fg_arena_backing) { free(g.fg_arena_backing); g.fg_arena_backing = NULL; }
    if (g.fg_raw) { free(g.fg_raw); g.fg_raw = NULL; }
    g.fg_raw_len = 0;
    g.fg_frame = NULL;
    memset(&g.fg_arena, 0, sizeof(g.fg_arena));
    g.fg_app_id[0] = '\0';
}

static esp_err_t fg_load_frame(const uint8_t *bytes, size_t len)
{
    if (!bytes || len == 0) return ESP_ERR_INVALID_ARG;
    if (!g.fg_arena_backing) {
        g.fg_arena_backing = (uint8_t *)malloc(APPRT_ARENA_BYTES);
        if (!g.fg_arena_backing) return ESP_ERR_NO_MEM;
        pgr_cbor_arena_init(&g.fg_arena, g.fg_arena_backing, APPRT_ARENA_BYTES);
    } else {
        pgr_cbor_arena_reset(&g.fg_arena);
    }
    if (g.fg_raw) free(g.fg_raw);
    g.fg_raw = (uint8_t *)malloc(len);
    if (!g.fg_raw) return ESP_ERR_NO_MEM;
    memcpy(g.fg_raw, bytes, len);
    g.fg_raw_len = len;
    pgr_cbor_err_t r = pgr_cbor_decode(g.fg_raw, g.fg_raw_len, &g.fg_arena, &g.fg_frame);
    if (r != PGR_CBOR_OK) {
        ESP_LOGW(TAG, "decode: %s", pgr_cbor_strerror(r));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t pageros_apprt_init(const pageros_apprt_opts_t *opts)
{
    if (g.inited) return ESP_OK;
    pageros_apprt_opts_t resolved = {0};
    if (opts) resolved = *opts;
    g.opts = resolved;
    g.state = PAGEROS_APPRT_STATE_IDLE;
    memset(&g.stats, 0, sizeof(g.stats));
    // NVS init is the caller's job at boot; we just open our namespace.
    recents_load();
    open_apps_load();
    g.inited = true;
    ESP_LOGI(TAG, "init: %u recents, %u open loaded",
             (unsigned)g.recents_len, (unsigned)g.open_apps_len);
    return ESP_OK;
}

esp_err_t pageros_apprt_shutdown(void)
{
    if (!g.inited) return ESP_OK;
    fg_release();
    memset(&g, 0, sizeof(g));
    return ESP_OK;
}

esp_err_t pageros_apprt_open(const char *app_id,
                             const uint8_t *initial_frame_bytes,
                             size_t initial_frame_len)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!app_id || !*app_id) return ESP_ERR_INVALID_ARG;
    if (strlen(app_id) >= PAGEROS_APPRT_MAX_APP_ID) return ESP_ERR_INVALID_ARG;

    g.state = PAGEROS_APPRT_STATE_LOADING;
    if (g.fg_app_id[0] != '\0' && strcmp(g.fg_app_id, app_id) != 0) {
        // Different app — drop the prior fg.
        g.stats.closes++;
        fg_release();
    }
    esp_err_t r = fg_load_frame(initial_frame_bytes, initial_frame_len);
    if (r != ESP_OK) {
        fg_release();
        g.state = PAGEROS_APPRT_STATE_IDLE;
        return r;
    }
    strncpy(g.fg_app_id, app_id, sizeof(g.fg_app_id) - 1);
    g.fg_app_id[sizeof(g.fg_app_id) - 1] = '\0';
    recents_touch(app_id);
    g.state = PAGEROS_APPRT_STATE_FOREGROUND;
    g.stats.opens++;
    (void)now_unix();
    return ESP_OK;
}

esp_err_t pageros_apprt_set_frame(const uint8_t *bytes, size_t len)
{
    if (!g.inited || g.fg_app_id[0] == '\0') return ESP_ERR_INVALID_STATE;
    return fg_load_frame(bytes, len);
}

const char *pageros_apprt_foreground_id(void)
{
    if (!g.inited || g.fg_app_id[0] == '\0') return NULL;
    return g.fg_app_id;
}

const pgr_cbor_value_t *pageros_apprt_foreground_frame(void)
{
    return g.inited ? g.fg_frame : NULL;
}

pageros_apprt_state_t pageros_apprt_state(void)
{
    return g.inited ? g.state : PAGEROS_APPRT_STATE_IDLE;
}

const char *pageros_apprt_back(void)
{
    if (!g.inited || g.fg_app_id[0] == '\0') return NULL;
    g.state = PAGEROS_APPRT_STATE_QUITTING;
    g.stats.back_pops++;
    g.stats.closes++;
    char prev[PAGEROS_APPRT_MAX_APP_ID];
    strncpy(prev, g.fg_app_id, sizeof(prev));
    recents_drop(g.fg_app_id);
    fg_release();
    // Pull the next-most-recent.
    if (g.recents_len > 0 && strcmp(g.recents[0], prev) != 0) {
        // The caller fetches the Frame for this app and calls open().
        // We don't auto-load here; we just report the id.
        g.state = PAGEROS_APPRT_STATE_IDLE;
        return g.recents[0];
    }
    g.state = PAGEROS_APPRT_STATE_IDLE;
    return NULL;
}

esp_err_t pageros_apprt_kill(void)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (g.fg_app_id[0] != '\0') {
        recents_drop(g.fg_app_id);
        g.stats.kills++;
        g.stats.closes++;
    }
    fg_release();
    g.state = PAGEROS_APPRT_STATE_IDLE;
    return ESP_OK;
}

size_t pageros_apprt_recents(const char *out_app_ids[], size_t cap)
{
    if (!g.inited || !out_app_ids) return 0;
    size_t n = g.recents_len < cap ? g.recents_len : cap;
    for (size_t i = 0; i < n; i++) out_app_ids[i] = g.recents[i];
    return n;
}

void pageros_apprt_get_stats(pageros_apprt_stats_t *out)
{
    if (!out) return;
    out->state                   = g.state;
    out->foreground_arena_bytes  = g.fg_arena.used;
    out->opens                   = g.stats.opens;
    out->closes                  = g.stats.closes;
    out->kills                   = g.stats.kills;
    out->back_pops               = g.stats.back_pops;
}
