# PagerOS UI Protocol Test Vectors (PROTO-003)

Language-agnostic conformance vectors for the **UI protocol** layer defined in
`protocol/spec.md` (PROTO-002) and `protocol/tag-registry.md` (PROTO-001).
These vectors are the source of truth that every SDK (Python, JS, future) and
the firmware/simulator renderer MUST satisfy. The PROTO-005 conformance runner
drives them against an SDK HTTP endpoint and reports pass/fail.

> Scope: **UI protocol** — Frames, widgets, events, HTTP-level error frames,
> forward-compat. Lower-layer LoRa-envelope vectors live in
> `../lora/` (LORA-001).

---

## Layout

```
ui/
  README.md            this file
  generate.py          self-contained generator (pure Python, no deps)
  index.json           machine-readable index of every vector
  vectors/
    <name>.json        descriptor (input + expected metadata)
    <name>.cbor        canonical CBOR bytes
```

Every vector ships as a **JSON descriptor + CBOR byte file pair**. The two
files always agree: `descriptor.expected_cbor_hex == hex(cbor_file)`. A
language-agnostic consumer reads one or both depending on what it needs:

- **Encoder-conformance tests:** read `<name>.json#input`, encode it through
  the SDK, compare bytes against `<name>.cbor` (or `expected_cbor_hex`).
- **Decoder-conformance tests:** read `<name>.cbor`, decode it through the
  SDK, compare against `<name>.json#input` (or `#expected_decoded` for
  decode-only vectors that don't have a clean encoder counterpart).

---

## Descriptor schema

```jsonc
{
  "name": "widget_text_minimal_string",
  "category": "widget" | "event" | "error" | "oversized" | "forward_compat",
  "kind":     "encode" | "decode_only" | "negative",
  "tag_form": "string" | "numeric" | "mixed" | null,
  "encoding": "canonical_rfc8949",
  "description": "Human-readable summary",
  "input":            { /* logical Frame or event payload (for encode kind) */ },
  "input_cbor_hex":   "…",         // for decode_only / negative
  "expected_cbor_hex":"…",         // present on every vector
  "expected_decoded": { /* what decoder MUST yield (decode_only) */ },
  "expect_error":     "…",         // for negative
  "expected_size_bytes": 49,
  "notes": "…"
}
```

### Special JSON markers

JSON cannot natively express CBOR byte strings, so descriptors use a marker
object:

```json
{"$bytes": "04a1b2c3"}
```

is interpreted by both the generator and any conformant test runner as the
raw byte string `0x04a1b2c3` (CBOR major type 2). This keeps every descriptor
fully JSON-serializable.

For decode-only vectors, decoded forms occasionally use the same `$bytes:hex`
sentinel inside `expected_decoded` to denote byte-string fields the decoder
must surface.

For unknown-widget tests, `expected_decoded` may include:

- `$render_placeholder` — the literal string the renderer MUST emit in place
  of the unsupported widget (e.g., `[unsupported: 24]`).
- `raw_widget` — the original CBOR-decoded map, kept so test runners can also
  check that the bytes survived parsing.

### `kind`

- **encode** — round-trippable. The JSON `input` is canonical CBOR-encodable;
  the bytes in `<name>.cbor` are the unique canonical output.
- **decode_only** — the input is most naturally described in CBOR (e.g.,
  unknown numeric widget tags) and the SDK is only expected to **decode**
  gracefully. `expected_decoded` documents the structural result.
- **negative** — the SDK MUST reject the bytes with an error in the class
  named by `expect_error`. Used for unknown major-version frames, malformed
  top-level shapes, and inbound events with unknown `type`.

### `tag_form`

For widget / event vectors, indicates whether `t` / `type` values use the
string canonical form, the numeric canonical form (registry §2), or mixed
(forward-compat cases that combine both).

---

## Encoding rules

All `expected_cbor_hex` bytes are produced under **RFC 8949 §4.2 canonical
deterministic encoding**:

- Smallest unsigned integer encoding that fits.
- Definite-length arrays and maps.
- Map keys sorted lexicographically by their CBOR-encoded bytes (RFC 8949
  §4.2.1).
- 32-bit floats when the value round-trips through binary32; otherwise 64-bit.
- No semantic tags (PagerOS does not use them — registry §2.4).

This is stricter than what the on-the-wire spec requires (spec §3.1 allows
unordered maps and any minimal-or-not int encoding). The strict-canonical
choice is deliberate: it makes vectors **byte-exact reproducible**. A
conformance runner targeting an SDK that does **not** emit canonical CBOR
should compare structurally (decode both sides and check equality) rather
than byte-for-byte.

---

## Coverage

| Category          | Count | Notes |
|-------------------|-------|-------|
| `widget`          | 46    | Every v1 widget (text, list, input, form, button, image, map, notification, presence_list, chat) plus top-bar actions, a kitchen-sink Frame, and three Frame-level `subscribe_groups` cases (empty, multi-group with bound widgets, combined with `subscribe`) covering PROTO-004 spec §6.2.3. Each case has a string-tag and a numeric-tag variant. |
| `event`           | 18    | Every device→app event (`nfc_scan`, `location`, `back`, `tick`, `notification_action`) and every server→device group event (`member_joined`, `member_left`, `presence_update`, `group_message`). String + numeric `type` discriminator variants for each. |
| `error`           | 8     | Server-provided bodies for 400 / 401 / 403 / 404 / 410 / 429 / 5xx, plus the locally-built error frame shape (`id: "err_local"`, `style: "error"`) from spec §7.4.1. |
| `oversized`       | 3     | Chat-heavy, list-heavy, and form-heavy Frames each well over the 200 B LoRa budget. SDKs MUST surface the LoRa size warning (PY-011 hook) for these. |
| `forward_compat`  | 11    | Unknown widget tags in numeric reserved (11), v2 pre-allocated (24), on-device runtime (64), vendor (200), and string form; unknown top-level Frame field; unknown widget field; unknown event tag in `subscribe`; unknown inbound server→device event type (dropped); unknown major version (`v=99`, rejected); top-level non-map (rejected). |

`index.json` lists every vector with its files and size; see `generate.py`
for the canonical case list (the script IS the spec for which vectors exist).

---

## Workflows

### Regenerate vectors

```
python3 protocol/test-vectors/ui/generate.py
```

Idempotent. Clears `vectors/` first so removed cases do not linger. No
third-party dependencies — pure stdlib.

### Verify on-disk state

```
python3 protocol/test-vectors/ui/generate.py --verify
```

Re-reads every descriptor and re-encodes the input through the bundled
canonical CBOR encoder, asserting byte equality with the sibling `.cbor`
file. Should be run in CI before PROTO-005 wires the conformance harness.

### Consume from another language

1. Walk `vectors/*.json` (or read `index.json`).
2. For each `encode` vector, encode `descriptor.input` through your SDK and
   compare against `vectors/<name>.cbor`. If your SDK does not emit canonical
   CBOR, decode both sides and compare structurally.
3. For each `decode_only` vector, decode `vectors/<name>.cbor` and compare
   the structural result against `descriptor.expected_decoded`. Unknown
   widgets must surface as `[unsupported: <tag>]` lines.
4. For each `negative` vector, decoding MUST raise/return an error matching
   the class named in `descriptor.expect_error`.

---

## Adding a new vector

1. Edit `generate.py`, add a `Vector(...)` entry in the matching `*_vectors()`
   function.
2. Run `python3 protocol/test-vectors/ui/generate.py`.
3. Commit both the generator change and the resulting `vectors/*.json` /
   `vectors/*.cbor` updates and the regenerated `index.json` together.

Any change to widget/event tag identifiers requires a corresponding change
in `protocol/tag-registry.md` and `protocol/spec.md` (registry §3.2 / §4.1
spec gates). Tag-registry / spec edits require CEO approval per `CLAUDE.md`.

---

## Related

- `protocol/spec.md` — UI protocol v1.0 (PROTO-002).
- `protocol/tag-registry.md` — Numeric/string tag canonical assignments
  (PROTO-001).
- `protocol/test-vectors/lora/` — LoRa outer envelope vectors (LORA-001).
- `protocol/test-vectors/ui/` (this directory) — UI protocol vectors
  (PROTO-003).
- Future PROTO-005 conformance runner: walks `index.json`, drives the vectors
  against an SDK endpoint, emits a pass/fail report for CI.
