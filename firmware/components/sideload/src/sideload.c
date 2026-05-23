// SPDX-License-Identifier: Apache-2.0
// FW-034 — sideload by URL.

#include "pageros_sideload.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "pageros_cbor.h"
#include "pageros_network.h"
#include "pageros_storage.h"

static const char *TAG = "sideload";

// Extract a top-level text-string field by name from a manifest's CBOR map.
// Returns the field length on success, -1 on missing/non-text/over-capacity.
static int extract_text_field(const uint8_t *cbor, size_t len,
                              const char *key, char *out, size_t cap)
{
    if (cap < 2) return -1;
    size_t klen = strlen(key);
    uint8_t arena_buf[1024];
    pgr_cbor_arena_t arena; pgr_cbor_arena_init(&arena, arena_buf, sizeof(arena_buf));
    pgr_cbor_value_t *root = NULL;
    if (pgr_cbor_decode(cbor, len, &arena, &root) != PGR_CBOR_OK) return -1;
    if (!root || root->kind != PGR_CBOR_KIND_MAP) return -1;
    for (size_t i = 0; i < root->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &root->v.map.items[i];
        if (p->key.kind != PGR_CBOR_KIND_TEXT) continue;
        if (p->key.v.bytes.len != klen) continue;
        if (memcmp(p->key.v.bytes.data, key, klen) != 0) continue;
        if (p->val.kind != PGR_CBOR_KIND_TEXT) return -1;
        size_t l = p->val.v.bytes.len;
        if (l == 0 || l >= cap) return -1;
        memcpy(out, p->val.v.bytes.data, l);
        out[l] = '\0';
        return (int)l;
    }
    return -1;
}

static int extract_app_id(const uint8_t *cbor, size_t len,
                          char *out, size_t cap)
{
    return extract_text_field(cbor, len, "id", out, cap);
}

int pageros_sideload_extract_field(const uint8_t *cbor, size_t cbor_len,
                                   const char *key, char *out, size_t cap)
{
    if (!cbor || !key || !out) return -1;
    return extract_text_field(cbor, cbor_len, key, out, cap);
}

esp_err_t pageros_sideload_install_from_url(const char *url,
                                            char *out_app_id, size_t cap)
{
    if (!url || !out_app_id || cap < 4) return ESP_ERR_INVALID_ARG;

    // Build full manifest URL.
    char full_url[256];
    int n = snprintf(full_url, sizeof(full_url), "%s%s.pageros/manifest.cbor",
                     url, url[strlen(url) - 1] == '/' ? "" : "/");
    if (n < 0 || n >= (int)sizeof(full_url)) return ESP_ERR_INVALID_SIZE;

    uint8_t *buf = (uint8_t *)malloc(PAGEROS_SIDELOAD_MAX_MANIFEST);
    if (!buf) return ESP_ERR_NO_MEM;
    pageros_https_response_t r = {0};
    ESP_LOGI(TAG, "GET %s", full_url);
    esp_err_t e = pageros_https_get(full_url, buf, PAGEROS_SIDELOAD_MAX_MANIFEST, &r);
    ESP_LOGI(TAG, "fetch result: e=%s status=%d body=%u%s",
             esp_err_to_name(e), r.status_code, (unsigned)r.body_len,
             r.body_truncated ? " TRUNCATED" : "");
    if (e != ESP_OK || r.status_code / 100 != 2) {
        free(buf);
        return e != ESP_OK ? e : ESP_FAIL;
    }
    size_t mlen = r.body_len < PAGEROS_SIDELOAD_MAX_MANIFEST ? r.body_len : PAGEROS_SIDELOAD_MAX_MANIFEST;

    char app_id[PAGEROS_SIDELOAD_MAX_APP_ID];
    int id_len = extract_app_id(buf, mlen, app_id, sizeof(app_id));
    if (id_len < 0) {
        ESP_LOGW(TAG, "extract_app_id failed; first 16 bytes:");
        for (size_t i = 0; i < (mlen < 16 ? mlen : 16); i++) {
            ESP_LOGW(TAG, "  [%u]=0x%02x", (unsigned)i, buf[i]);
        }
        free(buf); return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "extracted app_id='%s'", app_id);

    // Extract the manifest's own `url` so we can save it as source_url
    // — for store installs the URL we were passed is the registry endpoint
    // (`<base>/apps/<id>`), not the app server. Done before freeing `buf`.
    char manifest_url[200];
    int url_len = extract_text_field(buf, mlen, "url",
                                     manifest_url, sizeof(manifest_url));

    char dir[256], path[300];
    snprintf(dir, sizeof(dir), "%s/%s", PAGEROS_DIR_APPS, app_id);
    esp_err_t mk = pageros_storage_mkdir_p(dir);
    ESP_LOGI(TAG, "mkdir %s: %s", dir, esp_err_to_name(mk));
    snprintf(path, sizeof(path), "%s/manifest.cbor", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s failed: %s", path, strerror(errno));
        free(buf); return ESP_FAIL;
    }
    size_t wrote = fwrite(buf, 1, mlen, f);
    fclose(f);
    ESP_LOGI(TAG, "wrote %u/%u bytes to %s", (unsigned)wrote, (unsigned)mlen, path);
    free(buf);

    snprintf(path, sizeof(path), "%s/.sideloaded", dir);
    f = fopen(path, "wb");
    if (f) { fclose(f); }

    // Persist whichever URL actually points at the app's HTTP server.
    // Sideload-by-URL hits the app directly, so the install URL is correct;
    // store install passes a registry endpoint and needs the manifest's url.
    const char *source_url = (url_len > 0) ? manifest_url : url;
    snprintf(path, sizeof(path), "%s/.source_url", dir);
    f = fopen(path, "wb");
    if (f) { fwrite(source_url, 1, strlen(source_url), f); fclose(f); }
    ESP_LOGI(TAG, "source_url=%s (%s)", source_url,
             url_len > 0 ? "from manifest" : "from install url");

    strncpy(out_app_id, app_id, cap - 1);
    out_app_id[cap - 1] = '\0';
    ESP_LOGI(TAG, "installed %s", app_id);
    return ESP_OK;
}
