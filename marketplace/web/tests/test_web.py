"""Tests for the marketplace public web UI (MKT-004).

Acceptance: public marketplace.pageros.org renders app list, category
filter, full-text search, and app detail page with icon + description +
donate link.
"""

from __future__ import annotations

import re

import pytest
from fastapi.testclient import TestClient

from pageros_marketplace import Manifest, Registry
from pageros_marketplace_web import create_app


def _manifest(
    *,
    app_id: str,
    name: str | None = None,
    description: str = "An app.",
    categories: list[str] | None = None,
    donate_url: str | None = None,
    icon: str | None = None,
    version: int = 1,
) -> Manifest:
    return Manifest.model_validate(
        {
            "id": app_id,
            "name": name or app_id.split(".", 1)[0].title(),
            "description": description,
            "icon": icon or f"https://{app_id}/icon.png",
            "url": f"https://{app_id}/",
            "categories": categories or ["productivity"],
            "maintainer": {"name": "Maint", "contact": f"mail@{app_id}"},
            "version": version,
            **({"donate_url": donate_url} if donate_url else {}),
        }
    )


@pytest.fixture
def registry() -> Registry:
    r = Registry()
    r.register(_manifest(app_id="notes.mafu.dev", name="Notes", description="A simple notepad.", categories=["productivity"]))
    r.register(_manifest(app_id="chess.mafu.dev", name="Chess", description="Play chess offline.", categories=["games"]))
    r.register(_manifest(app_id="weather.mafu.dev", name="Weather", description="Current weather and forecast.", categories=["utilities"]))
    return r


@pytest.fixture
def client(registry: Registry) -> TestClient:
    return TestClient(create_app(registry))


# --- browse + search ----------------------------------------------------- #


def test_browse_lists_all_apps(client: TestClient):
    r = client.get("/")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    body = r.text
    for label in ("Notes", "Chess", "Weather"):
        assert label in body
    # Each card links to the detail page.
    assert 'href="/app/notes.mafu.dev"' in body
    assert 'href="/app/chess.mafu.dev"' in body


def test_browse_search_full_text_filters_results(client: TestClient):
    r = client.get("/?q=chess")
    assert r.status_code == 200
    body = r.text
    assert "Chess" in body
    assert "Play chess offline." in body
    assert "Notes" not in body
    # Search box preserves the query.
    assert 'value="chess"' in body


def test_browse_search_by_description_substring(client: TestClient):
    r = client.get("/?q=forecast")
    assert r.status_code == 200
    body = r.text
    assert "Weather" in body
    assert "Notes" not in body


def test_browse_search_no_results_shows_empty_message(client: TestClient):
    r = client.get("/?q=nothingmatches")
    assert r.status_code == 200
    body = r.text
    assert "No apps match" in body
    assert "<q>nothingmatches</q>" in body


def test_browse_category_filter(client: TestClient):
    r = client.get("/?category=games")
    assert r.status_code == 200
    body = r.text
    assert "Chess" in body
    assert "Notes" not in body
    # The category dropdown should mark "games" as selected.
    assert '<option value="games" selected' in body


def test_browse_category_filter_with_no_matches(client: TestClient):
    r = client.get("/?category=health")
    assert r.status_code == 200
    body = r.text
    # No apps in that category — empty state.
    assert "No apps to show." in body or "Showing" not in body


def test_browse_summary_counts(client: TestClient):
    r = client.get("/")
    body = r.text
    assert re.search(r"Showing\s+1[–-]3\s+of\s+3", body)


def test_browse_category_dropdown_populated_from_registry(client: TestClient):
    r = client.get("/")
    body = r.text
    for cat in ("games", "productivity", "utilities"):
        assert f'value="{cat}"' in body


def test_search_route_renders_same_browse_layout(client: TestClient):
    r = client.get("/search?q=weather")
    assert r.status_code == 200
    body = r.text
    assert "Weather" in body
    assert "Notes" not in body


# --- detail page --------------------------------------------------------- #


def test_app_detail_renders_icon_and_description(client: TestClient):
    r = client.get("/app/notes.mafu.dev")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    body = r.text
    assert "<h1>Notes</h1>" in body
    assert "A simple notepad." in body
    assert 'src="https://notes.mafu.dev/icon.png"' in body
    # And the title tag uses the app name.
    assert "<title>Notes" in body


def test_app_detail_shows_donate_link_when_set(registry: Registry):
    registry.register(
        _manifest(
            app_id="tippy.mafu.dev",
            name="Tippy",
            donate_url="https://liberapay.com/tippy",
        )
    )
    c = TestClient(create_app(registry))
    r = c.get("/app/tippy.mafu.dev")
    body = r.text
    assert "Tip developer" in body
    assert 'href="https://liberapay.com/tippy"' in body


def test_app_detail_hides_donate_link_when_unset(client: TestClient):
    body = client.get("/app/notes.mafu.dev").text
    assert "Tip developer" not in body


def test_app_detail_unknown_app_is_404(client: TestClient):
    r = client.get("/app/ghost.example.dev")
    assert r.status_code == 404
    assert "App not found" in r.text
    assert "ghost.example.dev" in r.text


def test_app_detail_renders_categories(client: TestClient):
    body = client.get("/app/chess.mafu.dev").text
    assert "games" in body


def test_app_detail_renders_unverified_when_no_tags(client: TestClient):
    body = client.get("/app/notes.mafu.dev").text
    assert "unverified" in body


def test_app_detail_renders_tags_when_present(registry: Registry):
    registry.register(_manifest(app_id="taggy.mafu.dev"))
    registry.add_tag("taggy.mafu.dev", "verified")
    c = TestClient(create_app(registry))
    body = c.get("/app/taggy.mafu.dev").text
    assert 'class="tag tag-verified"' in body
    assert ">verified<" in body


# --- safety: HTML escaping ----------------------------------------------- #


def test_xss_is_escaped_in_search_query(client: TestClient):
    payload = "<script>alert(1)</script>"
    r = client.get("/", params={"q": payload})
    assert r.status_code == 200
    body = r.text
    assert "<script>alert(1)</script>" not in body
    assert "&lt;script&gt;" in body


def test_xss_is_escaped_in_app_description(registry: Registry):
    registry.register(
        _manifest(
            app_id="evil.mafu.dev",
            name="<img src=x onerror=alert(1)>",
            description="hi<script>steal()</script>",
        )
    )
    c = TestClient(create_app(registry))
    body = c.get("/app/evil.mafu.dev").text
    assert "<script>steal()</script>" not in body
    assert "&lt;script&gt;steal()&lt;/script&gt;" in body
    assert "<img src=x onerror=" not in body


# --- ancillary routes ---------------------------------------------------- #


def test_healthz(client: TestClient):
    r = client.get("/healthz")
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}


def test_stylesheet_served(client: TestClient):
    r = client.get("/static/style.css")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/css")
    assert ".cards" in r.text


def test_browse_page_links_to_stylesheet(client: TestClient):
    body = client.get("/").text
    assert 'href="/static/style.css"' in body


# --- pagination ---------------------------------------------------------- #


def test_browse_pagination_controls_appear_when_overflow(registry: Registry):
    for i in range(30):
        registry.register(_manifest(app_id=f"a{i:02d}.mafu.dev", name=f"App {i:02d}"))
    c = TestClient(create_app(registry))
    r = c.get("/?limit=10")
    body = r.text
    assert "Next" in body
    # Page 1 shouldn't have a Prev link.
    assert "Prev" not in body
    r2 = c.get("/?limit=10&offset=10")
    body2 = r2.text
    assert "Prev" in body2
    assert "Next" in body2


# --- site title customization ------------------------------------------- #


def test_site_title_is_customizable(registry: Registry):
    c = TestClient(create_app(registry, site_title="My Test Market"))
    body = c.get("/").text
    assert "My Test Market" in body
