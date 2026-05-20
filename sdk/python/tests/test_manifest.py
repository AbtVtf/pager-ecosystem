"""Tests for the PY-010 manifest generator.

Cross-validates SDK-produced YAML against the canonical MKT-001 validator in
``marketplace/api/pageros_marketplace`` so the two implementations cannot
drift silently.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
import yaml

from pageros.manifest import App, Maintainer


# Make the in-repo marketplace validator importable without adding a runtime
# dep on the marketplace package; PY-010's acceptance criterion is exactly
# this cross-validation, so the test environment carries the responsibility.
_MARKETPLACE_API = (
    Path(__file__).resolve().parents[3] / "marketplace" / "api"
)
if str(_MARKETPLACE_API) not in sys.path:
    sys.path.insert(0, str(_MARKETPLACE_API))

from pageros_marketplace.manifest import (  # noqa: E402  (sys.path insert above)
    Manifest,
    ManifestValidationError,
    validate_manifest_yaml,
)


def _minimal_app(**overrides) -> App:
    kwargs = dict(
        id="notes.mafu.dev",
        name="Notes",
        description="A simple notepad.",
        icon="https://notes.app/icon.png",
        url="https://notes.app/",
        maintainer=Maintainer(name="Jane Doe", contact="jane@example.com"),
        categories=["productivity"],
    )
    kwargs.update(overrides)
    return App(**kwargs)


def test_minimal_manifest_validates_against_mkt001() -> None:
    text = _minimal_app().manifest()
    parsed: Manifest = validate_manifest_yaml(text)
    assert parsed.id == "notes.mafu.dev"
    assert parsed.version == 1
    assert parsed.permissions == []
    assert parsed.lora_compatible is False


def test_full_manifest_round_trips_through_validator() -> None:
    app = _minimal_app(
        pubkey="V170Ggb4s4sjtCxklLrHe5ymXKse45YVptlXwVb63jM=",
        permissions=["location", "notifications"],
        lora_compatible=True,
        multi_device=True,
        donate_url="https://notes.app/donate",
        categories=["productivity", "writing"],
        version=3,
    )
    parsed = validate_manifest_yaml(app.manifest())
    assert parsed.lora_compatible is True
    assert parsed.multi_device is True
    assert parsed.donate_url == "https://notes.app/donate"
    assert parsed.permissions == ["location", "notifications"]
    assert parsed.categories == ["productivity", "writing"]
    assert parsed.version == 3


def test_manifest_yaml_preserves_spec_field_order() -> None:
    # The §10.2 example documents a specific field order — generated YAML
    # should read the same way so it's diffable against the spec.
    text = _minimal_app().manifest()
    keys = [line.split(":", 1)[0] for line in text.splitlines() if line and not line.startswith(" ")]
    # Required keys appear in spec order; optional pubkey/donate_url are absent here.
    assert keys[:5] == ["id", "name", "description", "icon", "url"]
    assert keys.index("maintainer") < keys.index("version")


def test_omits_optional_fields_when_unset() -> None:
    data = yaml.safe_load(_minimal_app().manifest())
    assert "pubkey" not in data
    assert "donate_url" not in data


def test_includes_pubkey_and_donate_url_when_set() -> None:
    app = _minimal_app(
        pubkey="V170Ggb4s4sjtCxklLrHe5ymXKse45YVptlXwVb63jM=",
        donate_url="https://notes.app/donate",
    )
    data = yaml.safe_load(app.manifest())
    assert data["pubkey"] == "V170Ggb4s4sjtCxklLrHe5ymXKse45YVptlXwVb63jM="
    assert data["donate_url"] == "https://notes.app/donate"


def test_invalid_id_is_rejected_by_validator() -> None:
    bad = _minimal_app(id="NotReverseDNS")
    with pytest.raises(ManifestValidationError):
        validate_manifest_yaml(bad.manifest())


def test_unknown_permission_is_rejected_by_validator() -> None:
    bad = _minimal_app(permissions=["camera"])  # not in KNOWN_PERMISSIONS
    with pytest.raises(ManifestValidationError):
        validate_manifest_yaml(bad.manifest())


def test_categories_required_to_be_nonempty() -> None:
    bad = _minimal_app(categories=[])
    with pytest.raises(ManifestValidationError):
        validate_manifest_yaml(bad.manifest())


def test_maintainer_block_serialises_as_mapping() -> None:
    data = yaml.safe_load(_minimal_app().manifest())
    assert data["maintainer"] == {"name": "Jane Doe", "contact": "jane@example.com"}


def test_manifest_method_returns_str() -> None:
    assert isinstance(_minimal_app().manifest(), str)
