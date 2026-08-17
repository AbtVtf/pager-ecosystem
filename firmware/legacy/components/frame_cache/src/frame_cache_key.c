// SPDX-License-Identifier: Apache-2.0
//
// FW-017 — key derivation. Split out so host tests can exercise it
// without dragging in mbedTLS or the rest of the cache state.

#include "pageros_frame_cache.h"

#include <string.h>

#include "mbedtls/sha256.h"

static char hexchar(unsigned v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

int pageros_frame_cache_key_to_hex(const char *app_id,
                                   const char *screen_id,
                                   char out_hex[65])
{
    if (!app_id || !screen_id || !out_hex) return -1;
    size_t la = strlen(app_id);
    size_t ls = strlen(screen_id);
    if (la == 0 || ls == 0) return -1;
    if (la > PAGEROS_FC_MAX_APP_ID_LEN) return -1;
    if (ls > PAGEROS_FC_MAX_SCREEN_ID_LEN) return -1;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return -1; }
    const uint8_t sep = 0x1F;  // ASCII unit separator — cannot appear in URLs
    if (mbedtls_sha256_update(&ctx, (const unsigned char *)app_id, la) != 0) goto err;
    if (mbedtls_sha256_update(&ctx, &sep, 1) != 0) goto err;
    if (mbedtls_sha256_update(&ctx, (const unsigned char *)screen_id, ls) != 0) goto err;

    unsigned char digest[32];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) goto err;
    mbedtls_sha256_free(&ctx);

    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexchar((digest[i] >> 4) & 0xF);
        out_hex[i * 2 + 1] = hexchar(digest[i] & 0xF);
    }
    out_hex[64] = '\0';
    return 64;

err:
    mbedtls_sha256_free(&ctx);
    return -1;
}
