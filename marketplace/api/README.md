# pageros-marketplace

PagerOS marketplace registry. Currently implements **MKT-001** — the YAML manifest schema and validator from [SPEC.md §10.2](../../SPEC.md).

Future tasks in this package: registry REST API (MKT-002), DNS TXT challenge service (MKT-003), moderation queue (MKT-006), etc.

## Install (dev)

```bash
cd marketplace/api
python -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'
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

## Schema

See `pageros_marketplace/manifest.py` and SPEC.md §10.2 for the canonical schema. Recognized permissions come from SPEC §9.4.
