# PagerOS UI Protocol — v1.0

> **Status:** v1.0 (PROTO-002)
> **Authority:** This document is the **single source of truth** for the
> PagerOS UI protocol. It is the contract between App Servers (any SDK),
> the device firmware renderer, the simulator, and the conformance test
> runner. SDK authors implementing PagerOS targets MUST read this end to
> end.
> **Companion documents:**
> - `protocol/tag-registry.md` — canonical numeric/string tags for widgets and events (PROTO-001).
> - `protocol/test-vectors/` — language-agnostic input/expected pairs (PROTO-003).
> - `SPEC.md` §5 — original prose; this file refines and extracts it. On any disagreement, **this file wins** for protocol matters; `SPEC.md` wins for system-wide context.

---

## 1. Scope

This spec defines the **on-the-wire UI protocol** between an App Server
and a PagerOS-compatible client (T-LoRa Pager firmware, simulator,
or future hardware). It covers:

- The **Frame** — the CBOR object the device renders.
- The **widget catalog** — every visual primitive available in v1.
- The **event model** — how the device sends user input and ambient
  signals back to the app, plus how the app pushes incremental updates.
- The **request envelope** — HTTP method, path conventions, headers,
  body encoding.
- The **response envelope** — successful responses, error responses,
  status codes.
- **Caching, versioning, and forward compatibility.**

It does **not** cover:

- Lower-layer transport (TLS, LoRa link format, Push Relay packet
  layout) — see `SPEC.md` §6.
- Identity / signing primitives — see `SPEC.md` §9.
- Marketplace manifest schema — see `SPEC.md` §10.

The protocol assumes the transport hands the SDK a complete, decrypted
request body and accepts an opaque response body. Everything in this
document is transport-agnostic above the LoRa inner envelope
(`SPEC.md` §6.2.2) and HTTPS body.

---

## 2. Design principles (normative)

1. **Server-authoritative.** The server owns app state. The device
   renders what it is told. No client-side state machines.
2. **Tiny on the wire.** A typical Frame MUST fit in a single LoRa
   packet (≤ 200 B encoded payload — `SPEC.md` §14).
3. **No client-side logic.** The DSL has no expressions, conditionals,
   loops, or computed bindings at runtime. All dynamism happens server-side.
4. **Forward-compatible.** Unknown widgets and unknown fields are
   silently ignored or rendered as `[unsupported: <tag>]` placeholders
   (§4 unknown-widget rule).
5. **Bounded latency.** A Frame received by the device MUST be
   renderable in < 100 ms on target hardware (`SPEC.md` §14).

These principles bind every other rule in this document. When a future
SDK or runtime question is ambiguous, fall back to these.

---

## 3. Wire format

### 3.1 CBOR

Frames and request/response bodies are **CBOR** ([RFC 8949](https://www.rfc-editor.org/rfc/rfc8949)).

- Maps are unordered; consumers MUST NOT depend on field order.
- Strings are UTF-8 (CBOR major type 3).
- Integers SHOULD use the smallest CBOR encoding that fits the value
  (CBOR is canonical-encoding agnostic, but smaller is preferred for
  LoRa).
- Floats are permitted but discouraged in widgets; prefer integers
  where the unit is implicit (e.g., `ttl` in seconds).
- `null` is permitted; treat it as field-absent.
- Indefinite-length arrays/maps are permitted on decode; producers
  SHOULD use definite-length forms.

No RFC 8949 semantic tags are used in v1 (`protocol/tag-registry.md` §2.4).

### 3.2 Wi-Fi captures (debug)

For human-readable captures, an equivalent JSON form is acceptable in
documentation and logs. The wire format itself is always CBOR.

---

## 4. The Frame

A **Frame** is a CBOR map representing one screen of UI. The
device-side renderer consumes Frames; the App Server produces them.

### 4.1 Top-level shape

```cbor
{
  "v":      1,                          ; uint, required — protocol major version
  "id":     "scr_a3f9",                 ; tstr, required — server-assigned screen id (cache key)
  "ttl":    60,                         ; uint, optional — seconds the device may cache (default 0 = no cache)
  "title":  "Notes",                    ; tstr, optional — top-bar title
  "body":   [ <widget>, … ],            ; array, required — ordered widgets, rendered top to bottom
  "actions": [ <action>, … ],           ; array, optional — top-bar / soft-key actions
  "subscribe":        [ <event-tag>, … ],          ; array, optional — built-in events (§6.1)
  "subscribe_groups": [ "grp_…", … ],   ; array, optional — group ids (§6.2)
  "meta":   { … }                       ; map,  optional — SDK-defined hints (debugging, telemetry)
}
```

#### 4.1.1 Field rules

| Field | Required | Type | Notes |
|---|---|---|---|
| `v` | yes | uint | MUST be `1` for v1.x. Devices MUST reject unknown major versions with a graceful error frame (§7.4). |
| `id` | yes | tstr | Server-stable per (app, screen). Used as cache key. Devices MUST treat unequal ids as distinct screens even if content matches. |
| `ttl` | no | uint | Seconds. `0` (or absent) disables caching (§8). |
| `title` | no | tstr | Top bar text. Truncated by the renderer to fit the bar. |
| `body` | yes | array of widget | Empty array is valid; renders a blank screen. |
| `actions` | no | array of action | See §5.12. |
| `subscribe` | no | array | Mixed numeric / string event tags accepted (§6.1, registry). |
| `subscribe_groups` | no | array of tstr | Group identifiers; opaque to the protocol. See §6.2.3 for replacement semantics and the 8-group SHOULD cap. |
| `meta` | no | map | Renderers MUST ignore unknown keys here. Reserved for SDK→server round-tripping (e.g., correlation ids). |

Unknown top-level keys MUST be silently ignored (forward compat).

### 4.2 Unknown-widget rule

If the renderer encounters a widget whose `t` (or numeric tag) is not in
`protocol/tag-registry.md`, it MUST render the placeholder line
`[unsupported: <tag>]` in place of that widget and continue rendering
the rest of `body`. The Frame is not invalid; this is the forward-compat
mechanism.

### 4.3 Size budget

A Frame targeted at LoRa transport SHOULD encode to ≤ 200 B (CBOR,
inner envelope payload). The SDK is responsible for surfacing a warning
when a Frame intended for LoRa exceeds this; see the Python SDK
PY-011 warning hook. Frames > 200 B encode and transmit correctly via
LoRa fragmentation (`SPEC.md` §6.2.3) but cost extra round trips.

---

## 5. Widget catalog (v1)

Every widget is a CBOR map with a `t` field that holds either the
canonical string tag or the canonical numeric tag from
`protocol/tag-registry.md` §3. Both forms are equivalent; producers
SHOULD pick one form per Frame for cleaner captures.

For each widget below, only the fields it defines are listed. Unknown
fields MUST be silently ignored. Required fields are marked **required**.

### 5.1 `text` (tag 1)

Renders a paragraph of text.

```cbor
{ "t": "text", "s": "Hello, world", "style": "body" }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `s` | tstr | yes | Display text. UTF-8. |
| `style` | enum | no | `body` (default) \| `heading` \| `dim` \| `mono`. Unknown values render as `body`. |

### 5.2 `list` (tag 2)

A scrollable, encoder-navigable list. ENTER on a highlighted item
triggers its `href`.

```cbor
{
  "t": "list",
  "items": [
    { "label": "Item 1", "href": "/item/1", "sub": "extra info" },
    { "label": "Item 2", "href": "/item/2" }
  ]
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `items` | array of map | yes | At least one item recommended. Empty list renders a single dim "empty" placeholder. |

Each `items[i]`:

| Field | Type | Required | Notes |
|---|---|---|---|
| `label` | tstr | yes | Primary line. |
| `href` | tstr | yes | Target URL (relative to app base or absolute). Activated via ENTER. |
| `method` | enum | no | `GET` (default) \| `POST`. |
| `sub` | tstr | no | Secondary dim line, single line truncated. |

### 5.3 `input` (tag 3)

A single field. Used inside a `form`; standalone use is permitted but
rare (typically as a quick "ask a value" screen with a default Submit
action).

```cbor
{
  "t": "input",
  "name": "title",
  "label": "Title",
  "type": "text",
  "value": "",
  "max": 80
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | tstr | yes | Form field name; key in submitted body. |
| `label` | tstr | yes | Visible label. |
| `type` | enum | no | `text` (default) \| `password` \| `number` \| `email`. Renderer adapts keyboard hints. |
| `value` | tstr | no | Initial / default value. |
| `max` | uint | no | Max character length (renderer-enforced; servers MUST also validate). |

### 5.4 `form` (tag 4)

A submittable group of fields.

```cbor
{
  "t": "form",
  "action": "/save",
  "method": "POST",
  "fields": [
    { "t": "input", "name": "title", "label": "Title" },
    { "t": "input", "name": "body",  "label": "Body" }
  ],
  "submit": "Save"
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `action` | tstr | yes | Submission URL. |
| `method` | enum | no | `POST` (default) \| `GET` (form fields encoded as CBOR body either way; see §7.2). |
| `fields` | array of widget | yes | Each entry is itself a widget map, usually `input`; nesting non-input widgets is allowed (e.g., a `text` heading between fields). |
| `submit` | tstr | no | Submit button label. Defaults to "Submit". |

On submit, the device sends a request to `action` with a CBOR body
whose keys are the `name` fields of all `input` children with values
filled in by the user (§7.2.1).

### 5.5 `button` (tag 5)

A standalone activatable action.

```cbor
{
  "t": "button",
  "label": "Delete",
  "href": "/delete/42",
  "method": "POST",
  "confirm": "Are you sure?"
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `label` | tstr | yes | Button text. |
| `href` | tstr | yes | Target URL. |
| `method` | enum | no | `GET` (default) \| `POST`. |
| `confirm` | tstr | no | If present, renderer MUST show a Yes/No dialog with this prompt before issuing the request. |

### 5.6 `image` (tag 6)

A content-addressed bitmap.

```cbor
{ "t": "image", "src": "img:7f3a…", "w": 96, "h": 96, "alt": "logo" }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `src` | tstr | yes | `img:<sha256-hex>` — full SHA-256 of the image bytes. |
| `w` | uint | no | Display width. Renderer scales/crops as needed. |
| `h` | uint | no | Display height. |
| `alt` | tstr | no | Accessibility text; also used in render-failure fallback line. |

Fetch contract: on first encounter, device issues
`GET <app-base>/img/<full-sha256>` and caches by hash forever
(`SPEC.md` §5.6). Max dimensions 480 × 222, max body 8 KB, PNG required
(JPEG optional).

### 5.7 `map` (tag 7)

A native map view.

```cbor
{ "t": "map", "lat": 45.5, "lon": -73.6, "zoom": 14, "markers": [ … ] }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `lat` | float | yes | Center latitude. |
| `lon` | float | yes | Center longitude. |
| `zoom` | uint | no | Tile zoom level (1–18). Default 14. |
| `markers` | array of map | no | Each marker: `{ "lat": …, "lon": …, "label": tstr? }`. |

Renderer uses OSM tile data (`SPEC.md` §5.3.7). If GPS is permitted
and a fix is present, the device-side renderer MAY auto-inject the
current location as a marker; the App Server MUST NOT depend on this.

### 5.8 `notification` (tag 8)

A top-of-screen flash for transient messages.

```cbor
{ "t": "notification", "level": "info", "s": "Saved." }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `s` | tstr | yes | Text. |
| `level` | enum | no | `info` (default) \| `warn` \| `error`. Affects color/icon. |

`notification` MAY appear at any position in `body`; the renderer
displays it as an overlay band and auto-dismisses after a short
timeout. It is **not** state-bearing; subsequent Frames omit it.

### 5.9 `presence_list` (tag 9, multi-device)

```cbor
{
  "t": "presence_list",
  "group_id": "grp_abc",
  "members": [
    { "id": "<pubkey>", "name": "alice", "online": true },
    { "id": "<pubkey>", "name": "bob",   "online": false }
  ]
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `group_id` | tstr | yes | The group whose membership is shown. |
| `members` | array of map | yes | Each: `id` (tstr, required), `name` (tstr, optional), `online` (bool, optional default `false`). |

Updates: incremental via `presence_update` events (§6.2) — the
renderer MUST mutate the cached widget in place without a full Frame
refresh.

### 5.10 `chat` (tag 10, multi-device)

```cbor
{
  "t": "chat",
  "group_id": "grp_abc",
  "messages": [
    { "from": "alice", "ts": 1716200000, "s": "hi" },
    { "from": "bob",   "ts": 1716200012, "s": "yo" }
  ],
  "compose": { "name": "msg", "submit": "/send" }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `group_id` | tstr | yes | Group identifier. |
| `messages` | array of map | yes | Each: `from` (tstr), `ts` (uint epoch seconds), `s` (tstr). May be empty. |
| `compose` | map | no | If present, the renderer shows an inline composer. `name` is the body key sent on submit; `submit` is the POST target. |

Updates: incremental via `group_message` events (§6.2) — appended to
`messages` in cache, no full Frame refresh.

### 5.11 Future widgets

Tags 24–29 are pre-allocated for the v2 widget batch
(`chart`, `progress`, `qr`, `audio`, `picker`, `keyboard_passthrough`
— `protocol/tag-registry.md` §3.1). v1 servers MUST NOT emit them;
v1 renderers MUST treat them as unknown (§4.2).

### 5.12 Top-bar actions

`Frame.actions` is an array of soft-key entries:

```cbor
{ "label": "New", "key": "n", "href": "/new", "method": "GET" }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `label` | tstr | yes | Display text. |
| `key` | tstr | no | Single-character hint shown in the top bar; pressing this key activates the action. |
| `href` | tstr | yes | Target URL. |
| `method` | enum | no | `GET` (default) \| `POST`. |

Actions are not widgets; they do not appear in the tag registry. They
are reserved as a top-level Frame concept to keep the body widget
list focused on the scroll region.

---

## 6. Event model

Events flow in two directions:

- **Device → App.** User activations (any widget with `href`) and
  built-in ambient events (`nfc_scan`, `location`, `back`, `tick`,
  `notification_action`).
- **App → Device.** Group events that mutate already-rendered widgets
  without a full Frame refresh (`member_joined`, `member_left`,
  `presence_update`, `group_message`).

Event identifiers (string and numeric) live in
`protocol/tag-registry.md` §4.

### 6.1 Built-in (device-emitted) events

The device emits these only to the **currently foregrounded app**, and
only after the app has subscribed via `Frame.subscribe` containing the
event tag (string or numeric).

| Event (string \| numeric) | Trigger | Payload |
|---|---|---|
| `nfc_scan` \| 1 | NFC tag detected | `{ "uid": <bstr>, "records": [ … ] }` |
| `location` \| 2 | GPS fix updated | `{ "lat": <float>, "lon": <float>, "accuracy": <float-meters> }` |
| `back` \| 3 | BACK key | `{}` (empty) |
| `tick` \| 4 | Periodic timer (configured by app via `meta.tick_seconds`) | `{}` |
| `notification_action` \| 5 | User taps a delivered notification | `{ "id": <tstr> }` |

Delivery is a regular request (§7) to the app's `/event` endpoint
(by convention; the app MAY specify a different path via
`meta.event_url`). The request body is:

```cbor
{
  "type": "nfc_scan",   ; tstr or uint (matches subscribed form)
  "payload": { … }      ; per the table above
}
```

The response is a Frame (treated as a normal screen response).
A response body of CBOR `null` or HTTP 204 means "no UI change."

### 6.2 Group events (multi-device)

Apps that opt in via the manifest's `multi_device: true` flag
(`SPEC.md` §10.2) can send events from the App Server to the device
that mutate already-rendered widgets without a full Frame refetch.
This section is normative for the four v1 group events
(`member_joined`, `member_left`, `presence_update`, `group_message`)
and the `subscribe_groups` Frame field that scopes their delivery.

Group state (membership lists, history) is owned by the App Server.
The device only renders; it does not maintain authoritative group
state and never sources a group event itself (§6.2.6).

#### 6.2.1 `group_id`

A **group id** (`group_id`) is a tstr chosen by the App Server. It is
opaque to the protocol: the device, renderer, SDKs, and Push Relay
treat it as an unanalyzed key. Producers SHOULD keep it ≤ 64 UTF-8
codepoints; longer values encode and route correctly but bloat cache
fingerprints and LoRa frames.

`group_id` appears in exactly three contexts in the UI protocol:

1. **Inside a multi-device widget map** — `presence_list.group_id`
   (§5.9) and `chat.group_id` (§5.10). This binds a rendered widget
   instance to the group whose state it mirrors. A Screen MAY contain
   multiple `presence_list` / `chat` widgets bound to different group
   ids; the renderer applies an inbound event to every widget whose
   `group_id` matches.
2. **Inside a group event payload** (§6.2.4) — every group event
   carries `group_id` as its first routing key, used to locate the
   matching widget(s) in the cached Screen.
3. **Inside `Frame.subscribe_groups`** (§6.2.3) — declares the set of
   group ids the device wants events for while the Screen is
   foregrounded.

A device receiving a group event whose `group_id` is not present in
both the most recently rendered `Frame.subscribe_groups` array and at
least one cached `presence_list` / `chat` widget MUST drop the event
silently — no error to the App Server, no log surfaced to the user.
This makes mis-routing on the App Server side observably benign at
the renderer.

#### 6.2.2 Spec gate for new group events

Adding a new group event tag follows the same gate as §6.1's built-in
events (tag-registry §4.1): a `SPEC.md` change defining the payload
shape, an entry in this section's table (§6.2.4), an allocation in
`protocol/tag-registry.md` §4 within the 20–23 reserved band, and a
matching pair of test vectors under `protocol/test-vectors/ui/`.

#### 6.2.3 `Frame.subscribe_groups`

`Frame.subscribe_groups` is the **complete and authoritative** set of
group subscriptions for the rendered Screen. The device:

- MUST replace any prior subscription set with this Frame's value on
  every Frame render — subscriptions do not accumulate across Frames.
- MUST treat absence and an empty array as equivalent (no group
  subscriptions for this Screen). Encoders MAY omit an empty array.
- MUST silently ignore non-tstr entries (forward-compat with future
  subscription forms).
- SHOULD cap the active subscription set at 8 group ids per Screen;
  entries beyond the eighth MAY be dropped. (Chat and presence widgets
  typically reference a single group; the cap is implementation
  conservatism, not a wire limit.)

Unlike `subscribe` (§6.1), `subscribe_groups` has no numeric-tag form:
its entries are opaque App-Server-defined identifiers (§6.2.1), not
registry tags. The two arrays are independent namespaces; a Frame MAY
populate both.

#### 6.2.4 Group event envelope

Every server→device group event is a single top-level CBOR map with
the same two-key shape as a §6.1 device→app event:

```cbor
{
  "type":    <event-tag>,           ; tstr or uint (registry §4)
  "payload": { "group_id": <tstr>,
               … event-specific … }
}
```

`type` and `payload` are the only top-level keys. Producers MAY use
the string or numeric form of `type`; consumers MUST accept both
(registry §2.2). Every v1 group event payload MUST carry `group_id`
as a tstr; the remaining keys are event-specific.

| Event (string \| numeric) | Payload (other than `group_id`) | Renderer effect |
|---|---|---|
| `member_joined` \| 16 | `member: { id: tstr, name: tstr? }` | Append `member` to every cached `presence_list.members` bound to `group_id`, unless the same `id` is already present. |
| `member_left` \| 17 | `member_id: tstr` | Remove the matching `id` from every cached `presence_list.members` bound to `group_id`. No-op if absent. |
| `presence_update` \| 18 | `members: [ { id: tstr, online: bool }, … ]` | For every cached `presence_list` bound to `group_id`, replace the `online` flag on each matching member. Members not yet known MAY be appended with the given `online` flag and no `name`. |
| `group_message` \| 19 | `from: tstr, ts: uint, s: tstr` | Append `{ from, ts, s }` to every cached `chat.messages` bound to `group_id`. `ts` is epoch-seconds. |

Unknown payload keys MUST be silently ignored (§9.2 forward-compat).
An envelope missing `type`, missing `payload`, or missing a required
payload field for the resolved event type MUST be dropped without
mutating any widget.

The cached Screen is mutated in place; no full Frame refetch is
triggered solely by a group event.

#### 6.2.5 Delivery channels

A single logical event is delivered to a device over exactly one
channel per delivery attempt, chosen by the App Server:

- **Wi-Fi, app foregrounded:** the SDK exposes `GET /events?since=<cursor>`
  as a long-poll endpoint. The response body is a CBOR array of group
  event envelopes (§6.2.4); the device applies each in order and
  reissues the poll with the latest cursor. The cursor is opaque to
  the device — only the App Server interprets it. The endpoint returns
  HTTP 204 with an empty body when the long-poll times out with no
  pending events.
- **Anything else** (LoRa-only, Wi-Fi backgrounded, app not running):
  the App Server SHOULD enqueue the event onto the Push Relay
  (`SPEC.md` §6.6) as a `kind: "group_event"` envelope. The
  X25519+ChaCha20-Poly1305-encrypted inner payload (relay §6.6.3) is
  exactly the §6.2.4 CBOR map. The device drains the queue on every
  wake/poll and applies events in the relay's enqueue-time order.

Both channels are **at-least-once**. Renderer effects under §6.2.4 are
idempotent (joining a member twice is a no-op; replacing the same
`online` flag is a no-op). Chat consumers SHOULD dedupe messages by
`(from, ts, s)` when an at-least-once delivery resurfaces a message
already in the cached Screen.

#### 6.2.6 Devices do not emit group events

In v1, group events flow server→device only. A device has no
device→server group-event path. User-originated state changes happen
via normal request envelopes (§7) — for example, posting a `chat`
composer submission to the app's `/send` endpoint, which the App
Server then fans out as a `group_message` event to other group
members.

### 6.3 Unknown events

Unknown event tags in a `subscribe` or `subscribe_groups` array MUST
be silently ignored by the device. This lets newer apps target older
firmware without breaking subscription setup. Inbound server→device
events with unknown `type` are dropped.

---

## 7. Request / response envelope

The protocol layers a thin set of conventions on top of HTTP/1.1
(over Wi-Fi) or the LoRa inner envelope (`SPEC.md` §6.2.2). SDK
authors see a uniform shape regardless of transport.

### 7.1 Methods and URLs

- Only `GET` and `POST` are used in v1.
- Activation of any widget's `href` issues a request to that URL.
- URLs MAY be absolute (`https://notes.app/save`) or relative to the
  app's base URL declared in the Marketplace manifest. Relative URLs
  resolve against the app base, never against the previous screen's
  URL (no `../` semantics in the device).

### 7.2 Request body

#### 7.2.1 From a `form`

```cbor
{
  "title": "Groceries",
  "body":  "Milk, eggs"
}
```

Keys are the `name` fields of each `input` child. Values are strings
for `text`/`password`/`email`, an integer for `number`. Missing
optional inputs are omitted, not sent as `null`.

#### 7.2.2 From a `button` or `list` item

The request body is CBOR `null` (or omitted — see §7.5). The
information the server needs is encoded in the URL path.

#### 7.2.3 From a built-in event

See §6.1: a CBOR map with `type` and `payload`.

### 7.3 Headers (Wi-Fi path)

The device sends these headers on every Wi-Fi HTTPS request:

| Header | Value | Required | Notes |
|---|---|---|---|
| `Accept` | `application/cbor; pagerOS=1` | yes | Advertises supported protocol versions; comma-separate for multi-version (`pagerOS=1,pagerOS=2`). |
| `Content-Type` | `application/cbor` | for requests with a body | Body is CBOR. |
| `PagerOS-Device` | base64 device Ed25519 pubkey | yes | Stable device identity (`SPEC.md` §9.1). |
| `PagerOS-Sig` | base64 Ed25519 signature | yes | Signature over `method || url || timestamp || sha256(body)` (`SPEC.md` §9.2). |
| `PagerOS-Timestamp` | unix-seconds | yes | Used by server for replay window (±5 min, `SPEC.md` §9.2). |
| `PagerOS-Session` | opaque token | no | Server-issued session token (cookie equivalent); device echoes back if present. |
| `User-Agent` | `PagerOS/<fw-version>` | yes | For server-side telemetry. |

#### 7.3.1 LoRa path

On the LoRa path, these headers travel as fields of the inner
envelope (`SPEC.md` §6.2.2). The mapping is:

- `to` ↔ method + URL (string `"<METHOD> <url>"`).
- `from` ↔ `PagerOS-Device`.
- `sig` ↔ `PagerOS-Sig`.
- `nonce` ↔ replaces `PagerOS-Timestamp` for replay protection
  (the timestamp is included in the signature input regardless).
- `body` ↔ CBOR request body bytes.

Servers receiving via Exit Node see the rehydrated HTTP request and
SHOULD NOT distinguish transport for application logic.

### 7.4 Response

A successful response is a Frame (§4), encoded as CBOR.

| Status | Meaning |
|---|---|
| 200 | Frame body in response. Device renders it. |
| 204 | No content. Device keeps the current Frame; useful for fire-and-forget events. |
| 301 / 302 / 303 / 307 / 308 | Redirect to the `Location` header URL. Device follows automatically for `GET`; for non-`GET`, follows only on 307/308 and only within the same app host. |
| 400 | Bad request body. Device renders an inline error notification (`level: error`) with `"Bad request"`. |
| 401 | Unauthorized / signature invalid. Device drops session token, retries once with a fresh signature; second 401 surfaces as `"Sign-in required"` error frame. |
| 403 | Forbidden. Device renders an error notification with the response body's `s` field if it is a `text` widget, else `"Not allowed"`. |
| 404 | Not found. Device renders `"Not found"`. |
| 410 | Gone. Device evicts cached Screens for this `(app, screen_id)` and shows `"Removed"`. |
| 429 | Rate limited. Device backs off per `Retry-After`, shows transient notification. |
| 5xx | Server error. Device shows `"Server error"` with retry option. |

For 4xx / 5xx, the response body MAY be a single-widget Frame
(`body: [ { "t": "text", "s": "Quota exceeded." } ]`) — if so, the
device prefers that body's text over the generic message above.

#### 7.4.1 Error frame shape

When the device generates an error frame locally (no usable server
response, e.g., DNS failure, unknown protocol version), it constructs:

```cbor
{
  "v":     1,
  "id":    "err_local",
  "ttl":   0,
  "title": "Error",
  "body":  [ { "t": "text", "s": "<message>", "style": "error" } ]
}
```

(`style: "error"` is renderer-only; servers MUST NOT emit it.)

### 7.5 Empty bodies

A request with no body sends `Content-Length: 0` (no `Content-Type`
header required). A response with no body MUST use 204.

### 7.6 Idempotency

`GET` requests MUST be idempotent and side-effect free
(servers SHOULD treat them as cacheable per `Cache-Control`).
`POST` requests are not assumed idempotent; the device does not
retry a `POST` automatically after a successful TCP send, even on a
later read failure. The retry / timeout table in `SPEC.md` §6.4
applies to connection-establishment failures only.

---

## 8. Caching

- The device maintains a Screen cache keyed by `(app_id, screen.id)`.
- On opening an app, the device immediately renders the last cached
  Frame for `/` (if any), then fires the refresh request — "instant
  open" UX.
- `ttl` (Frame field, §4.1) controls how stale a cached Screen may be
  served when re-entered. `ttl: 0` (or absent) disables caching.
- `ttl` is **not** a TTL on the request itself; it is a TTL on the
  cached render. The device MUST always issue a refresh in the
  background when re-opening, even if the cached Frame is still
  within `ttl`.
- For widgets that receive incremental updates (`chat`,
  `presence_list`), the device updates the cached Screen in place
  instead of discarding it (§6.2). On re-fetch, the server's Frame
  takes precedence over the locally accumulated state.
- Image bytes are cached separately, keyed by their full SHA-256, and
  are immutable (content-addressed). Image cache is never invalidated
  by Frame `ttl`.

---

## 9. Versioning & forward compatibility

### 9.1 Major version

The `v` field is the protocol **major** version. v1.x devices accept
any Frame with `v == 1`. v2 devices MAY accept `v == 1` or `v == 2`
based on the `Accept` header negotiation.

### 9.2 Unknown fields

Renderers MUST silently ignore unknown top-level Frame fields, unknown
widget fields, and unknown action fields. This is the primary
forward-compat mechanism for minor revisions.

### 9.3 Unknown widget / event tags

See §4.2 (unknown widgets) and §6.3 (unknown events). Both render-time
unknowns are non-fatal.

### 9.4 Server feature detection

Servers SHOULD inspect the `Accept` header to decide which protocol
features to use. A server MUST NOT emit v2 widgets (tags 24–29) to a
device whose `Accept` does not include `pagerOS=2`.

---

## 10. Quick reference for SDK authors

A minimum-viable PagerOS SDK MUST:

1. Encode and decode CBOR per §3.
2. Build Frames matching §4 with at least the v1 widget set (§5.1–5.10)
   and `actions` (§5.12).
3. Accept POST request bodies in the form of §7.2.1, dispatching them
   to handler functions keyed by URL.
4. Set the response headers in §7.3 on returned Frames (servers receive
   the request headers; they only need to set `Content-Type` and any
   custom session headers on responses).
5. Implement signature verification per `SPEC.md` §9.2.
6. Surface a warning when a Frame intended for LoRa transport encodes
   > 200 B (recommend, not MUST).
7. Pass all PROTO-003 test vectors (when available) for the widgets
   the SDK supports.

An SDK that adds extra ergonomics (Pythonic decorators, JSX-ish DSL,
etc.) MUST still emit Frames that conform to this document at the
wire level. Pre-publication, run the PROTO-005 conformance runner.

---

## 11. Open issues against this document

These are tracked outside the spec — log them as PROTO subsystem
issues — but listed here as a courtesy to readers:

- Whether `tick` ambient events should use a coarser server-driven
  schedule instead of the current "app declares `meta.tick_seconds`"
  hint (deferred to PROTO-004 work).
- Whether `form` submissions over LoRa should chunk on field
  boundaries to keep error messages localized (currently the whole
  form fails as a unit).

These do not block v1; v1.0 SDKs MUST be built against this document
as-is.

---

*End of v1.0 protocol spec.*
