// SPDX-License-Identifier: Apache-2.0
//
// Thin PagerOS wrappers over Monocypher's RFC 8032 Ed25519 implementation.

#include "pageros_crypto.h"

#include <string.h>

#include "monocypher-ed25519.h"

void pageros_crypto_ed25519_pubkey(uint8_t pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   uint8_t seed  [PAGEROS_ED25519_SEED_LEN])
{
    // Monocypher's `crypto_ed25519_key_pair` returns the 64-byte libsodium-
    // style "secret key" (seed || pubkey). We only need the pubkey portion
    // here; the caller already owns the seed.
    uint8_t sk[64];
    crypto_ed25519_key_pair(sk, pubkey, seed);
    memset(sk, 0, sizeof(sk));
}
