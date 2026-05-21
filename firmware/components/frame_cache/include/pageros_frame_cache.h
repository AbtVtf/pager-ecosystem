// SPDX-License-Identifier: Apache-2.0
//
// PagerOS Frame cache — FW-017.
//
// Two-tier cache for CBOR-encoded Frames (SPEC §5.2, §5.5):
//
//   L1 = PSRAM, bounded by entry count + total bytes, LRU evicted.
//        Hit cost: a hash lookup + a memcpy of the cached payload.
//        Target: <1 ms per spec §14 performance budget.
//
//   L2 = SD card under /cache/frames/<app_id>/<screen_id>.cbor, capped
//        only by available SD bytes. Persists across reboots so the
//        instant-open UX (§5.5) survives a deep-sleep wake.
//        Target: <50 ms per spec.
//
// Lookups walk L1 first; on a hit the entry is moved to the front of
// the LRU list. On a miss we read L2 and (if found + non-expired)
// repopulate L1 so subsequent hits stay in the fast tier.
//
// TTL handling is per SPEC §5.5: a stored Frame may be served while
// `now() - stored_at < ttl_s`. `ttl_s == 0` disables caching for that
// entry (the API rejects the Put with PAGEROS_FC_TTL_ZERO so callers
// see the rejection rather than silently skipping a write).
//
// Keys are case-sensitive UTF-8 strings; the cache file path uses a
// hex-encoded SHA-256 of the (app_id, screen_id) pair so arbitrary
// punctuation in screen ids ("scr/index?v=1") never collides with the
// SD filesystem.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Cache-layer error codes layered on top of esp_err_t. Negative range
// to avoid colliding with the ESP-IDF ESP_ERR_* space.
#define PAGEROS_FC_MISS         (ESP_ERR_NOT_FOUND)
#define PAGEROS_FC_TTL_ZERO     (ESP_ERR_INVALID_ARG)
#define PAGEROS_FC_TOO_LARGE    (ESP_ERR_INVALID_SIZE)

#define PAGEROS_FC_MAX_APP_ID_LEN     128
#define PAGEROS_FC_MAX_SCREEN_ID_LEN  128

// Sensible defaults; callers can override via pageros_frame_cache_init_opts.
#define PAGEROS_FC_DEFAULT_L1_MAX_ENTRIES   64
#define PAGEROS_FC_DEFAULT_L1_MAX_BYTES     (256 * 1024)  // 256 KiB PSRAM tier
#define PAGEROS_FC_DEFAULT_PER_ENTRY_BYTES  (4 * 1024)    // refuse oversized Frames

typedef struct {
    size_t l1_max_entries;
    size_t l1_max_bytes;
    size_t per_entry_max_bytes;

    // Wallclock for TTL math. Pass NULL to use `time(NULL)` (the
    // ESP-IDF default); tests inject a controlled clock.
    uint64_t (*now_unix)(void);

    // SD card mount root. Pass NULL to use the storage component's
    // PAGEROS_DIR_CACHE_FRAMES.
    const char *sd_root;

    // When true, skip the L2 (SD) tier entirely. Useful for tests and
    // for hosts that haven't mounted an SD card yet.
    bool l1_only;
} pageros_frame_cache_opts_t;

esp_err_t pageros_frame_cache_init(const pageros_frame_cache_opts_t *opts);
esp_err_t pageros_frame_cache_shutdown(void);

// Store a Frame. Copies the bytes into the L1 arena and (unless
// `l1_only`) writes a sidecar file in L2. `ttl_s` MUST be > 0.
esp_err_t pageros_frame_cache_put(const char *app_id,
                                  const char *screen_id,
                                  const uint8_t *frame_bytes,
                                  size_t frame_len,
                                  uint32_t ttl_s);

// Fetch a Frame.
//
//   *out_buf is set to a caller-owned heap buffer the caller must free
//   with `free()`. `*out_len` is the buffer length. The double-pointer
//   form lets the caller forward ownership directly to a renderer/codec
//   without an extra copy.
//
//   Returns PAGEROS_FC_MISS if the key is absent or expired in both
//   tiers. On error the out-params are not modified.
esp_err_t pageros_frame_cache_get(const char *app_id,
                                  const char *screen_id,
                                  uint8_t **out_buf,
                                  size_t *out_len);

// Drop an entry (both tiers).
esp_err_t pageros_frame_cache_invalidate(const char *app_id,
                                         const char *screen_id);

// Wipe the cache. Both tiers.
esp_err_t pageros_frame_cache_clear(void);

typedef struct {
    size_t l1_entries;
    size_t l1_bytes;
    uint64_t hits_l1;
    uint64_t hits_l2;
    uint64_t misses;
    uint64_t evictions_lru;
    uint64_t evictions_expired;
} pageros_frame_cache_stats_t;

void pageros_frame_cache_get_stats(pageros_frame_cache_stats_t *out);

// --- testable surfaces (pure functions, host-tested) --------------- //

// Hex SHA-256 of "<app_id>\x1f<screen_id>" into out_hex (65 bytes
// including NUL). The 0x1f separator means app/screen splits can't be
// spoofed by an app_id ending in "/" + a screen_id starting with the
// same. Used as the L2 filename.
//
// Returns 64 on success (no NUL), or -1 if app_id or screen_id is
// NULL/empty/over-length.
int pageros_frame_cache_key_to_hex(const char *app_id,
                                   const char *screen_id,
                                   char out_hex[65]);

#ifdef __cplusplus
}
#endif
