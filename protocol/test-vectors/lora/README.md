# LoRa Envelope & Fragment Test Vectors (LORA-001, LORA-002)

Authoritative byte-level test vectors for the outer LoRa envelope
(`SPEC.md` §6.2.1) and the fragmentation header (`SPEC.md` §6.2.3).
Cross-implementation conformance: every implementation (Go in
`exit-node/`, C in `firmware/`, future SDKs) must reproduce these bytes
from the documented fields and parse them back to the same fields.

Wire layout:

```
┌────────────┬────────────┬──────────────┬─────────────┬─────────────┐
│ magic (2B) │ version(1B)│ type (1B)    │ msg_id (4B) │ payload     │
└────────────┴────────────┴──────────────┴─────────────┴─────────────┘
```

- `magic` is `0x50 0x47` (ASCII "PG").
- `msg_id` is big-endian u32.
- `payload` is opaque to the codec (CBOR inner envelope for type 0x01/0x02).

## Files

Hex-encoded one packet per file. Filenames describe the case.

| File | version | type | msg_id (hex) | payload (hex) | Notes |
|---|---|---|---|---|---|
| `01_request_empty.hex` | 1 | 0x01 (request) | `cafebabe` | (none) | Header-only round-trip. |
| `02_response_hello.hex` | 1 | 0x02 (response) | `00000001` | `68656c6c6f` ("hello") | Short payload. |
| `03_ack_maxid.hex` | 1 | 0x03 (ack) | `ffffffff` | `deadbeef` | Max msg_id. |
| `04_advert_run.hex` | 1 | 0x04 (advert) | `0000002a` | 200×`5a` | Larger payload. |
| `05_unknown_type.hex` | 1 | 0x7f (unknown) | `00000005` | `0102` | Decoder MUST drop (per spec), header still parseable. |

## Negative cases (MUST be rejected)

| File | Why |
|---|---|
| `neg_01_swapped_magic.hex` | Magic bytes reversed (`47 50 …`). |
| `neg_02_wrong_magic.hex` | First magic byte wrong (`ff ff …`). |
| `neg_03_short.hex` | 4 bytes — shorter than the 8-byte header. |

## Fragment vectors (LORA-002)

Each fragment travels inside an outer envelope's Payload field with the
fixed header:

```
┌─────────────┬─────────────┬──────────────┐
│ frag_id 1B  │ total 1B    │ data (var.)  │
└─────────────┴─────────────┴──────────────┘
```

`total` is the count of fragments in the message (1..255). A message
that fits in a single packet uses `total=1, frag_id=0` — fragmentation
is always present on the wire so receivers have one code path. The
outer envelope's `msg_id` groups fragments of one logical message.

| File | frag_id | total | data (hex) | Notes |
|---|---|---|---|---|
| `frag_01_single.hex` | 0 | 1 | `68656c6c6f` ("hello") | Single-fragment message. |
| `frag_02_first_of_three.hex` | 0 | 3 | 50×`ab` | First of three fragments. |
| `frag_03_last_of_three.hex` | 2 | 3 | `0102…0a` (10B) | Last fragment, shorter tail. |

### Fragment negative cases (MUST be rejected)

| File | Why |
|---|---|
| `neg_frag_01_total_zero.hex` | `total=0` is invalid (must be >= 1). |
| `neg_frag_02_id_out_of_range.hex` | `frag_id=5` with `total=3` (id must be < total). |

## Inner-envelope vectors (LORA-003)

The inner envelope rides inside the reassembled outer-envelope payload
(SPEC.md §6.2.2). On the wire it is a CBOR map with five fields:

| Field   | CBOR type | Width  | Notes |
|---------|-----------|--------|-------|
| `to`    | tstr      | var    | Target URL, cleartext (Exit Node reads to route + EXIT-003 HTTPS gate). |
| `from`  | bstr      | 32 B   | Sender X25519 pubkey, cleartext (recipient uses it for ECDH). |
| `nonce` | bstr      | 16 B   | Bytes 0..11 = SEC-001 §1.2 AEAD nonce (`sender_id_byte`, u64_be counter, 3 zero bytes); bytes 12..15 = sig-binding tail (4 bytes of additional entropy folded into the Ed25519 signature input). |
| `sig`   | bstr      | 64 B   | Ed25519 signature over `u16_be(len(to)) ‖ to ‖ u32_be(len(body)) ‖ body ‖ nonce` by the sender's Ed25519 key. |
| `body`  | bstr      | var    | ChaCha20-Poly1305 IETF ciphertext + 16 B Poly1305 tag, with AEAD nonce = `nonce[0..12]`, AAD = `to ‖ from`, key = `HKDF-SHA256(X25519(sender_priv, recipient_pub), salt=zero[32], info="pageros/v1/e2e", L=32)` (SEC-001 §1.1). |

| File | Plaintext | Sender X25519 priv | Recipient X25519 priv | Sender Ed25519 seed | Counter | sigSalt | Notes |
|---|---|---|---|---|---|---|---|
| `inner_01_seal.hex` | `"hello pageros"` | RFC 7748 Alice (`77076d…`) | RFC 7748 Bob (`5dab08…`) | RFC 8032 TEST 1 seed (`9d61b1…`) | 7 | `deadbeef` | Reference seal-and-sign. Recipient pubkey is RFC 7748 Bob pub. |

Implementations MUST: (1) reproduce these wire bytes from the inputs, and (2) decode + decrypt + signature-verify with the recipient's X25519 priv and the sender's Ed25519 pub.

## Reference implementation

`exit-node/internal/lora` (Go). Unit tests in that package encode/decode each
of the cases above; if those tests pass, this directory is in sync.
