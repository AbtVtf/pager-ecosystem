"""Tests for the manifest validator (SPEC §10.2)."""

from __future__ import annotations

import base64
from pathlib import Path

import pytest

from pageros_marketplace import (
    KNOWN_PERMISSIONS,
    Manifest,
    ManifestValidationError,
    load_manifest,
    validate_manifest,
    validate_manifest_yaml,
)

FIXTURES = Path(__file__).parent / "fixtures"


def _valid_dict() -> dict:
    return {
        "id": "notes.mafu.dev",
        "name": "Notes",
        "description": "A simple notepad.",
        "icon": "https://notes.app/icon.png",
        "url": "https://notes.app/",
        "categories": ["productivity"],
        "maintainer": {"name": "Jane Doe", "contact": "jane@example.com"},
        "version": 1,
    }


# --- happy paths ----------------------------------------------------------- #


def test_minimal_manifest_validates():
    m = validate_manifest(_valid_dict())
    assert isinstance(m, Manifest)
    assert m.id == "notes.mafu.dev"
    assert m.lora_compatible is False
    assert m.multi_device is False
    assert m.permissions == []
    assert m.pubkey is None
    assert m.donate_url is None


def test_full_manifest_from_yaml_file():
    m = load_manifest(FIXTURES / "valid_full.yaml")
    assert m.lora_compatible is True
    assert "notifications" in m.permissions
    assert "location" in m.permissions
    assert m.donate_url == "https://notes.app/tip"
    assert m.categories == ["productivity", "utilities"]


def test_minimal_yaml_fixture_loads():
    m = load_manifest(FIXTURES / "valid_minimal.yaml")
    assert m.id == "notes.mafu.dev"


def test_pubkey_accepts_32_bytes_b64():
    key32 = base64.b64encode(b"\x01" * 32).decode()
    data = _valid_dict() | {"pubkey": key32}
    m = validate_manifest(data)
    assert m.pubkey == key32


def test_pubkey_accepts_url_safe_b64():
    key32 = base64.urlsafe_b64encode(b"\x02" * 32).decode().rstrip("=")
    data = _valid_dict() | {"pubkey": key32}
    m = validate_manifest(data)
    assert m.pubkey == key32


def test_known_permissions_match_spec():
    # SPEC §9.4 must stay in sync with the validator's allowlist.
    assert KNOWN_PERMISSIONS == frozenset(
        {"location", "nfc", "notifications", "groups", "lora_send", "contacts"}
    )


# --- field-level rejections ------------------------------------------------ #


def _assert_field_error(exc: ManifestValidationError, field: str, fragment: str):
    matches = [e for e in exc.errors if e.field == field]
    assert matches, (
        f"expected error on field '{field}', got: "
        f"{[(e.field, e.message) for e in exc.errors]}"
    )
    assert any(fragment.lower() in e.message.lower() for e in matches), (
        f"expected message containing '{fragment}' on field '{field}', "
        f"got: {[e.message for e in matches]}"
    )


def test_rejects_non_mapping():
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest("just a string")
    _assert_field_error(ei.value, "<root>", "mapping")


def test_rejects_empty_yaml():
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest_yaml("")
    _assert_field_error(ei.value, "<root>", "empty")


def test_rejects_invalid_yaml():
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest_yaml("id: notes\n  bad indent: x")
    _assert_field_error(ei.value, "<yaml>", "invalid yaml")


def test_rejects_missing_required_fields():
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest({})
    fields = {e.field for e in ei.value.errors}
    for required in ("id", "name", "description", "icon", "url", "maintainer", "version"):
        assert required in fields, f"missing error for '{required}', got {fields}"


def test_rejects_extra_unknown_field():
    data = _valid_dict() | {"surprise": "boom"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "surprise", "not allowed")


def test_rejects_extra_unknown_field_in_maintainer():
    data = _valid_dict()
    data["maintainer"] = {"name": "x", "contact": "y", "twitter": "@x"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "maintainer.twitter", "not allowed")


@pytest.mark.parametrize(
    "bad_id",
    [
        "Notes",            # no dot, uppercase
        "notes",            # no dot
        "Notes.Mafu.Dev",   # uppercase
        ".notes.dev",       # leading dot
        "notes..dev",       # empty label
        "notes.dev.",       # trailing dot
        "notes.-dev",       # leading hyphen in label
        "notes.dev-",       # trailing hyphen in label
        "notes.dev/extra",  # slash
    ],
)
def test_rejects_bad_id_format(bad_id):
    data = _valid_dict() | {"id": bad_id}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "id", "reverse-dns")


def test_rejects_non_http_url():
    data = _valid_dict() | {"url": "ftp://notes.app/"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "url", "http")


def test_rejects_url_without_host():
    data = _valid_dict() | {"icon": "https:///icon.png"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "icon", "host")


def test_rejects_bad_donate_url():
    data = _valid_dict() | {"donate_url": "javascript:alert(1)"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "donate_url", "http")


def test_rejects_pubkey_wrong_length():
    short = base64.b64encode(b"\x00" * 16).decode()
    data = _valid_dict() | {"pubkey": short}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "pubkey", "32 bytes")


def test_rejects_pubkey_non_base64():
    data = _valid_dict() | {"pubkey": "this is not base64!!!"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "pubkey", "base64")


def test_rejects_unknown_permission():
    data = _valid_dict() | {"permissions": ["root", "notifications"]}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "permissions", "unknown permission")


def test_rejects_duplicate_permissions():
    data = _valid_dict() | {"permissions": ["nfc", "nfc"]}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "permissions", "duplicate")


def test_rejects_empty_categories():
    data = _valid_dict() | {"categories": []}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "categories", "at least 1")


def test_rejects_bad_category_slug():
    data = _valid_dict() | {"categories": ["Productivity!"]}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "categories", "invalid category slug")


def test_rejects_duplicate_categories():
    data = _valid_dict() | {"categories": ["productivity", "productivity"]}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "categories", "duplicate")


def test_rejects_version_zero():
    data = _valid_dict() | {"version": 0}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "version", "greater than or equal")


def test_rejects_version_non_integer():
    # 1.5 is a real float — must not be silently coerced to int.
    data = _valid_dict() | {"version": 1.5}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    matches = [e for e in ei.value.errors if e.field == "version"]
    assert matches


def test_rejects_version_string():
    data = _valid_dict() | {"version": "v1"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    matches = [e for e in ei.value.errors if e.field == "version"]
    assert matches


def test_rejects_empty_maintainer_name():
    data = _valid_dict()
    data["maintainer"] = {"name": "", "contact": "x@y.z"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "maintainer.name", "at least 1")


def test_rejects_missing_maintainer_contact():
    data = _valid_dict()
    data["maintainer"] = {"name": "Jane"}
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    _assert_field_error(ei.value, "maintainer.contact", "required")


def test_lora_compatible_must_be_bool():
    data = _valid_dict() | {"lora_compatible": "yes"}
    # pydantic will coerce string "yes"? It will not — bool strict_mode is off by default
    # but "yes" is not in the recognized literal set. Either way, ensure result is sane.
    try:
        m = validate_manifest(data)
        # If it coerced, must be a real bool.
        assert isinstance(m.lora_compatible, bool)
    except ManifestValidationError as exc:
        assert any(e.field == "lora_compatible" for e in exc.errors)


def test_error_message_aggregates_multiple_fields():
    data = _valid_dict() | {
        "id": "BAD",
        "version": 0,
        "categories": [],
    }
    with pytest.raises(ManifestValidationError) as ei:
        validate_manifest(data)
    fields = {e.field for e in ei.value.errors}
    assert {"id", "version", "categories"}.issubset(fields)
    # The rendered exception lists each error.
    text = str(ei.value)
    assert "id:" in text
    assert "version:" in text
    assert "categories:" in text


def test_load_manifest_missing_file():
    with pytest.raises(ManifestValidationError) as ei:
        load_manifest("/no/such/path.yaml")
    _assert_field_error(ei.value, "<file>", "not found")
