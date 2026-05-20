# PagerOS Crypto Suite Selection

**Status:** v0.2 (SEC-001)
**Scope:** Concrete library choices for the three primitives PagerOS depends on (Ed25519, X25519, ChaCha20-Poly1305), and the shared test-vector set every implementation MUST pass.

This document fixes the libraries each subsystem links against. Anything else (alternative implementations, "but lib X is faster") needs a SEC issue and CEO sign-off — the goal of this doc is interop, not microbenchmarks.

## 1. Primitives in use

PagerOS only uses three primitives (per `SPEC.md` §9):

| Primitive | Use | Notes |
|---|---|---|
| **Ed25519** | Device identity keypair, request signing (`PagerOS-Sig`), `join_group` / `leave_group` signatures, app manifest signing on push | RFC 8032, deterministic variant. SHA-512 internal. |
| **X25519** | E2E key agreement between device ↔ app (LoRa-bound and push-bound payloads); device ↔ Push Relay; group fan-out (where apps implement it) | RFC 7748. Clamped scalars; reject all-zero shared-secret outputs. |
| **ChaCha20-Poly1305** | AEAD for the inner envelope after X25519 ECDH | IETF variant (RFC 8439), 96-bit nonce, 128-bit tag. **Not** the original 64-bit-nonce ChaCha20-Poly1305. |

No other primitives are in scope for v1. No AES, no NIST P-curves, no HKDF-SHA256 chains beyond the trivial KDF below.

### 1.1 Key derivation from X25519 shared secret

Every encryption use of X25519 produces a 32-byte shared secret. Implementations MUST derive the ChaCha20-Poly1305 symmetric key as:

```
key = HKDF-SHA256(
        ikm = X25519(priv_a, pub_b),
        salt = "" (32 zero bytes),
        info = "pageros/v1/e2e",
        L = 32
)
```

Rationale: a single, named KDF closes the "raw shared secret used as key" footgun and pins a domain separator so future protocol revisions can rotate the `info` string.

### 1.2 Nonce construction

For each AEAD encryption:

- **Push / app→device envelopes:** 12-byte nonce is `sender_id_byte || u64_be(message_counter) || 3 zero bytes`, where `sender_id_byte` is `0x01` for app→device, `0x02` for device→app. Counter MUST be persisted across reboots on the device. Replay below current counter is rejected by the receiver.
- **LoRa-bound envelopes:** same construction; counter is per `(src_pubkey, dst_pubkey)` pair.
- **Group fan-out:** apps choose, but MUST NOT reuse a `(key, nonce)` pair. Recommended: per-recipient counter with the same layout as above.

Implementations MUST NOT use random 96-bit nonces — the embedded device has weak entropy after wake-from-sleep, and a counter scheme is verifiable in our test vectors.

## 2. Library choices

| Subsystem | Language | Library | Rationale |
|---|---|---|---|
| **firmware** | C / ESP-IDF | **libsodium** (full build, not `-mini`) | Available as ESP-IDF managed component (`espressif/libsodium`). `crypto_sign_ed25519`, `crypto_scalarmult` (X25519), `crypto_aead_chacha20poly1305_ietf_*`. Hardware-friendly, audited, no integration surprises. We rejected `libsodium-mini`: the stripped build drops the IETF ChaCha20-Poly1305 we require. mbedTLS was rejected: no first-class IETF ChaCha20-Poly1305 AEAD API in the ESP-IDF version pinned by `firmware/`. |
| **Python SDK** | Python ≥ 3.10 | **PyNaCl ≥ 1.5** | Thin wrapper over libsodium → binary-identical to firmware. `nacl.signing.SigningKey` (Ed25519), `nacl.public.PrivateKey` (X25519), `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt`. Avoid `cryptography` for AEAD here — it uses OpenSSL's ChaCha20-Poly1305, which is the same IETF variant but a different library path; harder to keep our shared vectors honest when both wrappers exist. |
| **JS SDK** | TypeScript / browser + Node | **libsodium-wrappers-sumo ≥ 0.7.13** | WASM-backed libsodium. `crypto_sign_detached` / `crypto_sign_verify_detached`, `crypto_scalarmult`, `crypto_aead_chacha20poly1305_ietf_*`. The `sumo` build includes `crypto_scalarmult` (raw X25519); the non-sumo "browsers" build does not. Initialization is async (`await sodium.ready`) — wrap in our `cryptoReady()` helper. |
| **CLI / Exit Node / Push Relay** | Go 1.22+ | **`crypto/ed25519`** (stdlib) + **`golang.org/x/crypto/curve25519`** + **`golang.org/x/crypto/chacha20poly1305`** | All three are first-party / x/crypto. No CGo. Matches the firmware libsodium output byte-for-byte for the test vectors below. |
| **Simulator core** | Rust (Tauri) | **`ed25519-dalek` v2** + **`x25519-dalek` v2** + **`chacha20poly1305` crate v0.10** | RustCrypto stack. Already pulled in by other Tauri deps. `ed25519-dalek` v2's "strict" verification matches libsodium / Go behavior (rejects non-canonical signatures). |

Every choice above resolves to a libsodium-compatible byte layout. The shared test vectors in §3 are how we keep them honest.

### 2.1 What we are NOT doing

- No "pluggable" crypto. The library is pinned per subsystem.
- No version-negotiation in the wire format. The protocol assumes the v1 suite; future suite changes ship as a new protocol version.
- No password-derived keys, no PAKE, no PIN-protected keystore on device for v1. Device privkey lives in NVS behind flash encryption (`SPEC.md` §9.1).
- No certificate chains. Trust is pinned via the Marketplace DNS TXT challenge (`SPEC.md` §10.5).

## 3. Shared test vectors

The canonical test-vector file lives at `docs/spec/crypto-test-vectors.json`. **Every SDK MUST load this file and pass every vector before its test suite passes.** Adding a new primitive use case = adding a vector here first.

Each vector is one of three `kind`s:

- `ed25519` — `{ seed (32B hex), pub (32B hex), msg (hex), sig (64B hex) }`. Test = derive pub from seed, sign msg, byte-equal sig; verify sig against pub + msg.
- `x25519` — `{ priv (32B hex), pub_peer (32B hex), shared (32B hex), derived_key (32B hex) }`. Test = compute X25519(priv, pub_peer) byte-equal to `shared`; HKDF-derive `key` per §1.1 byte-equal to `derived_key`.
- `chacha20poly1305` — `{ key (32B hex), nonce (12B hex), aad (hex), plaintext (hex), ciphertext (hex) }`. `ciphertext` includes the 16-byte Poly1305 tag at the end. Test in both directions: encrypt = byte-equal ciphertext; decrypt = byte-equal plaintext. Decrypt with mutated tag MUST fail with the library's auth-error path.

### 3.1 Vectors included (v0.2)

The initial set is small but covers every code path we care about. PROTO-005 (conformance suite) will grow this set; SEC owns approving additions.

1. **Ed25519 / RFC 8032 §7.1 TEST 1** — empty message. Roundtrip baseline.
2. **Ed25519 / RFC 8032 §7.1 TEST 2** — 1-byte message `0x72`.
3. **Ed25519 / RFC 8032 §7.1 TEST 3** — 2-byte message `0xaf82`. Picks up byte-order regressions.
4. **Ed25519 / PagerOS-Sig sample** — message = `"POST" || "/v1/events" || "1700000000" || sha256("{}")` to exercise the §9.2 signing input layout end-to-end.
5. **X25519 / RFC 7748 §6.1** — Alice & Bob known-good pair → shared secret.
6. **X25519 / PagerOS KDF** — same pair, then HKDF per §1.1 → `derived_key`.
7. **ChaCha20-Poly1305 / RFC 8439 §2.8.2** — the canonical "Ladies and Gentlemen" vector.
8. **ChaCha20-Poly1305 / PagerOS nonce** — encrypt a 0-byte plaintext with the §1.2 counter-nonce construction (sender_id=0x01, counter=0). Confirms the nonce layout, not just the cipher.
9. **ChaCha20-Poly1305 / tag mutation** — same as (7), with the last byte of the tag flipped; decrypt MUST fail.

Vector 4, 6, 8, 9 are PagerOS-specific and were generated with PyNaCl + a small Go program; both produced byte-identical outputs, which is exactly the cross-impl property we're relying on.

### 3.2 How each subsystem consumes the vectors

- **firmware:** `firmware/components/pageros_crypto/test/` builds a host-side unit test (`idf.py --preview build -p test`) that reads the JSON via the bundled `cJSON`. The on-device build links the same code path but uses a stripped subset (vectors 1, 5, 7) due to flash budget.
- **sdk/python:** `pytest tests/test_crypto_vectors.py` loads the JSON via stdlib `json`. CI gate.
- **sdk/js:** `tests/crypto-vectors.spec.ts` (Vitest). Bundled into both Node and JSDOM runs.
- **cli / exit-node / push-relay:** `internal/cryptotest/vectors_test.go` shared via Go internal package.
- **simulator:** `cargo test --package pageros-core crypto::vectors`.

Each path is one of the green checks PROTO-005's conformance suite will roll up. Until PROTO-005 lands, vector pass/fail is checked locally per subsystem before commit.

## 4. Update / deprecation policy

- Adding a vector: SEC issue, vector reviewed against at least two reference impls (libsodium + Go x/crypto), then merged with a SEC-* commit.
- Changing a library version: minor bumps fine; major bumps require a SEC issue (e.g. ed25519-dalek v1 → v2 required a strict-verify migration check).
- Changing a primitive: not in scope for v1. Would ship as PagerOS v2 wire format, not as a patch.

## 5. Open items

- **SEC-002** (threat model) will reference §1.2 nonce construction when reasoning about replay across reboots.
- **SEC-003** (external review) will get the test-vector file plus the four implementations' test outputs as one of its inputs.
- Hardware-backed key storage on ESP32-S3 beyond NVS flash encryption (e.g. eFuse-bound HMAC) is **not** v1; tracked separately when FW-secure-element work begins.
