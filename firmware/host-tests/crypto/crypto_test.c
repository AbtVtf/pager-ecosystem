// SPDX-License-Identifier: Apache-2.0
//
// Host-side validation for FW-015 crypto primitive wrappers. Drives the
// wrappers in `firmware/components/crypto/` (which call Monocypher) and
// checks them against the RFC test vectors committed in
// `docs/spec/crypto-test-vectors.json`:
//
//   * Ed25519 sign + verify  — RFC 8032 §7.1 TEST 1 + TEST 2
//   * X25519 pubkey + ECDH    — RFC 7748 §6.1 Alice/Bob
//   * ChaCha20-Poly1305       — RFC 8439 §2.8.2
//   * Negative AEAD case      — mutated-tag from the same JSON
//
// Plus a sign→verify→tamper round trip and an end-to-end ECDH-derived
// AEAD seal/open. Together these are the "Each operation passes RFC
// test vectors" half of the FW-015 acceptance.
//
// We link the wrapper source + Monocypher directly so the host gcc
// build doesn't need ESP-IDF.

#include "pageros_crypto.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static size_t hex_decode(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t hlen = strlen(hex);
    if (hlen % 2 != 0) return 0;
    size_t n = hlen / 2;
    if (n > out_cap) return 0;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Ed25519 — RFC 8032 §7.1
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    const char *seed_hex;
    const char *pub_hex;
    const char *msg_hex;     // empty string means zero-length message
    const char *sig_hex;
} ed25519_vec_t;

static const ed25519_vec_t ED25519_VECS[] = {
    { "rfc8032 TEST 1",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "rfc8032 TEST 2",
      "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
};

static void test_ed25519_sign_matches_rfc(void)
{
    for (size_t i = 0; i < sizeof(ED25519_VECS) / sizeof(ED25519_VECS[0]); i++) {
        const ed25519_vec_t *v = &ED25519_VECS[i];
        uint8_t seed[32], pub[32], want_sig[64];
        uint8_t msg[256];
        size_t msg_len;
        CHECK(hex_decode(v->seed_hex, seed, 32) == 32, "%s seed", v->name);
        CHECK(hex_decode(v->pub_hex,  pub,  32) == 32, "%s pub",  v->name);
        CHECK(hex_decode(v->sig_hex,  want_sig, 64) == 64, "%s sig", v->name);
        msg_len = strlen(v->msg_hex) > 0
                    ? hex_decode(v->msg_hex, msg, sizeof(msg))
                    : 0;

        uint8_t got_sig[64];
        pageros_crypto_ed25519_sign(got_sig, seed, pub, msg, msg_len);
        CHECK(memcmp(got_sig, want_sig, 64) == 0,
              "%s: sign output != RFC", v->name);

        CHECK(pageros_crypto_ed25519_verify(want_sig, pub, msg, msg_len),
              "%s: verify(rfc sig) must succeed", v->name);

        // Tamper a bit of the signature and confirm verify fails.
        uint8_t bad_sig[64];
        memcpy(bad_sig, want_sig, 64);
        bad_sig[7] ^= 0x40;
        CHECK(!pageros_crypto_ed25519_verify(bad_sig, pub, msg, msg_len),
              "%s: verify(tampered sig) must fail", v->name);

        // Tamper the message: every byte change must invalidate.
        if (msg_len > 0) {
            uint8_t bad_msg[256];
            memcpy(bad_msg, msg, msg_len);
            bad_msg[0] ^= 0x01;
            CHECK(!pageros_crypto_ed25519_verify(want_sig, pub, bad_msg, msg_len),
                  "%s: verify(tampered msg) must fail", v->name);
        }
    }
}

// ---------------------------------------------------------------------------
// X25519 — RFC 7748 §6.1 (Alice/Bob)
// ---------------------------------------------------------------------------

static void test_x25519_rfc7748_alice_bob(void)
{
    // Alice's private + public, Bob's public, and the expected shared.
    static const char *alice_priv =
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    static const char *alice_pub  =
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    static const char *bob_pub    =
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
    static const char *shared_x   =
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

    uint8_t priv[32], want_pub[32], want_shared[32], peer[32];
    hex_decode(alice_priv, priv,        32);
    hex_decode(alice_pub,  want_pub,    32);
    hex_decode(shared_x,   want_shared, 32);
    hex_decode(bob_pub,    peer,        32);

    uint8_t got_pub[32], got_shared[32];
    pageros_crypto_x25519_pubkey(got_pub, priv);
    CHECK(memcmp(got_pub, want_pub, 32) == 0,
          "X25519 public_key(alice_priv) != RFC 7748 §6.1");

    pageros_crypto_x25519_ecdh(got_shared, priv, peer);
    CHECK(memcmp(got_shared, want_shared, 32) == 0,
          "X25519 ECDH(alice_priv, bob_pub) != RFC 7748 §6.1");
}

// Round trip: two fresh keypairs derive the same shared secret regardless
// of which side does the ECDH. This is the property exit-nodes and push-relay
// rely on for envelope decryption (§9.3).
static void test_x25519_ecdh_symmetric(void)
{
    uint8_t a_priv[32] = {1}, b_priv[32] = {2};
    uint8_t a_pub[32], b_pub[32];
    pageros_crypto_x25519_pubkey(a_pub, a_priv);
    pageros_crypto_x25519_pubkey(b_pub, b_priv);

    uint8_t sa[32], sb[32];
    pageros_crypto_x25519_ecdh(sa, a_priv, b_pub);
    pageros_crypto_x25519_ecdh(sb, b_priv, a_pub);
    CHECK(memcmp(sa, sb, 32) == 0,
          "X25519 ECDH must be symmetric across the two parties");
}

// ---------------------------------------------------------------------------
// ChaCha20-Poly1305 — RFC 8439 §2.8.2
// ---------------------------------------------------------------------------

static void test_chacha20poly1305_rfc8439(void)
{
    // RFC 8439 §2.8.2 reference vector.
    static const char *key_hex =
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
    static const char *nonce_hex = "070000004041424344454647";
    static const char *aad_hex   = "50515253c0c1c2c3c4c5c6c7";
    static const char *pt_hex =
        "4c616469657320616e642047656e746c656d656e206f662074686520636c61"
        "7373206f66202739393a204966204920636f756c64206f6666657220796f75"
        "206f6e6c79206f6e652074697020666f7220746865206675747572652c2073"
        "756e73637265656e20776f756c642062652069742e";
    // The "ciphertext" field in the JSON / RFC is ct || tag concatenated.
    static const char *ct_tag_hex =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd060"
        "0691";

    uint8_t key[32], nonce[12], aad[12], pt[256], want_ct[256], want_tag[16];
    hex_decode(key_hex,   key,   32);
    hex_decode(nonce_hex, nonce, 12);
    hex_decode(aad_hex,   aad,   12);
    size_t pt_len = hex_decode(pt_hex, pt, sizeof(pt));

    uint8_t ct_tag_buf[256];
    size_t ct_tag_len = hex_decode(ct_tag_hex, ct_tag_buf, sizeof(ct_tag_buf));
    CHECK(ct_tag_len == pt_len + 16, "ct||tag length mismatch (%zu vs %zu)",
          ct_tag_len, pt_len + 16);
    memcpy(want_ct,  ct_tag_buf,          pt_len);
    memcpy(want_tag, ct_tag_buf + pt_len, 16);

    uint8_t got_ct[256], got_tag[16];
    pageros_crypto_aead_seal(got_ct, got_tag, key, nonce, aad, 12, pt, pt_len);

    CHECK(memcmp(got_ct,  want_ct,  pt_len) == 0,
          "ChaCha20-Poly1305 ciphertext != RFC 8439 §2.8.2");
    CHECK(memcmp(got_tag, want_tag, 16) == 0,
          "ChaCha20-Poly1305 tag != RFC 8439 §2.8.2");

    // open() round trip.
    uint8_t got_pt[256];
    bool ok = pageros_crypto_aead_open(got_pt, want_tag, key, nonce,
                                       aad, 12, want_ct, pt_len);
    CHECK(ok, "open() must accept the RFC ciphertext+tag");
    CHECK(memcmp(got_pt, pt, pt_len) == 0,
          "open() produced wrong plaintext");
}

// Mutated-tag vector from the same JSON file. Decrypt MUST fail.
static void test_chacha20poly1305_mutated_tag(void)
{
    static const char *key_hex =
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
    static const char *nonce_hex = "070000004041424344454647";
    static const char *aad_hex   = "50515253c0c1c2c3c4c5c6c7";
    static const char *ct_tag_hex =
        // Same as RFC 8439 §2.8.2 but final byte XOR 0x01.
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd060"
        "0690";

    uint8_t key[32], nonce[12], aad[12], buf[256];
    hex_decode(key_hex,   key,   32);
    hex_decode(nonce_hex, nonce, 12);
    hex_decode(aad_hex,   aad,   12);
    size_t blen = hex_decode(ct_tag_hex, buf, sizeof(buf));
    size_t pt_len = blen - 16;

    uint8_t pt[256];
    bool ok = pageros_crypto_aead_open(pt, buf + pt_len, key, nonce,
                                       aad, 12, buf, pt_len);
    CHECK(!ok, "mutated-tag ciphertext must be rejected");
}

// End-to-end: ECDH-derived key + AEAD seal/open mirrors the LoRa
// envelope path described in SPEC §9.3 (without the HKDF stage —
// HKDF is a separate primitive owned by the protocol layer).
static void test_e2e_ecdh_aead(void)
{
    uint8_t a_priv[32] = { 0xaa, 0x11 }, b_priv[32] = { 0xbb, 0x22 };
    uint8_t a_pub[32], b_pub[32], shared[32];
    pageros_crypto_x25519_pubkey(a_pub, a_priv);
    pageros_crypto_x25519_pubkey(b_pub, b_priv);
    pageros_crypto_x25519_ecdh(shared, a_priv, b_pub);

    uint8_t nonce[12] = { 0x01, 0x02, 0x03 };
    uint8_t aad[]     = "hdr";
    uint8_t msg[]     = "hello over LoRa";
    uint8_t ct[sizeof(msg)], tag[16], rt[sizeof(msg)];

    pageros_crypto_aead_seal(ct, tag, shared, nonce, aad, sizeof(aad), msg, sizeof(msg));

    // The B side derives the same shared key via the reverse ECDH.
    uint8_t shared_b[32];
    pageros_crypto_x25519_ecdh(shared_b, b_priv, a_pub);
    bool ok = pageros_crypto_aead_open(rt, tag, shared_b, nonce, aad, sizeof(aad),
                                       ct, sizeof(msg));
    CHECK(ok, "B must be able to decrypt with the symmetric shared secret");
    CHECK(memcmp(rt, msg, sizeof(msg)) == 0, "round-trip plaintext mismatch");
}

// ---------------------------------------------------------------------------
// FW-015 perf clause: "benchmarked at >= 100 ops/s". This is a *host* bench
// just to prove the wrappers are nowhere near the floor — the real target
// device is faster than this gcc -O2 host build for the symmetric path and
// roughly the same order of magnitude for Ed25519 / X25519. The test fails
// the suite if any operation comes in under 100 ops/s on this host.
// ---------------------------------------------------------------------------

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void test_perf_floor(void)
{
    // Sign perf
    uint8_t seed[32] = {0}; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    uint8_t pub[32], sig[64];
    uint8_t seed_copy[32]; memcpy(seed_copy, seed, 32);
    pageros_crypto_ed25519_pubkey(pub, seed_copy);

    const uint8_t msg[64] = {0};
    const int sign_iters = 200;
    double t0 = now_s();
    for (int i = 0; i < sign_iters; i++) {
        pageros_crypto_ed25519_sign(sig, seed, pub, msg, sizeof(msg));
    }
    double sign_ops_s = sign_iters / (now_s() - t0);
    CHECK(sign_ops_s >= 100.0,
          "Ed25519 sign perf %.0f ops/s < 100 ops/s floor", sign_ops_s);

    // X25519 perf
    uint8_t priv[32] = {2}, peer[32];
    pageros_crypto_x25519_pubkey(peer, (uint8_t[32]){3});
    const int x_iters = 200;
    uint8_t shared[32];
    t0 = now_s();
    for (int i = 0; i < x_iters; i++) {
        pageros_crypto_x25519_ecdh(shared, priv, peer);
    }
    double x_ops_s = x_iters / (now_s() - t0);
    CHECK(x_ops_s >= 100.0,
          "X25519 ECDH perf %.0f ops/s < 100 ops/s floor", x_ops_s);

    // AEAD perf (per-call seal of a 64-byte message — the order of
    // magnitude representative of LoRa frames).
    uint8_t key[32] = {7}, nonce[12] = {0};
    uint8_t ct[64], tag[16];
    const int aead_iters = 5000;
    t0 = now_s();
    for (int i = 0; i < aead_iters; i++) {
        pageros_crypto_aead_seal(ct, tag, key, nonce, NULL, 0, msg, 64);
    }
    double aead_ops_s = aead_iters / (now_s() - t0);
    CHECK(aead_ops_s >= 100.0,
          "AEAD seal perf %.0f ops/s < 100 ops/s floor", aead_ops_s);

    printf("perf: ed25519_sign=%.0f/s  x25519_ecdh=%.0f/s  aead_seal=%.0f/s\n",
           sign_ops_s, x_ops_s, aead_ops_s);
}

int main(void)
{
    test_ed25519_sign_matches_rfc();
    test_x25519_rfc7748_alice_bob();
    test_x25519_ecdh_symmetric();
    test_chacha20poly1305_rfc8439();
    test_chacha20poly1305_mutated_tag();
    test_e2e_ecdh_aead();
    test_perf_floor();

    if (fail_count == 0) {
        printf("OK (7 test groups; ed25519/x25519/aead RFC vectors)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
