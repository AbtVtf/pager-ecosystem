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

// --- Frame builders ------------------------------------------------- //

esp_err_t pageros_shell_emit_home(uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    app_listing_t apps[MAX_APPS];
    size_t n_apps = list_installed(apps, MAX_APPS);

    // Frame top-level map: {v, id, title, body}
    // body: array of widgets — heading + (list-of-apps OR "no apps" text)
    size_t off = 0;
    if (emit_map(buf, cap, &off, 4) != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "v") != 0)     return ESP_ERR_INVALID_SIZE;
    if (emit_u(buf, cap, &off, 0, 1) != 0)        return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "id") != 0)    return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "scr_home") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "title") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "PagerOS") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "body") != 0)  return ESP_ERR_INVALID_SIZE;

    size_t body_items = 1 + (n_apps > 0 ? 1 : 1);  // heading + (list or text)
    if (emit_array(buf, cap, &off, body_items) != 0) return ESP_ERR_INVALID_SIZE;

    // Widget 1: {t: text, s: "Home", style: heading}
    if (emit_map(buf, cap, &off, 3) != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "t") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "text") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "s") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "Home") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "style") != 0) return ESP_ERR_INVALID_SIZE;
    if (emit_text(buf, cap, &off, "heading") != 0) return ESP_ERR_INVALID_SIZE;

    // Widget 2: list of apps, or "no apps installed" text
    if (n_apps > 0) {
        if (emit_map(buf, cap, &off, 2) != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "t") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "list") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "items") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_array(buf, cap, &off, n_apps) != 0) return ESP_ERR_INVALID_SIZE;
        for (size_t i = 0; i < n_apps; i++) {
            // {label, href}
            if (emit_map(buf, cap, &off, 2) != 0) return ESP_ERR_INVALID_SIZE;
            if (emit_text(buf, cap, &off, "label") != 0) return ESP_ERR_INVALID_SIZE;
            if (emit_text(buf, cap, &off, apps[i].name) != 0) return ESP_ERR_INVALID_SIZE;
            if (emit_text(buf, cap, &off, "href") != 0) return ESP_ERR_INVALID_SIZE;
            // href: shell-internal "open:<id>" — input router interprets
            char href[80]; snprintf(href, sizeof(href), "open:%s", apps[i].id);
            if (emit_text(buf, cap, &off, href) != 0) return ESP_ERR_INVALID_SIZE;
        }
    } else {
        if (emit_map(buf, cap, &off, 3) != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "t") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "text") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "s") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "No apps installed. Open Settings to add one.") != 0)
            return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "style") != 0) return ESP_ERR_INVALID_SIZE;
        if (emit_text(buf, cap, &off, "dim") != 0) return ESP_ERR_INVALID_SIZE;
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
