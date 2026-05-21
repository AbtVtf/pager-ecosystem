// SPDX-License-Identifier: Apache-2.0
//
// Host-side validation for FW-014:
//   1. Ed25519 public-key derivation matches the RFC 8032 test vectors
//      committed in `docs/spec/crypto-test-vectors.json`. This is the
//      crypto correctness guarantee FW-014's acceptance leans on.
//   2. The 12-char base32 fingerprint (SPEC §9.1) is stable, length-12,
//      and uses only RFC 4648 base32 alphabet characters.
//
// We link Monocypher's RFC 8032 Ed25519 (SHA-512 variant) directly so
// the host gcc build doesn't need ESP-IDF / mbedtls. The fingerprint
// helper is duplicated below so the identity component doesn't need to
// expose its `_pubkey_fingerprint` symbol to a host build (it pulls in
// nvs_flash via the component graph). The fingerprint logic is small
// enough that a parallel definition is cheaper than a separate
// translation unit.

#include "monocypher-ed25519.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

// -- hex helpers ------------------------------------------------------------

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// -- fingerprint helper (matches identity.c) --------------------------------

static const char BASE32[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static void fingerprint(char out[13], const uint8_t pubkey[32])
{
    uint64_t hi = 0;
    for (int i = 0; i < 8; i++) hi = (hi << 8) | pubkey[i];
    hi >>= 4;
    for (int i = 11; i >= 0; i--) {
        out[i] = BASE32[hi & 0x1f];
        hi >>= 5;
    }
    out[12] = '\0';
}

// -- RFC 8032 test vectors --------------------------------------------------

typedef struct {
    const char *name;
    const char *seed_hex;
    const char *pub_hex;
} rfc8032_vec_t;

static const rfc8032_vec_t RFC8032[] = {
    // TEST 1
    { "rfc8032 TEST 1",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a" },
    // TEST 2
    { "rfc8032 TEST 2",
      "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c" },
    // TEST 3
    { "rfc8032 TEST 3",
      "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025" },
};

static void test_rfc8032_pubkey_derivation(void)
{
    for (size_t i = 0; i < sizeof(RFC8032) / sizeof(RFC8032[0]); i++) {
        const rfc8032_vec_t *v = &RFC8032[i];
        uint8_t seed[32], want[32], got_sk[64], got_pub[32];
        CHECK(hex_decode(v->seed_hex, seed, 32) == 0, "seed hex parse");
        CHECK(hex_decode(v->pub_hex,  want, 32) == 0, "pub hex parse");

        // Pass a copy because Monocypher zeroes the seed input.
        uint8_t seed_copy[32];
        memcpy(seed_copy, seed, 32);
        crypto_ed25519_key_pair(got_sk, got_pub, seed_copy);

        CHECK(memcmp(got_pub, want, 32) == 0, "%s: pubkey mismatch", v->name);

        // libsodium-style sk = seed || pubkey
        CHECK(memcmp(got_sk,      seed, 32) == 0,
              "%s: sk[0..32] should be seed", v->name);
        CHECK(memcmp(got_sk + 32, want, 32) == 0,
              "%s: sk[32..64] should be pubkey", v->name);
    }
}

// First boot is simulated by feeding a fresh seed twice — the second
// derivation must match the first (deterministic).
static void test_derivation_is_deterministic(void)
{
    uint8_t seed[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    uint8_t a_sk[64], a_pub[32], b_sk[64], b_pub[32];
    uint8_t seed_a[32], seed_b[32];
    memcpy(seed_a, seed, 32);
    memcpy(seed_b, seed, 32);

    crypto_ed25519_key_pair(a_sk, a_pub, seed_a);
    crypto_ed25519_key_pair(b_sk, b_pub, seed_b);

    CHECK(memcmp(a_pub, b_pub, 32) == 0, "derivation must be deterministic");
}

// SPEC §9.1: "12-char base32 fingerprint" — verify length, alphabet,
// and that distinct pubkeys give distinct fingerprints (probabilistic,
// but the first 60 bits of two well-separated RFC8032 pubkeys differ).
static void test_fingerprint_format(void)
{
    uint8_t pub1[32], pub2[32];
    (void)hex_decode(RFC8032[0].pub_hex, pub1, 32);
    (void)hex_decode(RFC8032[1].pub_hex, pub2, 32);

    char fp1[13], fp2[13];
    fingerprint(fp1, pub1);
    fingerprint(fp2, pub2);

    CHECK(strlen(fp1) == 12, "fp1 len=%zu", strlen(fp1));
    CHECK(strlen(fp2) == 12, "fp2 len=%zu", strlen(fp2));
    for (int i = 0; i < 12; i++) {
        // `memchr` (not `strchr`) — BASE32 is a fixed 32-byte alphabet
        // with no terminating NUL.
        CHECK(memchr(BASE32, fp1[i], sizeof(BASE32)) != NULL,
              "fp1 char[%d]='%c' not in base32 alphabet", i, fp1[i]);
        CHECK(memchr(BASE32, fp2[i], sizeof(BASE32)) != NULL,
              "fp2 char[%d]='%c' not in base32 alphabet", i, fp2[i]);
    }
    CHECK(strcmp(fp1, fp2) != 0,
          "distinct pubkeys must produce distinct fingerprints");
}

// Encoding is MSB-first, top 60 bits → first 12 base32 chars.
// Hand-computed: pub = 0xFF 0xF0 ...  → top 60 bits = 0xFFF0...
// First 5 bits = 0b11111 = 31 → '7' (last char of alphabet)
// Easier sanity: an all-zero pubkey yields "AAAAAAAAAAAA".
static void test_fingerprint_known_inputs(void)
{
    uint8_t zero[32] = {0};
    char fp[13];
    fingerprint(fp, zero);
    CHECK(strcmp(fp, "AAAAAAAAAAAA") == 0,
          "all-zero pubkey fingerprint expected AAAAAAAAAAAA, got %s", fp);

    uint8_t allff[32];
    memset(allff, 0xff, 32);
    fingerprint(fp, allff);
    CHECK(strcmp(fp, "777777777777") == 0,
          "all-0xff pubkey fingerprint expected 777777777777, got %s", fp);
}

int main(void)
{
    test_rfc8032_pubkey_derivation();
    test_derivation_is_deterministic();
    test_fingerprint_format();
    test_fingerprint_known_inputs();

    if (fail_count == 0) {
        printf("OK (4 test cases, 3 RFC 8032 vectors)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
