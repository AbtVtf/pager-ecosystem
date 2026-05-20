"""Tests for featured curation tooling (MKT-010).

Acceptance: admins can pin an app to "Featured"; pinned apps show up at the
top of the listing the Shell's in-device "Apps" home (MKT-005) consumes.
"""

from __future__ import annotations

import time

import pytest
from fastapi.testclient import TestClient

from pageros_marketplace import (
    ModerationStore,
    Registry,
    create_app,
)


def _manifest(app_id: str) -> dict:
    return {
        "id": app_id,
        "name": app_id.split(".", 1)[0].title(),
        "description": f"App {app_id}.",
        "icon": f"https://{app_id}/icon.png",
        "url": f"https://{app_id}/",
        "categories": ["productivity"],
        "maintainer": {"name": "Maint", "contact": f"mail@{app_id}"},
        "version": 1,
    }


@pytest.fixture
def registry() -> Registry:
    return Registry()


@pytest.fixture
def moderation() -> ModerationStore:
    return ModerationStore()


@pytest.fixture
def client(registry: Registry, moderation: ModerationStore) -> TestClient:
    return TestClient(
        create_app(
            registry,
            moderation=moderation,
            challenge_required=False,
        )
    )


@pytest.fixture
def gated_client(registry: Registry, moderation: ModerationStore) -> TestClient:
    return TestClient(
        create_app(
            registry,
            moderation=moderation,
            challenge_required=False,
            admin_token="s3cret",
        )
    )


def _seed(client: TestClient, *ids: str) -> None:
    for app_id in ids:
        client.post("/apps", json=_manifest(app_id)).raise_for_status()


def _auth() -> dict[str, str]:
    return {"Authorization": "Bearer s3cret"}


# --- store: featured_at tracking ----------------------------------------- #


def test_registry_records_featured_at_when_tag_added(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    entry = registry.add_tag("notes.mafu.dev", "featured")
    assert entry.featured_at is not None
    assert entry.is_featured


def test_registry_clears_featured_at_when_tag_removed(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    registry.add_tag("notes.mafu.dev", "featured")
    entry = registry.remove_tag("notes.mafu.dev", "featured")
    assert entry.featured_at is None
    assert not entry.is_featured


def test_registry_featured_at_unchanged_for_other_tag_mutations(
    registry: Registry,
):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    entry1 = registry.add_tag("notes.mafu.dev", "featured")
    pinned_at = entry1.featured_at
    # Touching an unrelated tag must NOT bump the featured_at.
    entry2 = registry.add_tag("notes.mafu.dev", "verified")
    assert entry2.featured_at == pinned_at
    entry3 = registry.remove_tag("notes.mafu.dev", "verified")
    assert entry3.featured_at == pinned_at


def test_registry_set_tags_handles_featured_transitions(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    e1 = registry.set_tags("notes.mafu.dev", ["featured", "verified"])
    assert e1.featured_at is not None
    e2 = registry.set_tags("notes.mafu.dev", ["verified"])  # drop featured
    assert e2.featured_at is None
    e3 = registry.set_tags("notes.mafu.dev", ["featured"])  # re-add featured
    assert e3.featured_at is not None and e3.featured_at >= e1.featured_at


def test_registry_pin_featured_re_pins_with_fresh_timestamp(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    e1 = registry.pin_featured("notes.mafu.dev")
    time.sleep(0.01)
    e2 = registry.pin_featured("notes.mafu.dev")
    assert e2.is_featured
    assert e2.featured_at > e1.featured_at, "re-pin must bump featured_at"


def test_registry_unpin_featured_is_idempotent(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest("notes.mafu.dev")))
    e1 = registry.unpin_featured("notes.mafu.dev")  # not featured to begin with
    assert e1.featured_at is None
    registry.pin_featured("notes.mafu.dev")
    e2 = registry.unpin_featured("notes.mafu.dev")
    assert not e2.is_featured
    assert e2.featured_at is None
    # Idempotent: another unpin keeps state.
    e3 = registry.unpin_featured("notes.mafu.dev")
    assert not e3.is_featured


# --- store: ordering ----------------------------------------------------- #


def test_registry_list_featured_orders_most_recent_first(registry: Registry):
    from pageros_marketplace import Manifest

    for app_id in ("a.example.dev", "b.example.dev", "c.example.dev"):
        registry.register(Manifest.model_validate(_manifest(app_id)))

    registry.pin_featured("b.example.dev")
    time.sleep(0.01)
    registry.pin_featured("a.example.dev")
    time.sleep(0.01)
    registry.pin_featured("c.example.dev")

    featured, total = registry.list_featured()
    assert total == 3
    assert [e.manifest.id for e in featured] == [
        "c.example.dev",
        "a.example.dev",
        "b.example.dev",
    ]


def test_registry_list_featured_first_sorts_featured_before_rest(
    registry: Registry,
):
    from pageros_marketplace import Manifest

    for app_id in ("a.example.dev", "b.example.dev", "c.example.dev"):
        registry.register(Manifest.model_validate(_manifest(app_id)))

    # Pin b first, then c → expected order: c, b, then a (alpha).
    registry.pin_featured("b.example.dev")
    time.sleep(0.01)
    registry.pin_featured("c.example.dev")

    page, total = registry.list(featured_first=True)
    assert total == 3
    assert [e.manifest.id for e in page] == [
        "c.example.dev",
        "b.example.dev",
        "a.example.dev",
    ]


def test_registry_default_list_unchanged(registry: Registry):
    from pageros_marketplace import Manifest

    for app_id in ("b.example.dev", "a.example.dev"):
        registry.register(Manifest.model_validate(_manifest(app_id)))
    registry.pin_featured("b.example.dev")

    # Default ordering remains id ASC for back-compat.
    page, _ = registry.list()
    assert [e.manifest.id for e in page] == ["a.example.dev", "b.example.dev"]


# --- HTTP: admin pin/unpin ----------------------------------------------- #


def test_admin_pin_endpoint_pins_and_returns_featured_at(client: TestClient):
    _seed(client, "notes.mafu.dev")
    r = client.post("/admin/apps/notes.mafu.dev/featured")
    assert r.status_code == 200, r.text
    body = r.json()
    assert "featured" in body["tags"]
    assert body["featured_at"] is not None


def test_admin_pin_endpoint_repin_bumps_timestamp(client: TestClient):
    _seed(client, "notes.mafu.dev")
    r1 = client.post("/admin/apps/notes.mafu.dev/featured").json()
    time.sleep(0.01)
    r2 = client.post("/admin/apps/notes.mafu.dev/featured").json()
    assert r2["featured_at"] > r1["featured_at"]


def test_admin_unpin_endpoint_clears_featured_at(client: TestClient):
    _seed(client, "notes.mafu.dev")
    client.post("/admin/apps/notes.mafu.dev/featured")
    r = client.delete("/admin/apps/notes.mafu.dev/featured")
    assert r.status_code == 200
    body = r.json()
    assert "featured" not in body["tags"]
    assert body["featured_at"] is None


def test_admin_pin_unknown_app_is_404(client: TestClient):
    r = client.post("/admin/apps/ghost.example.dev/featured")
    assert r.status_code == 404
    assert r.json()["error"] == "not_found"


def test_admin_pin_requires_admin_token(gated_client: TestClient):
    gated_client.post("/apps", json=_manifest("notes.mafu.dev")).raise_for_status()
    r = gated_client.post("/admin/apps/notes.mafu.dev/featured")
    assert r.status_code == 401
    r = gated_client.post(
        "/admin/apps/notes.mafu.dev/featured", headers=_auth()
    )
    assert r.status_code == 200


def test_admin_pin_logs_moderation_action(
    client: TestClient, moderation: ModerationStore
):
    _seed(client, "notes.mafu.dev")
    client.post("/admin/apps/notes.mafu.dev/featured")
    client.delete("/admin/apps/notes.mafu.dev/featured")
    log = client.get("/moderation/log").json()
    actions = [e["action"] for e in log["items"]]
    assert "app.featured.pin" in actions
    assert "app.featured.unpin" in actions


# --- HTTP: public surfaces ---------------------------------------------- #


def test_public_featured_endpoint_returns_only_pinned_apps(client: TestClient):
    _seed(client, "a.example.dev", "b.example.dev", "c.example.dev")
    client.post("/admin/apps/b.example.dev/featured").raise_for_status()
    time.sleep(0.01)
    client.post("/admin/apps/c.example.dev/featured").raise_for_status()

    body = client.get("/apps/featured").json()
    ids = [item["manifest"]["id"] for item in body["items"]]
    assert ids == ["c.example.dev", "b.example.dev"]
    assert body["total"] == 2


def test_public_featured_endpoint_is_unauthenticated(gated_client: TestClient):
    gated_client.post("/apps", json=_manifest("notes.mafu.dev")).raise_for_status()
    gated_client.post(
        "/admin/apps/notes.mafu.dev/featured", headers=_auth()
    ).raise_for_status()
    # No Authorization header — should still work.
    r = gated_client.get("/apps/featured")
    assert r.status_code == 200
    assert len(r.json()["items"]) == 1


def test_public_featured_endpoint_paginates(client: TestClient):
    ids = [f"app{i}.example.dev" for i in range(5)]
    _seed(client, *ids)
    for app_id in ids:
        client.post(f"/admin/apps/{app_id}/featured").raise_for_status()

    page = client.get("/apps/featured?limit=2&offset=0").json()
    assert page["total"] == 5
    assert len(page["items"]) == 2

    page2 = client.get("/apps/featured?limit=2&offset=2").json()
    assert len(page2["items"]) == 2
    # Pages do not overlap.
    first = {it["manifest"]["id"] for it in page["items"]}
    second = {it["manifest"]["id"] for it in page2["items"]}
    assert first.isdisjoint(second)


def test_apps_featured_first_lists_pinned_apps_at_top(client: TestClient):
    """The Shell home (MKT-005) uses featured_first=true; pinned apps must lead."""
    _seed(client, "a.example.dev", "b.example.dev", "c.example.dev")
    client.post("/admin/apps/b.example.dev/featured").raise_for_status()

    body = client.get("/apps?featured_first=true").json()
    ids = [item["manifest"]["id"] for item in body["items"]]
    assert ids[0] == "b.example.dev"
    assert ids == ["b.example.dev", "a.example.dev", "c.example.dev"]


def test_apps_default_list_unchanged_for_back_compat(client: TestClient):
    """Without featured_first=true, the default sort stays id ASC."""
    _seed(client, "b.example.dev", "a.example.dev")
    client.post("/admin/apps/b.example.dev/featured").raise_for_status()

    body = client.get("/apps").json()
    ids = [item["manifest"]["id"] for item in body["items"]]
    assert ids == ["a.example.dev", "b.example.dev"]


def test_openapi_lists_curation_endpoints(client: TestClient):
    paths = client.get("/openapi.json").json()["paths"]
    assert "/apps/featured" in paths
    assert "/admin/apps/{app_id}/featured" in paths


def test_app_record_exposes_featured_at(client: TestClient):
    _seed(client, "notes.mafu.dev")
    assert client.get("/apps/notes.mafu.dev").json()["featured_at"] is None
    client.post("/admin/apps/notes.mafu.dev/featured").raise_for_status()
    assert client.get("/apps/notes.mafu.dev").json()["featured_at"] is not None
