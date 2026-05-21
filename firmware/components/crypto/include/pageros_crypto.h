// SPDX-License-Identifier: Apache-2.0
//
// PagerOS firmware crypto primitives wrappers — FW-014 + FW-015.
//
// Thin, opinionated API over Monocypher (vendored under `src/`,
// BSD-2-Clause / CC0-1.0 dual-licensed). The variant choices match the
// PagerOS SDK (which targets libsodium) so signatures and ciphertexts
// produced on the device round-trip with the off-device SDK code:
//
//   * Ed25519: RFC 8032 SHA-512 variant (libsodium-compatible — *not*
//     Monocypher's default BLAKE2b EdDSA).
//   * X25519: RFC 7748 ECDH on Curve25519.
//   * AEAD: RFC 8439 ChaCha20-Poly1305 with the IETF 12-byte nonce.
//
// All operations are validated against the RFC test vectors committed
// in `docs/spec/crypto-test-vectors.json` by the host tests under
// `firmware/host-tests/identity/` and `firmware/host-tests/crypto/`.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Sizes ------------------------------------------------------------------

#define PAGEROS_ED25519_SEED_LEN     32
#define PAGEROS_ED25519_PUBKEY_LEN   32
#define PAGEROS_ED25519_SIG_LEN      64

#define PAGEROS_X25519_SECRET_LEN    32
#define PAGEROS_X25519_PUBKEY_LEN    32
#define PAGEROS_X25519_SHARED_LEN    32

#define PAGEROS_AEAD_KEY_LEN         32
#define PAGEROS_AEAD_NONCE_LEN       12   // RFC 8439 IETF nonce
#define PAGEROS_AEAD_TAG_LEN         16

// -- Ed25519 (RFC 8032 / SHA-512 variant, libsodium-compatible) -------------

// Derive the Ed25519 public key for a given 32-byte seed. The `seed`
// buffer is **zeroed in place** before return; copy it first if you
// need to keep it.
void pageros_crypto_ed25519_pubkey(uint8_t pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   uint8_t seed  [PAGEROS_ED25519_SEED_LEN]);

// Produce a detached signature over `msg`. The caller passes seed +
// pubkey separately (the pubkey is typically already cached by the
// identity component); we assemble the libsodium-style 64-byte
// "secret_key" internally and wipe it before return.
void pageros_crypto_ed25519_sign(uint8_t        sig    [PAGEROS_ED25519_SIG_LEN],
                                 const uint8_t  seed   [PAGEROS_ED25519_SEED_LEN],
                                 const uint8_t  pubkey [PAGEROS_ED25519_PUBKEY_LEN],
                                 const uint8_t *msg, size_t msg_len);

// Verify a detached signature. Returns true on success, false on
// any signature/pubkey mismatch. Constant-time.
bool pageros_crypto_ed25519_verify(const uint8_t sig   [PAGEROS_ED25519_SIG_LEN],
                                   const uint8_t pubkey[PAGEROS_ED25519_PUBKEY_LEN],
                                   const uint8_t *msg, size_t msg_len);

// -- X25519 (RFC 7748 ECDH on Curve25519) -----------------------------------

// Derive the X25519 public key for a given 32-byte private scalar.
// Monocypher clamps the private scalar internally per RFC 7748.
void pageros_crypto_x25519_pubkey(uint8_t       pubkey  [PAGEROS_X25519_PUBKEY_LEN],
                                  const uint8_t priv_key[PAGEROS_X25519_SECRET_LEN]);

// Compute the raw X25519 shared secret. **The output is not a key —
// callers must hash it through an HKDF (or equivalent) before using it
// to key a cipher.** SPEC §6.4 / §9.3 define HKDF-SHA-256 with
// `salt=zero`, `info="pageros/v1/e2e"` as the canonical KDF.
void pageros_crypto_x25519_ecdh(uint8_t       shared  [PAGEROS_X25519_SHARED_LEN],
                                const uint8_t my_priv [PAGEROS_X25519_SECRET_LEN],
                                const uint8_t peer_pub[PAGEROS_X25519_PUBKEY_LEN]);

// -- ChaCha20-Poly1305 AEAD (RFC 8439, IETF 12-byte nonce) ------------------

// Encrypt + authenticate `pt_len` bytes of plaintext into `ct`
// (same length) plus a 16-byte tag. AAD may be NULL/0. The caller is
// responsible for nonce uniqueness per key.
void pageros_crypto_aead_seal(uint8_t       *ct,
                              uint8_t        tag   [PAGEROS_AEAD_TAG_LEN],
                              const uint8_t  key   [PAGEROS_AEAD_KEY_LEN],
                              const uint8_t  nonce [PAGEROS_AEAD_NONCE_LEN],
                              const uint8_t *aad,  size_t aad_len,
                              const uint8_t *pt,   size_t pt_len);

// Verify the tag and decrypt. Returns true on success and writes
// `ct_len` bytes of plaintext into `pt`; returns false if the tag does
// not match (in which case `pt` contents are unspecified — do not use
// them).
bool pageros_crypto_aead_open(uint8_t       *pt,
                              const uint8_t  tag   [PAGEROS_AEAD_TAG_LEN],
                              const uint8_t  key   [PAGEROS_AEAD_KEY_LEN],
                              const uint8_t  nonce [PAGEROS_AEAD_NONCE_LEN],
                              const uint8_t *aad,  size_t aad_len,
                              const uint8_t *ct,   size_t ct_len);

#ifdef __cplusplus
}
#endif
