"""Tests for the moderation queue + admin tooling (MKT-006)."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from pageros_marketplace import (
    ALLOWED_TAGS,
    InvalidTagError,
    ModerationStore,
    Registry,
    ReportStatus,
    create_app,
)


APP_ID = "notes.mafu.dev"


def _manifest(*, id: str = APP_ID, version: int = 1) -> dict:
    return {
        "id": id,
        "name": "Notes",
        "description": "A simple notepad.",
        "icon": "https://notes.app/icon.png",
        "url": "https://notes.app/",
        "categories": ["productivity"],
        "maintainer": {"name": "Jane Doe", "contact": "jane@example.com"},
        "version": version,
    }


@pytest.fixture
def registry() -> Registry:
    return Registry()


@pytest.fixture
def moderation() -> ModerationStore:
    return ModerationStore()


# --- registry tag operations ---------------------------------------------- #


def test_registry_set_tags_replaces_full_set(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest()))
    entry = registry.set_tags(APP_ID, ["verified", "featured"])
    assert set(entry.tags) == {"verified", "featured"}

    entry = registry.set_tags(APP_ID, ["flagged"])
    assert entry.tags == ("flagged",)


def test_registry_set_tags_rejects_unknown_tag(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest()))
    with pytest.raises(InvalidTagError):
        registry.set_tags(APP_ID, ["nope"])


def test_registry_add_remove_tag_is_idempotent(registry: Registry):
    from pageros_marketplace import Manifest

    registry.register(Manifest.model_validate(_manifest()))
    e1 = registry.add_tag(APP_ID, "verified")
    e2 = registry.add_tag(APP_ID, "verified")
    assert e1.tags == e2.tags == ("verified",)
    e3 = registry.remove_tag(APP_ID, "verified")
    e4 = registry.remove_tag(APP_ID, "verified")
    assert e3.tags == e4.tags == ()


# --- moderation store ----------------------------------------------------- #


def test_moderation_store_file_and_list_reports(moderation: ModerationStore):
    r1 = moderation.file_report(app_id=APP_ID, reason="Contains spam links")
    r2 = moderation.file_report(
        app_id="other.example.dev",
        reason="Impersonating Notes",
        reporter_contact="alice@example.com",
    )
    reports, total = moderation.list_reports()
    assert total == 2
    assert {r.id for r in reports} == {r1.id, r2.id}
    assert reports[0].filed_at >= reports[1].filed_at  # newest first


def test_moderation_store_resolve_and_dismiss(moderation: ModerationStore):
    r = moderation.file_report(app_id=APP_ID, reason="bad")
    resolved = moderation.resolve_report(r.id, actor="admin", note="took action")
    assert resolved.status is ReportStatus.RESOLVED
    assert resolved.resolved_by == "admin"
    assert resolved.resolution_note == "took action"
    # Cannot reopen via dismiss.
    from pageros_marketplace import ReportAlreadyClosedError

    with pytest.raises(ReportAlreadyClosedError):
        moderation.dismiss_report(r.id, actor="admin")


def test_moderation_store_filters_by_status(moderation: ModerationStore):
    open_r = moderation.file_report(app_id=APP_ID, reason="r1")
    closed_r = moderation.file_report(app_id=APP_ID, reason="r2")
    moderation.dismiss_report(closed_r.id, actor="admin")
    open_only, _ = moderation.list_reports(status=ReportStatus.OPEN)
    assert [r.id for r in open_only] == [open_r.id]
    dismissed_only, _ = moderation.list_reports(status=ReportStatus.DISMISSED)
    assert [r.id for r in dismissed_only] == [closed_r.id]


def test_moderation_log_appends_in_order(moderation: ModerationStore):
    moderation.log(action="x", actor="a")
    moderation.log(action="y", actor="a", app_id=APP_ID, details={"tag": "verified"})
    entries, total = moderation.list_log()
    assert total == 2
    # Newest first
    assert entries[0].action == "y"
    assert entries[1].action == "x"
    # Details preserved
    assert entries[0].details == {"tag": "verified"}


# --- HTTP layer ----------------------------------------------------------- #


def _seed_app(client: TestClient, manifest: dict | None = None) -> None:
    client.post("/apps", json=manifest or _manifest()).raise_for_status()


@pytest.fixture
def open_client(registry: Registry, moderation: ModerationStore) -> TestClient:
    # No admin token configured → admin endpoints are open (test mode).
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


def _auth() -> dict[str, str]:
    return {"Authorization": "Bearer s3cret"}


# Public report endpoint
def test_http_file_report_against_existing_app(
    open_client: TestClient, moderation: ModerationStore
):
    _seed_app(open_client)
    r = open_client.post(
        "/reports",
        json={"app_id": APP_ID, "reason": "Contains spam", "reporter_contact": "a@b.co"},
    )
    assert r.status_code == 201, r.text
    body = r.json()
    assert body["app_id"] == APP_ID
    assert body["status"] == "open"
    assert body["reporter_contact"] == "a@b.co"
    assert moderation.list_reports()[1] == 1


def test_http_file_report_for_unknown_app_is_404(open_client: TestClient):
    r = open_client.post(
        "/reports", json={"app_id": "ghost.example.dev", "reason": "wat"}
    )
    assert r.status_code == 404
    assert r.json()["error"] == "not_found"


def test_http_file_report_rejects_short_reason(open_client: TestClient):
    _seed_app(open_client)
    r = open_client.post("/reports", json={"app_id": APP_ID, "reason": "ab"})
    assert r.status_code == 422


# Admin list/get/resolve/dismiss
def test_http_admin_list_and_resolve_report(
    open_client: TestClient, moderation: ModerationStore
):
    _seed_app(open_client)
    open_client.post("/reports", json={"app_id": APP_ID, "reason": "spam links"})
    listing = open_client.get("/admin/reports").json()
    assert listing["total"] == 1
    report_id = listing["items"][0]["id"]

    r = open_client.post(
        f"/admin/reports/{report_id}/resolve", json={"note": "removed"}
    )
    assert r.status_code == 200
    assert r.json()["status"] == "resolved"
    assert r.json()["resolution_note"] == "removed"

    # Filter by status
    open_count = open_client.get("/admin/reports?status=open").json()["total"]
    resolved_count = open_client.get("/admin/reports?status=resolved").json()["total"]
    assert open_count == 0 and resolved_count == 1


def test_http_admin_resolve_already_closed_is_409(open_client: TestClient):
    _seed_app(open_client)
    open_client.post("/reports", json={"app_id": APP_ID, "reason": "wat"})
    rid = open_client.get("/admin/reports").json()["items"][0]["id"]
    open_client.post(f"/admin/reports/{rid}/resolve", json={})
    r = open_client.post(f"/admin/reports/{rid}/dismiss", json={})
    assert r.status_code == 409
    assert r.json()["error"] == "report_already_closed"


def test_http_admin_get_unknown_report_is_404(open_client: TestClient):
    r = open_client.get("/admin/reports/not-real")
    assert r.status_code == 404
    assert r.json()["error"] == "report_not_found"


def test_http_admin_invalid_status_filter_is_400(open_client: TestClient):
    r = open_client.get("/admin/reports?status=banana")
    assert r.status_code == 400
    assert r.json()["error"] == "invalid_status"


# Admin tag mutation
def test_http_admin_set_tags(open_client: TestClient):
    _seed_app(open_client)
    r = open_client.put(
        f"/admin/apps/{APP_ID}/tags", json={"tags": ["verified", "featured"]}
    )
    assert r.status_code == 200
    assert set(r.json()["tags"]) == {"verified", "featured"}
    # GET reflects the change
    assert set(open_client.get(f"/apps/{APP_ID}").json()["tags"]) == {
        "verified",
        "featured",
    }


def test_http_admin_set_tags_rejects_unknown(open_client: TestClient):
    _seed_app(open_client)
    r = open_client.put(f"/admin/apps/{APP_ID}/tags", json={"tags": ["bogus"]})
    assert r.status_code == 400
    assert r.json()["error"] == "invalid_tag"


def test_http_admin_add_and_remove_single_tag(open_client: TestClient):
    _seed_app(open_client)
    r = open_client.post(f"/admin/apps/{APP_ID}/tags/verified")
    assert r.status_code == 200
    assert r.json()["tags"] == ["verified"]
    r = open_client.delete(f"/admin/apps/{APP_ID}/tags/verified")
    assert r.status_code == 200
    assert r.json()["tags"] == []


def test_http_admin_add_tag_unknown_is_400(open_client: TestClient):
    _seed_app(open_client)
    r = open_client.post(f"/admin/apps/{APP_ID}/tags/sketchy")
    assert r.status_code == 400


def test_http_admin_remove_tag_idempotent(open_client: TestClient):
    _seed_app(open_client)
    # Removing a tag that isn't set is a no-op (200), not 404.
    r = open_client.delete(f"/admin/apps/{APP_ID}/tags/featured")
    assert r.status_code == 200
    assert r.json()["tags"] == []


# Admin delete + action log
def test_http_admin_delete_app_logs_action(
    open_client: TestClient, moderation: ModerationStore
):
    _seed_app(open_client)
    open_client.delete(f"/apps/{APP_ID}").raise_for_status()
    log = open_client.get("/admin/log").json()
    actions = [e["action"] for e in log["items"]]
    assert "app.delete" in actions


def test_http_admin_log_paginates(
    open_client: TestClient, moderation: ModerationStore
):
    _seed_app(open_client)
    open_client.post(f"/admin/apps/{APP_ID}/tags/verified")
    open_client.post(f"/admin/apps/{APP_ID}/tags/featured")
    open_client.delete(f"/admin/apps/{APP_ID}/tags/verified")
    # 3 actions logged
    page = open_client.get("/admin/log?limit=2").json()
    assert page["total"] == 3
    assert len(page["items"]) == 2


# Admin auth gate
def test_http_admin_endpoints_require_token(gated_client: TestClient):
    _seed_app_gated(gated_client)
    r = gated_client.get("/admin/reports")
    assert r.status_code == 401
    assert r.headers.get("WWW-Authenticate", "").lower().startswith("bearer")
    assert r.json()["error"] == "admin_unauthorized"

    # Bad scheme
    r = gated_client.get("/admin/reports", headers={"Authorization": "Token s3cret"})
    assert r.status_code == 401

    # Wrong token
    r = gated_client.get(
        "/admin/reports", headers={"Authorization": "Bearer wrong"}
    )
    assert r.status_code == 401


def test_http_admin_endpoints_accept_valid_token(gated_client: TestClient):
    _seed_app_gated(gated_client)
    r = gated_client.get("/admin/reports", headers=_auth())
    assert r.status_code == 200


def test_http_admin_delete_app_requires_token(gated_client: TestClient):
    _seed_app_gated(gated_client)
    r = gated_client.delete(f"/apps/{APP_ID}")
    assert r.status_code == 401
    r = gated_client.delete(f"/apps/{APP_ID}", headers=_auth())
    assert r.status_code == 204


def test_http_admin_set_tags_requires_token(gated_client: TestClient):
    _seed_app_gated(gated_client)
    r = gated_client.put(
        f"/admin/apps/{APP_ID}/tags", json={"tags": ["verified"]}
    )
    assert r.status_code == 401


def _seed_app_gated(client: TestClient) -> None:
    # Public POST /apps doesn't need admin; only the admin endpoints do.
    client.post("/apps", json=_manifest()).raise_for_status()


# Admin UI
def test_http_admin_ui_renders_html(open_client: TestClient):
    _seed_app(open_client)
    open_client.post("/reports", json={"app_id": APP_ID, "reason": "bad app"})
    r = open_client.get("/admin/ui")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    body = r.text
    assert "<title>PagerOS Marketplace · Admin</title>" in body
    assert APP_ID in body
    assert "bad app" in body
    # Tag checkboxes are rendered for each allowed tag
    for tag in ALLOWED_TAGS:
        assert f'data-tag="{tag}"' in body


def test_http_admin_ui_requires_token(gated_client: TestClient):
    r = gated_client.get("/admin/ui")
    assert r.status_code == 401
    r = gated_client.get("/admin/ui", headers=_auth())
    assert r.status_code == 200


# OpenAPI includes the moderation routes
def test_openapi_lists_moderation_endpoints(open_client: TestClient):
    paths = open_client.get("/openapi.json").json()["paths"]
    assert "/reports" in paths
    assert "/admin/reports" in paths
    assert "/admin/reports/{report_id}" in paths
    assert "/admin/reports/{report_id}/resolve" in paths
    assert "/admin/reports/{report_id}/dismiss" in paths
    assert "/admin/apps/{app_id}/tags" in paths
    assert "/admin/apps/{app_id}/tags/{tag}" in paths
    assert "/admin/log" in paths
