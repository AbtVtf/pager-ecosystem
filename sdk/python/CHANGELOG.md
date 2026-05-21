# Changelog

All notable changes to the `pageros` Python SDK are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Version is tracked in `pageros/__init__.py:__version__` (single source of
truth). The release workflow (`.github/workflows/pysdk-release.yml`)
asserts that the tag suffix `pysdk/vX.Y.Z` matches `__version__` before
publishing to PyPI.

## [Unreleased]

### Added
- PY-001..PY-011: initial SDK surface — `App`, routes, dev server, codec,
  signing, encryption, push, group helpers (`broadcast`,
  `@app.group_event`), widget builders (`Screen`, `Text`, `List`, `Form`,
  `Input`, `Button`, `Image`, `Map`, `Notification`, `PresenceList`,
  `Chat`), manifest, LoRa size-budget warning, `ctx` object.
- PY-013: PyPI publishing — `pyproject.toml` metadata + classifiers,
  `py.typed` marker, GitHub Actions CI (`pysdk-ci.yml`) and release
  (`pysdk-release.yml`) workflows wired to PyPI Trusted Publishing.
- PY-012: PROTO-005 conformance — `pageros.conformance` HTTP adapter
  (`POST /conformance/encode|decode`) drives all 86 cross-language
  test vectors green; `python -m pageros.conformance` boots the
  adapter, and `tests/test_conformance.py` integration test asserts
  100% pass from the runner in pytest.

## [0.0.1] - unreleased

Initial pre-release placeholder. Cut the first real release by bumping
`__version__` to `0.1.0`, adding a `[0.1.0]` entry above this line, and
tagging `pysdk/v0.1.0`.
