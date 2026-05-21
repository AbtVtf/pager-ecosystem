# `marketplace/web` — PagerOS Marketplace public web UI (MKT-004)

Server-rendered HTML for `marketplace.pageros.org`. Browse, full-text search,
category filter, app detail page with icon + description + donate link.

## Stack

- **FastAPI** for routing.
- **Python string templates** (no Jinja) for HTML, escaped via `html.escape`
  — matches the MKT-006 admin UI convention.
- **In-process `Registry`** for v1. The web app accepts a `Registry`
  (and optional `ModerationStore`) reference; the API and web typically
  run in the same Python process so they share state. Splitting them
  over HTTP is a deployment refactor for MKT-011.

## Routes

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | Landing / browse — list of apps, category filter, search box. |
| `GET` | `/browse` | Same as `/` (canonical browse URL). |
| `GET` | `/search?q=...` | Search results (HTML). |
| `GET` | `/app/{app_id}` | App detail: icon, name, description, categories, tags, donate link. |
| `GET` | `/static/style.css` | Minimal stylesheet. |
| `GET` | `/healthz` | Liveness probe (returns `{"status":"ok"}`). |

The detail page surfaces a **Tip developer** link when the manifest has
a `donate_url` (SPEC §10.2 / §10.4).

## Run locally

```bash
# From marketplace/web/:
pip install -e ../api -e .[serve,dev]
pageros-marketplace-web serve --host 127.0.0.1 --port 8081
```

## Tests

```bash
pytest -q
```
