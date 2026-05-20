"""Tests for the marketplace registry REST API (MKT-002)."""

from __future__ import annotations

import json

import pytest
from fastapi.testclient import TestClient

from pageros_marketplace import Registry, create_app


def _manifest(
    *,
    id: str = "notes.mafu.dev",
    name: str = "Notes",
    description: str = "A simple notepad.",
    categories: list[str] | None = None,
    version: int = 1,
    permissions: list[str] | None = None,
) -> dict:
    return {
        "id": id,
        "name": name,
        "description": description,
        "icon": "https://notes.app/icon.png",
        "url": "https://notes.app/",
        "categories": categories or ["productivity"],
        "maintainer": {"name": "Jane Doe", "contact": "jane@example.com"},
        "version": version,
        "permissions": permissions or [],
    }


@pytest.fixture
def client() -> TestClient:
    # MKT-002 tests assert pre-challenge-gate behavior. The challenge gate is
    # covered separately in test_challenges.py.
    return TestClient(create_app(Registry(), challenge_required=False))


# --- register -------------------------------------------------------------- #


def test_register_returns_201_with_record(client: TestClient):
    r = client.post("/apps", json=_manifest())
    assert r.status_code == 201, r.text
    body = r.json()
    assert body["manifest"]["id"] == "notes.mafu.dev"
    assert body["tags"] == []
    assert "created_at" in body and "updated_at" in body


def test_register_rejects_duplicate_id(client: TestClient):
    client.post("/apps", json=_manifest()).raise_for_status()
    r = client.post("/apps", json=_manifest())
    assert r.status_code == 409
    assert r.json()["error"] == "duplicate_app"


def test_register_rejects_invalid_manifest(client: TestClient):
    bad = _manifest() | {"id": "NotReverseDns"}
    r = client.post("/apps", json=bad)
    assert r.status_code == 422
    # FastAPI's stock validation envelope contains the offending field.
    body = json.dumps(r.json()).lower()
    assert "id" in body


# --- list ------------------------------------------------------------------ #


def test_list_empty_returns_zero_total(client: TestClient):
    r = client.get("/apps")
    assert r.status_code == 200
    assert r.json() == {"items": [], "total": 0, "offset": 0, "limit": 50}


def test_list_paginates_and_orders_by_id(client: TestClient):
    for app_id in ["c.example.dev", "a.example.dev", "b.example.dev"]:
        client.post("/apps", json=_manifest(id=app_id)).raise_for_status()

    r = client.get("/apps", params={"limit": 2, "offset": 0})
    body = r.json()
    assert [e["manifest"]["id"] for e in body["items"]] == [
        "a.example.dev",
        "b.example.dev",
    ]
    assert body["total"] == 3

    r2 = client.get("/apps", params={"limit": 2, "offset": 2})
    assert [e["manifest"]["id"] for e in r2.json()["items"]] == ["c.example.dev"]


def test_list_filters_by_category(client: TestClient):
    client.post("/apps", json=_manifest(id="games.example.dev", categories=["games"]))
    client.post("/apps", json=_manifest(id="notes.example.dev", categories=["productivity"]))

    r = client.get("/apps", params={"category": "games"})
    body = r.json()
    assert body["total"] == 1
    assert body["items"][0]["manifest"]["id"] == "games.example.dev"


# --- get ------------------------------------------------------------------- #


def test_get_existing_app(client: TestClient):
    client.post("/apps", json=_manifest()).raise_for_status()
    r = client.get("/apps/notes.mafu.dev")
    assert r.status_code == 200
    assert r.json()["manifest"]["id"] == "notes.mafu.dev"


def test_get_unknown_app_is_404(client: TestClient):
    r = client.get("/apps/unknown.example.dev")
    assert r.status_code == 404
    assert r.json()["error"] == "not_found"


def test_get_rejects_invalid_id_path(client: TestClient):
    r = client.get("/apps/NotReverseDns")
    # Path validation: FastAPI returns 422 for failed Path patterns.
    assert r.status_code == 422


# --- update ---------------------------------------------------------------- #


def test_update_replaces_manifest_when_version_increases(client: TestClient):
    client.post("/apps", json=_manifest()).raise_for_status()
    r = client.put(
        "/apps/notes.mafu.dev",
        json=_manifest(name="Notes 2", version=2),
    )
    assert r.status_code == 200
    assert r.json()["manifest"]["name"] == "Notes 2"
    assert r.json()["manifest"]["version"] == 2


def test_update_requires_version_increase(client: TestClient):
    client.post("/apps", json=_manifest(version=2)).raise_for_status()
    r = client.put("/apps/notes.mafu.dev", json=_manifest(version=2))
    assert r.status_code == 409
    assert r.json()["error"] == "version_not_increased"


def test_update_rejects_id_mismatch(client: TestClient):
    client.post("/apps", json=_manifest()).raise_for_status()
    r = client.put(
        "/apps/notes.mafu.dev",
        json=_manifest(id="other.example.dev", version=2),
    )
    assert r.status_code == 400
    assert r.json()["error"] == "id_mismatch"


def test_update_unknown_app_is_404(client: TestClient):
    r = client.put("/apps/unknown.example.dev", json=_manifest(id="unknown.example.dev"))
    assert r.status_code == 404


# --- delete ---------------------------------------------------------------- #


def test_delete_removes_app(client: TestClient):
    client.post("/apps", json=_manifest()).raise_for_status()
    r = client.delete("/apps/notes.mafu.dev")
    assert r.status_code == 204
    assert client.get("/apps/notes.mafu.dev").status_code == 404


def test_delete_unknown_is_404(client: TestClient):
    r = client.delete("/apps/unknown.example.dev")
    assert r.status_code == 404


# --- search ---------------------------------------------------------------- #


@pytest.fixture
def populated_client(client: TestClient) -> TestClient:
    apps = [
        _manifest(id="notes.mafu.dev", name="Notes", description="A simple notepad."),
        _manifest(
            id="weather.example.dev",
            name="Weather",
            description="Local forecast for hikers.",
            categories=["utilities", "outdoors"],
        ),
        _manifest(
            id="chat.example.dev",
            name="Chat",
            description="Lightweight messaging app.",
            categories=["communication"],
        ),
    ]
    for m in apps:
        client.post("/apps", json=m).raise_for_status()
    return client


def test_search_matches_name(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": "weather"})
    body = r.json()
    assert body["total"] == 1
    assert body["items"][0]["manifest"]["id"] == "weather.example.dev"


def test_search_matches_description(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": "notepad"})
    assert r.status_code == 200
    body = r.json()
    assert body["total"] == 1
    assert body["items"][0]["manifest"]["id"] == "notes.mafu.dev"


def test_search_matches_category(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": "outdoors"})
    assert r.status_code == 200
    body = r.json()
    assert {e["manifest"]["id"] for e in body["items"]} == {"weather.example.dev"}


def test_search_is_case_insensitive(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": "CHAT"})
    assert r.status_code == 200
    assert r.json()["total"] == 1


def test_search_empty_query_returns_full_list(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": ""})
    assert r.status_code == 200
    assert r.json()["total"] == 3


def test_search_no_match_is_empty(populated_client: TestClient):
    r = populated_client.get("/apps/search", params={"q": "zzzz"})
    assert r.status_code == 200
    assert r.json() == {"items": [], "total": 0, "offset": 0, "limit": 50}


def test_search_ranks_prefix_first(populated_client: TestClient):
    # Add another app whose description contains 'note' so multiple match.
    populated_client.post(
        "/apps",
        json=_manifest(
            id="diary.example.dev",
            name="Diary",
            description="Daily notes about your week.",
        ),
    ).raise_for_status()
    r = populated_client.get("/apps/search", params={"q": "note"})
    ids = [e["manifest"]["id"] for e in r.json()["items"]]
    # "notes.mafu.dev" matches by name prefix → rank 0; "diary..." matches description → rank 2.
    assert ids[0] == "notes.mafu.dev"
    assert "diary.example.dev" in ids


# --- OpenAPI doc ----------------------------------------------------------- #


def test_openapi_doc_is_served(client: TestClient):
    r = client.get("/openapi.json")
    assert r.status_code == 200
    schema = r.json()
    assert schema["info"]["title"] == "PagerOS Marketplace Registry"
    paths = schema["paths"]
    for path in ("/apps", "/apps/{app_id}", "/apps/search"):
        assert path in paths, f"missing {path} in OpenAPI doc"
    # Acceptance bullet: register, list, get-by-id, search-by-keyword.
    assert "post" in paths["/apps"]
    assert "get" in paths["/apps"]
    assert "get" in paths["/apps/{app_id}"]
    assert "get" in paths["/apps/search"]


def test_openapi_dump_cli(tmp_path, capsys):
    from pageros_marketplace.cli import main

    out = tmp_path / "openapi.json"
    rc = main(["dump-openapi", "--output", str(out)])
    assert rc == 0
    schema = json.loads(out.read_text(encoding="utf-8"))
    assert schema["info"]["title"] == "PagerOS Marketplace Registry"


# --- healthz --------------------------------------------------------------- #


def test_healthz(client: TestClient):
    r = client.get("/healthz")
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}
