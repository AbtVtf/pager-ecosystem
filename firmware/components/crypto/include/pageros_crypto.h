// SPDX-License-Identifier: Apache-2.0
//
// PagerOS firmware crypto primitives — currently only the bits needed
// by FW-014 (Ed25519 keypair generation and public-key derivation).
//
// The underlying implementation is vendored Monocypher
// (BSD-2-Clause / CC0-1.0 dual-licensed, see `src/monocypher.{c,h}` and
// `src/monocypher-ed25519.{c,h}`). We expose only the RFC 8032 SHA-512
// Ed25519 variant — *not* Monocypher's default BLAKE2b EdDSA — because
// the PagerOS SDK and exit-nodes interoperate via libsodium, which only
// implements the SHA-512 variant (see SPEC §9.1 / SDK `signing.py`).
//
// FW-015 (crypto primitives wrapper) will expand this header with
// signing, verification, X25519 ECDH, ChaCha20-Poly1305, etc. — those
// primitives are already linked in via Monocypher and just need their
// thin wrappers here.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_ED25519_SEED_LEN    32
#define PAGEROS_ED25519_PUBKEY_LEN  32

// Derive the Ed25519 (RFC 8032 / SHA-512 variant) public key for a given
// 32-byte seed. Bit-for-bit compatible with libsodium's
// `crypto_sign_ed25519_seed_keypair`.
//
// The `seed` buffer is consumed and **zeroed in place** before return —
// callers that need to keep it must copy it first.
void pageros_crypto_ed25519_pubkey(uint8_t       pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   uint8_t       seed  [PAGEROS_ED25519_SEED_LEN]);

#ifdef __cplusplus
}
#endif
