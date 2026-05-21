# pageros (Python SDK)

Python SDK for building PagerOS apps. See repo root `SPEC.md` and `TASKS.md`.

## Install

```
pip install pageros
```

Requires Python 3.10+. The package ships PEP 561 inline type hints (a
`py.typed` marker), so mypy/pyright pick up SDK types automatically.

## Dev server (PY-009)

Run a user app with auto-reload on file change:

```
python -m pageros.devserver --app path/to/app.py --host 127.0.0.1 --port 8000
```

`pagerctl dev` (CLI-002) wraps this entry point and pairs it with the simulator.

## LoRa size budget (PY-011)

Apps that set `lora_compatible=True` must keep encoded Frames at or below
200 bytes so they fit in a single LoRa packet. The SDK exposes a helper
that logs a warning when a Frame exceeds that budget:

```python
from pageros import check_frame_size

check_frame_size(encoded_frame, lora_compatible=app.lora_compatible)
```

`encoded_frame` may be the CBOR bytes (any bytes-like) or its length as an
`int`. Pass `frame_label="GET /home"` to include the route in the warning.

## Releasing to PyPI (PY-013)

Releases are published on a tag push that follows the convention
`pysdk/vX.Y.Z`. The release workflow asserts that
`pageros.__version__` matches the tag suffix and fails fast if they
drift — `__init__.py:__version__` is the single source of truth.

### One-time setup

1. Register PyPI **Trusted Publishing** for the `pageros` project:
   - Owner: this GitHub org/repo
   - Workflow filename: `pysdk-release.yml`
   - Environment name: `pypi`
2. Create a `pypi` GitHub Environment on the repo (no secrets required —
   trusted publishing uses OIDC).

### Cutting a release

```
# 1. Bump the version in two places:
#    - sdk/python/pageros/__init__.py: __version__ = "1.2.3"
#    - sdk/python/CHANGELOG.md: new entry under [Unreleased] → [1.2.3]
git commit -am "[PY-013] pysdk v1.2.3"
git push origin main

# 2. Tag the commit and push:
git tag pysdk/v1.2.3
git push origin pysdk/v1.2.3

# 3. Watch the `pysdk release` GitHub Actions run:
#    - build: pytest + sdist/wheel + twine check + artifact upload
#    - publish: PyPI upload via OIDC + GitHub Release with attached
#      sdist/wheel files
```

The `pysdk ci` workflow runs on every PR touching `sdk/python/`
(cross-OS pytest matrix on supported Python versions + `python -m build`
+ `twine check`). A `workflow_dispatch` trigger on `pysdk-release.yml`
with `dry_run: true` builds the artifacts without publishing — useful
for verifying packaging changes before tagging.
