// SPDX-License-Identifier: Apache-2.0
//
// FW-029 — Shell implementation.

#include "pageros_shell.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "pageros_apprt.h"
#include "pageros_storage.h"

// We hand-roll the canonical CBOR rather than spin up the codec tree
// for a small fixed payload — the home Frame is < 256 bytes and the
// rendered shape is fully known at compile time.

static const char *TAG = "shell";

// --- canonical CBOR emit helpers (smallest unsigned form) ----------- //

static int emit_u(uint8_t *buf, size_t cap, size_t *off, uint8_t major, uint64_t v)
{
    if (!buf || *off >= cap) return -1;
    uint8_t mt = major << 5;
    if (v < 24) {
        buf[(*off)++] = mt | (uint8_t)v;
        return 0;
    }
    if (v < 0x100) {
        if (*off + 2 > cap) return -1;
        buf[(*off)++] = mt | 24;
        buf[(*off)++] = (uint8_t)v;
        return 0;
    }
    if (v < 0x10000) {
        if (*off + 3 > cap) return -1;
        buf[(*off)++] = mt | 25;
        buf[(*off)++] = (uint8_t)(v >> 8);
        buf[(*off)++] = (uint8_t)v;
        return 0;
    }
    if (v < 0x100000000ULL) {
        if (*off + 5 > cap) return -1;
        buf[(*off)++] = mt | 26;
        buf[(*off)++] = (uint8_t)(v >> 24);
        buf[(*off)++] = (uint8_t)(v >> 16);
        buf[(*off)++] = (uint8_t)(v >> 8);
        buf[(*off)++] = (uint8_t)v;
        return 0;
    }
    if (*off + 9 > cap) return -1;
    buf[(*off)++] = mt | 27;
    for (int i = 7; i >= 0; i--) buf[(*off)++] = (uint8_t)(v >> (i * 8));
    return 0;
}

static int emit_text(uint8_t *buf, size_t cap, size_t *off, const char *s)
{
    size_t l = strlen(s);
    if (emit_u(buf, cap, off, 3, l) != 0) return -1;
    if (*off + l > cap) return -1;
    memcpy(buf + *off, s, l);
    *off += l;
    return 0;
}

static int emit_array(uint8_t *buf, size_t cap, size_t *off, size_t n)
{ return emit_u(buf, cap, off, 4, n); }

static int emit_map(uint8_t *buf, size_t cap, size_t *off, size_t n)
{ return emit_u(buf, cap, off, 5, n); }

// --- app discovery -------------------------------------------------- //

#define MAX_APPS 12

typedef struct {
    char id[64];
    char name[64];
} app_listing_t;

static size_t list_installed(app_listing_t *out, size_t cap)
{
    if (!cap) return 0;
    DIR *d = opendir(PAGEROS_DIR_APPS);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (e->d_name[0] == '.') continue;
        // Heuristic: dir whose name is the app id; name == last segment.
        strncpy(out[n].id, e->d_name, sizeof(out[n].id) - 1);
        out[n].id[sizeof(out[n].id) - 1] = '\0';
        const char *dot = strrchr(e->d_name, '.');
        const char *base = dot ? dot + 1 : e->d_name;
        // capitalise first letter for display
        out[n].name[0] = (base[0] >= 'a' && base[0] <= 'z')
                             ? (char)(base[0] - 32) : base[0];
        strncpy(out[n].name + 1, base + 1, sizeof(out[n].name) - 2);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

// --- Frame builder helpers ------------------------------------------ //

#define EMIT_TEXT(buf, cap, off, s) \
    do { if (emit_text(buf, cap, off, s) != 0) return ESP_ERR_INVALID_SIZE; } while (0)
#define EMIT_MAP(buf, cap, off, n) \
    do { if (emit_map(buf, cap, off, n) != 0) return ESP_ERR_INVALID_SIZE; } while (0)
#define EMIT_ARRAY(buf, cap, off, n) \
    do { if (emit_array(buf, cap, off, n) != 0) return ESP_ERR_INVALID_SIZE; } while (0)
#define EMIT_U(buf, cap, off, major, v) \
    do { if (emit_u(buf, cap, off, major, v) != 0) return ESP_ERR_INVALID_SIZE; } while (0)

static esp_err_t emit_text_widget(uint8_t *buf, size_t cap, size_t *off,
                                  const char *s, const char *style)
{
    EMIT_MAP(buf, cap, off, style ? 3 : 2);
    EMIT_TEXT(buf, cap, off, "t");     EMIT_TEXT(buf, cap, off, "text");
    EMIT_TEXT(buf, cap, off, "s");     EMIT_TEXT(buf, cap, off, s);
    if (style) { EMIT_TEXT(buf, cap, off, "style"); EMIT_TEXT(buf, cap, off, style); }
    return ESP_OK;
}

static esp_err_t emit_input_widget(uint8_t *buf, size_t cap, size_t *off,
                                   const char *name, const char *label,
                                   const char *value)
{
    EMIT_MAP(buf, cap, off, 4);
    EMIT_TEXT(buf, cap, off, "t");      EMIT_TEXT(buf, cap, off, "input");
    EMIT_TEXT(buf, cap, off, "name");   EMIT_TEXT(buf, cap, off, name);
    EMIT_TEXT(buf, cap, off, "label");  EMIT_TEXT(buf, cap, off, label);
    EMIT_TEXT(buf, cap, off, "value");  EMIT_TEXT(buf, cap, off, value ? value : "");
    return ESP_OK;
}

static esp_err_t emit_button_widget(uint8_t *buf, size_t cap, size_t *off,
                                    const char *label, const char *href)
{
    EMIT_MAP(buf, cap, off, 3);
    EMIT_TEXT(buf, cap, off, "t");     EMIT_TEXT(buf, cap, off, "button");
    EMIT_TEXT(buf, cap, off, "label"); EMIT_TEXT(buf, cap, off, label);
    EMIT_TEXT(buf, cap, off, "href");  EMIT_TEXT(buf, cap, off, href);
    return ESP_OK;
}

static esp_err_t emit_notif_widget(uint8_t *buf, size_t cap, size_t *off,
                                   const char *s, const char *level)
{
    EMIT_MAP(buf, cap, off, level ? 3 : 2);
    EMIT_TEXT(buf, cap, off, "t");     EMIT_TEXT(buf, cap, off, "notification");
    EMIT_TEXT(buf, cap, off, "s");     EMIT_TEXT(buf, cap, off, s);
    if (level) { EMIT_TEXT(buf, cap, off, "level"); EMIT_TEXT(buf, cap, off, level); }
    return ESP_OK;
}

static esp_err_t emit_simple_list(uint8_t *buf, size_t cap, size_t *off,
                                  const char *const *labels,
                                  const char *const *hrefs,
                                  size_t n)
{
    EMIT_MAP(buf, cap, off, 2);
    EMIT_TEXT(buf, cap, off, "t");      EMIT_TEXT(buf, cap, off, "list");
    EMIT_TEXT(buf, cap, off, "items");  EMIT_ARRAY(buf, cap, off, n);
    for (size_t i = 0; i < n; i++) {
        EMIT_MAP(buf, cap, off, 2);
        EMIT_TEXT(buf, cap, off, "label"); EMIT_TEXT(buf, cap, off, labels[i]);
        EMIT_TEXT(buf, cap, off, "href");  EMIT_TEXT(buf, cap, off, hrefs[i]);
    }
    return ESP_OK;
}

static esp_err_t emit_frame_envelope(uint8_t *buf, size_t cap, size_t *off,
                                     const char *screen_id, const char *title,
                                     size_t body_widget_count)
{
    EMIT_MAP(buf, cap, off, 4);
    EMIT_TEXT(buf, cap, off, "v");      EMIT_U(buf, cap, off, 0, 1);
    EMIT_TEXT(buf, cap, off, "id");     EMIT_TEXT(buf, cap, off, screen_id);
    EMIT_TEXT(buf, cap, off, "title");  EMIT_TEXT(buf, cap, off, title);
    EMIT_TEXT(buf, cap, off, "body");   EMIT_ARRAY(buf, cap, off, body_widget_count);
    return ESP_OK;
}

// --- Frame builders ------------------------------------------------- //

esp_err_t pageros_shell_emit_home(uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    app_listing_t apps[MAX_APPS];
    size_t n_apps = list_installed(apps, MAX_APPS);

    // Body widgets: heading + one list (Settings + installed apps).
    size_t off = 0;
    esp_err_t r = emit_frame_envelope(buf, cap, &off, "scr_home", "PagerOS", 2);
    if (r != ESP_OK) return r;
    if ((r = emit_text_widget(buf, cap, &off, "Home", "heading")) != ESP_OK) return r;

    // List: Settings (always first) + each installed app + a hint at
    // the bottom when no apps exist.
    size_t n_items = 1 + n_apps + (n_apps == 0 ? 1 : 0);
    EMIT_MAP(buf, cap, &off, 2);
    EMIT_TEXT(buf, cap, &off, "t");     EMIT_TEXT(buf, cap, &off, "list");
    EMIT_TEXT(buf, cap, &off, "items"); EMIT_ARRAY(buf, cap, &off, n_items);

    // Settings entry.
    EMIT_MAP(buf, cap, &off, 2);
    EMIT_TEXT(buf, cap, &off, "label"); EMIT_TEXT(buf, cap, &off, "⚙ Settings");
    EMIT_TEXT(buf, cap, &off, "href");  EMIT_TEXT(buf, cap, &off, "shell:settings");

    // Installed apps.
    for (size_t i = 0; i < n_apps; i++) {
        EMIT_MAP(buf, cap, &off, 2);
        EMIT_TEXT(buf, cap, &off, "label"); EMIT_TEXT(buf, cap, &off, apps[i].name);
        EMIT_TEXT(buf, cap, &off, "href");
        char href[80]; snprintf(href, sizeof(href), "open:%s", apps[i].id);
        EMIT_TEXT(buf, cap, &off, href);
    }

    // "No apps" hint, rendered as a list item too so the layout stays
    // a single list. Its href is harmless ("noop").
    if (n_apps == 0) {
        EMIT_MAP(buf, cap, &off, 2);
        EMIT_TEXT(buf, cap, &off, "label");
        EMIT_TEXT(buf, cap, &off, "(no apps yet — add one in Settings)");
        EMIT_TEXT(buf, cap, &off, "href");
        EMIT_TEXT(buf, cap, &off, "shell:settings");
    }

    *out_len = off;
    return ESP_OK;
}

esp_err_t pageros_shell_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

esp_err_t pageros_shell_mount_home(void)
{
    uint8_t buf[512];
    size_t n = 0;
    esp_err_t r = pageros_shell_emit_home(buf, sizeof(buf), &n);
    if (r != ESP_OK) return r;
    return pageros_apprt_open(PAGEROS_SHELL_APP_ID, buf, n);
}

// --- settings menu ------------------------------------------------- //

esp_err_t pageros_shell_emit_settings(uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    size_t off = 0;
    const char *labels[] = { "Wi-Fi", "Add app by URL", "About", "Back" };
    const char *hrefs[]  = { "shell:wifi", "shell:add-app", "shell:about", "shell:home" };
    esp_err_t r;
    if ((r = emit_frame_envelope(buf, cap, &off, "scr_settings", "Settings", 2)) != ESP_OK) return r;
    if ((r = emit_text_widget(buf, cap, &off, "Settings", "heading")) != ESP_OK) return r;
    if ((r = emit_simple_list(buf, cap, &off, labels, hrefs, 4)) != ESP_OK) return r;
    *out_len = off;
    return ESP_OK;
}

// --- Wi-Fi config -------------------------------------------------- //

esp_err_t pageros_shell_emit_wifi_config(uint8_t *buf, size_t cap, size_t *out_len,
                                         const char *ssid, const char *psk_masked,
                                         const char *status)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    size_t off = 0;
    esp_err_t r;
    char status_line[80];
    snprintf(status_line, sizeof(status_line), "Status: %s",
             status && *status ? status : "Disconnected");
    if ((r = emit_frame_envelope(buf, cap, &off, "scr_wifi", "Wi-Fi", 6)) != ESP_OK) return r;
    if ((r = emit_text_widget(buf, cap, &off, "Wi-Fi", "heading")) != ESP_OK) return r;
    if ((r = emit_text_widget(buf, cap, &off, status_line, "dim")) != ESP_OK) return r;
    if ((r = emit_input_widget(buf, cap, &off, "ssid",     "SSID",     ssid ? ssid : "")) != ESP_OK) return r;
    if ((r = emit_input_widget(buf, cap, &off, "password", "Password", psk_masked ? psk_masked : "")) != ESP_OK) return r;
    if ((r = emit_button_widget(buf, cap, &off, "Connect", "shell:wifi-connect")) != ESP_OK) return r;
    if ((r = emit_button_widget(buf, cap, &off, "Back",    "shell:settings")) != ESP_OK) return r;
    *out_len = off;
    return ESP_OK;
}

// --- add app by URL ----------------------------------------------- //

esp_err_t pageros_shell_emit_add_app(uint8_t *buf, size_t cap, size_t *out_len,
                                     const char *url, const char *message,
                                     const char *message_level)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    size_t off = 0;
    esp_err_t r;
    size_t widget_count = 4 + (message ? 1 : 0);  // heading, [notif?], hint, input, install, back
    widget_count = 5 + (message ? 1 : 0);
    if ((r = emit_frame_envelope(buf, cap, &off, "scr_add_app", "Add app", widget_count)) != ESP_OK) return r;
    if ((r = emit_text_widget(buf, cap, &off, "Add app by URL", "heading")) != ESP_OK) return r;
    if (message) {
        if ((r = emit_notif_widget(buf, cap, &off, message, message_level)) != ESP_OK) return r;
    }
    if ((r = emit_text_widget(buf, cap, &off, "Enter the app's base URL.", "dim")) != ESP_OK) return r;
    if ((r = emit_input_widget(buf, cap, &off, "url", "URL", url ? url : "")) != ESP_OK) return r;
    if ((r = emit_button_widget(buf, cap, &off, "Install", "shell:install")) != ESP_OK) return r;
    if ((r = emit_button_widget(buf, cap, &off, "Back",    "shell:settings")) != ESP_OK) return r;
    *out_len = off;
    return ESP_OK;
}
