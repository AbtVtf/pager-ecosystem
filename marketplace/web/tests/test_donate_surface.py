"""MKT-007 acceptance: app-detail screen surfaces a 'Tip developer' action
when ``donate_url`` is set on the manifest.

The rendering itself was added by Vendor as part of MKT-004 (web UI); this
test pins the contract so future renderer changes don't silently drop the
donate surface.
"""

from __future__ import annotations

import re

from fastapi.testclient import TestClient

from pageros_marketplace import Manifest, Registry
from pageros_marketplace_web import create_app


def _manifest(*, app_id: str, donate_url: str | None) -> Manifest:
    data = {
        "id": app_id,
        "name": app_id.split(".")[-1].title(),
        "description": "demo",
        "icon": "https://example.com/i.png",
        "url": f"https://{app_id}/",
        "categories": ["misc"],
        "maintainer": {"name": "Maintainer", "contact": "m@example.com"},
        "version": 1,
    }
    if donate_url is not None:
        data["donate_url"] = donate_url
    return Manifest.model_validate(data)


def _client_with(*, donate_url: str | None) -> tuple[TestClient, str]:
    reg = Registry()
    m = _manifest(app_id="x.example", donate_url=donate_url)
    reg.register(m)
    app = create_app(registry=reg, site_title="Marketplace")
    return TestClient(app), m.id


def test_donate_link_renders_when_donate_url_is_set():
    client, app_id = _client_with(donate_url="https://give.example.com/x")
    r = client.get(f"/app/{app_id}")
    assert r.status_code == 200
    html = r.text
    assert 'class="donate"' in html
    assert "Tip developer" in html
    assert "https://give.example.com/x" in html


def test_no_donate_link_when_donate_url_unset():
    client, app_id = _client_with(donate_url=None)
    r = client.get(f"/app/{app_id}")
    assert r.status_code == 200
    assert "Tip developer" not in r.text
    assert 'class="donate"' not in r.text


def test_donate_link_is_html_escaped():
    """A donate_url containing HTML-unsafe characters must be escaped."""
    weird = "https://give.example.com/x?a=b&c=<script>"
    client, app_id = _client_with(donate_url=weird)
    r = client.get(f"/app/{app_id}")
    assert "<script>" not in r.text
    assert "&lt;script&gt;" in r.text or "%3Cscript%3E" in r.text


def test_donate_link_uses_rel_nofollow_noopener():
    """Anti-spam / anti-tab-jack hardening — surfaced for security review."""
    client, app_id = _client_with(donate_url="https://give.example.com/x")
    r = client.get(f"/app/{app_id}")
    m = re.search(r'<a[^>]+class="donate"[^>]*>', r.text)
    assert m is not None
    assert "nofollow" in m.group(0)
    assert "noopener" in m.group(0)
