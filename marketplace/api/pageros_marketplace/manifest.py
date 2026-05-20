"""PagerOS app manifest schema + validator.

Implements SPEC.md §10.2 (manifest schema) and §9.4 (permission allowlist).
"""

from __future__ import annotations

import base64
import binascii
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlparse

import yaml
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
    field_validator,
)


KNOWN_PERMISSIONS: frozenset[str] = frozenset(
    {
        "location",
        "nfc",
        "notifications",
        "groups",
        "lora_send",
        "contacts",
    }
)

# Reverse-DNS-style id: lowercase labels separated by dots, at least two labels.
# Each label: alphanumeric, hyphens allowed internally, must start & end alnum.
_ID_LABEL = r"[a-z0-9]([a-z0-9-]*[a-z0-9])?"
_ID_PATTERN = re.compile(rf"^{_ID_LABEL}(\.{_ID_LABEL})+$")

# Category slug: lowercase, hyphens, underscores allowed.
_CATEGORY_PATTERN = re.compile(r"^[a-z0-9]([a-z0-9_-]*[a-z0-9])?$")

X25519_KEY_BYTES = 32


class Maintainer(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    name: str = Field(min_length=1, max_length=120)
    contact: str = Field(min_length=1, max_length=200)


class Manifest(BaseModel):
    """The canonical PagerOS app manifest (SPEC §10.2)."""

    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    id: str = Field(min_length=3, max_length=253)
    name: str = Field(min_length=1, max_length=64)
    description: str = Field(min_length=1, max_length=280)
    icon: str = Field(min_length=1)
    url: str = Field(min_length=1)
    pubkey: str | None = None
    permissions: list[str] = Field(default_factory=list)
    lora_compatible: bool = False
    multi_device: bool = False
    donate_url: str | None = None
    categories: list[str] = Field(default_factory=list, min_length=1)
    maintainer: Maintainer
    version: int = Field(ge=1)

    @field_validator("id")
    @classmethod
    def _id_format(cls, v: str) -> str:
        if not _ID_PATTERN.match(v):
            raise ValueError(
                "must be reverse-DNS style (e.g. notes.mafu.dev): "
                "lowercase labels separated by dots, alphanumeric with internal hyphens"
            )
        return v

    @field_validator("icon", "url")
    @classmethod
    def _http_url(cls, v: str) -> str:
        _require_http_url(v)
        return v

    @field_validator("donate_url")
    @classmethod
    def _optional_http_url(cls, v: str | None) -> str | None:
        if v is None:
            return v
        _require_http_url(v)
        return v

    @field_validator("pubkey")
    @classmethod
    def _pubkey_b64(cls, v: str | None) -> str | None:
        if v is None:
            return v
        v = v.strip()
        if not v:
            raise ValueError("must be a non-empty base64-encoded X25519 public key")
        raw = _decode_base64(v)
        if len(raw) != X25519_KEY_BYTES:
            raise ValueError(
                f"X25519 public keys are {X25519_KEY_BYTES} bytes; "
                f"decoded {len(raw)} bytes"
            )
        return v

    @field_validator("permissions")
    @classmethod
    def _permissions_allowlist(cls, v: list[str]) -> list[str]:
        unknown = [p for p in v if p not in KNOWN_PERMISSIONS]
        if unknown:
            allowed = ", ".join(sorted(KNOWN_PERMISSIONS))
            raise ValueError(
                f"unknown permission(s): {', '.join(unknown)}. "
                f"Allowed: {allowed}"
            )
        if len(set(v)) != len(v):
            raise ValueError("permissions must not contain duplicates")
        return v

    @field_validator("categories")
    @classmethod
    def _categories_slug(cls, v: list[str]) -> list[str]:
        if len(set(v)) != len(v):
            raise ValueError("categories must not contain duplicates")
        bad = [c for c in v if not _CATEGORY_PATTERN.match(c)]
        if bad:
            raise ValueError(
                f"invalid category slug(s): {', '.join(bad)}. "
                "Use lowercase alphanumerics, hyphens, or underscores."
            )
        return v


@dataclass(frozen=True)
class ManifestError:
    """Single field-level validation error."""

    field: str
    message: str

    def __str__(self) -> str:
        return f"{self.field}: {self.message}"


class ManifestValidationError(Exception):
    """Raised when a manifest fails validation. Aggregates per-field errors."""

    def __init__(self, errors: Iterable[ManifestError]):
        self.errors: list[ManifestError] = list(errors)
        super().__init__(self._render())

    def _render(self) -> str:
        if not self.errors:
            return "manifest validation failed"
        lines = [f"manifest validation failed ({len(self.errors)} error(s)):"]
        lines.extend(f"  - {e}" for e in self.errors)
        return "\n".join(lines)


def validate_manifest(data: Any) -> Manifest:
    """Validate a decoded manifest dict and return a `Manifest` instance.

    Raises `ManifestValidationError` with a clear list of field errors if invalid.
    """
    if not isinstance(data, dict):
        raise ManifestValidationError(
            [ManifestError("<root>", "manifest must be a YAML mapping (key/value object)")]
        )
    try:
        return Manifest.model_validate(data)
    except ValidationError as exc:
        raise ManifestValidationError(_translate_pydantic(exc)) from exc


def validate_manifest_yaml(text: str) -> Manifest:
    """Validate manifest YAML source text."""
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise ManifestValidationError(
            [ManifestError("<yaml>", f"invalid YAML: {exc}")]
        ) from exc
    if data is None:
        raise ManifestValidationError(
            [ManifestError("<root>", "manifest file is empty")]
        )
    return validate_manifest(data)


def load_manifest(path: str | Path) -> Manifest:
    """Read and validate a manifest YAML file from disk."""
    p = Path(path)
    if not p.exists():
        raise ManifestValidationError(
            [ManifestError("<file>", f"manifest file not found: {p}")]
        )
    return validate_manifest_yaml(p.read_text(encoding="utf-8"))


# --- helpers --------------------------------------------------------------- #


def _require_http_url(v: str) -> None:
    try:
        parsed = urlparse(v)
    except ValueError as exc:
        raise ValueError(f"invalid URL: {exc}") from exc
    if parsed.scheme not in ("http", "https"):
        raise ValueError("URL must use the http or https scheme")
    if not parsed.netloc:
        raise ValueError("URL must include a host")


def _decode_base64(v: str) -> bytes:
    # Accept either standard or URL-safe base64; tolerate missing padding.
    s = v.replace("-", "+").replace("_", "/")
    padding = (-len(s)) % 4
    s_padded = s + ("=" * padding)
    try:
        return base64.b64decode(s_padded, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(f"not valid base64: {exc}") from exc


def _translate_pydantic(exc: ValidationError) -> list[ManifestError]:
    """Convert pydantic v2 ValidationError into our flat ManifestError list."""
    errors: list[ManifestError] = []
    for err in exc.errors():
        field = ".".join(str(p) for p in err["loc"]) or "<root>"
        msg = err.get("msg") or "invalid value"
        etype = err.get("type", "")
        # Strip the pydantic "Value error, " prefix added for custom validators.
        if msg.startswith("Value error, "):
            msg = msg[len("Value error, "):]
        # Make a couple of pydantic's stock messages friendlier.
        if etype == "missing":
            msg = "required field is missing"
        elif etype == "extra_forbidden":
            msg = "unknown field is not allowed"
        errors.append(ManifestError(field, msg))
    return errors
