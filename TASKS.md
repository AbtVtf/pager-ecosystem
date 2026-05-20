# PagerOS — Implementation Tasks

Derived from `SPEC.md` v0.2 §18. Each task is a coherent unit of work with explicit acceptance criteria and dependencies. No time estimates.

## How to read this

- **ID prefixes** group by subsystem: `PROTO`, `FW`, `PY`, `JS`, `SIM`, `CLI`, `LORA`, `EXIT`, `PUSH`, `MKT`, `SEC`, `DOCS`.
- **Deps** are other task IDs that must complete first. Tasks with no `Deps` line have no hard prereqs.
- **Accepts** is the acceptance criterion — the minimum observable behavior that proves the task is done.
- Section §X.Y references point into `SPEC.md`.

## Subsystem index
1. [Protocol (PROTO)](#1-protocol)
2. [Firmware (FW)](#2-firmware)
3. [Python SDK (PY)](#3-python-sdk)
4. [JavaScript/TypeScript SDK (JS)](#4-javascripttypescript-sdk)
5. [Simulator (SIM)](#5-simulator)
6. [CLI — pagerctl (CLI)](#6-cli--pagerctl)
7. [LoRa Transport (LORA)](#7-lora-transport)
8. [Exit Node (EXIT)](#8-exit-node)
9. [Push Relay (PUSH)](#9-push-relay)
10. [Marketplace (MKT)](#10-marketplace)
11. [Security — cross-cutting (SEC)](#11-security--cross-cutting)
12. [Documentation & Examples (DOCS)](#12-documentation--examples)
13. [Critical path & milestones](#13-critical-path--milestones)

---

## 1. Protocol

The protocol is the contract between every other subsystem. Lock it down first; every other group blocks on it.

- **PROTO-001** — Define the CBOR widget tag registry
  - **Accepts:** A document assigning canonical numeric/string tags to every widget in §5.3 and every event in §5.4. Reserves a tag range for future widgets and on-device runtime (§16.4).
- **PROTO-002** — Extract & refine `protocol/spec.md` from `SPEC.md` §5
  - **Accepts:** A self-contained spec doc usable as the single source of truth for SDK authors. Includes all widgets, events, request envelope, headers.
  - **Deps:** PROTO-001
- **PROTO-003** — Build cross-language test vectors
  - **Accepts:** A `protocol/test-vectors/` directory with input/expected-CBOR pairs covering every widget, every event, error cases, oversized payloads, and unknown-widget forward-compat. Vectors are language-agnostic JSON+CBOR pairs.
  - **Deps:** PROTO-002
- **PROTO-004** — Group event envelope spec
  - **Accepts:** Section in `protocol/spec.md` defining `group_id` placement, all 4 group event types (§5.4.2), and the `subscribe_groups` Frame field. Includes test vectors.
  - **Deps:** PROTO-002, PROTO-003
- **PROTO-005** — Conformance test runner
  - **Accepts:** A CLI tool that takes an SDK HTTP endpoint and runs all test vectors against it, producing a pass/fail report. Used in CI for SDK PRs.
  - **Deps:** PROTO-003

---

## 2. Firmware

The device OS. Largest subsystem by code volume.

### Core bring-up
- **FW-001** — ESP-IDF project skeleton + partition table
  - **Accepts:** `firmware/` builds with `idf.py build` and produces a flashable binary. Partition layout per §13.2 (A/B + OTA staging + recovery).
- **FW-002** — Bootloader self-test
  - **Accepts:** On boot, validates flash + PSRAM + display + radios; logs result; halts on hard fail.
  - **Deps:** FW-001
- **FW-003** — Filesystem layout + SD card mount
  - **Accepts:** SD mounted with directory tree per §7.4. Creates missing dirs on first boot.
  - **Deps:** FW-002
- **FW-004** — Logger (rotating SD log)
  - **Accepts:** `LOG_INFO`/`LOG_ERROR` macros write to `/logs/pageros.log`, rotating at 1 MB.
  - **Deps:** FW-003

### Drivers
- **FW-005** — Display driver: 2.33" IPS 480×222
  - **Accepts:** Renders solid color, gradient, and a bitmap region from PSRAM framebuffer. Sustained ≥ 30 fps for full-screen redraws.
  - **Deps:** FW-002
- **FW-006** — Input driver: QWERTY keyboard matrix
  - **Accepts:** All 44 keys produce distinct events with no missed presses up to 5 keys/sec.
  - **Deps:** FW-002
- **FW-007** — Input driver: rotary encoder + ENTER/BACK
  - **Accepts:** CW/CCW ticks + click + back distinguished. No spurious events at rest.
  - **Deps:** FW-002
- **FW-008** — LoRa driver wrapper (SX1262)
  - **Accepts:** TX/RX of arbitrary byte buffers at 868 MHz + 915 MHz, configurable bandwidth/SF, RSSI/SNR exposed.
  - **Deps:** FW-002
- **FW-009** — Wi-Fi + HTTPS client with Mozilla CA bundle
  - **Accepts:** `https_get(url)` and `https_post(url, body)` return body or error; cert chain validates against bundled roots.
  - **Deps:** FW-002
- **FW-010** — NFC driver (ST25R3916)
  - **Accepts:** Detects ISO 14443-A tags within 3 cm; emits scan event with UID and any NDEF records.
  - **Deps:** FW-002
- **FW-011** — GPS driver (u-blox MIA-M10Q)
  - **Accepts:** Cold fix < 60 s in good sky; emits lat/lon/accuracy updates at ≥ 1 Hz when fixed.
  - **Deps:** FW-002
- **FW-012** — IMU driver stub (BHI260AP)
  - **Accepts:** Initializes IMU, exposes raw read API. No event dispatch in v1 (reserved for v2).
  - **Deps:** FW-002
- **FW-013** — Audio driver (ES8311 codec + speaker)
  - **Accepts:** Plays a bundled 16-bit/8 kHz PCM clip end-to-end with no glitches.
  - **Deps:** FW-002

### Crypto & identity
- **FW-014** — Identity keypair generation + secure storage
  - **Accepts:** First boot generates Ed25519 keypair, stores private in NVS with flash encryption enabled; subsequent boots load it. Public key derivable.
  - **Deps:** FW-002
- **FW-015** — Crypto primitives wrapper (Ed25519 sign, X25519 ECDH, ChaCha20-Poly1305 AEAD)
  - **Accepts:** Each operation passes RFC test vectors; benchmarked at ≥ 100 ops/s.
  - **Deps:** FW-002, SEC-001

### Protocol implementation
- **FW-016** — CBOR codec (integrate TinyCBOR or equivalent)
  - **Accepts:** Encoder + decoder pass all PROTO-003 test vectors.
  - **Deps:** FW-002, PROTO-003
- **FW-017** — Frame cache (PSRAM L1, SD L2)
  - **Accepts:** Set/get by `(app_id, screen_id)`; PSRAM hits < 1 ms, SD hits < 50 ms; respects TTL; LRU eviction.
  - **Deps:** FW-016, FW-003
- **FW-018** — Image cache + PNG decoder
  - **Accepts:** Content-addressed by sha256. Fetches via HTTPS once, caches forever on SD. Decodes a 96×96 PNG in < 50 ms.
  - **Deps:** FW-009, FW-003
- **FW-019** — Font bundle + Unicode renderer with fallback chain
  - **Accepts:** All bundled Noto Sans subsets in `/system/fonts/`. Per-codepoint fallback walks all bundled fonts. Missing glyph renders as `□`. LRU glyph cache in PSRAM.
  - **Deps:** FW-005, FW-003
- **FW-020** — Widget renderer: text, list, input, form, button, notification
  - **Accepts:** Each renders pixel-correct per the simulator's reference rendering; passes PROTO-003 visual vectors.
  - **Deps:** FW-005, FW-019, FW-016
- **FW-021** — Widget renderer: image
  - **Accepts:** Renders cached images at declared `w`/`h`; fetches on miss; placeholder while loading.
  - **Deps:** FW-020, FW-018
- **FW-022** — Widget renderer: map (OSM tiles + cap enforcement)
  - **Accepts:** Renders OSM tiles at zoom 0-18, pans with encoder, displays GPS marker. Counts fetches against per-day soft cap; logs warning on hit.
  - **Deps:** FW-020, FW-009, FW-011
- **FW-023** — Widget renderer: presence_list, chat (mutate-in-place)
  - **Accepts:** Renders initial state from Frame; mutates in place on `presence_update` and `group_message` events without full Frame refetch.
  - **Deps:** FW-020, FW-031 (group session client)
- **FW-024** — Input router (route to foreground app/widget)
  - **Accepts:** Encoder/keyboard events reach the focused widget; widgets bubble unhandled events up to the app shell.
  - **Deps:** FW-006, FW-007

### Transport
- **FW-025** — Transport selector (Wi-Fi / LoRa / Push per §6.3)
  - **Accepts:** Implements selection logic exactly per spec; reports which transport served each request.
  - **Deps:** FW-008, FW-009, FW-026 (LoRa client), FW-030 (push client)
- **FW-026** — LoRa transport client (envelope + fragmentation + retries)
  - **Accepts:** Sends a request and receives a response over LoRa via an Exit Node, with fragmentation handled transparently. Honors retry/backoff per §6.4.
  - **Deps:** FW-008, FW-015, LORA-001, LORA-002, LORA-003
- **FW-027** — Exit Node discovery + ranking
  - **Accepts:** Listens for `exit-node-advertise` packets, maintains ranked list by RSSI + load, selects best on each request.
  - **Deps:** FW-008, LORA-005

### Higher layers
- **FW-028** — App lifecycle (foreground, recent apps, kill, switch)
  - **Accepts:** Switching apps reclaims PSRAM cleanly. Recent-apps list (5) persists across reboots. Hold-BACK force-quits.
  - **Deps:** FW-020
- **FW-029** — Shell app (home, app drawer, settings) — written in DSL
  - **Accepts:** Boots to home screen showing installed apps. App drawer scrolls. Settings exposes identity, network, permissions, OTA.
  - **Deps:** FW-028, FW-017
- **FW-030** — Push notification client (poll, decrypt, dedupe, queue, surface)
  - **Accepts:** Pulls from `push.pageros.org` on wake; decrypts to plaintext; stores in `/notifications/inbox.cbor`; surfaces as top-of-screen flash + tone on next interaction.
  - **Deps:** FW-009, FW-015, PUSH-003
- **FW-031** — Group session client (subscribe_groups + event dispatch)
  - **Accepts:** Subscribes to declared groups on Frame load; dispatches incoming `member_joined`/`member_left`/`presence_update`/`group_message` events to live widgets.
  - **Deps:** FW-026, FW-030
- **FW-032** — Notification tones playback + per-app override
  - **Accepts:** Plays bundled tones (`default`, `low_priority`, `alert`, `success`, `error`); honors push payload's tone id; global mute respected.
  - **Deps:** FW-013, FW-030
- **FW-033** — Permission prompt UX
  - **Accepts:** First request from an app for a permission shows a dialog (allow / deny / always). Decision persists. Revocable from Settings.
  - **Deps:** FW-029
- **FW-034** — Sideload-by-URL flow
  - **Accepts:** Settings → "Add app by URL" → enter URL → fetches `.pageros/manifest.cbor` → app appears in app drawer with `sideloaded` tag.
  - **Deps:** FW-029, FW-009
- **FW-035** — Power management state machine
  - **Accepts:** Transitions through active → dim → screen-off → light sleep → deep sleep per §7.5 timings. Wakes on input/RTC/push interval. Measured idle current ≤ 5 mA average.
  - **Deps:** FW-028
- **FW-036** — OTA update client (A/B + signed manifest)
  - **Accepts:** Polls `updates.pageros.org`, verifies signature, downloads to inactive partition, sets boot flag, reboots; rolls back if new partition fails to boot.
  - **Deps:** FW-009, FW-015

---

## 3. Python SDK

- **PY-001** — `App` class + `@app.screen` / `@app.handler` decorators
  - **Accepts:** Minimal "hello world" app (≤ 20 LOC) returns a Frame from `GET /`.
  - **Deps:** PROTO-002
- **PY-002** — CBOR Frame encoder
  - **Accepts:** All PROTO-003 test vectors encode correctly. Round-trips equal input.
  - **Deps:** PROTO-003
- **PY-003** — Widget builders (`Screen`, `Text`, `List`, `Form`, `Input`, `Button`, `Image`, `Map`, `Notification`, `PresenceList`, `Chat`)
  - **Accepts:** Idiomatic Python constructors; type hints; produce valid CBOR via PY-002.
  - **Deps:** PY-002
- **PY-004** — Request signature verification middleware
  - **Accepts:** Rejects requests with missing/invalid `PagerOS-Sig`; populates `ctx.device_id` on success; tested against firmware-generated signatures.
  - **Deps:** SEC-001
- **PY-005** — X25519 decryption for LoRa & push payloads
  - **Accepts:** Apps can declare a keypair; SDK transparently decrypts inbound encrypted requests.
  - **Deps:** SEC-001
- **PY-006** — `app.push(device_id, payload)` helper
  - **Accepts:** Signs and encrypts payload, POSTs to Push Relay, returns 2xx or raises with relay error.
  - **Deps:** PY-005, PUSH-002
- **PY-007** — Group helpers: `app.broadcast(group_id, event, payload)`, `@app.group_event(name)`
  - **Accepts:** Reference chat example works end-to-end with two simulator instances.
  - **Deps:** PY-005, PUSH-007
- **PY-008** — `ctx` object (device_id, session, transport, granted, location, groups)
  - **Accepts:** All fields populated per spec; documented.
  - **Deps:** PY-004
- **PY-009** — Dev server with auto-reload
  - **Accepts:** `pagerctl dev` (CLI-002) reloads on file change in < 1 s.
- **PY-010** — Manifest generator from app config
  - **Accepts:** `app.manifest()` returns a valid YAML manifest matching MKT-001 schema.
  - **Deps:** MKT-001
- **PY-011** — LoRa size budget warning
  - **Accepts:** SDK logs a warning when a Frame's encoded size exceeds 200 B for an app with `lora_compatible: true`.
- **PY-012** — Test suite + PROTO-005 conformance run
  - **Accepts:** `pytest` passes; conformance runner reports 100%.
  - **Deps:** PROTO-005, PY-001..PY-011
- **PY-013** — PyPI publishing setup
  - **Accepts:** `pip install pageros` installs the SDK; tagged release pushes to PyPI via CI.

---

## 4. JavaScript/TypeScript SDK

Mirror of Python SDK with idiomatic API. One task per Python counterpart.

- **JS-001..JS-013** — direct counterparts to PY-001..PY-013.
  - **Accepts:** Same behavior; conformance runner reports 100%.
  - **Deps:** Equivalent PY deps + npm publishing.

*(Listed as one bundle since the structure mirrors PY exactly. Will be expanded into individual tickets at execution time.)*

---

## 5. Simulator

- **SIM-001** — Tauri project skeleton + Rust core
  - **Accepts:** Builds and launches an empty 480×222 window on macOS, Linux, Windows.
- **SIM-002** — Pixel-accurate Frame renderer (Rust crate shared with FW)
  - **Accepts:** Renders the same pixels as firmware for all PROTO-003 vectors (golden-image comparison ≤ 1% pixel diff).
  - **Deps:** SIM-001, PROTO-003
- **SIM-003** — Keyboard input mapping (host keys → device QWERTY + encoder + ENTER/BACK)
  - **Accepts:** Documented key map; all input types produce events identical to firmware.
  - **Deps:** SIM-002
- **SIM-004** — Direct mode (connect to local app server)
  - **Accepts:** Renders any app served at `http://localhost:8080/`.
  - **Deps:** SIM-002
- **SIM-005** — Network panel (raw CBOR + decoded view of each exchange)
  - **Accepts:** Side panel shows request, response, latency, transport. Decoded CBOR pretty-printed.
  - **Deps:** SIM-004
- **SIM-006** — Proxy mode (simulated LoRa with injectable latency + loss)
  - **Accepts:** Latency slider 0-30 s; loss rate 0-50%; reproduces fragmentation behavior.
  - **Deps:** SIM-005
- **SIM-007** — Multi-instance support for group session testing
  - **Accepts:** Launch N simulator windows sharing a "fake mesh"; chat example works between them.
  - **Deps:** SIM-006
- **SIM-008** — Release pipeline (signed binaries for macOS, Linux, Windows)
  - **Accepts:** `simulator.pageros.org/download` serves notarized macOS dmg, AppImage, msi.

---

## 6. CLI — pagerctl

Built in Go for single-binary distribution.

- **CLI-001** — `pagerctl init <lang>` — scaffold a new app
  - **Accepts:** Generates a runnable hello-world project in Python or JS.
  - **Deps:** PY-001, JS-001
- **CLI-002** — `pagerctl dev` — run local app server + simulator
  - **Accepts:** One command spins up the user's app + opens the simulator pointing at it; reloads on file change.
  - **Deps:** SIM-004
- **CLI-003** — `pagerctl simulate <url>` — render a remote app in the simulator
  - **Accepts:** Opens simulator pointing at any URL.
  - **Deps:** SIM-004
- **CLI-004** — `pagerctl publish` — validate, DNS challenge, register with Marketplace
  - **Accepts:** Publishes a valid app end-to-end; returns app id and listing URL; rejects invalid manifests with clear errors.
  - **Deps:** MKT-002, MKT-003
- **CLI-005** — `pagerctl flash <fw.bin>` — flash firmware to attached device
  - **Accepts:** Detects connected device over USB; flashes; verifies; reports success.
  - **Deps:** FW-001
- **CLI-006** — `pagerctl link` — pair real device to local dev server
  - **Accepts:** Real device fetches Frames from the dev server over USB-tethered Wi-Fi; live reload works.
  - **Deps:** CLI-002, FW-009
- **CLI-007** — Templates: `hello`, `form`, `list-detail`, `location`, `nfc-counter`, `chat`, `push-reminder`
  - **Accepts:** Each `pagerctl init <template>` produces a working example app per DOCS-005..011.
  - **Deps:** DOCS-005..DOCS-011
- **CLI-008** — Release pipeline (binaries for macOS, Linux, Windows)
  - **Accepts:** `brew install pagerctl` / `apt install pagerctl` / Windows installer all work.

---

## 7. LoRa Transport

- **LORA-001** — LoRa envelope codec (magic/version/type/msg_id/payload)
  - **Accepts:** Round-trips through encode/decode; bad magic rejected; unknown types ignored.
- **LORA-002** — Fragmentation & reassembly
  - **Accepts:** Splits payload > MTU into numbered fragments; reassembles in order; per-fragment retry on loss.
  - **Deps:** LORA-001
- **LORA-003** — Inner envelope (encrypted) format
  - **Accepts:** Encodes `(to, from, sig, nonce, body)`; encrypted with X25519+ChaCha20; decrypts only with target's key.
  - **Deps:** LORA-001, SEC-001
- **LORA-004** — Flood routing with TTL hops
  - **Accepts:** Packets propagate up to TTL hops; loops detected and dropped (msg_id cache).
  - **Deps:** LORA-001
- **LORA-005** — Exit Node discovery protocol (advertise + ranking)
  - **Accepts:** Exit Nodes advertise every 30 s; devices maintain ranked list (RSSI + load); test with 3 simulated nodes.
  - **Deps:** LORA-001

---

## 8. Exit Node

Reference implementation in Go.

- **EXIT-001** — Go project skeleton + config
  - **Accepts:** `exit-node` binary loads YAML config, opens LoRa device, prints status.
- **EXIT-002** — LoRa RX/TX loop
  - **Accepts:** Receives incoming envelopes; transmits responses; survives sustained load.
  - **Deps:** EXIT-001, LORA-001
- **EXIT-003** — HTTPS proxy: forward decapsulated requests to public internet
  - **Accepts:** Validates `to` URL is HTTPS; performs request; encodes response into LoRa envelope.
  - **Deps:** EXIT-002, LORA-002, LORA-003
- **EXIT-004** — Rate limiter per device pubkey
  - **Accepts:** Configurable cap (default 60 req/min/device); over-limit returns error envelope.
  - **Deps:** EXIT-003
- **EXIT-005** — Discovery beacon (advertise)
  - **Accepts:** Emits `exit-node-advertise` packets per LORA-005.
  - **Deps:** EXIT-002, LORA-005
- **EXIT-006** — Response caching (per §6.5)
  - **Accepts:** GETs with `Cache-Control: max-age` cached by URL+body hash; opt-out via request flag.
  - **Deps:** EXIT-003
- **EXIT-007** — Stats publisher (opt-in)
  - **Accepts:** Anonymized uptime + requests-per-hour POSTed to public dashboard.
  - **Deps:** EXIT-003
- **EXIT-008** — Packaging (Raspberry Pi image, Docker image, .deb)
  - **Accepts:** Pi image boots, finds USB LoRa, runs exit-node service. Docker image runs on x86 + ARM.
  - **Deps:** EXIT-001..EXIT-007

---

## 9. Push Relay

**Open decision (carry into PUSH-001):** Go vs Rust for the relay service. Either is fine; pick one and commit.

- **PUSH-001** — Service skeleton + chosen storage backend
  - **Accepts:** HTTP service starts, accepts TLS, exposes `/healthz`. Storage backend (Redis or Postgres + queue table) provisioned via Docker compose.
- **PUSH-002** — `POST /push/<device_pubkey>` endpoint with app sig verify
  - **Accepts:** Accepts encrypted payload + app signature; verifies sig against manifest pubkey (fetched from Marketplace); enqueues with TTL. Rejects unknown apps with 403.
  - **Deps:** PUSH-001, MKT-002, SEC-001
- **PUSH-003** — `GET /pull/<device_pubkey>` endpoint with device sig verify
  - **Accepts:** Returns pending notifications for device; verifies device sig; returns 401 on fail.
  - **Deps:** PUSH-001, SEC-001
- **PUSH-004** — `DELETE /pull/<device_pubkey>/<notification_id>` (ack)
  - **Accepts:** Removes acked notification from queue; idempotent.
  - **Deps:** PUSH-003
- **PUSH-005** — Storage backend with TTL eviction
  - **Accepts:** Per-device queues capped at 16 notifications + 7-day TTL + 1 MB total; oldest dropped when over limit.
  - **Deps:** PUSH-001
- **PUSH-006** — Rate limiter (per app/device, per device global)
  - **Accepts:** Enforces 60/hour, 1000/day per (app, device); 429 on overflow.
  - **Deps:** PUSH-002
- **PUSH-007** — Group event envelope routing
  - **Accepts:** Apps push group events to all subscribed device pubkeys via the relay; uses separate rate-limit bucket from user notifications.
  - **Deps:** PUSH-002, PUSH-005
- **PUSH-008** — Admin / abuse dashboard
  - **Accepts:** Authenticated view of per-app/per-device send volumes; ability to ban a sender pubkey.
  - **Deps:** PUSH-002
- **PUSH-009** — Deployment & monitoring
  - **Accepts:** Deployed to production hosting; uptime + queue depth alerts wired up; key rotation procedure documented.
  - **Deps:** PUSH-001..PUSH-008
- **PUSH-010** — Public SLO + uptime page
  - **Accepts:** Published SLO document; live uptime page at `status.pageros.org`.
  - **Deps:** PUSH-009

---

## 10. Marketplace

- **MKT-001** — Manifest schema (YAML) + validator
  - **Accepts:** Validates a manifest against schema in §10.2; rejects invalid fields with clear errors.
- **MKT-002** — Registry REST API (CRUD apps, list, search)
  - **Accepts:** Endpoints for register, list, get-by-id, search-by-keyword; OpenAPI doc generated.
  - **Deps:** MKT-001
- **MKT-003** — DNS TXT challenge service
  - **Accepts:** Issues a challenge token at publish time; verifies presence in DNS TXT record before accepting registration.
  - **Deps:** MKT-002
- **MKT-004** — Web UI: browse, search, app detail
  - **Accepts:** Public marketplace.pageros.org renders app list, category filter, full-text search, app detail page with icon + description + donate link.
  - **Deps:** MKT-002
- **MKT-005** — In-device "Apps" screens (Frames served from MKT backend)
  - **Accepts:** Shell's app drawer pulls from `market.pageros.org/`, renders categories + featured + search subscreens — all in the PagerOS DSL.
  - **Deps:** MKT-002, FW-029
- **MKT-006** — Moderation queue + admin UI
  - **Accepts:** Authenticated admin can view reports, tag apps (`unverified`/`verified`/`featured`/`flagged`), remove apps.
  - **Deps:** MKT-002
- **MKT-007** — Donate-link surfacing
  - **Accepts:** App detail screen shows "Tip developer" action when `donate_url` is set.
  - **Deps:** MKT-005
- **MKT-008** — Public moderation log
  - **Accepts:** All admin actions (accept, tag, remove) logged to a public read-only feed.
  - **Deps:** MKT-006
- **MKT-009** — Trust tagging system in API + UI
  - **Accepts:** Manifests expose current tag; UI shows tag badge; Shell filterable by tag.
  - **Deps:** MKT-006, MKT-005
- **MKT-010** — Featured curation tooling
  - **Accepts:** Admins can pin an app to "Featured"; shows up at top of MKT-005 home.
  - **Deps:** MKT-006
- **MKT-011** — Deployment + ops
  - **Accepts:** Production deployment with backups, monitoring, scaling plan.
  - **Deps:** MKT-001..MKT-010

---

## 11. Security — cross-cutting

- **SEC-001** — Crypto suite selection doc
  - **Accepts:** A short doc choosing concrete implementations of Ed25519, X25519, ChaCha20-Poly1305 for: firmware (e.g., libsodium-mini or mbedTLS), Python SDK (PyNaCl), JS SDK (libsodium.js), Go (crypto/ed25519 + golang.org/x/crypto). Shared test vectors across all.
- **SEC-002** — Threat model document
  - **Accepts:** Documented adversary list (malicious app, hostile Exit Node, compromised push relay, lost device), attack scenarios, mitigations, residual risks.
- **SEC-003** — Security review of the v1 stack before public launch
  - **Accepts:** External review (paid or community) signs off on: crypto choices, key management, transport security, marketplace trust model.
  - **Deps:** All FW + PUSH + MKT tasks substantially complete.

---

## 12. Documentation & Examples

### Docs
- **DOCS-001** — User manual
  - **Accepts:** Covers: first boot, identity, installing apps, settings, troubleshooting. Published at `docs.pageros.org`.
- **DOCS-002** — Developer getting-started guide
  - **Accepts:** Walks a new dev from `pagerctl init hello` to publishing in < 30 min of reading.
  - **Deps:** CLI-001, CLI-004
- **DOCS-003** — Spec docs site
  - **Accepts:** `SPEC.md` + `protocol/spec.md` rendered with nav at `spec.pageros.org`.
  - **Deps:** PROTO-002
- **DOCS-004** — Contribution guide
  - **Accepts:** `CONTRIBUTING.md` covers code style, PR process, sign-off, testing requirements.

### Reference apps
- **DOCS-005** — `hello` — single screen text
  - **Accepts:** ≤ 20 LOC in Python and JS; works in simulator and on device.
  - **Deps:** PY-001, JS-001
- **DOCS-006** — `notes` — list/detail + forms + persistence
  - **Accepts:** Add, view, edit, delete notes; per-device storage.
  - **Deps:** PY-003
- **DOCS-007** — `weather` — image + text from a public API
  - **Accepts:** Renders current conditions for user's GPS location.
  - **Deps:** PY-003, FW-011
- **DOCS-008** — `gps-tracker` — map widget + location event
  - **Accepts:** Shows live position; logs track points.
  - **Deps:** FW-022
- **DOCS-009** — `nfc-counter` — nfc_scan event
  - **Accepts:** Counts tag scans per UID.
  - **Deps:** FW-010
- **DOCS-010** — `chat` — multi-device reference (uses `chat` + `presence_list`)
  - **Accepts:** Two pagers in a group can exchange messages with online status; works online and via push when offline.
  - **Deps:** PY-007, FW-023, FW-031
- **DOCS-011** — `push-reminder` — sends a push at a user-set time
  - **Accepts:** User sets a time; app pushes a notification at that time; tone plays on device.
  - **Deps:** PY-006, FW-030, FW-032

### Marketing
- **DOCS-012** — Project landing page (`pageros.org`)
  - **Accepts:** Tagline, demo gif, "get a device" + "build an app" CTAs, link to docs and spec.

---

## 13. Critical path & milestones

The full dependency graph is large; here is the recommended sequencing.

### Milestone M0 — Protocol & dev tooling exist
> Goal: Anyone can write a PagerOS app and see it render in a simulator.

Required: **PROTO-001..005**, **PY-001..003, PY-008..009, PY-012**, **SIM-001..005**, **CLI-001..003**, **DOCS-005**

Exit criterion: `pagerctl init hello && pagerctl dev` renders Hello World in the simulator.

### Milestone M1 — First app runs on real hardware over Wi-Fi
> Goal: A real T-LoRa Pager fetches and renders a Frame from a developer's laptop.

Required: M0 + **FW-001..009, FW-014..021, FW-024, FW-025 (Wi-Fi subset), FW-028..029, FW-033..035**, **CLI-005..006**

Exit criterion: Real device boots, joins Wi-Fi, opens dev-server app, completes a form, sees response.

### Milestone M2 — Push relay live, notifications work
> Goal: A dev's server can wake a real device with a notification.

Required: M1 + **SEC-001**, **PUSH-001..006, PUSH-009..010**, **FW-013, FW-030, FW-032**, **PY-004..006**

Exit criterion: `push-reminder` example sends a push that plays a tone on a sleeping device.

### Milestone M3 — Marketplace public, anyone can publish
> Goal: A third-party developer with no contact to the team can ship an app.

Required: M2 + **MKT-001..006, MKT-011**, **PY-010, PY-013**, **JS bundle (JS-001..013)**, **CLI-004, CLI-007..008**, **DOCS-001..004, DOCS-012**

Exit criterion: External dev runs `pagerctl publish`, app appears in the marketplace, installs on a fresh device.

### Milestone M4 — LoRa mesh + Exit Nodes operational
> Goal: A device with no Wi-Fi can use an app via the LoRa mesh.

Required: M3 + **LORA-001..005**, **FW-026..027**, **EXIT-001..008**

Exit criterion: Device with Wi-Fi disabled completes a notes-app round trip through a public Exit Node.

### Milestone M5 — Multi-device sessions
> Goal: The `chat` app works between two devices, online or offline.

Required: M4 + **PROTO-004**, **PUSH-007**, **PY-007**, **JS group helper**, **FW-023, FW-031**, **DOCS-010**

Exit criterion: Two pagers in a group exchange messages with presence updates, both online and via push when offline.

### Milestone M6 — Production readiness
> Goal: Public launch.

Required: M5 + **FW-036**, **MKT-007..010**, **SEC-002..003**, **DOCS** completion, marketing site

Exit criterion: External security review passed, status page green, marketing live.

---

*End of v1 task list. Update as scope changes; keep IDs stable.*
