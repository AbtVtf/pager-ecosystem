# push-relay — Operations Runbook (PUSH-009)

This document is the operator-facing reference for running the push relay in
production. It covers deployment, monitoring, alert response, and the
day-to-day "what does this metric mean" questions. SPEC-level decisions live
in [SPEC.md §6.6](../../SPEC.md) — this file is downstream of those.

> **Workspace scope.** This runbook is part of the `push-relay/` subsystem.
> Anything cross-cutting (org-wide SLO, status page, public uptime feed) is
> tracked separately under PUSH-010.

---

## 1. Deployment topology

The production stack is described in [`docker-compose.prod.yml`](../docker-compose.prod.yml).
It runs four containers on a single host:

| Service        | Image                            | Purpose                                   |
| -------------- | -------------------------------- | ----------------------------------------- |
| `push-relay`   | built from this repo's Dockerfile | HTTP service on `:8443` (TLS).            |
| `redis`        | `redis:7-alpine`                  | Queue backend (PUSH-001 / PUSH-005).      |
| `prometheus`   | `prom/prometheus:v2.55.0`         | Scrapes `/metrics` every 15s.             |
| `alertmanager` | `prom/alertmanager:v0.27.0`       | Routes firing alerts to pager / Slack.    |

Single-host is fine for v1 traffic; SPEC §17.2 explicitly accepts that the
"production numbers depend on observed app behavior; revisit after the first
5 apps are in the wild". To scale horizontally:

1. Externalize Redis (managed Redis or a separate replica set).
2. Run multiple `push-relay` containers behind a TLS-terminating load
   balancer; they are stateless aside from the in-memory rate limiter +
   admin store. SPEC §6.6.4 quotas are per-(app, device) — if you scale
   horizontally, those quotas become per-instance, which is more permissive
   than spec-strict. For v1 traffic that's acceptable; future work tracks
   sharing the limiter via Redis if it becomes a problem.

### Required environment variables

| Variable                   | Required        | Notes                                                                                      |
| -------------------------- | --------------- | ------------------------------------------------------------------------------------------ |
| `PUSH_RELAY_ADDR`          | no (`:8443`)    | Listen address.                                                                            |
| `PUSH_RELAY_TLS_CERT`/`_KEY` | yes (prod)    | PEM paths. Both must be set together. Production deploys MUST set these.                   |
| `PUSH_RELAY_REDIS_URL`     | yes             | `redis://relay:<password>@redis:6379/0` for the compose stack.                             |
| `PUSH_RELAY_MARKETPLACE_URL` | yes (prod)    | `/push` returns 503 if unset — the relay can't verify app signatures without a registry.   |
| `PUSH_RELAY_ADMIN_TOKEN`   | yes (prod)      | ≥ 16 chars (config refuses shorter). Recommended: 32-char hex via `openssl rand -hex 32`.  |
| `PUSH_RELAY_BUILD_TAG`     | no (`dev`)      | Surfaced as a label on `push_relay_build_info` for ops sanity-checking the deployed SHA.   |

### First-boot checklist

1. Provision a TLS cert + key for `push.pageros.org` (or your equivalent
   hostname). Mount them at `./certs/cert.pem` and `./certs/key.pem`.
2. Generate the admin token and Redis passwords (see [`KEY-ROTATION.md`](./KEY-ROTATION.md)).
3. Edit `deploy/alertmanager/alertmanager.yml` to point at the on-call rotation
   (PagerDuty integration key or Slack webhook).
4. `docker compose -f docker-compose.prod.yml up -d`.
5. Verify `curl https://push.pageros.org/healthz` returns `{"status":"ok"}`.
6. Verify `curl -s https://push.pageros.org/metrics | head` shows
   `push_relay_build_info{tag="<your-tag>"} 1`.
7. Confirm Prometheus is scraping at `http://<host>:9090/targets`. Both
   `push-relay-1` and `alertmanager` should be UP.

---

## 2. Monitoring surface

All metrics live at `GET /metrics` (Prometheus text exposition, version 0.0.4).
The relay never authenticates `/metrics` — production deploys must restrict
network access to it (the compose file binds Prometheus + Alertmanager to
`127.0.0.1` so they are not internet-reachable).

### Metric reference

| Metric                             | Type    | Labels             | Meaning                                                            |
| ---------------------------------- | ------- | ------------------ | ------------------------------------------------------------------ |
| `push_relay_build_info`            | gauge   | `tag`              | Always `1`. Use the `tag` label to identify the deployed build.    |
| `push_relay_started_at_seconds`    | gauge   | none               | Unix seconds at process start. Subtract from `time()` for uptime.  |
| `push_relay_storage_up`            | gauge   | none               | `1` if the last storage Ping succeeded, `0` otherwise.             |
| `push_relay_queue_devices`         | gauge   | none               | Devices with ≥1 queued notification.                               |
| `push_relay_queue_entries`         | gauge   | none               | Total queued notifications across all devices.                     |
| `push_relay_queue_depth_max`       | gauge   | none               | Largest single-device queue depth (capped at 16 by SPEC §6.6.4).   |
| `push_relay_push_requests_total`   | counter | `result`           | Outcomes of `POST /push`.                                          |
| `push_relay_pull_requests_total`   | counter | `result`           | Outcomes of `GET /pull`.                                           |
| `push_relay_ack_requests_total`    | counter | `result`           | Outcomes of `DELETE /pull/{id}`.                                   |
| `push_relay_group_requests_total`  | counter | `result`           | Outcomes of `POST /group_push`.                                    |
| `push_relay_group_recipients_total` | counter | `status`          | Per-recipient outcomes inside group pushes.                        |

The `result` enum is: `accepted`, `bad_request`, `unauthorized`, `forbidden`,
`payload_too_large`, `rate_limited`, `lookup_unavailable`, `storage_error`,
`not_found`, `other`. The group `status` enum mirrors the response field —
see `internal/server/group.go`.

The queue gauges are refreshed by a background sampler every 30 s
(`server.DefaultStatsInterval`). They are deliberately approximate at scrape
time so `/metrics` requests stay O(1) instead of triggering a Redis scan.

### Alert rules

See [`deploy/prometheus/alerts.yml`](../deploy/prometheus/alerts.yml). The
PUSH-009-mandated alerts are:

- **PushRelayDown** — Prometheus has failed to scrape for ≥ 1 min. Pages.
- **PushRelayStorageDown** — `push_relay_storage_up == 0` for ≥ 2 min. Pages.
- **PushRelayQueueBacklog** — aggregate queue > 5000 for ≥ 10 min. Warns.
- **PushRelayQueueStuck** — at least one device at `MaxQueueLen=16` for
  ≥ 30 min. Warns.
- **PushRelayPushErrorRateHigh** — 5xx burn rate > 10% over 5 min. Warns.

---

## 3. Alert response

### alert: PushRelayDown

The relay is unreachable. In rough priority:

1. `docker compose -f docker-compose.prod.yml ps` — is the container up?
2. `docker compose logs --tail=200 push-relay` — startup error, panic?
3. `curl -kv https://<host>:8443/healthz` from the host — TLS handshake or
   network drop?
4. Check the TLS cert expiry (`openssl s_client -connect host:8443 -servername host </dev/null`) —
   an expired cert silently 451s upstream LBs.
5. If down for > 5 min and root cause not obvious, fail over: spin up the
   stack on a backup host and update the DNS record. The relay's queue lives
   in Redis volumes; mount the same volume to preserve queues.

### alert: PushRelayStorageDown

Redis is unreachable from the relay's container.

1. `docker compose logs --tail=200 redis` — OOM, AOF rewrite failure,
   network rule change?
2. `docker compose exec redis redis-cli -a "$REDIS_OPS_PASSWORD" PING` — does
   Redis itself answer?
3. If the AOF is corrupt, the recovery path is `redis-check-aof --fix
   /data/appendonly.aof` — see Redis docs. **Do this with a snapshot**
   before any destructive operation.
4. The relay returns 503 to `/push` and `/pull` while storage is down; that
   is correct behavior (PUSH-001 healthz contract). Senders retry with
   backoff.

### alert: PushRelayQueueBacklog

Aggregate queue depth elevated. Usually means devices are offline en masse
(carrier outage, scheduled maintenance window) or a sender is over-sending.

1. Cross-check `push_relay_push_requests_total{result="accepted"}` rate
   against historical baseline. If 10× normal, look at
   `admin.Store.Snapshot()` (`GET /admin/metrics` with the admin token) to
   find the noisy app id.
2. If a single app is responsible, consider banning (`POST /admin/bans`) and
   then opening a marketplace moderation issue.
3. If the spike is broad, do nothing — devices will drain on next wake. The
   per-device cap (16 entries / 1 MiB / 7 d) bounds the worst case.

### alert: PushRelayQueueStuck

A specific device is at `MaxQueueLen` for 30+ min.

The relay does not expose per-device pubkey labels (cardinality), so to find
which device:

```sh
docker compose exec redis redis-cli -a "$REDIS_OPS_PASSWORD" \
  --scan --pattern 'pageros:push:queue:*' \
  | while read key; do
      depth=$(redis-cli -a "$REDIS_OPS_PASSWORD" ZCARD "$key")
      if [ "$depth" -ge 16 ]; then echo "$depth $key"; fi
    done | sort -nr | head
```

Then look at `admin.Store.Snapshot()` to find which app is filling that
device's queue, and triage from there.

### alert: PushRelayPushErrorRateHigh

5xx rate spiked. The label set tells you whether it's storage
(`result="storage_error"`) or marketplace (`result="lookup_unavailable"`).

- `storage_error`: see PushRelayStorageDown above.
- `lookup_unavailable`: the marketplace registry is reachable but slow or
  erroring. Check the marketplace service health (`MKT-002`).

---

## 4. Routine operations

### Deploy a new build

```sh
git pull
docker compose -f docker-compose.prod.yml build push-relay
docker compose -f docker-compose.prod.yml up -d push-relay
```

The `push_relay_started_at_seconds` gauge resets on each deploy — a sudden
drop in `time() - push_relay_started_at_seconds` is a deploy event, not
necessarily a crash.

### Rotate the admin token

See [`KEY-ROTATION.md`](./KEY-ROTATION.md) §2.

### Rotate the TLS cert

See [`KEY-ROTATION.md`](./KEY-ROTATION.md) §1.

### Drain a host for maintenance

The relay has no long-lived connections; a graceful shutdown finishes
in-flight requests within the 10s `Shutdown` deadline configured in
`cmd/push-relay/main.go`. For HA you would do this behind a load balancer
that pre-drains the host. For single-host, accept the short outage.

```sh
docker compose -f docker-compose.prod.yml stop push-relay
# ... maintenance ...
docker compose -f docker-compose.prod.yml start push-relay
```

### Inspect the admin dashboard

```sh
curl -H "Authorization: Bearer $PUSH_RELAY_ADMIN_TOKEN" https://<host>:8443/admin/metrics
curl -H "Authorization: Bearer $PUSH_RELAY_ADMIN_TOKEN" https://<host>:8443/admin/bans
```

See `internal/admin/http.go` for the full surface.

### Ban a misbehaving sender

```sh
curl -X POST -H "Authorization: Bearer $PUSH_RELAY_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"app_id":"com.example.spam","reason":"abuse 2026-01-15"}' \
  https://<host>:8443/admin/bans
```

Banned senders are rejected with 403 before the marketplace lookup, so they
do not generate registry traffic. SPEC §6.6.2 / PUSH-008.

---

## 5. Capacity model

SPEC §17.2 calls out: "Production numbers depend on observed app behavior;
revisit after the first 5 apps are in the wild." Until then:

- **Per-device storage:** 16 entries × ≤ 1 MiB total × 7-day TTL.
- **Per-(app, device) rate:** 60/hour and 1000/day for `/push`; same caps,
  separate bucket, for `/group_push`.
- **Single host @ 1 GiB RAM Redis:** comfortably supports ~10,000 active
  devices with average queue depth ~5 (≈ 50,000 entries, ≈ 100 MiB stored).
  Above that, externalize Redis.
- **Single relay process:** Go HTTP server with no GIL-equivalent. Limiter
  contention dominates at extreme rates — saw ~50k req/s synthetic in CI.

When the metrics show > 70% of any of those numbers for 24h running average,
plan a scale-up issue assigned to CEO.

---

## 6. Where to find things

| Concern                              | File                                                                 |
| ------------------------------------ | -------------------------------------------------------------------- |
| TLS cert + admin token rotation      | [`KEY-ROTATION.md`](./KEY-ROTATION.md)                               |
| Production compose stack             | [`docker-compose.prod.yml`](../docker-compose.prod.yml)              |
| Prometheus scrape + alert rules      | [`deploy/prometheus/`](../deploy/prometheus/)                        |
| Alertmanager routing                 | [`deploy/alertmanager/`](../deploy/alertmanager/)                    |
| Redis prod config                    | [`redis/redis.conf`](../redis/redis.conf)                            |
| Metrics implementation               | [`internal/metrics/`](../internal/metrics/)                          |
| Metric wiring + gauge refresher      | [`internal/server/metrics.go`](../internal/server/metrics.go)        |
| Admin / ban list (PUSH-008)          | [`internal/admin/`](../internal/admin/)                              |
| SPEC §6.6 (Push Relay)               | [`../../SPEC.md`](../../SPEC.md)                                     |
