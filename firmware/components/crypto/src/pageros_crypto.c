// SPDX-License-Identifier: Apache-2.0
//
// Thin PagerOS wrappers over Monocypher (vendored under `src/`).
// See `pageros_crypto.h` for the variant choices and rationale.

#include "pageros_crypto.h"

#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

// -- Ed25519 (RFC 8032 / SHA-512) -------------------------------------------

void pageros_crypto_ed25519_pubkey(uint8_t pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   uint8_t seed  [PAGEROS_ED25519_SEED_LEN])
{
    // Monocypher's `crypto_ed25519_key_pair` returns the 64-byte libsodium-
    // style "secret_key" (seed || pubkey). We only need the pubkey; the
    // caller still owns their copy of the seed.
    uint8_t sk[64];
    crypto_ed25519_key_pair(sk, pubkey, seed);
    crypto_wipe(sk, sizeof(sk));
}

void pageros_crypto_ed25519_sign(uint8_t        sig   [PAGEROS_ED25519_SIG_LEN],
                                 const uint8_t  seed  [PAGEROS_ED25519_SEED_LEN],
                                 const uint8_t  pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                 const uint8_t *msg, size_t msg_len)
{
    // libsodium / Monocypher format: secret_key = seed || pubkey.
    uint8_t sk[64];
    memcpy(sk,      seed,   32);
    memcpy(sk + 32, pubkey, 32);
    crypto_ed25519_sign(sig, sk, msg, msg_len);
    crypto_wipe(sk, sizeof(sk));
}

bool pageros_crypto_ed25519_verify(const uint8_t sig   [PAGEROS_ED25519_SIG_LEN],
                                   const uint8_t pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   const uint8_t *msg, size_t msg_len)
{
    return crypto_ed25519_check(sig, pubkey, msg, msg_len) == 0;
}

// -- X25519 (RFC 7748) ------------------------------------------------------

void pageros_crypto_x25519_pubkey(uint8_t       pubkey  [PAGEROS_X25519_PUBKEY_LEN],
                                  const uint8_t priv_key[PAGEROS_X25519_SECRET_LEN])
{
    crypto_x25519_public_key(pubkey, priv_key);
}

void pageros_crypto_x25519_ecdh(uint8_t       shared  [PAGEROS_X25519_SHARED_LEN],
                                const uint8_t my_priv [PAGEROS_X25519_SECRET_LEN],
                                const uint8_t peer_pub[PAGEROS_X25519_PUBKEY_LEN])
{
    crypto_x25519(shared, my_priv, peer_pub);
}

// -- ChaCha20-Poly1305 AEAD (RFC 8439 IETF) ---------------------------------

void pageros_crypto_aead_seal(uint8_t       *ct,
                              uint8_t        tag   [PAGEROS_AEAD_TAG_LEN],
                              const uint8_t  key   [PAGEROS_AEAD_KEY_LEN],
                              const uint8_t  nonce [PAGEROS_AEAD_NONCE_LEN],
                              const uint8_t *aad,  size_t aad_len,
                              const uint8_t *pt,   size_t pt_len)
{
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, ct, tag, aad, aad_len, pt, pt_len);
    crypto_wipe(&ctx, sizeof(ctx));
}

bool pageros_crypto_aead_open(uint8_t       *pt,
                              const uint8_t  tag   [PAGEROS_AEAD_TAG_LEN],
                              const uint8_t  key   [PAGEROS_AEAD_KEY_LEN],
                              const uint8_t  nonce [PAGEROS_AEAD_NONCE_LEN],
                              const uint8_t *aad,  size_t aad_len,
                              const uint8_t *ct,   size_t ct_len)
{
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    int rc = crypto_aead_read(&ctx, pt, tag, aad, aad_len, ct, ct_len);
    crypto_wipe(&ctx, sizeof(ctx));
    return rc == 0;
}
