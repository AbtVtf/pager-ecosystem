# pageros-marketplace

Server-side of the PagerOS marketplace registry (SPEC.md §10).

| Task | What | Status |
|---|---|---|
| MKT-001 | YAML manifest schema + validator | done |
| MKT-002 | Registry REST API (CRUD apps, list, search) + OpenAPI doc | done |
| MKT-011 | Deployment artifacts + ops plan (registry portion) | scaffolded, see `DEPLOYMENT.md` |
| MKT-003 | DNS TXT challenge service | upcoming |
| MKT-006 | Moderation queue + trust tagging | upcoming |

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
| `POST` | `/apps` | Register a new app from a manifest. Rejects duplicates by `id` (`409`). |
| `GET` | `/apps` | List apps. Supports `offset`, `limit`, optional `category` filter. |
| `GET` | `/apps/{app_id}` | Get one app by its manifest `id`. |
| `PUT` | `/apps/{app_id}` | Replace an app's manifest. `version` must strictly increase. |
| `DELETE` | `/apps/{app_id}` | Remove an app from the registry. |
| `GET` | `/apps/search?q=...` | Case-insensitive search across `id`, `name`, `description`, `categories`. |
| `GET` | `/healthz` | Liveness probe. |

Manifests are validated against the SPEC §10.2 schema on every write — invalid
submissions get a structured 422 (FastAPI's standard validation envelope) or a
400 for higher-level errors. DNS TXT verification (MKT-003) and moderation
tagging (MKT-006) layer on top of this surface; this milestone only delivers
the unauthenticated CRUD surface.

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
