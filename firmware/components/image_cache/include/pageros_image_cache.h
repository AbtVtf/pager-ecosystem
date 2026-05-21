// SPDX-License-Identifier: Apache-2.0
//
// PagerOS Image cache + PNG decoder — FW-018.
//
// Content-addressed image store:
//
//   - Images are referenced from Frames by `src: "img:<sha256-prefix>"`
//     (SPEC §5.3.6). The full sha256 of the image bytes is stable across
//     servers, so caching by hash means the device never re-fetches the
//     same icon twice — even if two apps reference the same image under
//     different URLs.
//
//   - Cache lives under /cache/images/<sha256>.png on the SD card.
//     There's no in-memory tier: PNG decoding is the expensive bit, and
//     a once-decoded RGB565 framebuffer is held by the renderer (FW-021)
//     for the lifetime of the screen.
//
//   - PNG decoding via vendored LodePNG (ZLib license). Output is
//     RGBA-8888; callers reshape to RGB565 via the display's helpers.
//     SPEC §5.6 caps images at 480×222 pixels and 8 KiB encoded; we
//     reject anything outside those bounds.
//
//   - HTTPS fetch is the caller's job — the cache only takes bytes,
//     hashes them, and writes them out. The FW-018 + FW-009 wiring is
//     done by FW-021 (the image widget renderer).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_IMG_SHA256_HEX_LEN     64
#define PAGEROS_IMG_MAX_ENCODED_BYTES  (8 * 1024)   // SPEC §5.6
#define PAGEROS_IMG_MAX_WIDTH          480
#define PAGEROS_IMG_MAX_HEIGHT         222

typedef struct {
    const char *sd_root;     // default: /cache/images on the SD mount
} pageros_image_cache_opts_t;

esp_err_t pageros_image_cache_init(const pageros_image_cache_opts_t *opts);
esp_err_t pageros_image_cache_shutdown(void);

// Store raw PNG bytes for the given sha256 hex. The hex MUST be 64
// lowercase chars; the function recomputes the hash and rejects with
// ESP_ERR_INVALID_CRC if the bytes don't match the claimed hex. Idempotent.
esp_err_t pageros_image_cache_put(const char *sha256_hex,
                                  const uint8_t *png_bytes,
                                  size_t png_len);

// True if the cache already has this image. Cheap stat-only check.
bool pageros_image_cache_has(const char *sha256_hex);

// Decode an image (loads from cache, decodes PNG to RGBA-8888).
//
//   `*out_rgba` is a caller-owned heap buffer the caller must free.
//   `*out_w` / `*out_h` are populated on success.
//
// Returns ESP_ERR_NOT_FOUND if the image isn't in the cache.
esp_err_t pageros_image_cache_decode(const char *sha256_hex,
                                     uint8_t **out_rgba,
                                     int *out_w, int *out_h);

esp_err_t pageros_image_cache_invalidate(const char *sha256_hex);

typedef struct {
    uint64_t puts;
    uint64_t hits;
    uint64_t misses;
    uint64_t hash_mismatches;
    uint64_t decode_failures;
} pageros_image_cache_stats_t;

void pageros_image_cache_get_stats(pageros_image_cache_stats_t *out);

// --- testable helpers --------------------------------------------- //

// Validate `hex` is exactly 64 lowercase hex chars + NUL.
bool pageros_image_cache_valid_hex(const char *hex);

#ifdef __cplusplus
}
#endif
