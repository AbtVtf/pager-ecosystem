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
