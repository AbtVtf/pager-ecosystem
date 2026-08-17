// SPDX-License-Identifier: Apache-2.0
//
// FW-018 — image cache impl. See pageros_image_cache.h.

#include "pageros_image_cache.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "pageros_storage.h"

#include "lodepng.h"

static const char *TAG = "image_cache";

// Forward decl — definition at bottom of file.
static int sha256_hex_impl_check(const uint8_t *buf, size_t len,
                                 char out[65], const char *expected);

static struct {
    bool inited;
    char sd_root[128];
    pageros_image_cache_stats_t stats;
} g;

bool pageros_image_cache_valid_hex(const char *hex)
{
    if (!hex) return false;
    for (int i = 0; i < PAGEROS_IMG_SHA256_HEX_LEN; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return hex[PAGEROS_IMG_SHA256_HEX_LEN] == '\0';
}

static int sha256_hex(const uint8_t *buf, size_t len, char out[65])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return -1; }
    if (mbedtls_sha256_update(&ctx, buf, len) != 0) { mbedtls_sha256_free(&ctx); return -1; }
    unsigned char digest[32];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) { mbedtls_sha256_free(&ctx); return -1; }
    mbedtls_sha256_free(&ctx);
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hexchars[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hexchars[digest[i] & 0xF];
    }
    out[64] = '\0';
    return 0;
}

static int build_path(const char *hex, char out[256])
{
    return snprintf(out, 256, "%s/%s.png", g.sd_root, hex);
}

esp_err_t pageros_image_cache_init(const pageros_image_cache_opts_t *opts)
{
    if (g.inited) return ESP_OK;
    const char *root = (opts && opts->sd_root) ? opts->sd_root : PAGEROS_DIR_CACHE_IMAGES;
    strncpy(g.sd_root, root, sizeof(g.sd_root) - 1);
    g.sd_root[sizeof(g.sd_root) - 1] = '\0';
    (void)pageros_storage_mkdir_p(g.sd_root);
    memset(&g.stats, 0, sizeof(g.stats));
    g.inited = true;
    ESP_LOGI(TAG, "init: dir=%s", g.sd_root);
    return ESP_OK;
}

esp_err_t pageros_image_cache_shutdown(void)
{
    g.inited = false;
    return ESP_OK;
}

bool pageros_image_cache_has(const char *sha256_hex)
{
    if (!g.inited || !pageros_image_cache_valid_hex(sha256_hex)) return false;
    char path[256]; build_path(sha256_hex, path);
    struct stat st;
    return (stat(path, &st) == 0 && st.st_size > 0);
}

esp_err_t pageros_image_cache_put(const char *sha256_hex,
                                  const uint8_t *png_bytes,
                                  size_t png_len)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!sha256_hex || !png_bytes) return ESP_ERR_INVALID_ARG;
    if (!pageros_image_cache_valid_hex(sha256_hex)) return ESP_ERR_INVALID_ARG;
    if (png_len == 0 || png_len > PAGEROS_IMG_MAX_ENCODED_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Verify the bytes hash to the claimed hex — caller controls the
    // hex but the trusted root is the content itself.
    char computed[65];
    if (sha256_hex_impl_check(png_bytes, png_len, computed, sha256_hex) != 0) {
        g.stats.hash_mismatches++;
        return ESP_ERR_INVALID_CRC;
    }

    char path[256]; build_path(sha256_hex, path);
    if (pageros_image_cache_has(sha256_hex)) {
        return ESP_OK;  // idempotent
    }
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    if (fwrite(png_bytes, 1, png_len, f) != png_len) {
        fclose(f); unlink(path); return ESP_FAIL;
    }
    fclose(f);
    g.stats.puts++;
    return ESP_OK;
}

esp_err_t pageros_image_cache_decode(const char *sha256_hex,
                                     uint8_t **out_rgba,
                                     int *out_w, int *out_h)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!sha256_hex || !out_rgba || !out_w || !out_h) return ESP_ERR_INVALID_ARG;
    if (!pageros_image_cache_valid_hex(sha256_hex)) return ESP_ERR_INVALID_ARG;

    char path[256]; build_path(sha256_hex, path);
    FILE *f = fopen(path, "rb");
    if (!f) { g.stats.misses++; return ESP_ERR_NOT_FOUND; }
    g.stats.hits++;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > PAGEROS_IMG_MAX_ENCODED_BYTES) {
        fclose(f); return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *png = (uint8_t *)malloc((size_t)sz);
    if (!png) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(png, 1, (size_t)sz, f) != (size_t)sz) {
        free(png); fclose(f); return ESP_FAIL;
    }
    fclose(f);

    unsigned w = 0, h = 0;
    unsigned char *rgba = NULL;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png, (size_t)sz);
    free(png);
    if (err != 0) {
        if (rgba) free(rgba);
        g.stats.decode_failures++;
        ESP_LOGW(TAG, "decode %s: lodepng err=%u", sha256_hex, err);
        return ESP_FAIL;
    }
    if (w == 0 || h == 0 || w > PAGEROS_IMG_MAX_WIDTH || h > PAGEROS_IMG_MAX_HEIGHT) {
        free(rgba);
        g.stats.decode_failures++;
        return ESP_ERR_INVALID_SIZE;
    }
    *out_rgba = rgba;
    *out_w = (int)w;
    *out_h = (int)h;
    return ESP_OK;
}

esp_err_t pageros_image_cache_invalidate(const char *sha256_hex)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!pageros_image_cache_valid_hex(sha256_hex)) return ESP_ERR_INVALID_ARG;
    char path[256]; build_path(sha256_hex, path);
    unlink(path);
    return ESP_OK;
}

void pageros_image_cache_get_stats(pageros_image_cache_stats_t *out)
{
    if (out) *out = g.stats;
}

// Inline check helper — computes hash and constant-time compares.
// Defined here (not via the public hex helper above) so we can keep
// the impl local without an extra header.
static int sha256_hex_impl_check(const uint8_t *buf, size_t len,
                                 char out[65], const char *expected)
{
    if (sha256_hex(buf, len, out) != 0) return -1;
    int d = 0;
    for (int i = 0; i < 64; i++) d |= (out[i] ^ expected[i]);
    return d == 0 ? 0 : -1;
}
