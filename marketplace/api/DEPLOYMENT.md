# Marketplace registry — deployment & ops (MKT-011)

This document is the operations runbook for the PagerOS marketplace registry
service. It covers the production deployment topology, the backup, monitoring,
and scaling plans, and the cutover punch list that remains gated on
MKT-003..MKT-010.

> **Status (2026-05-20):** MKT-001 (manifest validator) and MKT-002 (REST API)
> are merged. The registry store is currently in-memory; the durable backend
> (Postgres) lands with MKT-003+ and unblocks the "real" production rollout.
> Everything below is wired for that future state — the Dockerfile, compose
> stack, and procedures are usable today against the in-memory store.

---

## 1. Topology

```
                ┌──────────────────────────────────┐
   clients ───► │ Caddy (TLS, HTTP/2, gzip, hdrs)  │ ──► api (uvicorn + FastAPI)
                └──────────────────────────────────┘            │
                                                                │ (MKT-003+)
                                                                ▼
                                                  ┌─────────────────────────┐
                                                  │ Postgres 16 (managed)   │
                                                  │ + S3-compatible object  │
                                                  │ store (icons, MKT-003)  │
                                                  └─────────────────────────┘
```

- **api** — a stateless FastAPI/ASGI service exposing the routes documented in
  `openapi.json`. Built from `marketplace/api/Dockerfile`. Runs as a non-root
  user, single worker per container, scaled horizontally.
- **caddy** — TLS termination + HTTP/2 + sensible hardening headers. The
  shipped `deploy/Caddyfile` covers the local testing case; the production
  block in §2 below shows the `market.pageros.org` form.
- **db** — Postgres 16 instance for the durable registry store. Not yet used
  by the in-memory `Registry` (MKT-002); the connection string is plumbed via
  `PAGEROS_MKT_DATABASE_URL` so the eventual switch is config-only.

## 2. Production deployment

Two supported modes:

### 2.1 Docker Compose (single-host)

Suitable for the v1 marketplace at launch (low traffic, single region).

```bash
cd marketplace/api
docker compose up --build -d
docker compose ps
curl -fsS http://localhost/healthz   # → {"status":"ok"}
```

Promote to public DNS by editing `deploy/Caddyfile`:

```caddy
market.pageros.org {
    encode zstd gzip
    reverse_proxy api:8080 {
        header_up X-Real-IP {remote_host}
        header_up X-Forwarded-For {remote_host}
        header_up X-Forwarded-Proto {scheme}
    }
    header {
        Strict-Transport-Security "max-age=31536000; includeSubDomains; preload"
        X-Content-Type-Options "nosniff"
        Referrer-Policy "strict-origin-when-cross-origin"
        -Server
    }
}
```

Caddy provisions a Let's Encrypt certificate on first request and renews
automatically.

### 2.2 Kubernetes (multi-host)

Once durable storage and request volume warrant it:

- One `Deployment` for the api (`pageros/marketplace-api`) with `replicas >= 2`.
- A managed Postgres (RDS / Cloud SQL / Neon) referenced via
  `PAGEROS_MKT_DATABASE_URL`.
- `HorizontalPodAutoscaler` keyed on CPU + custom RPS metric (see §4).
- Ingress with cert-manager-issued certs.

The image built from `marketplace/api/Dockerfile` runs unchanged on K8s — no
container-image gymnastics needed.

### 2.3 Configuration

| Env var                       | Default     | Notes                                                                 |
|-------------------------------|-------------|-----------------------------------------------------------------------|
| `PAGEROS_MKT_HOST`            | `0.0.0.0`   | uvicorn bind host.                                                    |
| `PAGEROS_MKT_PORT`            | `8080`      | uvicorn bind port.                                                    |
| `PAGEROS_MKT_DATABASE_URL`    | _(unused)_  | Reserved for MKT-003+ durable store. Today the registry is in-memory. |
| `PAGEROS_MKT_LOG_LEVEL`       | `info`      | Reserved for MKT-006 structured logging rollout.                      |

## 3. Backups

### 3.1 Today (in-memory)

The MKT-002 registry is process-local memory. A container restart loses every
registration. This is acceptable for the pre-launch milestone, not for
production. The "backup plan" for this phase is **don't run in-memory in
production** — gate launch on §6.1.

### 3.2 Once durable storage lands (MKT-003+)

| Asset                | Where                                  | Backup mechanism                                 | RPO   | RTO   |
|----------------------|----------------------------------------|--------------------------------------------------|-------|-------|
| Manifest registry    | Postgres `marketplace` DB              | Managed PITR + nightly logical dump to object store | ≤ 5m  | ≤ 30m |
| App icons (MKT-003)  | S3-compatible bucket                   | Bucket versioning + cross-region replication     | ≤ 1m  | ≤ 15m |
| TLS certs            | `caddy_data` volume / cert-manager     | Reproducible from ACME; back up `acme.json` daily | n/a   | ≤ 15m |
| OpenAPI snapshot     | `marketplace/api/openapi.json` in git  | Git history                                      | n/a   | n/a   |

Operational rules:

1. **Verify, don't trust.** Run a monthly restore drill — spin a staging
   instance from the latest dump and replay the integration suite from MKT-002.
2. **Encrypt at rest** (managed-DB native + bucket SSE).
3. **Retain** logical dumps for 30 days, PITR for 7 days, off-cloud copy for 90
   days.

## 4. Monitoring

### 4.1 Health & liveness

- `/healthz` returns `{"status":"ok"}` (200) — used by the container, Caddy,
  and any external probe. No DB roundtrip today; will gain an
  optional `?deep=1` mode when durable storage lands.

### 4.2 Logs

- uvicorn access logs and Caddy access logs go to stdout (JSON in Caddy's
  case). The shipped compose stack uses `restart: unless-stopped` and lets the
  container engine handle rotation; on K8s, the standard log-pipeline
  (Loki/Fluent Bit/CloudWatch) collects them.

### 4.3 Metrics

Counters and histograms we want as soon as the metrics middleware lands:

- `http_requests_total{route,method,status}` — request volume.
- `http_request_duration_seconds_bucket{route,method}` — latency histogram.
- `marketplace_registry_apps_total` — gauge: registered app count.
- `marketplace_register_failures_total{reason}` — counter: rejected
  registrations broken down by reason (`duplicate`, `manifest_invalid`, ...).
- `marketplace_search_query_duration_seconds_bucket` — search latency.

A reference Prometheus scrape config (post middleware rollout):

```yaml
- job_name: marketplace-api
  static_configs:
    - targets: ['api:8080']
  metrics_path: /metrics
```

### 4.4 Alerts

| Alert                                  | Trigger                                                          | Severity |
|----------------------------------------|------------------------------------------------------------------|----------|
| `MarketplaceAPIDown`                   | `up{job="marketplace-api"} == 0` for 2m                          | page     |
| `MarketplaceErrorRateHigh`             | 5xx rate > 1% for 10m                                            | page     |
| `MarketplaceLatencyHigh`               | p99 latency > 800ms for 10m                                      | warn     |
| `MarketplaceRegistrationFailuresSpike` | `rate(marketplace_register_failures_total[5m])` > baseline × 5   | warn     |
| `PostgresReplicationLag` (MKT-003+)    | replica lag > 60s for 5m                                         | warn     |

Pageable alerts must auto-resolve. Warn-level alerts post to the moderation
channel that MKT-006/MKT-008 will set up — track that dep in §6.1.

## 5. Scaling plan

The api process is **stateless**. Anything user-affecting is durable in
Postgres (once MKT-003+ lands) or static (`openapi.json`, manifest icons in
the object store). That fact drives the whole scaling story:

### 5.1 Vertical bounds (per container)

- 1 uvicorn worker + 1 vCPU + 256 MiB RAM is the minimum useful unit.
- A single container comfortably handles ~500 RPS read / ~50 RPS write at
  the steady-state shape today (small payloads, no DB on read path). When
  durable storage lands, expect ~150 RPS read / ~30 RPS write per container
  before DB pool contention dominates.

### 5.2 Horizontal scale

- Run `>= 2` replicas in production at all times for HA.
- Scale out on:
  - `cpu > 70%` sustained over 2 minutes; **or**
  - `http_requests_in_flight > 200` per pod.
- Scale in cautiously (cooldown ≥ 10 minutes; never below 2 replicas).
- Reads are cache-friendly: put a small CDN / shared HTTP cache (Cloudflare,
  Fastly, or Caddy `cache` plugin) in front of `GET /apps`, `GET /apps/{id}`,
  and `GET /apps/search?q=` with a short TTL (30–60s) before scaling pods —
  it's an order-of-magnitude cheaper than vertical/horizontal capacity.

### 5.3 Database (MKT-003+)

- Single primary + 1 read replica at launch; promote the replica to take
  read traffic only when the primary's `cpu > 50%` sustained.
- Connection pool: `min=5 max=10` per pod (asyncpg/SQLAlchemy).
- Index on `manifest_id` (unique), `categories` (GIN), and a full-text search
  index over `name || description` (Postgres `tsvector`) — that pairs cleanly
  with the keyword-search route from MKT-002.

### 5.4 Limits & abuse controls (MKT-006 territory)

- Rate-limit `POST /apps` at the edge (Caddy `rate_limit` or Cloudflare WAF):
  ≤ 6 registrations / minute / IP, ≤ 60 / hour / IP.
- DNS TXT challenge from MKT-003 is the primary anti-spam control. The
  registry should reject registrations whose challenge hasn't been verified
  in the last 5 minutes.

## 6. Cutover punch list

Final go-live is gated on items in §6.1; §6.2 captures the routine that
ships once those land.

### 6.1 Blocked-on-deps (must complete before production traffic)

- **MKT-003** — DNS TXT challenge service. Without it the registry is open to
  squatting; we cannot accept public registrations.
- **MKT-004** — Public web UI at `marketplace.pageros.org`. Hosted alongside
  the API.
- **MKT-005** — In-device app screens served from the registry backend.
- **MKT-006** — Moderation queue + admin UI. We need a hand-of-stop before
  inviting publishers.
- **MKT-007..010** — Donate-link surfacing, public moderation log, trust tags,
  featured curation. None block initial launch but should ship before the
  first marketing push.

These are referenced from MKT-011's `Deps:` field in `TASKS.md`.

### 6.2 Operational dry-run (after the above)

1. Cut a staging compose stack identical to production (same image, same
   Postgres major version).
2. Run the MKT-002 test suite + a load test (e.g. `oha` / `k6`) hitting
   register/list/search at 2× projected launch traffic.
3. Trigger a backup, drop the DB, restore from PITR — measure RTO.
4. Trigger an alert by killing one api container; verify auto-replacement
   and page routing.
5. Roll a no-op deploy through CI to validate zero-downtime rollout.
6. Sign-off comment from QA on the launch issue, then flip DNS.

## 7. Local smoke check

The deployment artifacts ship with a smoke test (`tests/test_deployment.py`)
that asserts:

- `Dockerfile` is parseable and exposes the expected port + entry command.
- `docker-compose.yml` is valid YAML, references the Dockerfile, and wires
  api → caddy + api → db with healthchecks.
- The FastAPI factory referenced by the Docker CMD resolves and serves
  `/healthz` via an in-process TestClient.

That suite runs as part of `pytest` — no Docker daemon required in CI.
