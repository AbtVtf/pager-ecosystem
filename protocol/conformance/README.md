# PagerOS UI-Protocol Conformance Runner (PROTO-005)

A small, pure-stdlib Python 3 CLI that drives every test vector under
`protocol/test-vectors/ui/` against an SDK's conformance HTTP endpoint, then
prints a pass/fail report. Used in CI for SDK PRs (PY-012 et al.).

The protocol itself is defined in `protocol/spec.md` (PROTO-002). The vectors
are produced by `protocol/test-vectors/ui/generate.py` (PROTO-003).

---

## Invoking the runner

```
python3 protocol/conformance/proto_conformance.py \
    --endpoint http://localhost:8080 \
    [--vectors protocol/test-vectors/ui] \
    [--filter '<glob>'] \
    [--report text|json] \
    [--allow-non-canonical] \
    [--timeout 10]
```

| Flag | Default | Meaning |
|---|---|---|
| `--endpoint URL` | (required) | Base URL of the SDK under test. The runner POSTs to `<URL>/conformance/encode` and `<URL>/conformance/decode`. Trailing slash optional. |
| `--vectors DIR` | `protocol/test-vectors/ui` | Path to a vector directory. Must contain `index.json`. |
| `--filter GLOB` | none | Run only vectors whose `name` matches the fnmatch glob. Repeat the flag to OR multiple globs. |
| `--report MODE` | `text` | `text` for human-readable summary, `json` for machine-readable. |
| `--allow-non-canonical` | off | Skip byte-equal comparison on `encode` vectors when the SDK does not advertise canonical CBOR. Always uses structural compare in that case. |
| `--timeout SECONDS` | `10` | Per-request HTTP timeout. |
| `--verbose` | off | Print each vector's outcome instead of only failures. |

Exit codes: `0` if every vector passed, `1` if any vector failed, `2` for a
runner-level error (endpoint unreachable, malformed vector directory, etc.).

---

## SDK conformance HTTP contract

An SDK is "conformance-ready" when its HTTP server exposes two endpoints that
behave as described below.

### `POST /conformance/encode`

The SDK MUST encode the given logical input through its protocol encoder and
return the CBOR bytes.

| Direction | Header | Body |
|---|---|---|
| Request | `Content-Type: application/json` | `{ "name": "<vector-name>", "input": <descriptor.input> }` |
| Response (success) | `Content-Type: application/cbor` plus optional `X-PagerOS-Canonical: true` | The raw CBOR bytes the encoder produced. |
| Response (refusal) | `Content-Type: application/json`, HTTP 4xx | `{ "error": "<class>", "message": "<optional human text>" }` |

Behavior:

- `name` is a hint for logs; the SDK MUST encode purely from `input` and not
  rely on a name-to-output table.
- `$bytes` markers inside `input` carry CBOR byte strings (`{"$bytes":"<hex>"}`)
  as described in `protocol/test-vectors/ui/README.md` § "Special JSON
  markers". Producers MUST translate them to CBOR major type 2.
- `X-PagerOS-Canonical: true` declares the response is RFC 8949 §4.2
  canonical. The runner then byte-equal-compares to the vector's
  `expected_cbor_hex`. Otherwise the runner structurally decodes both sides
  and compares.
- Refusing to encode an `encode`-kind vector counts as a failure.

### `POST /conformance/decode`

The SDK MUST decode the given CBOR bytes through its decoder and return a
structural JSON form.

| Direction | Header | Body |
|---|---|---|
| Request | `Content-Type: application/cbor` | Raw CBOR bytes (= the vector's `input_cbor_hex` decoded from hex, or `expected_cbor_hex` for `encode`-kind round-trip checks). |
| Response (success) | `Content-Type: application/json` | The decoded structure. Byte strings MUST be surfaced as `{"$bytes": "<hex>"}`. Unknown widgets MUST be surfaced per `protocol/test-vectors/ui/README.md` § "Special JSON markers" (`$render_placeholder` + `raw_widget`). |
| Response (refusal) | `Content-Type: application/json`, HTTP 4xx | `{ "error": "<class>", "message": "<optional human text>" }` |

Behavior:

- For a `decode_only` vector, the response body must structurally match the
  vector's `expected_decoded`.
- For a `negative` vector, the SDK MUST return HTTP 4xx with the
  `error` field equal to the vector's `expect_error` value.

### Error classes (negative vectors)

Conformance-relevant error classes the SDK can surface in `error`:

| Class | When |
|---|---|
| `invalid_frame_shape` | Top-level CBOR value is not a map, or required fields missing / wrong type. |
| `unknown_major_version` | Frame's `v` value names a major version the SDK does not implement. |
| `unknown_event_dropped` | Inbound server→device event has unknown `type` and was dropped per spec §6.3. |

SDKs MAY surface additional classes for their own use, but vectors only
exercise the classes above today. Future negative vectors will introduce new
classes in lockstep.

---

## Compare semantics

Decoded JSON comparison treats unordered map keys as equal regardless of
serialization order (Python `dict` equality) and treats `$bytes`,
`$render_placeholder`, `raw_widget` markers as ordinary keys — they only
matter because the SDK is expected to emit them with the same convention.

Floats compare bitwise: if a vector's `expected_decoded` contains a float,
the SDK is expected to round-trip it through its CBOR decoder identically.
All current vectors use floats that round-trip cleanly in binary32 / binary64.

---

## Wiring an SDK

The conformance contract is deliberately tiny so SDKs can expose it from a
thin adapter without touching production code. A typical wiring (Python SDK
sketch):

```python
from pageros import codec

def conformance_encode(name, input_json):
    return codec.encode_frame(_unmarshal_bytes(input_json))

def conformance_decode(cbor_bytes):
    return _marshal_bytes(codec.decode_frame(cbor_bytes))
```

`_unmarshal_bytes` / `_marshal_bytes` translate the `{"$bytes":"<hex>"}`
marker between JSON and Python `bytes`. The SDK's HTTP server (devserver,
Flask, etc.) wraps these in two POST handlers that follow the response shape
above.

This conformance surface is intentionally **not** required at runtime — it
exists for CI only. Production SDK builds MAY exclude it.

---

## Self-test

The runner ships with a stub SDK so its own behavior is testable without
depending on PY-001..PY-011:

```
python3 protocol/conformance/test_runner.py
```

The stub looks vectors up by name and serves their canonical bytes /
expected-decoded forms; the test driver then asserts the runner reports
86 / 86 pass with exit code 0, and that a deliberately broken stub causes
the runner to exit 1 with the failing vector named in the report.

---

## Related

- `protocol/spec.md` — UI protocol v1.0 (PROTO-002).
- `protocol/tag-registry.md` — Canonical tag assignments (PROTO-001).
- `protocol/test-vectors/ui/` — Vector source of truth (PROTO-003).
- `PY-012` — Python SDK conformance run (wires this runner into CI).
- `JS-011` (and equivalent) — JS SDK conformance run.
