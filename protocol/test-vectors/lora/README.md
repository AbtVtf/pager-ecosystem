# LoRa Envelope Test Vectors (LORA-001)

Authoritative byte-level test vectors for the outer LoRa envelope defined in
`SPEC.md` §6.2.1. Cross-implementation conformance: every implementation
(Go in `exit-node/`, C in `firmware/`, future SDKs) must reproduce these
bytes from the documented fields and parse them back to the same fields.

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

## Reference implementation

`exit-node/internal/lora` (Go). Unit tests in that package encode/decode each
of the cases above; if those tests pass, this directory is in sync.
