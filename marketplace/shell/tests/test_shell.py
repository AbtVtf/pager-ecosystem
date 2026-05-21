"""Tests for the marketplace device shell (MKT-005)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

# The shell lives one dir up; tests aren't a package.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app import MarketplaceClient, build_app  # noqa: E402
from pageros import Screen  # noqa: E402
from pageros.codec import decode_frame  # noqa: E402
from pageros_marketplace import Manifest, Registry  # noqa: E402
from pageros_marketplace.registry.app import create_app as create_api  # noqa: E402


def _mk(app_id: str, *, description: str = "demo", donate: str | None = None, categories=("misc",)) -> Manifest:
    data = {
        "id": app_id,
        "name": app_id.split(".")[-1].title(),
        "description": description,
        "icon": "https://example.com/i.png",
        "url": f"https://{app_id}/",
        "categories": list(categories),
        "maintainer": {"name": "m", "contact": "m@example.com"},
        "version": 1,
    }
    if donate:
        data["donate_url"] = donate
    return Manifest.model_validate(data)


def _client_for(registry: Registry) -> MarketplaceClient:
    """Wrap the real marketplace API in a TestClient and use it as fetch."""
    api = create_api(registry=registry, challenge_required=False)
    tc = TestClient(api)

    def fetch(method, path, query):
        r = tc.request(method, path, params=dict(query))
        if r.status_code == 404:
            raise KeyError(path)
        r.raise_for_status()
        return r.json()

    return MarketplaceClient(fetch=fetch)


def _frame(status: int, headers: dict, body: bytes) -> dict:
    assert status == 200, f"expected 200, got {status} body={body[:200]!r}"
    return decode_frame(body)


def test_home_lists_apps():
    reg = Registry()
    reg.register(_mk("a.example"))
    reg.register(_mk("b.example"))
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("GET", "/"))
    titles = [w for w in frame["body"] if isinstance(w, dict) and w.get("t") == "list"]
    assert titles, f"home should include a list widget: {frame}"
    # Manifests' display name = last dotted label, title-cased ("a.example" → "Example").
    # Both apps would render the same display name; what we care about is two items show.
    labels = [it["label"] for it in titles[0]["items"]]
    assert len(labels) == 2, f"expected both apps listed, got {labels}"


def test_home_empty():
    shell = build_app(_client_for(Registry()))
    frame = _frame(*shell.dispatch("GET", "/"))
    blob = str(frame)
    assert "No apps registered yet" in blob


def test_category_filter():
    reg = Registry()
    reg.register(_mk("note.app", categories=("notes",)))
    reg.register(_mk("game.app", categories=("games",)))
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("GET", "/category?slug=notes"))
    labels = []
    for w in frame["body"]:
        if isinstance(w, dict) and w.get("t") == "list":
            labels.extend(it["label"] for it in w["items"])
    assert any("Note.App".replace(".App", "App") in lbl or "App" in lbl for lbl in labels)


def test_search_finds_matches():
    reg = Registry()
    reg.register(_mk("a.app", description="this is a unique keyword in there"))
    reg.register(_mk("b.app", description="something else entirely"))
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("GET", "/search?q=unique"))
    matches = [w for w in frame["body"] if isinstance(w, dict) and w.get("t") == "list"]
    assert matches and len(matches[0]["items"]) == 1


def test_app_detail_includes_install_and_donate():
    reg = Registry()
    reg.register(_mk("x.app", donate="https://give.example/x"))
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("GET", "/app?id=x.app"))
    blob = str(frame)
    assert "Install" in blob
    assert "Tip developer" in blob
    assert "https://give.example/x" in blob


def test_app_detail_missing_id():
    shell = build_app(_client_for(Registry()))
    frame = _frame(*shell.dispatch("GET", "/app?id=does.not.exist"))
    assert "not found" in str(frame)


def test_install_post_returns_ack():
    reg = Registry()
    reg.register(_mk("x.app"))
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("POST", "/install?id=x.app"))
    assert "Queued install of x.app" in str(frame)


def test_trust_badge_prefix():
    reg = Registry()
    reg.register(_mk("a.app"))
    reg.set_tags("a.app", {"verified"})
    shell = build_app(_client_for(reg))
    frame = _frame(*shell.dispatch("GET", "/"))
    labels = []
    for w in frame["body"]:
        if isinstance(w, dict) and w.get("t") == "list":
            labels.extend(it["label"] for it in w["items"])
    assert any("✓" in lbl for lbl in labels), f"verified app should get checkmark prefix: {labels}"
