# PagerOS CBOR Tag Registry

> **Status:** v1.0 (PROTO-001)
> **Authority:** This document is the canonical assignment of widget and event
> identifiers for the PagerOS wire protocol. It is referenced normatively by
> `protocol/spec.md` (PROTO-002) and `protocol/test-vectors/` (PROTO-003).
> **Source spec:** `SPEC.md` v0.2 §5.3, §5.4, §16.4.

---

## 1. Scope

PagerOS Frames are CBOR maps (`SPEC.md` §5.2). Two pieces of a Frame need
stable identifiers across every SDK, the firmware renderer, and the simulator:

1. **Widget type** — the value of a widget map's `t` field (`SPEC.md` §5.3).
2. **Event type** — the values listed in a Frame's `subscribe` /
   `subscribe_groups` arrays (`SPEC.md` §5.4.1, §5.4.2) and the type discriminator
   in event payloads sent server→device.

For each, this document defines:

- The **canonical string tag** — matches `SPEC.md` literally; required for
  human-readable encodings and recommended over the wire on Wi-Fi.
- The **canonical numeric tag** — a small unsigned integer; recommended on the
  wire when the Frame targets LoRa transport (≤ 200 B budget, `SPEC.md` §14).

Both forms are **equally canonical**. They identify the same widget/event and
are interchangeable.

---

## 2. Encoding rules

The following rules apply uniformly to widget `t` fields and event type
discriminators.

### 2.1 Producer rules

- A producer (App SDK, server) **MAY** emit either the string form or the
  numeric form.
- A producer **MUST NOT** mix forms within a single Frame's widget tree just
  to save bytes; pick one form per Frame for readability of captures and
  test vectors. (This is a SHOULD-strength convention; decoders accept mixed
  forms.)
- A producer **SHOULD** use the numeric form when `ctx.transport == "lora"`
  (`SPEC.md` §8.3) and the string form otherwise.

### 2.2 Consumer rules

- A consumer (firmware renderer, simulator, conformance runner) **MUST**
  accept both forms and treat them as equivalent.
- A consumer **MUST** treat an unrecognized widget tag (in either form) as
  `[unsupported: <tag>]` per `SPEC.md` §5.3 — graceful forward-compatibility.
- A consumer **MUST** treat an unrecognized event tag in a subscribe array
  as a silently-ignored entry; this lets newer apps target older devices
  without breaking subscription setup.

### 2.3 Byte cost

CBOR encodes small unsigned integers compactly:

| Range          | CBOR bytes (head + body) |
|----------------|--------------------------|
| 0 – 23         | 1 byte                   |
| 24 – 255       | 2 bytes                  |
| 256 – 65 535   | 3 bytes                  |

Compare against the string form, which costs 1 head byte + 1 byte/UTF-8
codepoint (e.g., `"presence_list"` = 14 bytes; numeric `9` = 1 byte). The
v1 widget set is intentionally packed into the 1-byte range.

### 2.4 No conflict with RFC 8949 semantic tags

The integers defined here are CBOR **values** carried in the `t` field of a
widget map or as the type discriminator of an event payload. They are
**not** IANA-registered CBOR semantic tags (RFC 8949 §3.4). PagerOS does
not assign or rely on any RFC 8949 semantic tag in v1.

---

## 3. Widget tag registry

Defined widgets occupy the **single-byte CBOR range (0–23)**. Ranges above
that are reserved per §5.

| Numeric | String           | Spec      | Range / status      |
|--------:|------------------|-----------|---------------------|
| 0       | *(reserved)*     | —         | Invalid / sentinel. Producers MUST NOT emit; consumers MUST treat as unknown. |
| 1       | `text`           | §5.3.1    | v1 core             |
| 2       | `list`           | §5.3.2    | v1 core             |
| 3       | `input`          | §5.3.3    | v1 core             |
| 4       | `form`           | §5.3.4    | v1 core             |
| 5       | `button`         | §5.3.5    | v1 core             |
| 6       | `image`          | §5.3.6    | v1 core             |
| 7       | `map`            | §5.3.7    | v1 core             |
| 8       | `notification`   | §5.3.8    | v1 core             |
| 9       | `presence_list`  | §5.3.9    | v1 core (multi-device) |
| 10      | `chat`           | §5.3.10   | v1 core (multi-device) |
| 11–23   | *(reserved)*     | —         | v1.x additions; see §5.1 |

### 3.1 Pre-allocated v2 widgets (§5.3.11)

Widgets listed in `SPEC.md` §5.3.11 as "reserved for post-v1" are pre-assigned
here so v2 implementations land without renegotiating tag numbers. These sit
in the 2-byte range (24–63).

| Numeric | String                   | Notes                             |
|--------:|--------------------------|-----------------------------------|
| 24      | `chart`                  | sparkline                         |
| 25      | `progress`               | determinate / indeterminate bar   |
| 26      | `qr`                     | QR code rendering                 |
| 27      | `audio`                  | inline tone / clip trigger        |
| 28      | `picker`                 | segmented choice                  |
| 29      | `keyboard_passthrough`   | raw key events (games)            |
| 30–39   | *(reserved)*             | v2 additions, batch 2             |

### 3.2 Spec gates

Adding a new widget to §3 or §3.1 requires:

1. A `SPEC.md` change defining the widget map shape (CEO approval).
2. A bump to `protocol/spec.md` (PROTO-002).
3. A corresponding test vector in `protocol/test-vectors/` (PROTO-003).
4. An entry in this registry, in the same commit, taking the lowest free
   number in the appropriate range.

---

## 4. Event tag registry

Event tags live in a separate namespace from widget tags. The same integer
value may identify both a widget and an event with no ambiguity, because
widget tags only appear in widget `t` fields and event tags only appear in
`subscribe` / `subscribe_groups` arrays and event payloads.

Defined events occupy the **single-byte CBOR range (0–23)**.

| Numeric | String                 | Spec      | Direction       | Notes                              |
|--------:|------------------------|-----------|-----------------|------------------------------------|
| 0       | *(reserved)*           | —         | —               | Invalid / sentinel.                |
| **Built-in device events (§5.4.1)** ||||
| 1       | `nfc_scan`             | §5.4.1    | device → app    |                                    |
| 2       | `location`             | §5.4.1    | device → app    |                                    |
| 3       | `back`                 | §5.4.1    | device → app    |                                    |
| 4       | `tick`                 | §5.4.1    | device → app    |                                    |
| 5       | `notification_action`  | §5.4.1    | device → app    |                                    |
| 6–15    | *(reserved)*           | —         | —               | v1.x device-emitted events         |
| **Group events (§5.4.2)** ||||
| 16      | `member_joined`        | §5.4.2    | server → device |                                    |
| 17      | `member_left`          | §5.4.2    | server → device |                                    |
| 18      | `presence_update`      | §5.4.2    | server → device |                                    |
| 19      | `group_message`        | §5.4.2    | server → device |                                    |
| 20–23   | *(reserved)*           | —         | —               | v1.x group events                  |

### 4.1 Spec gates

Adding a new event mirrors §3.2: spec change, `protocol/spec.md` update,
test vectors, registry entry — same commit, lowest free number in the
appropriate band.

---

## 5. Reserved ranges

These ranges are reserved at v1.0 and **MUST NOT** be allocated without
going through the spec gate (§3.2 / §4.1) or the on-device runtime gate
(§5.3).

### 5.1 Widget tag ranges

| Range       | Reserved for                                            |
|-------------|----------------------------------------------------------|
| 0           | Invalid / sentinel                                       |
| 1 – 10      | **Allocated** — v1 core widgets (§3)                     |
| 11 – 23     | v1.x widget additions, single-byte slots (12 free)       |
| 24 – 29     | **Allocated** — pre-assigned v2 widgets (§3.1)           |
| 30 – 39     | v2 widget additions, batch 2                             |
| 40 – 63     | v3+ widget headroom                                      |
| **64 – 127**| **On-device runtime (§16.4).** Reserved exclusively for the v2 sandboxed-app runtime to define new widget primitives without colliding with server-driven additions. |
| 128 – 255   | **Vendor / experimental.** Producers using this range MUST NOT publish to the Marketplace; consumers MUST render as `[unsupported: <tag>]`. Intended for in-house forks and local research. |
| 256 +       | Unallocated. Future expansion only after the 1- and 2-byte spaces are exhausted. |

### 5.2 Event tag ranges

| Range       | Reserved for                                                  |
|-------------|---------------------------------------------------------------|
| 0           | Invalid / sentinel                                            |
| 1 – 5       | **Allocated** — v1 built-in device events (§4)                |
| 6 – 15      | v1.x device-emitted event additions                           |
| 16 – 19     | **Allocated** — v1 group events (§4)                          |
| 20 – 23     | v1.x group event additions                                    |
| 24 – 63     | v2 event additions (any direction)                            |
| **64 – 127**| **On-device runtime (§16.4).** Reserved for runtime-emitted events that have no v1 device-firmware analogue. |
| 128 – 255   | **Vendor / experimental.** Same constraints as §5.1.          |
| 256 +       | Unallocated.                                                  |

### 5.3 On-device runtime gate (§16.4)

`SPEC.md` §16.4 keeps "widget tag space and event types unused so a future
runtime can be slotted in without breaking v1 apps." The 64–127 range in both
tables is the concrete reservation that satisfies that promise. v1
implementations:

- **MUST** decode tags in 64–127 as syntactically valid CBOR.
- **MUST** treat them as unknown widgets/events (graceful fallback per §2.2).
- **MUST NOT** emit them from any v1 server SDK or v1 firmware path.

When v2 introduces an on-device runtime, allocations in 64–127 are made by
the v2 spec, not by amending this v1 document.

---

## 6. Versioning

This registry is versioned in lockstep with `SPEC.md`:

- v1.0 — this document. Matches `SPEC.md` v0.2.
- Subsequent v1.x adds allocate within the v1.x reserved bands only.
- v2.0 — a separate document; may renumber only the v2-reserved ranges
  (§5.1 rows from 40 onward, §5.2 rows from 24 onward), never the
  v1 allocations.

A change to this registry is itself a spec change and follows the
authorization rules in `CLAUDE.md` (CEO approval to modify `SPEC.md`;
this registry is owned by the PROTO subsystem and editable by the
PROTO engineer, but allocations must match a `SPEC.md` change).

---

## 7. Quick reference

For implementers in a hurry:

```
WIDGETS                       EVENTS
  1  text                       1  nfc_scan
  2  list                       2  location
  3  input                      3  back
  4  form                       4  tick
  5  button                     5  notification_action
  6  image                     16  member_joined
  7  map                       17  member_left
  8  notification              18  presence_update
  9  presence_list             19  group_message
 10  chat
 24  chart        (v2)         RANGES
 25  progress     (v2)          0       reserved (invalid)
 26  qr           (v2)          64–127  on-device runtime
 27  audio        (v2)          128–255 vendor / experimental
 28  picker       (v2)
 29  keyboard_passthrough (v2)
```

*End of v1.0 registry.*
