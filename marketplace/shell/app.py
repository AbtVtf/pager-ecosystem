"""PagerOS marketplace device shell (MKT-005).

The Shell's built-in "Apps" screen is itself a PagerOS app — served by
this module — that fetches data from the marketplace REST API (MKT-002)
and returns Frames the device renders natively.

Per SPEC §10.4 the in-device app drawer is reachable at the marketplace
root (e.g. ``market.pageros.org/``); this module is mounted under
``/device`` of the same deployment so it can coexist with the HTML web
UI (MKT-004) on the same domain.

Routes (all return CBOR Frames):

- ``GET /``                 — home: featured rail + categories
- ``GET /search?q=…``       — search results
- ``GET /category?slug=…``  — apps in a category
- ``GET /app?id=…``         — app detail (icon, description, donate, install)
- ``GET /featured``         — paginated featured list
- ``POST /install``         — confirms intent to install (device handles the local-state write)

The shell talks to the marketplace API through a small client wrapper so
tests can inject an in-process Registry without standing up an HTTP server.
"""

from __future__ import annotations

import json
import os
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping

from pageros import (
    App,
    Button,
    Form,
    Image,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

DEFAULT_PAGE_SIZE = 8

# --- API client ---------------------------------------------------------- #


class MarketplaceClient:
    """Tiny client over the MKT-002 REST API.

    The ``fetch`` callable accepts ``(method, path, query)`` and returns a
    parsed JSON body. Defaults to HTTPS against ``base_url`` via urllib; tests
    inject a function that calls a FastAPI ``TestClient`` directly so we don't
    need a running server.
    """

    def __init__(
        self,
        *,
        base_url: str = "http://localhost:8000",
        fetch: Callable[[str, str, Mapping[str, Any]], Any] | None = None,
        timeout: float = 5.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._fetch = fetch or self._http_fetch

    def list_apps(self, *, offset=0, limit=DEFAULT_PAGE_SIZE, category=None, tag=None, featured_first=False) -> dict:
        q: dict[str, Any] = {"offset": offset, "limit": limit, "featured_first": str(featured_first).lower()}
        if category: q["category"] = category
        if tag:      q["tag"] = tag
        return self._fetch("GET", "/apps", q)

    def get_app(self, app_id: str) -> dict | None:
        try:
            return self._fetch("GET", f"/apps/{urllib.parse.quote(app_id)}", {})
        except KeyError:
            return None

    def featured(self, *, offset=0, limit=DEFAULT_PAGE_SIZE) -> dict:
        return self._fetch("GET", "/apps/featured", {"offset": offset, "limit": limit})

    def search(self, q: str, *, offset=0, limit=DEFAULT_PAGE_SIZE) -> dict:
        return self._fetch("GET", "/apps/search", {"q": q, "offset": offset, "limit": limit})

    # default HTTP fetcher
    def _http_fetch(self, method: str, path: str, query: Mapping[str, Any]) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode([(k, v) for k, v in query.items() if v not in (None, "")])
        req = urllib.request.Request(url, method=method, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            if r.status == 404:
                raise KeyError(path)
            if r.status >= 400:
                raise RuntimeError(f"{method} {url} -> HTTP {r.status}")
            return json.loads(r.read())


# --- Screen builders ----------------------------------------------------- #


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _q_int(request, key: str, default: int) -> int:
    v = _first_q(request, key)
    try:
        return int(v) if v else default
    except ValueError:
        return default


def _app_item(entry: dict) -> ListItem:
    m = entry["manifest"]
    tags = entry.get("tags") or []
    badge = ""
    if "featured" in tags:
        badge = "★ "
    elif "verified" in tags:
        badge = "✓ "
    elif "flagged" in tags:
        badge = "⚠ "
    return ListItem(
        label=f"{badge}{m['name']}",
        sub=m.get("description", "")[:50] or m["id"],
        href=f"/app?id={urllib.parse.quote(m['id'])}",
    )


def build_app(client: MarketplaceClient, *, title: str = "Apps") -> App:
    """Construct the Shell app bound to a given marketplace client."""
    app = App(name="marketplace-shell")

    @app.screen("/")
    def home(request):
        page = client.list_apps(featured_first=True, limit=DEFAULT_PAGE_SIZE)
        items: list[ListItem] = [_app_item(e) for e in page.get("items", [])]
        body = [
            Form(
                action="/search", method="GET",
                fields=[Input(name="q", label="Search", max=40)],
                submit="Go",
            ),
        ]
        if items:
            body.append(List(items=items))
        else:
            body.append(Text(s="No apps registered yet.", style="dim"))
        if page.get("total", 0) > len(items):
            body.append(Button(label="More", href=f"/?offset={len(items)}"))
        return Screen(id="scr_home", title=title, body=body)

    @app.screen("/featured")
    def featured(request):
        page = client.featured(limit=DEFAULT_PAGE_SIZE,
                               offset=_q_int(request, "offset", 0))
        items = [_app_item(e) for e in page.get("items", [])]
        body = [Text(s=f"Featured ({page.get('total', 0)})", style="heading")]
        body.append(List(items=items) if items else Text(s="Nothing featured.", style="dim"))
        return Screen(id="scr_featured", title="Featured", body=body)

    @app.screen("/category")
    def category(request):
        slug = _first_q(request, "slug")
        if not slug:
            return Screen(id="scr_cat_404", title=title,
                          body=[Notification(s="Missing ?slug=", level="error")])
        page = client.list_apps(category=slug, limit=DEFAULT_PAGE_SIZE,
                                offset=_q_int(request, "offset", 0))
        items = [_app_item(e) for e in page.get("items", [])]
        return Screen(
            id=f"scr_cat_{slug}",
            title=f"#{slug}",
            body=[List(items=items) if items else Text(s=f"No apps in '{slug}'.", style="dim")],
        )

    @app.screen("/search")
    def search(request):
        q = _first_q(request, "q")
        if not q:
            return Screen(id="scr_search_empty", title="Search",
                          body=[Text(s="Type a query above.", style="dim")])
        page = client.search(q, limit=DEFAULT_PAGE_SIZE, offset=_q_int(request, "offset", 0))
        items = [_app_item(e) for e in page.get("items", [])]
        body = [Text(s=f"{page.get('total', 0)} matches for '{q}'", style="dim")]
        body.append(List(items=items) if items else Text(s="Nothing found.", style="dim"))
        return Screen(id=f"scr_search_{q[:8]}", title="Search", body=body)

    @app.screen("/app")
    def app_detail(request):
        app_id = _first_q(request, "id")
        if not app_id:
            return Screen(id="scr_404", title=title,
                          body=[Notification(s="Missing ?id=", level="error")])
        entry = client.get_app(app_id)
        if entry is None:
            return Screen(id="scr_404", title=title,
                          body=[Notification(s=f"App '{app_id}' not found.", level="error")])
        m = entry["manifest"]
        tags = entry.get("tags") or []
        body: list[Any] = [
            Text(s=m["name"], style="heading"),
            Text(s=m.get("description", ""), style="body"),
            Text(s="categories: " + (" · ".join(m.get("categories") or []) or "—"), style="dim"),
            Text(s="trust: " + (" · ".join(tags) if tags else "unverified"), style="dim"),
        ]
        if m.get("icon"):
            # The device renders an Image widget by content-hash; the
            # marketplace's image proxy resolves the hash for cached fetch.
            body.insert(0, Image(src=f"img:{m['icon']}", w=96, h=96, alt=m["name"]))
        body.append(Button(label="Install", href=f"/install?id={urllib.parse.quote(app_id)}", method="POST"))
        if m.get("donate_url"):
            body.append(Button(label="Tip developer", href=m["donate_url"]))
        return Screen(id=f"scr_app_{app_id}", title=m["name"], body=body)

    @app.handler("/install", method="POST")
    def install(request):
        app_id = _first_q(request, "id")
        if not app_id:
            return Screen(id="scr_install_err", title="Install",
                          body=[Notification(s="Missing ?id=", level="error")])
        # The actual local install (writing the manifest into /apps/<id>/
        # on the SD card) is a firmware concern (FW-029 / FW-034). The
        # shell's POST is a confirmation surface — the device sees the
        # 200 OK and acts locally.
        return Screen(
            id="scr_installed",
            title="Install",
            body=[
                Notification(s=f"Queued install of {app_id}", level="info"),
                Button(label="Back to Apps", href="/"),
            ],
        )

    return app


# --- Module-level instance (entrypoint) --------------------------------- #


def _default_app() -> App:
    base = os.environ.get("PAGEROS_MKT_API_URL", "http://localhost:8000")
    return build_app(MarketplaceClient(base_url=base))


app = _default_app()


if __name__ == "__main__":
    app.run()
