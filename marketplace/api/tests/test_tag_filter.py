"""MKT-009: Registry.list ?tag= filter."""

from pageros_marketplace import Manifest, Registry
from pageros_marketplace.registry.store import InvalidTagError
import pytest


def _mk(i: str) -> Manifest:
    return Manifest.model_validate({
        "id": i, "name": i, "description": "x",
        "icon": "https://example.com/i.png", "url": f"https://{i}/",
        "categories": ["misc"],
        "maintainer": {"name": "n", "contact": "n@example.com"},
        "version": 1,
    })


def test_list_filters_by_verified_tag():
    r = Registry()
    r.register(_mk("a.app"))
    r.register(_mk("b.app"))
    r.set_tags("a.app", {"verified"})
    entries, total = r.list(tag="verified")
    assert [e.id for e in entries] == ["a.app"]
    assert total == 1


def test_list_unverified_returns_untagged():
    r = Registry()
    r.register(_mk("a.app"))
    r.register(_mk("b.app"))
    r.set_tags("a.app", {"verified"})
    entries, _ = r.list(tag="unverified")
    assert [e.id for e in entries] == ["b.app"]


def test_list_rejects_unknown_tag():
    r = Registry()
    with pytest.raises(InvalidTagError):
        r.list(tag="bogus")
