# PagerOS — Technical Specification

> **Status:** v0.2 draft
> **Target hardware:** LILYGO T-LoRa Pager (ESP32-S3)
> **Audience:** Project contributors, app developers, exit-node operators
> **Project name:** PagerOS (confirmed).
> **Change log v0.1 → v0.2:** All 13 open/proposed items resolved. Multi-device app sessions and a project-operated push relay added to scope. Full-Unicode font bundle, OSM tile cap, and no-Meshtastic-interop made explicit.

---

## 1. Vision & Goals

### 1.1 Vision
A pocket-sized device that gives anyone, anywhere, access to a universe of small, useful, single-purpose apps — over the internet when available, over LoRa mesh when not. Apps are hosted by their developers on ordinary servers; the device is a thin, opinionated renderer that talks to them through a tiny, declarative UI protocol. A public marketplace makes every app discoverable from the device's home screen.

### 1.2 Goals (in priority order)
1. **Reach.** A user can interact with any registered app from anywhere a LoRa mesh or Wi-Fi network can reach them.
2. **Low barrier for developers.** Writing an app is closer to writing a web handler than to writing firmware. A "hello world" app is < 20 lines in any supported language.
3. **Snappy UX on slow links.** Common interactions feel instant. Worst-case (multi-hop LoRa) feels usable, not frustrating.
4. **Hardware-honest.** The protocol and OS respect the device's actual constraints (8 MB PSRAM, 480×222 screen, ~250 B/s LoRa, keyboard + encoder input).
5. **Open ecosystem.** Anyone can publish an app. Anyone can run an exit node. Anyone can fork the firmware.

### 1.3 Non-goals (explicit)
- **Not a smartphone.** No browser, no general-purpose runtime, no app store fees.
- **Not Meshtastic.** Users may dual-boot, but the firmwares do not interoperate live (§16.9).
- **No on-device app code execution in v1.** Apps run on servers. Spec keeps the door open for v2 (§16.4).
- **No anonymity guarantees.** The device has a stable identity; we are not building Tor.
- **No multi-user device support in v1.** One pager, one identity. *(Multiple users coordinating through an app's group session is supported — see §5.4.2 — but the device itself is single-user.)*

---

## 2. Glossary

| Term | Definition |
|---|---|
| **Device / Pager** | A LILYGO T-LoRa Pager (or compatible) running PagerOS firmware. |
| **App** | A service hosted on the public internet that conforms to the PagerOS App SDK contract. |
| **App Server** | The HTTP(S) server the app developer runs. |
| **Screen** | A declarative description of one view, returned by an App Server. |
| **Widget** | A primitive UI element inside a Screen (List, Text, Input, etc.). |
| **Event** | A user action (keypress, selection, etc.) or device signal (NFC scan, GPS update, group event) sent from device to App Server, or pushed from server to device. |
| **Frame** | The encoded wire representation of a Screen, transmitted from server to device. |
| **Transport** | The mechanism carrying a request/response: Wi-Fi (direct HTTPS), LoRa (mesh, via Exit Node), or Push Relay. |
| **Exit Node** | A device with both LoRa and internet connectivity that bridges LoRa traffic to HTTP. |
| **Push Relay** | A project-operated service that holds notifications for devices, pulled on wake. |
| **Marketplace / Directory** | The central registry where apps are published and discovered. |
| **Manifest** | The metadata describing an app (name, icon, URL, permissions, multi-device flag, etc.) registered with the Marketplace. |
| **Shell** | The built-in firmware UI: home screen, app launcher, marketplace browser, settings. |
| **Identity** | A device's persistent Ed25519 keypair, generated on first boot. |
| **Session** | An authenticated logical conversation between one device and one app. |
| **Group** | An app-scoped collection of devices that share state for a multi-device session (chat, multiplayer, presence). |

---

## 3. System Overview

### 3.1 Architectural diagram

```
┌─────────────────────┐                  ┌──────────────────────────┐
│  Marketplace (web)  │ ───── lists ───▶ │  Discoverable via Shell  │
│  - registry         │                  └──────────────────────────┘
│  - manifests        │
│  - search           │                            ▲
└─────────────────────┘                            │
        ▲                                          │ HTTP / LoRa / Push
        │ publish                                  │
        │                                          │
┌─────────────────────┐    ┌────────────────────┐  │   ┌────────────────┐
│   App SDK (libs)    │───▶│   App Server       │◀─┴──▶│  Device (Pager)│
│   - Python / JS /   │    │   - returns Frames │      │  - Shell       │
│     Go / Rust       │    │   - handles Events │      │  - Renderer    │
└─────────────────────┘    │   - sends push     │      │  - Transport   │
                           └────────────────────┘      │  - Identity    │
                                    │      ▲           │  - Cache       │
                                    │      │           └────────────────┘
                          POST /push │      │ pull           ▲
                                    ▼      │                │
                           ┌────────────────────┐            │
                           │   Push Relay       │            │
                           │   (project-run)    │            │
                           └────────────────────┘            │
                                    ▲                        │
                                    │                        │
                                    │            ┌───────────────────┐
                                    │            │   Exit Node       │
                                    └────────────│   (Wi-Fi ↔ LoRa)  │
                                                 └───────────────────┘
                                                          ▲
                                                          │ LoRa
                                                          │
                                                  [other pagers]
```

### 3.2 Trust boundaries
- Device ↔ App Server: mutually authenticated (device signs requests; app TLS cert verified). E2E encrypted when going through Exit Nodes or the Push Relay.
- Device ↔ Marketplace: standard TLS. Marketplace knows public manifests, not user activity.
- Device ↔ Push Relay: relay sees envelope (which device, from which app) but never plaintext payload.
- Exit Node: untrusted; sees envelope (device pubkey + destination URL hash) but not payload contents.

### 3.3 Three example flows

**Online — open Notes app, write entry**
```
1. User selects "Notes" from Shell.
2. Device → Wi-Fi → HTTPS GET https://notes.app/        → Frame (list of notes)
3. User selects "New". on_select → POST /new            → Frame (input form)
4. User types text, presses submit. on_submit → POST /save → Frame (list with new entry)
```

**Offline — same flow over LoRa**
```
1. Device packs request as PagerOS envelope, signs with Ed25519, encrypts payload to app pubkey.
2. Sends LoRa packet to nearest Exit Node (multi-hop allowed).
3. Exit Node decapsulates envelope, performs HTTPS to https://notes.app/ on device's behalf.
4. App Server returns Frame (≤ LoRa MTU).
5. Exit Node wraps response, sends back over LoRa to device.
6. Device decrypts, renders.
```

**Push — app sends a notification while device is asleep**
```
1. App Server encrypts message to device pubkey, POSTs to https://push.pageros.org/push/<device_pubkey>.
2. Push Relay verifies app signature, enqueues encrypted blob with TTL.
3. Device wakes (timer, button, LoRa interrupt, Wi-Fi sync window).
4. Device pulls https://push.pageros.org/pull/<device_pubkey> (signed).
5. Decrypts, plays tone, surfaces notification on next interaction.
```

---

## 4. Hardware Constraints

These constraints define the protocol. They are not implementation details.

| Resource | Value | Implication |
|---|---|---|
| MCU | ESP32-S3, dual-core 240 MHz | Plenty for a renderer; not enough for arbitrary app code. |
| Flash | 16 MB | Firmware (~10 MB) + fonts (~1.5 MB) + recovery + OTA staging (~2 MB). Tight. |
| PSRAM | 8 MB | Frame cache, image decoder buffers, font glyph LRU, group session state. |
| Display | 2.33" IPS, 480 × 222, 16-bit color | Limited screen real estate; layouts must be compact. |
| Input | 44-key QWERTY, rotary encoder, ENTER/BACK | Form-driven UX; no touch, no mouse. |
| LoRa | SX1262, ~250 B/s effective, ~256 B max packet | Frames must be tiny. Round trips are expensive. |
| Wi-Fi | 2.4 GHz b/g/n | Direct HTTPS + Push Relay polling. |
| GPS | u-blox MIA-M10Q | First-class location widget + events. |
| NFC | ST25R3916 | First-class NFC scan event. |
| IMU | BHI260AP w/ AI | Motion/gesture events (later). |
| Audio | ES8311 codec + speaker | Notification tones (§7.8). Voice/streaming out of scope. |
| Battery | 500 mAh | OS must aggressively sleep. Target: 24 h idle, 4 h active. |
| Storage | microSD | Frame cache, image cache, settings, offline app data, notification queue. |
| Font assets | ~1.5 MB flash | Bundled Noto Sans subsets (§7.7); enables full-Unicode rendering. |

---

## 5. UI Protocol Specification

### 5.1 Design principles
1. **Server-authoritative.** The server owns app state. The device renders what it's told.
2. **Tiny on the wire.** A typical Frame must fit in a single LoRa packet (≤ 200 B encoded payload).
3. **No client-side logic.** The DSL has no expressions, conditionals, or loops at runtime. All dynamism happens server-side.
4. **Forward-compatible.** Unknown widgets and unknown fields are gracefully ignored.
5. **Renderable in <100 ms** on the target hardware once received.

### 5.2 Wire format
**DECISION:** **CBOR** ([RFC 8949](https://www.rfc-editor.org/rfc/rfc8949)) with a fixed widget tag registry.

Why: smaller than JSON, faster to parse than Protobuf, schema-optional (good for forward-compat), wide language support.

A Frame is a CBOR map:
```cbor
{
  "v":  1,                       // protocol version
  "id": "scr_a3f9",              // server-assigned screen id (for cache key)
  "ttl": 60,                     // seconds the device may cache this screen
  "title": "Notes",              // top bar title (optional)
  "body": [                      // ordered list of widgets
    { "t": "list", "items": [ ... ] }
  ],
  "actions": [                   // top-bar / soft-key actions (optional)
    { "label": "New", "key": "n", "href": "/new" }
  ]
}
```

### 5.3 Widget catalog

Every widget is a CBOR map with a `t` field (type). Unknown types render as a placeholder line "[unsupported: t]".

#### 5.3.1 `text`
```cbor
{ "t": "text", "s": "Hello, world", "style": "body" }
```
- `s` (string, required)
- `style` (enum: `body` | `heading` | `dim` | `mono`; default `body`)

#### 5.3.2 `list`
```cbor
{
  "t": "list",
  "items": [
    { "label": "Item 1", "href": "/item/1", "sub": "extra info" },
    { "label": "Item 2", "href": "/item/2" }
  ]
}
```
- Renders scrollable, encoder-navigable. ENTER triggers `href`.
- `sub` (optional) shows dim secondary text.

#### 5.3.3 `input`
```cbor
{
  "t": "input",
  "name": "title",
  "label": "Title",
  "type": "text",        // text | password | number | email
  "value": "",
  "max": 80
}
```
- Used inside a `form`. Standalone use is allowed but rare.

#### 5.3.4 `form`
```cbor
{
  "t": "form",
  "action": "/save",
  "method": "POST",
  "fields": [
    { "t": "input", "name": "title", "label": "Title" },
    { "t": "input", "name": "body", "label": "Body", "type": "text" }
  ],
  "submit": "Save"
}
```

#### 5.3.5 `button`
```cbor
{ "t": "button", "label": "Delete", "href": "/delete/42", "method": "POST", "confirm": "Are you sure?" }
```

#### 5.3.6 `image`
```cbor
{ "t": "image", "src": "img:7f3a..." , "w": 96, "h": 96, "alt": "logo" }
```
- `src` is **content-addressed**: `img:<sha256-prefix>`. Device fetches once and caches forever.
- Server provides a fetch endpoint (see §5.6).

#### 5.3.7 `map`
```cbor
{ "t": "map", "lat": 45.5, "lon": -73.6, "zoom": 14, "markers": [ ... ] }
```
- Rendered natively using cached tile data.
- **DECISION:** Tiles fetched from OpenStreetMap (`tile.openstreetmap.org`), subject to OSM's [Tile Usage Policy](https://operations.osmfoundation.org/policies/tiles/).
- Per-device tile fetches soft-capped at 1,000 / day to respect upstream. Cached aggressively on SD (§7.4).
- Pins from GPS are auto-injected as a marker.

#### 5.3.8 `notification`
```cbor
{ "t": "notification", "level": "info", "s": "Saved." }
```
- Top-of-screen flash. `level`: `info` | `warn` | `error`.

#### 5.3.9 `presence_list` *(multi-device, see §5.4.2)*
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
- Shows who's currently in a group. Updates via `presence_update` events without a full Frame refresh.

#### 5.3.10 `chat` *(multi-device, see §5.4.2)*
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
- Scrollable message log + inline composer. Server appends new messages by emitting `group_message` events (§5.4.2) — the device's chat widget mutates in place, no full Frame refetch.

#### 5.3.11 Future widgets (reserved for post-v1)
- `chart` (sparkline), `progress`, `qr`, `audio`, `picker` (segmented), `keyboard_passthrough` (raw key events for games).
- Widget tag space deliberately leaves room for these and for the on-device runtime case (§16.4).

### 5.4 Event model

Every interactive widget has an `href` and optional `method` (default `GET`). Activating it sends a request:

```http
POST /save HTTP/1.1
Host: notes.app
PagerOS-Device: <base64 device pubkey>
PagerOS-Sig: <ed25519 signature>
PagerOS-Session: <opaque token>
Content-Type: application/cbor

{ "title": "Groceries", "body": "Milk, eggs" }
```

Response: a new Frame (CBOR).

#### 5.4.1 Built-in (device-emitted) events
The device can spontaneously emit events to the currently-foregrounded app:

| Event | Trigger | Payload |
|---|---|---|
| `nfc_scan` | NFC tag detected | tag UID + record data |
| `location` | GPS fix updated (if app subscribed) | lat, lon, accuracy |
| `back` | BACK key | none |
| `tick` | App requested periodic poll | none |
| `notification_action` | User taps a notification | notification id |

Apps subscribe to these via the Frame:
```cbor
{ ..., "subscribe": ["nfc_scan", "location"] }
```

#### 5.4.2 Group events (multi-device sessions)

For apps that coordinate multiple devices (chat, multiplayer, presence), the protocol defines additional event types. Apps opt in via the manifest's `multi_device: true` flag (§10.2).

| Event | Direction | Payload |
|---|---|---|
| `member_joined` | server → device | `group_id`, member pubkey, member display name |
| `member_left` | server → device | `group_id`, member pubkey |
| `presence_update` | server → device | `group_id`, list of `(member_id, online_bool)` |
| `group_message` | server → device | `group_id`, sender, timestamp, body |

Devices subscribe per-group within a Frame:
```cbor
{ ..., "subscribe_groups": ["grp_abc", "grp_xyz"] }
```

Delivery:
- **Wi-Fi connected:** the SDK's `events()` helper exposes a long-poll endpoint; device holds a connection while foregrounded.
- **Otherwise:** group events ride the Push Relay (§6.6) with `kind: "group_event"` envelope, polled on each wake.

Group state (membership, history) is owned by the app server. The device only renders; it does not maintain authoritative group state.

### 5.5 Caching & TTL
- The device keeps a Screen cache keyed by `(app_id, screen.id)`.
- On opening an app, the device immediately renders the last-seen Screen for `/`, then fires a refresh request in the background. Provides "instant open" UX.
- `ttl` controls how stale a cached Screen may be served. `ttl: 0` disables caching.
- For widgets that receive incremental updates (`chat`, `presence_list`), the device updates the cached Screen in place rather than discarding it.

### 5.6 Image fetching
- Images are content-addressed: device requests `GET /img/<full-sha256>` on the app's host.
- Device caches images on SD card by hash; never re-fetches.
- Max image dimensions: 480 × 222. Max bytes: 8 KB.
- Format: PNG (mandatory) + JPEG (optional). No animation.

### 5.7 Protocol versioning
- `v` field in every Frame.
- Device advertises supported versions in `Accept: application/cbor; pagerOS=1` header.
- Backward compatibility: a newer device MUST render older Frames. Forward compatibility: older devices MUST ignore unknown fields and unknown widget types.

---

## 6. Transport Layer

### 6.1 Wi-Fi path (direct)
- Standard HTTPS/1.1. TLS 1.2+ required.
- Device verifies cert chain against bundled root CA list (Mozilla CA Bundle, updated with firmware).
- Connection reused across requests within a session.

### 6.2 LoRa path (via Exit Node)

#### 6.2.1 LoRa envelope
A LoRa packet carrying a PagerOS request:
```
┌────────────┬────────────┬──────────────┬─────────────┬─────────────┐
│ magic (2B) │ version(1B)│ type (1B)    │ msg_id (4B) │ payload     │
└────────────┴────────────┴──────────────┴─────────────┴─────────────┘
```
- `magic`: `0xPA 0x47` (PG)
- `type`: `0x01` request, `0x02` response, `0x03` ack, `0x04` exit-node-advertise
- `msg_id`: random, used for ACK + response matching
- `payload`: CBOR-encoded inner envelope (see below)

#### 6.2.2 Inner envelope (encrypted)
```cbor
{
  "to":   "https://notes.app/save",  // target URL
  "from": <device pubkey>,
  "sig":  <ed25519 signature over (to, body, nonce)>,
  "nonce": <16 B>,
  "body": <bytes>                     // CBOR request body
}
```
Encrypted with X25519 ECDH between device and app's published pubkey (from manifest). Exit Node cannot read.

#### 6.2.3 Fragmentation
- Frames larger than the LoRa MTU are split into numbered fragments with a `frag_id` and `total`.
- Reassembled by recipient. Lost fragments retried individually.

#### 6.2.4 Exit Node discovery
- Exit Nodes periodically broadcast `exit-node-advertise` packets with: pubkey, internet-bandwidth class, current load.
- Devices maintain a ranked list, prefer nearest with lowest load.

#### 6.2.5 Routing
- Reuses Meshtastic-style flood routing in v1 (simple, proven).
- Each packet has TTL hops (default 3).
- Future: deterministic routing if/when the mesh matures.

### 6.3 Selection logic
On request:
1. If Wi-Fi associated and last successful HTTPS within 60 s: use Wi-Fi.
2. Else try Wi-Fi with 3 s timeout.
3. Else fall back to LoRa if app's manifest has `lora_compatible: true`.
4. Else return error: "App requires internet."

### 6.4 Retries & timeouts
| Transport | Initial timeout | Retries | Backoff |
|---|---|---|---|
| Wi-Fi | 5 s | 2 | 2 s, 4 s |
| LoRa | 30 s | 3 | 10 s, 30 s, 90 s |
| Push pull | 10 s | 2 | 5 s, 15 s |

### 6.5 Caching at the Exit Node
- Exit Nodes MAY cache `GET` responses keyed by URL + body hash for the response's stated `Cache-Control: max-age`.
- Reduces traffic for popular shared content (e.g., a weather app's frame).
- Privacy: opt-out via request flag.

### 6.6 Push Relay
**DECISION:** A central push relay is operated by the PagerOS project as a v1 deliverable.

#### 6.6.1 Architecture
- Apps POST notifications to `https://push.pageros.org/push/<device_pubkey>` with a CBOR payload.
- The relay stores ≤ 16 pending notifications per device with a 7-day TTL.
- On wake (or scheduled poll over Wi-Fi or LoRa), the device pulls pending notifications via `GET /pull/<device_pubkey>` with a signed request and acknowledges receipt via `DELETE /pull/<device_pubkey>/<notification_id>`.

#### 6.6.2 Authentication
- The relay verifies the app's Ed25519 signature against the app's registered manifest pubkey before accepting a push.
- The device's pull request is Ed25519-signed; relay verifies before returning data.

#### 6.6.3 Encryption
- Notification payload is encrypted end-to-end from app to device using X25519+ChaCha20-Poly1305 against the device's public key.
- Relay sees the envelope (sender app id, destination device pubkey, size) but never plaintext.

#### 6.6.4 Rate limits
- Per (app, device) pair: 60 notifications/hour, 1,000/day. Excess returns HTTP 429.
- Per device: 1 MB inbound queue cap. Exceeded → drop oldest first.

#### 6.6.5 Delivery semantics
- At-least-once. Each notification has a server-assigned id; device dedupes on read.
- If the relay is unreachable, apps may retry; no offline queueing on the app side is required by spec.

#### 6.6.6 Group events
- The same relay carries `group_event` envelopes when devices are offline (§5.4.2). Same auth, encryption, and rate-limit model; rate-limit bucket is separate from user-visible notifications.

#### 6.6.7 Operating burden
- The relay is a stateful service the project must operate (storage, abuse handling, key rotation). Tracked as a v1 subsystem (`push-relay`, §18).

---

## 7. Device Firmware Architecture

### 7.1 Layered model
```
┌──────────────────────────────────────────────┐
│  Shell  (home, app drawer, settings)        │  <- written in PagerOS UI DSL
├──────────────────────────────────────────────┤
│  App Runtime  (one foreground app at a time)│
├──────────────────────────────────────────────┤
│  UI Renderer  |  Input Router  |  Cache     │
├──────────────────────────────────────────────┤
│  Transport (Wi-Fi + LoRa + Push + selection)│
├──────────────────────────────────────────────┤
│  Identity  |  Power  |  Storage  |  Drivers │
├──────────────────────────────────────────────┤
│  ESP-IDF + FreeRTOS                         │
└──────────────────────────────────────────────┘
```

### 7.2 Boot flow
1. **Bootloader** (ESP-IDF stock).
2. **Self-test:** flash + PSRAM + display + radios.
3. **Identity load:** read keypair from secure storage, or generate if first boot.
4. **Storage mount:** SD card.
5. **Font load:** mmap-style access to `/system/fonts/` (no eager load — pages in on first glyph need).
6. **Shell launch:** render home screen from cached Frame, kick off background sync with Marketplace + Push Relay pull.

### 7.3 App switching model
- One foreground app at a time. Memory is reclaimed on switch.
- Last 5 apps remembered with their last Screen — "recent apps" list.
- BACK from app's root Screen returns to Shell. Hold BACK = force-quit.

### 7.4 Filesystem layout (SD card)
```
/system/
  identity.key       # encrypted device private key
  trust.cbor         # pinned app pubkeys
  settings.cbor
  fonts/             # bundled Noto Sans subsets, see §7.7
  tones/             # notification tone PCM clips, see §7.8
/cache/
  frames/<app_id>/<screen_id>.cbor
  images/<sha256>.png
  tiles/<z>/<x>/<y>.png  # OSM map tiles
/apps/
  <app_id>/manifest.cbor
  <app_id>/icon.png
/notifications/
  inbox.cbor         # pending notification queue (decrypted, indexed by ts)
/logs/
  pageros.log        # rotating, 1 MB
```

### 7.5 Power management
- **Idle (screen on, no input):** 30 s → screen dim, 60 s → screen off, 5 min → light sleep.
- **Light sleep:** Wi-Fi off, LoRa in RX duty cycle (1 s on / 9 s off), CPU sleeps. Wakes every 60 s for a Push Relay pull if any push-subscribed app is installed.
- **Deep sleep:** triggered by user or low battery. Only RTC alarm + power button wakes.

### 7.6 Notifications
- Apps push notifications via the project-operated Push Relay (§6.6).
- On wake or periodic poll, the firmware fetches pending notifications, decrypts them, and stores them in `/notifications/inbox.cbor`.
- Surfaced as a top-of-screen flash on next user interaction, plus a notification tone (§7.8).
- Notification tap routes the user to the originating app's `notification_action` handler (§5.4.1).
- User can mute per-app or globally in Settings.

### 7.7 Fonts
- Firmware ships with subsets of **Noto Sans** covering: Latin (extended), CJK (Han + Hiragana + Katakana + Hangul), Arabic, Cyrillic, Greek, Hebrew, Devanagari, plus a curated emoji subset.
- Total budgeted at ~1.5 MB across multiple `.ttf`/`.woff2` files in `/system/fonts/`.
- Renderer performs per-codepoint fallback: tries primary font, then walks the fallback chain until a glyph is found; substitutes the missing-glyph box only as a last resort.
- Font rasterization is cached per (font, size, codepoint) in PSRAM with an LRU policy.
- Future: downloadable language packs (e.g., extended Han, Thai, Tibetan) via Marketplace.

### 7.8 Audio (notification tones)
- Short (≤ 2 s) PCM clips bundled in firmware: `default`, `low_priority`, `alert`, `success`, `error`.
- Per-app override permitted: the push payload may reference one of the bundled tone ids.
- Volume + per-app tone preferences in Settings. Global "silent mode" disables all tones.

### 7.9 OTA update client
- Periodic check against `https://updates.pageros.org` (signed by project release key).
- Two-partition scheme (A/B) with rollback on failed boot.
- User confirms before installing; downloads paused over LoRa.

---

## 8. App SDK

### 8.1 Common contract (language-agnostic)
An app is an HTTP service that:
- Responds to `GET /` with a Frame.
- Optionally responds to other paths referenced by `href` fields it generates.
- Reads `PagerOS-Device` and `PagerOS-Sig` headers to identify and verify the caller.
- Optionally publishes a pubkey for E2E encryption over LoRa and the Push Relay.
- Optionally exposes a long-polling `events()` endpoint for online multi-device sessions.

### 8.2 Reference SDK (Python) — surface area

```python
from pageros import App, Screen, Text, List, Form, Input, Button

app = App(
    name="Notes",
    icon="icon.png",
    permissions=[],
    lora_compatible=True,
    multi_device=False,
)

@app.screen("/")
def home(ctx):
    notes = db.list_for(ctx.device_id)
    return Screen(
        title="Notes",
        body=[
            List(items=[
                {"label": n.title, "href": f"/view/{n.id}"} for n in notes
            ])
        ],
        actions=[{"label": "New", "key": "n", "href": "/new"}],
    )

@app.screen("/new")
def new_form(ctx):
    return Screen(
        title="New note",
        body=[
            Form(action="/save", fields=[
                Input(name="title", label="Title"),
                Input(name="body", label="Body"),
            ], submit="Save"),
        ],
    )

@app.handler("/save", method="POST")
def save(ctx, data):
    db.create(ctx.device_id, data["title"], data["body"])
    return app.redirect("/")

# Optional: push a notification
def remind(device_id, message):
    app.push(device_id, {"title": "Reminder", "body": message, "tone": "default"})

# Optional: multi-device group events (when multi_device=True)
@app.group_event("group_message")
def on_message(ctx, group_id, data):
    app.broadcast(group_id, "group_message", data)

if __name__ == "__main__":
    app.run(port=8080)
```

### 8.3 `ctx` object
| Field | Type | Description |
|---|---|---|
| `ctx.device_id` | string | Base64 device pubkey (verified). |
| `ctx.session` | dict | Server-managed session store (opt-in helper). |
| `ctx.transport` | enum | `wifi` or `lora` — apps can return smaller frames over LoRa. |
| `ctx.granted` | list | Permissions the user has approved. |
| `ctx.location` | (lat, lon, ts) or None | If app subscribed to location and user granted. |
| `ctx.groups` | list[str] | Groups this device is currently subscribed to (multi-device apps only). |

### 8.4 SDK responsibilities
- CBOR encoding of Frames.
- Signature verification of incoming requests.
- Optional X25519 decryption for LoRa-bound and push-bound payloads.
- A dev-mode HTTP echo of what the device would render (used by the Simulator).
- Helper to compute LoRa size budget and warn if Frame exceeds it.
- Push-send helper that signs and encrypts to a target device.
- Group registry helpers (broadcast to group, list members) for multi-device apps.

### 8.5 SDK ports
- v1: **Python** (primary), **JavaScript/TypeScript** (Node).
- v2: **Go**, **Rust**.
- Community can port to anything; spec is the contract.

---

## 9. Identity, Security & Permissions

### 9.1 Device identity
- On first boot, device generates an **Ed25519** keypair.
- Private key stored in ESP32-S3 secure storage (NVS with flash encryption).
- Public key is the device's stable identity. Displayable as a 12-char base32 fingerprint for user readability.

### 9.2 Request signing
- Every request to an App Server includes `PagerOS-Sig`: Ed25519 signature over `(method || url || timestamp || body_hash)`.
- Timestamp prevents replay (servers reject > 5 min skew).
- Apps verify signature before processing.

### 9.3 E2E encryption over LoRa and Push
- App publishes its X25519 pubkey in its Marketplace manifest.
- Device computes shared secret, encrypts inner envelope with ChaCha20-Poly1305.
- Same scheme used for Push Relay payloads.
- Exit Nodes and Push Relay see envelope only.

### 9.4 Permissions
Apps declare requested permissions in their manifest:

| Permission | Grants access to | Default |
|---|---|---|
| `location` | GPS coordinates + `location` event | Prompt |
| `nfc` | NFC scan events | Prompt |
| `notifications` | Push notifications to device | Prompt |
| `groups` | Subscribe device to multi-device groups | Prompt |
| `lora_send` | Send raw mesh messages on user's behalf (rare) | Prompt + warning |
| `contacts` | Read user's local contacts (if/when implemented) | Prompt |

Permissions are per-app, persistent until revoked from Settings.

### 9.5 App trust model
- Marketplace verifies that the manifest's pubkey controls the listed URL (DNS TXT challenge at publish time).
- No code signing (apps run on dev server, not on device).
- Users can self-host apps and add them by URL without going through Marketplace ("sideload by URL").

### 9.6 Group identity (multi-device sessions)
- A device joins an app-defined group by sending a signed `join_group` request: `{ group_id, device_pubkey, ts }` signed Ed25519.
- The app verifies the signature and admits the device per its own membership rules (invite codes, allowlists, etc. — out of the protocol's concern).
- Membership is **app-scoped**: there is no global "group" concept owned by the protocol. Each app maintains its own membership records.
- The same device pubkey may join arbitrarily many groups, in the same or different apps. Identity remains stable across all of them.
- Leaving a group is a signed `leave_group` request.
- Group messaging is end-to-end encrypted between members only when the app explicitly implements pairwise X25519 fan-out; otherwise messages are server-mediated cleartext (trust the app server).

---

## 10. Marketplace

### 10.1 Governance
**DECISION:** Operated as a single project for v1; not federated.
- The marketplace registry is run and moderated by the PagerOS core maintainers.
- All policy decisions (acceptance, removal, featured status) are documented in a public moderation log.
- A future transition to a foundation or federated model is possible but not promised.

### 10.2 Manifest schema
```yaml
id: notes.mafu.dev              # globally unique, reverse-DNS recommended
name: Notes
description: A simple notepad.
icon: https://notes.app/icon.png  # 96x96 PNG
url: https://notes.app/          # app server root
pubkey: <base64 x25519 pubkey>  # for E2E over LoRa + Push (optional but recommended)
permissions: []
lora_compatible: true
multi_device: false              # set true if the app supports multi-device sessions (§5.4.2)
donate_url: https://...          # optional; surfaced as "Tip developer" on the app detail screen
categories: [productivity]
maintainer:
  name: Jane Doe
  contact: jane@example.com
version: 1
```

### 10.3 Publishing flow
1. Developer runs `pagerctl publish ./manifest.yaml`.
2. CLI walks DNS TXT challenge to prove control of the URL's domain.
3. Marketplace validates manifest schema, fetches and stores icon, registers app.
4. App becomes discoverable.

### 10.4 In-device discovery
The Shell's "Apps" screen is itself a PagerOS app served by the Marketplace:
- `GET https://market.pageros.org/` returns a Frame with a list of apps (paginated).
- Subscreens for categories, search, app detail.
- App detail screen surfaces a "Tip developer" action when `donate_url` is set in the manifest.
- "Install" = add to user's home screen (just adds the manifest to local apps list).

### 10.5 Moderation
**DECISION:** Open registration + post-hoc moderation.
- Any app may register.
- Marketplace has a Trust & Safety queue for reports.
- Apps may be tagged `unverified`, `verified`, `featured`, `flagged`.
- The Shell can filter to verified-only.

### 10.6 Sideloading
- Settings → "Add app by URL" → enter URL → device fetches manifest at `<url>/.pageros/manifest.cbor`.
- App is added locally, marked `sideloaded` (not in Marketplace).

---

## 11. Exit Nodes

### 11.1 Hardware
Any device with: LoRa (SX1262 or compatible) + internet. Reference targets:
- Raspberry Pi + LoRa HAT.
- Another T-LoRa Pager connected via USB to a host (tethered exit node).
- ESP32-S3 + Ethernet shield.

### 11.2 Software
- Reference implementation in Go (cross-compiles cleanly for ARM/x86/ESP).
- Listens on LoRa for requests, performs HTTPS, returns responses.
- Configurable rate limits per device pubkey (anti-abuse).
- Publishes anonymized stats (uptime, requests/hour) to a public dashboard (opt-in).

### 11.3 Trust
- Exit Nodes are untrusted; they cannot read encrypted payloads.
- For unencrypted apps (no E2E key), Exit Node can read traffic. Apps marked `lora_compatible` SHOULD publish a pubkey to avoid this.

### 11.4 Incentives
- v1: pure volunteer.
- v2 (deferred): consider a Lightning-style microincentive for high-bandwidth nodes.

---

## 12. Developer Tooling

### 12.1 `pagerctl` CLI
| Command | Purpose |
|---|---|
| `pagerctl init <lang>` | Scaffold a new app in chosen language. |
| `pagerctl dev` | Run local app server + open Simulator. |
| `pagerctl simulate <url>` | Render a remote app in the Simulator. |
| `pagerctl publish` | Validate manifest, perform DNS challenge, register with Marketplace. |
| `pagerctl flash <fw.bin>` | Flash firmware to attached device. |
| `pagerctl link` | Pair a real device to local dev server for live testing. |

### 12.2 Simulator
**DECISION:** Built on **Tauri** (Rust core + system webview).
- ~10 MB binaries; shares Rust renderer code with the firmware's reference renderer for parity.
- Renders Frames pixel-accurate to device display (480 × 222).
- Keyboard input mapped to device QWERTY.
- Network panel shows raw CBOR exchanged.
- Two modes: **direct** (talks to local app server) and **proxy** (talks via simulated LoRa with injectable latency/loss).
- Group sessions simulatable by spinning up multiple sim instances.

### 12.3 Templates
Starter apps shipped with `pagerctl init`:
- `hello` — single screen with text.
- `form` — collect input and respond.
- `list-detail` — classic master/detail.
- `location` — GPS-aware example.
- `nfc-counter` — reacts to NFC scans.
- `chat` — multi-device group app reference (uses `chat` + `presence_list` widgets).
- `push-reminder` — sends a notification on a timer.

---

## 13. Versioning & Compatibility

### 13.1 Protocol version
- SemVer-like, but only `MAJOR` matters on the wire.
- v1.0 is the spec in this document.
- Breaking changes increment MAJOR; old devices must still receive a useful error.

### 13.2 Firmware update
- OTA updates served by `updates.pageros.org`, signed by project release key (§7.9).
- Verified at install; rollback partition kept.

### 13.3 SDK compatibility
- SDKs are independently versioned per language.
- An SDK targets a protocol version; can serve multiple if it negotiates.

---

## 14. Performance Budgets

| Metric | Target | Failure threshold |
|---|---|---|
| Frame decode + render (cached image, cached glyphs) | < 80 ms | > 200 ms |
| Wi-Fi cold request round trip | < 500 ms | > 2 s |
| LoRa request round trip (single hop) | < 8 s | > 30 s |
| Push pull latency (Wi-Fi) | < 600 ms | > 3 s |
| Boot to home screen | < 4 s | > 8 s |
| Idle battery life | 24 h | < 12 h |
| Active battery life (screen on, intermittent use) | 4 h | < 2 h |
| Encoded Frame size (LoRa-targeted) | < 200 B | > 250 B (must fragment) |
| Firmware + fonts on flash | < 14 MB | > 14 MB (eats OTA staging slot) |
| Group event delivery (online, both devices on Wi-Fi) | < 1 s | > 3 s |

---

## 15. Open Questions

Reduced to truly v2 items after the v0.2 decision pass.

1. **Sandboxing technology.** If and when we add on-device app code execution (§16.4), pick WASM (wasm3, wasmi) vs Lua vs both. Affects firmware footprint and security model.
2. **Push Relay capacity model.** The §6.6 limits (16 pending / 7-day TTL / 60 per hour) are first-cut. Production numbers depend on observed app behavior; revisit after the first 5 apps are in the wild.
3. **OSM tile cap policy.** Soft-cap of 1,000 tiles/device/day is an opening number. May need server-side enforcement (proxy through our infrastructure) if abuse appears.
4. **Group event scale.** First-class multi-device sessions are in scope, but the Push Relay's per-device queue cap (1 MB) sets an effective limit on group activity. Need real-world data on how this constrains group app design.

---

## 16. Explicitly Out of Scope (v1)

- 16.1 **Final visual identity / brand guidelines.** Name confirmed (PagerOS); logo/wordmark deferred.
- 16.2 **Non-ESP32-S3 device support.** Only T-LoRa Pager initially.
- 16.3 *(Moved to v1: see §6.6 Push Relay.)*
- 16.4 **On-device app code execution (WASM, Lua, etc.).** Reserved for v2. The spec keeps widget tag space and event types unused so a future runtime can be slotted in without breaking v1 apps.
- 16.5 **Voice / audio streaming.** Notification tones only (§7.8).
- 16.6 **Paid apps / in-app purchases.** Free + optional donation link only (§10.2).
- 16.7 **App-to-app communication on the device** (intents/IPC). Group apps coordinate server-side, not locally.
- 16.8 **Multi-user / shared device.** One device, one identity. Multi-device app sessions (§5.4.2) are *not* the same as multi-user devices.
- 16.9 **Meshtastic interop.** Users may dual-boot between PagerOS and Meshtastic firmware on the same hardware, but the two stacks do not interoperate live. PagerOS does not bundle, link, or re-implement Meshtastic packets.

---

## 17. Repository Layout (proposed)

```
pager-ecosystem/
├── SPEC.md                  # this document
├── README.md
├── protocol/                # CBOR schema, widget reference, test vectors
│   ├── spec.md
│   ├── widgets/
│   └── test-vectors/
├── firmware/                # device OS (C/C++, ESP-IDF)
│   ├── components/
│   ├── main/
│   ├── fonts/               # bundled Noto subsets
│   ├── tones/               # bundled PCM clips
│   └── partitions.csv
├── sdk/
│   ├── python/
│   └── js/
├── simulator/               # Tauri desktop app (Rust core)
├── marketplace/             # web app + registry API + moderation tooling
│   ├── api/
│   └── web/
├── push-relay/              # project-operated push service (§6.6)
├── exit-node/               # Go reference implementation
├── cli/                     # pagerctl, Go
├── examples/                # sample apps
└── docs/                    # tutorials, dev guides, user manual
```

---

## 18. Subsystems → Implementation Components

For task derivation. Each bullet maps to one or more discrete units of work.

### Protocol (§5)
- CBOR schema definition + reference encoder/decoder library
- Widget conformance test suite (cross-language test vectors)
- Group event envelope spec
- Spec doc (this section, extracted into `protocol/spec.md`)

### Firmware (§7)
- Bootloader + partition layout
- Display driver + render loop
- Input driver (keyboard, encoder)
- LoRa driver wrapper (on top of RadioLib or LoRaMesher)
- Wi-Fi + HTTPS client
- Transport selector
- Identity + crypto (Ed25519, X25519, ChaCha20-Poly1305)
- Frame cache (PSRAM + SD)
- Image cache + PNG decoder
- Map tile renderer + OSM client + tile cap enforcement
- Font bundle + Unicode renderer with fallback chain (FreeType or stb_truetype)
- Notification tones + audio output
- Push notification client (poll, decrypt, queue, surface)
- Group session client (subscribe_groups, group event dispatch, presence widget mutation)
- Shell app (in DSL)
- App switcher / lifecycle
- Power management
- OTA update client

### SDK (§8)
- Python: core lib + dev server + push helper + group helper + tests + docs
- JS/TS: same
- Go (v2)
- Rust (v2)

### Transport / LoRa (§6, §11)
- Envelope codec (request/response, fragmentation)
- Routing (flood, v1)
- Exit Node reference (Go)
- Exit Node discovery protocol

### Push Relay (§6.6)
- Relay HTTP service (Go or Rust): push, pull, ack endpoints
- Storage backend (per-device queue with TTL)
- Signature verification (app and device)
- E2E payload pass-through (relay never decrypts)
- Rate limiter
- Group-event envelope handling
- Admin / abuse dashboard
- Public-facing operations (uptime, deployment, scaling, key rotation)

### Marketplace (§10)
- Governance / moderation policy doc + public log
- Manifest validator
- DNS challenge service
- Public registry API (REST)
- Web UI (browse, search, app detail, donate-link surface)
- In-device app (Frames served from same backend)
- Moderation queue + admin UI

### Developer tooling (§12)
- `pagerctl` CLI (Go)
- Simulator (Tauri + Rust core renderer; group multi-instance support)
- Templates (one per language × per pattern, incl. chat + push-reminder)

### Identity & Security (§9)
- Crypto primitives (firmware + SDKs)
- Permission UX flow on device
- Sideload-by-URL flow
- Group join/leave signed-request handlers (in SDKs)

### Docs & community
- User manual
- Developer guide
- Spec docs site
- Contribution guide
- Reference apps (hello, notes, weather, GPS tracker, NFC counter, chat, push-reminder)

---

*End of v0.2 spec.*
