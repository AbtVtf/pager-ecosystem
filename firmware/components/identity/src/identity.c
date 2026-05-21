// SPDX-License-Identifier: Apache-2.0
//
// PagerOS device identity — see `pageros_identity.h`.

#include "pageros_identity.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pageros_crypto.h"

static const char *TAG = "identity";

#define NVS_NAMESPACE  "pageros_id"
#define NVS_KEY_SEED   "seed"
#define NVS_KEY_PUB    "pubkey"

static struct {
    bool     ready;
    uint8_t  seed  [PAGEROS_IDENTITY_SEED_LEN];
    uint8_t  pubkey[PAGEROS_IDENTITY_PUBKEY_LEN];
} s_state;

// RFC 4648 base32 alphabet (no padding, uppercase).
static const char BASE32[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static esp_err_t load_or_create(uint8_t seed[PAGEROS_IDENTITY_SEED_LEN],
                                uint8_t pubkey[PAGEROS_IDENTITY_PUBKEY_LEN])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    size_t seed_len = PAGEROS_IDENTITY_SEED_LEN;
    err = nvs_get_blob(h, NVS_KEY_SEED, seed, &seed_len);

    if (err == ESP_OK && seed_len == PAGEROS_IDENTITY_SEED_LEN) {
        // Existing identity — derive the public key from the stored
        // seed. We re-derive every boot rather than trust a stale cached
        // pubkey in NVS, but we also persist the pubkey alongside so a
        // future read-only audit doesn't need to re-derive.
        uint8_t seed_copy[PAGEROS_IDENTITY_SEED_LEN];
        memcpy(seed_copy, seed, sizeof(seed_copy));
        pageros_crypto_ed25519_pubkey(pubkey, seed_copy);
        // pageros_crypto_ed25519_pubkey already zeroed seed_copy.

        ESP_LOGI(TAG, "identity loaded from NVS");
        nvs_close(h);
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    // First boot (or seed corrupted / wrong size). Generate fresh.
    ESP_LOGI(TAG, "no identity in NVS, generating fresh keypair");
    esp_fill_random(seed, PAGEROS_IDENTITY_SEED_LEN);

    uint8_t seed_copy[PAGEROS_IDENTITY_SEED_LEN];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    pageros_crypto_ed25519_pubkey(pubkey, seed_copy);

    err = nvs_set_blob(h, NVS_KEY_SEED, seed, PAGEROS_IDENTITY_SEED_LEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob seed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_PUB, pubkey, PAGEROS_IDENTITY_PUBKEY_LEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob pub: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t pageros_identity_init(void)
{
    if (s_state.ready) return ESP_OK;

    esp_err_t err = load_or_create(s_state.seed, s_state.pubkey);
    if (err != ESP_OK) {
        // Don't leave half-initialised state behind if either step
        // failed — a later retry should restart from scratch.
        memset(s_state.seed,   0, sizeof(s_state.seed));
        memset(s_state.pubkey, 0, sizeof(s_state.pubkey));
        return err;
    }

    s_state.ready = true;

    char fp[PAGEROS_IDENTITY_FP_LEN];
    pageros_identity_pubkey_fingerprint(fp, s_state.pubkey);
    ESP_LOGI(TAG, "identity ready (fingerprint: %s)", fp);
    return ESP_OK;
}

esp_err_t pageros_identity_pubkey(uint8_t out[PAGEROS_IDENTITY_PUBKEY_LEN])
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    memcpy(out, s_state.pubkey, PAGEROS_IDENTITY_PUBKEY_LEN);
    return ESP_OK;
}

esp_err_t pageros_identity_seed(uint8_t out[PAGEROS_IDENTITY_SEED_LEN])
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    memcpy(out, s_state.seed, PAGEROS_IDENTITY_SEED_LEN);
    return ESP_OK;
}

void pageros_identity_pubkey_fingerprint(
        char out[PAGEROS_IDENTITY_FP_LEN],
        const uint8_t pubkey[PAGEROS_IDENTITY_PUBKEY_LEN])
{
    // Take the first 60 bits of the pubkey and base32-encode into 12
    // characters (12 * 5 = 60). RFC 4648 uppercase alphabet, no padding.
    // The first 60 bits fit in the first 8 bytes (7.5 bytes really); we
    // walk the bits MSB-first.
    uint64_t hi = 0;
    for (int i = 0; i < 8; i++) {
        hi = (hi << 8) | pubkey[i];
    }
    // hi now holds the first 64 bits; we want the top 60 of those (the
    // next 4 LSBs of byte 7 are discarded — they're not displayed).
    hi >>= 4;
    for (int i = 11; i >= 0; i--) {
        out[i] = BASE32[hi & 0x1f];
        hi >>= 5;
    }
    out[12] = '\0';
}

esp_err_t pageros_identity_fingerprint(char out[PAGEROS_IDENTITY_FP_LEN])
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    pageros_identity_pubkey_fingerprint(out, s_state.pubkey);
    return ESP_OK;
}
