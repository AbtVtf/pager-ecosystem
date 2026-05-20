"""App manifest generator (PY-010).

Provides a minimal :class:`App` whose ``manifest()`` method renders a YAML
manifest conforming to the Marketplace schema (SPEC.md §10.2, MKT-001).

The fuller :class:`App` runtime (decorators, request handling) lands with
PY-001; this module only covers the manifest-config surface so apps can be
described, validated, and published via the Marketplace today.
"""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any

import yaml


@dataclass(frozen=True)
class Maintainer:
    """Manifest maintainer block."""

    name: str
    contact: str

    def as_dict(self) -> dict[str, str]:
        return {"name": self.name, "contact": self.contact}


@dataclass
class App:
    """A PagerOS app, described well enough to publish.

    Fields mirror the Marketplace manifest schema 1:1. Optional fields default
    to values that render as a minimal-but-valid manifest. Defaults stay close
    to "off" so manifests don't accidentally advertise capabilities the app
    doesn't actually implement (LoRa, multi-device, permissions).
    """

    id: str
    name: str
    description: str
    icon: str
    url: str
    maintainer: Maintainer
    categories: list[str] = field(default_factory=list)
    version: int = 1
    pubkey: str | None = None
    permissions: list[str] = field(default_factory=list)
    lora_compatible: bool = False
    multi_device: bool = False
    donate_url: str | None = None

    def manifest_dict(self) -> dict[str, Any]:
        """Return the manifest as a plain dict.

        Output ordering follows SPEC §10.2 so generated YAML reads top-to-bottom
        the same way the spec presents it.
        """
        data: dict[str, Any] = {
            "id": self.id,
            "name": self.name,
            "description": self.description,
            "icon": self.icon,
            "url": self.url,
        }
        if self.pubkey is not None:
            data["pubkey"] = self.pubkey
        data["permissions"] = list(self.permissions)
        data["lora_compatible"] = self.lora_compatible
        data["multi_device"] = self.multi_device
        if self.donate_url is not None:
            data["donate_url"] = self.donate_url
        data["categories"] = list(self.categories)
        data["maintainer"] = self.maintainer.as_dict()
        data["version"] = self.version
        return data

    def manifest(self) -> str:
        """Return the YAML manifest text (MKT-001 schema)."""
        return yaml.safe_dump(
            self.manifest_dict(),
            sort_keys=False,
            allow_unicode=True,
            default_flow_style=False,
        )


__all__ = ["App", "Maintainer"]
