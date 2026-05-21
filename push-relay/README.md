# push-relay

Project-operated push notification relay for PagerOS (see [SPEC.md §6.6](../SPEC.md) and [TASKS.md §9](../TASKS.md)).

## Decisions (PUSH-001)

- **Language: Go.** Matches `exit-node/`; single static binary, simple cross-compile, mature `crypto/ed25519` and `net/http`.
- **Storage backend: Redis.** Per-device queues, 7-day TTL, and 1 MB cap (PUSH-005) map cleanly onto Redis lists + `EXPIRE` + `MEMORY USAGE`. We can revisit if abuse-handling needs (PUSH-008) require relational queries.

## Layout

```
push-relay/
  cmd/push-relay/        # main entrypoint
  internal/config/       # env-driven config
  internal/server/       # HTTP listener, /healthz
  internal/storage/      # Redis-backed Store interface
  scripts/gen-dev-tls.sh # self-signed cert for local TLS
  Dockerfile             # distroless build
  docker-compose.yml     # relay + redis stack
```

## Run locally

```sh
# 1. generate dev TLS (once)
./scripts/gen-dev-tls.sh

# 2. bring up redis + relay
docker compose up --build

# 3. verify
curl -k https://localhost:8443/healthz
# => {"status":"ok","storage":"ok","build_tag":"dev"}
```

To run the binary directly (no Docker):

```sh
PUSH_RELAY_REDIS_URL=redis://localhost:6379/0 \
PUSH_RELAY_TLS_CERT=./certs/cert.pem \
PUSH_RELAY_TLS_KEY=./certs/key.pem \
go run ./cmd/push-relay
```

Omit the `PUSH_RELAY_TLS_*` vars to run plain HTTP for local poking (the
service logs a warning — production deployments must set them).

## Configuration

| Env var                | Default                       | Notes                                  |
| ---------------------- | ----------------------------- | -------------------------------------- |
| `PUSH_RELAY_ADDR`      | `:8443`                       | Listen address                         |
| `PUSH_RELAY_TLS_CERT`  | _(unset)_                     | PEM cert path; required with TLS_KEY   |
| `PUSH_RELAY_TLS_KEY`   | _(unset)_                     | PEM key path; required with TLS_CERT   |
| `PUSH_RELAY_REDIS_URL` | `redis://localhost:6379/0`    | `redis://` or `rediss://`              |
| `PUSH_RELAY_BUILD_TAG` | `dev`                         | Surfaced in `/healthz` for ops sanity  |
| `PUSH_RELAY_MARKETPLACE_URL` | _(unset)_               | Registry root (e.g. `https://market.pageros.org`). When unset, `POST /push` returns 503 — the relay has no way to verify app signatures without it. |
| `PUSH_RELAY_ADMIN_TOKEN` | _(unset)_                   | Bearer token for `/admin/*` (PUSH-008). Must be ≥16 chars. When unset, all admin routes return `503 Service Unavailable`. |

## Test

```sh
go test ./...
```

## What this task delivers (PUSH-001)

- HTTP service that starts and binds (`./cmd/push-relay`).
- TLS support via cert+key env vars; plain HTTP allowed for dev with a startup warning.
- `/healthz` endpoint that returns `200` when storage is reachable, `503` otherwise.
- `Store` interface in `internal/storage` with a Redis implementation (PING-verified at startup).
- `docker-compose.yml` provisioning Redis + relay.

## What this task delivers (PUSH-005)

Per-device queue semantics on top of the `Store` interface
(`internal/storage`):

| Cap                 | Value             | Source        |
| ------------------- | ----------------- | ------------- |
| Entries per device  | 16                | SPEC §6.6.1   |
| Per-entry lifetime  | 7 days            | SPEC §6.6.1   |
| Bytes per device    | 1 MiB             | SPEC §6.6.4   |

Eviction rules (applied on every `Enqueue`, and TTL is also applied on `List`):

1. Drop entries older than `QueueTTL`.
2. If the queue exceeds `MaxQueueLen`, drop the oldest entries until it fits.
3. If the total stored byte size exceeds `MaxQueueBytes`, drop the oldest
   entries until it fits.

A new `NewMemory()` backend is provided for tests and dev usage — both
backends share the same observable semantics. The Redis backend uses a
sorted set keyed by enqueue timestamp and runs the enqueue+eviction step
inside a Lua script for atomicity.

## What this task delivers (PUSH-003)

`GET /pull/{device_pubkey}` is now wired in (`internal/server/pull.go`):

- Verifies the `PagerOS-Sig` headers (`internal/pageossig/`) against
  the request method + URL + timestamp + sha256(body) per SPEC §9.2.
  Rejects with `401 Unauthorized` on missing header, bad encoding,
  expired timestamp (>5 min skew), failed signature verify, or when
  the URL pubkey doesn't match the signed pubkey (defense in depth
  against cross-device pulls).
- Returns the device's queued notifications in oldest-first order as
  JSON: `{ notifications: [ { id, kind, app_id, payload (base64),
  enqueued_at } ] }`.
- Reads from the `storage.Store` interface added in PUSH-005, so TTL,
  queue length, and byte-size caps are enforced uniformly.

A standalone `pageossig` package cross-checks the construction against
the shared `ed25519-pageros-sig-sample` test vector
(`docs/spec/crypto-test-vectors.json`), keeping the Go relay
byte-aligned with the Python SDK and firmware libsodium implementation.

The actual `/push` and `/ack` (DELETE) HTTP handlers, plus rate
limiting, arrive in PUSH-002 / PUSH-004 / PUSH-006.

## What this task delivers (PUSH-002)

`POST /push/{device_pubkey}` is wired in (`internal/server/push.go`):

- Reads the opaque encrypted envelope from the request body (capped at
  `DefaultMaxPushBytes` = 64 KiB by default, overridable via
  `Options.MaxPushBytes`). The relay never decrypts (SPEC §6.6.3).
- Looks up the sender app's signing pubkey from the marketplace registry
  (`internal/manifest`, hitting `GET {PUSH_RELAY_MARKETPLACE_URL}/apps/{app_id}`).
  Unknown apps → **403 Forbidden** per SPEC §6.6.2. Registry unreachable →
  **503 Service Unavailable** so callers retry with backoff.
- Verifies the request's `PagerOS-Sig` (Ed25519 over
  `method || url || timestamp || sha256(body)`, SPEC §9.2) against the
  resolved app pubkey. Sig failures → **401 Unauthorized**.
- Enqueues the payload via the `storage.Store` interface (TTL + queue
  caps from PUSH-005 apply).

Request headers:

| Header              | Required | Notes                                           |
| ------------------- | -------- | ----------------------------------------------- |
| `PagerOS-App`       | yes      | Sender app id (reverse-DNS, matches manifest id) |
| `PagerOS-Sig`       | yes      | base64url Ed25519 sig                            |
| `PagerOS-Timestamp` | yes      | Unix seconds, ±5 min skew                        |

Response (`202 Accepted`):

```json
{ "id": "<server-assigned notification id>", "enqueued_at": 1715000000 }
```

Note on the manifest pubkey field: SPEC §10.2 documents the manifest
`pubkey` as X25519 (for E2E encryption), while §6.6.2 says the relay
verifies an Ed25519 sig against the manifest pubkey. The marketplace
registry currently stores a single 32-byte pubkey; this implementation
verifies against whatever the registry returns. Resolving the
"one key vs two" inconsistency is a marketplace concern tracked
separately.

## What this task delivers (PUSH-004)

`DELETE /pull/{device_pubkey}/{notification_id}` is wired in
(`internal/server/ack.go`):

- Same `PagerOS-Sig` device-signing scheme as `GET /pull` — re-uses
  `pageossig.Verify` and the URL/signed-pubkey cross-check so the two
  endpoints can never diverge in subtle ways.
- Idempotent: SPEC PUSH-004 says "Removes acked notification from
  queue; idempotent." Both "removed" and "did not exist" return
  `204 No Content`. Clients learn nothing useful from a 404 here and
  surfacing the distinction makes retry-driven acks noisier without
  improving correctness.
- Delegates to `storage.Store.Delete` (already in place from PUSH-005);
  no new storage surface added.

## What this task delivers (PUSH-006)

Per-(app, device) rate limiting in `internal/ratelimit`:

- **60 notifications/hour** and **1,000/day** per (app, device), matching
  SPEC §6.6.4.
- Sliding-window in-memory `MemoryLimiter` — beats fixed-window because
  a malicious sender can't burst 2× the cap across a window boundary.
- On overflow, `POST /push` responds with **429 Too Many Requests** and
  a `Retry-After` header (seconds-granularity, rounded up).
- The rate-limit check runs *after* signature verification so a forger
  cannot drain a legitimate tuple's budget with bogus signatures.

The limiter is wired via `server.Options.Limiter`; tests inject a
`NewMemoryWithLimits(...)` instance to keep test cases readable.
PUSH-007 will instantiate a *second* limiter for group events (SPEC
§6.6.6 keeps that bucket separate from user-visible notifications).

## What this task delivers (PUSH-007)

Group event fan-out via `POST /group_push` (`internal/server/group.go`):

- Apps send a single signed JSON request listing up to
  `DefaultMaxGroupRecipients` (64) recipients. Each carries its own
  opaque (already-encrypted) payload; the relay never decrypts —
  SPEC §6.6.3 / §6.2.5 keep the per-device X25519+ChaCha20-Poly1305
  layer entirely above this service.
- Auth + ban + sig verification mirror `POST /push` and reject the
  whole batch on failure (`401` / `403`). The marketplace lookup
  resolves the app pubkey, exactly the same path PUSH-002 already
  exercises.
- Per-recipient errors (invalid device pubkey, oversized payload,
  rate-limit overflow) are reported *per row* in the response so a
  single bad recipient cannot poison the rest of the batch.
- Group events use a **separate** `ratelimit.Limiter` instance from
  user notifications (SPEC §6.6.6 / wiring via
  `server.Options.GroupLimiter`). Exhausting the group bucket for an
  (app, device) tuple leaves the `/push` bucket untouched, and vice
  versa — covered by `TestGroupPushBucketIndependentFromPushBucket`.
- Successful enqueues are stored with `Kind = group_event` so
  `GET /pull` surfaces the right kind to the device renderer.

Request:

```json
POST /group_push
Headers: PagerOS-App, PagerOS-Sig, PagerOS-Timestamp (as /push)
{
  "recipients": [
    { "device_pubkey": "<base64url>", "payload_b64": "<base64url>" },
    ...
  ]
}
```

Response (`202 Accepted`):

```json
{
  "results": [
    { "device_pubkey": "...", "status": "accepted", "id": "...", "enqueued_at": 1715000000 },
    { "device_pubkey": "...", "status": "rate_limited", "retry_after": 60 },
    { "device_pubkey": "...", "status": "invalid_device_pubkey" }
  ]
}
```

Status values: `accepted`, `rate_limited`, `payload_empty`,
`payload_too_large`, `invalid_payload`, `invalid_device_pubkey`,
`storage_error`.

## What this task delivers (PUSH-008)

Admin / abuse dashboard surface in `internal/admin`:

- `admin.Store` tracks per-app and per-device send volumes for every
  successful `POST /push` (recorded after enqueue, so failed requests
  never inflate counters). It also owns a bear-bones ban list of app
  ids.
- The push handler consults `admin.Store.IsBanned(appID)` before the
  marketplace lookup and short-circuits banned apps with **403
  Forbidden**, matching the format SPEC §6.6.2 already uses for
  unknown apps. This stops a banned sender from generating extra
  registry traffic.
- HTTP surface mounted at `/admin/*`, all guarded by an
  `Authorization: Bearer ${PUSH_RELAY_ADMIN_TOKEN}` header
  (constant-time compare). When the env var is unset the routes
  return **503 Service Unavailable** — opt-in by design.

| Method | Path                          | Body / response                                      |
| ------ | ----------------------------- | ---------------------------------------------------- |
| GET    | `/admin/metrics`              | `{apps:[...], devices:[...]}` sorted by volume desc  |
| GET    | `/admin/bans`                 | `{bans:[{app_id, reason, banned_at}, ...]}`          |
| POST   | `/admin/bans`                 | `{"app_id":"x.y","reason":"..."}` → `201` with `Ban` |
| DELETE | `/admin/bans/{app_id}`        | `204`, or `404` if not currently banned              |

Storage is in-memory; counters reset on restart. Cross-restart
persistence and metrics export live with PUSH-009 alongside the
deployment + monitoring story — the M3 acceptance only requires an
authenticated *view* of volumes and a ban knob, which this delivers.
