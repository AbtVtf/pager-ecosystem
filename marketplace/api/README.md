# pageros-marketplace

Server-side of the PagerOS marketplace registry (SPEC.md §10).

| Task | What | Status |
|---|---|---|
| MKT-001 | YAML manifest schema + validator | done |
| MKT-002 | Registry REST API (CRUD apps, list, search) + OpenAPI doc | done |
| MKT-003 | DNS TXT publish-time challenge service | done |
| MKT-006 | Moderation queue + trust tagging + admin UI | done |
| MKT-011 | Deployment artifacts + ops plan (registry portion) | scaffolded, see `DEPLOYMENT.md` |

## Install (dev)

```bash
cd marketplace/api
python -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'
```

To also install the optional uvicorn ASGI server:

```bash
pip install -e '.[dev,serve]'
```

## Validate a manifest

As a library:

```python
from pageros_marketplace.manifest import load_manifest, ManifestValidationError

try:
    manifest = load_manifest("manifest.yaml")
except ManifestValidationError as exc:
    for err in exc.errors:
        print(f"{err.field}: {err.message}")
```

As a CLI:

```bash
pageros-marketplace validate path/to/manifest.yaml
```

Exits `0` on a valid manifest, non-zero with a line-per-error report otherwise.

## Registry REST API

The registry exposes a small REST surface for app discovery. All bodies are JSON.

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/apps/challenges` | Start a publish-time DNS TXT challenge (MKT-003). |
| `POST` | `/apps/challenges/{id}/verify` | Verify the developer published the expected TXT record. |
| `GET` | `/apps/challenges/{id}` | Inspect a challenge (state + TXT instructions). |
| `POST` | `/apps` | Register a new app from a manifest. Requires `X-Challenge-Token`. Rejects duplicates by `id` (`409`). |
| `GET` | `/apps` | List apps. Supports `offset`, `limit`, optional `category` filter. |
| `GET` | `/apps/{app_id}` | Get one app by its manifest `id`. |
| `PUT` | `/apps/{app_id}` | Replace an app's manifest. `version` must strictly increase. Requires `X-Challenge-Token`. |
| `DELETE` | `/apps/{app_id}` | Remove an app from the registry. |
| `GET` | `/apps/search?q=...` | Case-insensitive search across `id`, `name`, `description`, `categories`. |
| `GET` | `/healthz` | Liveness probe. |

Manifests are validated against the SPEC §10.2 schema on every write — invalid
submissions get a structured 422 (FastAPI's standard validation envelope) or a
400 for higher-level errors. Moderation tagging (MKT-006) layers on top of
this surface and is delivered separately.

### DNS TXT publish-time challenge (MKT-003)

`POST /apps` and `PUT /apps/{app_id}` require an `X-Challenge-Token` header
proving the caller controls the host in `manifest.url`. The flow is:

1. `POST /apps/challenges` with `{"app_id": "notes.mafu.dev", "url": "https://notes.app/"}`
   returns `{id, token, txt_name, txt_value, expires_at, ...}`.
2. Developer creates a DNS TXT record at `txt_name`
   (e.g. `_pageros-challenge.notes.app`) with value `txt_value`
   (e.g. `pageros-challenge=<token>`).
3. `POST /apps/challenges/{id}/verify` — marketplace performs a live DNS
   lookup; on a match the challenge is marked verified.
4. `POST /apps` with the manifest plus `X-Challenge-Token: <token>` — the
   registry consumes the token (single-use) iff `app_id` and the URL host
   match what the challenge was issued for.

Tokens default to a 1-hour issuance TTL and a 24-hour post-verification TTL.
Live DNS resolution uses `dnspython`; tests inject `FakeResolver` via
`create_app(..., dns_resolver=FakeResolver())`. To opt out of the gate for
embedded/local-dev setups, pass `challenge_required=False` to `create_app`.

### Moderation queue + admin tooling (MKT-006)

Open registration plus post-hoc moderation per SPEC §10.5. Any user may
`POST /reports` against an existing app; admins consume the queue.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `POST` | `/reports` | none | File a report against an app (`{app_id, reason, reporter_contact?}`). |
| `GET` | `/admin/reports` | admin | List reports; filter by `status` (`open`/`resolved`/`dismissed`) and `app_id`. |
| `GET` | `/admin/reports/{id}` | admin | Inspect a single report. |
| `POST` | `/admin/reports/{id}/resolve` | admin | Close a report as actioned. |
| `POST` | `/admin/reports/{id}/dismiss` | admin | Close a report as not actionable. |
| `PUT` | `/admin/apps/{app_id}/tags` | admin | Replace the full trust-tag set. |
| `POST` | `/admin/apps/{app_id}/tags/{tag}` | admin | Add one trust tag (idempotent). |
| `DELETE` | `/admin/apps/{app_id}/tags/{tag}` | admin | Remove one trust tag (idempotent). |
| `DELETE` | `/apps/{app_id}` | admin | Remove an app (logged to the moderation log). |
| `GET` | `/admin/log` | admin | Append-only moderation action log. |
| `GET` | `/moderation/log` | none | Public read-only moderation log (MKT-008). |
| `GET` | `/admin/ui` | admin | Server-rendered moderation dashboard (HTML). |

Trust tags are an exclusive allowlist (SPEC §10.5): `verified`, `featured`,
`flagged`. `unverified` is implicit (no `verified` tag applied). Tags are
admin-only; ordinary publish flow lands an app with `tags=[]`.

Admin auth: pass `admin_token="…"` to `create_app(…)`. Requests must then
include `Authorization: Bearer <token>`. When `admin_token` is `None` (the
default), the admin surface is unauthenticated — useful for tests and local
development but **must** be set in production.

Every admin mutation (tag add/remove/set, app delete, report
resolve/dismiss) and every report filing appends an entry to the in-memory
action log. The same entries are mirrored unauthenticated at
`GET /moderation/log` (MKT-008) — newest first, paginated, with optional
`app_id` / `action` filters. Per SPEC §10 this public feed is the canonical
record of marketplace policy decisions; it is append-only and entries are
never edited or removed after they are written.

### Run locally

```bash
pageros-marketplace serve --host 127.0.0.1 --port 8080
# OpenAPI doc: http://127.0.0.1:8080/openapi.json
# Swagger UI:  http://127.0.0.1:8080/docs
```

### OpenAPI snapshot

`openapi.json` at the package root is a generated artifact (OpenAPI 3.1). To
regenerate after route changes:

```bash
pageros-marketplace dump-openapi --output openapi.json
```

### Use the API factory directly

```python
from pageros_marketplace import Registry, create_app

registry = Registry()
app = create_app(registry)   # FastAPI instance — mount under your ASGI server.
```

## Schema

See `pageros_marketplace/manifest.py` and SPEC.md §10.2 for the canonical
manifest schema. Recognized permissions come from SPEC §9.4.

## Tests

```bash
pytest
```

## Deployment

See [`DEPLOYMENT.md`](./DEPLOYMENT.md) for the full deployment runbook,
including the backup, monitoring, and scaling plans (MKT-011). The shipped
`Dockerfile` + `docker-compose.yml` bring up the registry behind a Caddy
reverse proxy and a Postgres instance reserved for the upcoming durable
store.

```bash
docker compose up --build -d
curl -fsS http://localhost/healthz
```
