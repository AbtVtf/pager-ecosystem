"""FastAPI app factory for the marketplace public web UI (MKT-004).

The web UI is intentionally minimal: server-rendered HTML, no JS framework,
no build step. It reads from a ``Registry`` (the same one the API talks to)
so search, filter, and detail pages are always consistent with the JSON
surface.

For v1 the API and web run in the same Python process and share an
in-memory ``Registry`` instance. Splitting them across hosts is a
deployment-topology change for MKT-011.
"""

from __future__ import annotations

from fastapi import Depends, FastAPI, Path, Query, Request, status
from fastapi.responses import HTMLResponse, PlainTextResponse, Response

from pageros_marketplace.registry.store import (
    Registry,
    UnknownAppError,
)

from .render import (
    CSS,
    render_app_detail,
    render_browse,
    render_not_found,
)


_APP_ID_PATTERN = r"^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$"
_DEFAULT_PAGE_SIZE = 24


def create_app(
    registry: Registry,
    *,
    site_title: str = "PagerOS Marketplace",
) -> FastAPI:
    """Build a FastAPI app rendering the public marketplace.pageros.org pages.

    Parameters
    ----------
    registry:
        Shared ``Registry`` instance. The web app calls its read methods
        (``list``, ``search``, ``get``) directly — no JSON round-trip.
    site_title:
        Site title used in ``<title>`` and the header brand. Default is
        ``"PagerOS Marketplace"``.
    """
    app = FastAPI(
        title="PagerOS Marketplace Web UI",
        version="0.1.0",
        # The web UI is HTML-first; its routes do not need an OpenAPI doc.
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
    )
    app.state.registry = registry
    app.state.site_title = site_title
    _register_routes(app)
    return app


# --- routes -------------------------------------------------------------- #


def _get_registry(request: Request) -> Registry:
    return request.app.state.registry


def _get_site_title(request: Request) -> str:
    return request.app.state.site_title


def _register_routes(app: FastAPI) -> None:
    @app.get("/healthz", response_class=PlainTextResponse, include_in_schema=False)
    def healthz() -> Response:
        # Plain text liveness probe — matches the API's healthz contract.
        return PlainTextResponse('{"status":"ok"}', media_type="application/json")

    @app.get("/static/style.css", include_in_schema=False)
    def stylesheet() -> Response:
        return Response(
            CSS,
            media_type="text/css",
            headers={"Cache-Control": "public, max-age=300"},
        )

    @app.get("/", response_class=HTMLResponse, include_in_schema=False)
    @app.get("/browse", response_class=HTMLResponse, include_in_schema=False)
    def browse(
        q: str = Query(
            "",
            description="Free-text search across id, name, description, categories.",
            max_length=200,
        ),
        category: str | None = Query(
            None,
            min_length=1,
            max_length=64,
            description="Filter to apps that include this category slug.",
        ),
        offset: int = Query(0, ge=0),
        limit: int = Query(_DEFAULT_PAGE_SIZE, ge=1, le=100),
        registry: Registry = Depends(_get_registry),
        site_title: str = Depends(_get_site_title),
    ) -> HTMLResponse:
        query = q.strip()
        cat = (category or "").strip() or None
        if query:
            # Full-text search ignores the category filter (an explicit search
            # already narrows the result set; layering both makes the UX
            # confusing). We surface both inputs so the user can clear one
            # without losing the other.
            entries, total = registry.search(query, offset=offset, limit=limit)
        else:
            entries, total = registry.list(
                offset=offset, limit=limit, category=cat
            )
        categories = _collect_categories(registry)
        html_str = render_browse(
            site_title=site_title,
            apps=entries,
            total=total,
            offset=offset,
            limit=limit,
            query=query,
            category=cat,
            categories=categories,
        )
        return HTMLResponse(html_str)

    @app.get("/search", response_class=HTMLResponse, include_in_schema=False)
    def search(
        q: str = Query("", max_length=200),
        offset: int = Query(0, ge=0),
        limit: int = Query(_DEFAULT_PAGE_SIZE, ge=1, le=100),
        registry: Registry = Depends(_get_registry),
        site_title: str = Depends(_get_site_title),
    ) -> HTMLResponse:
        query = q.strip()
        entries, total = registry.search(query, offset=offset, limit=limit)
        html_str = render_browse(
            site_title=site_title,
            apps=entries,
            total=total,
            offset=offset,
            limit=limit,
            query=query,
            category=None,
            categories=_collect_categories(registry),
        )
        return HTMLResponse(html_str)

    @app.get(
        "/app/{app_id}",
        response_class=HTMLResponse,
        include_in_schema=False,
    )
    def app_detail(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
        site_title: str = Depends(_get_site_title),
    ) -> HTMLResponse:
        try:
            entry = registry.get(app_id)
        except UnknownAppError:
            return HTMLResponse(
                render_not_found(site_title=site_title, app_id=app_id),
                status_code=status.HTTP_404_NOT_FOUND,
            )
        return HTMLResponse(render_app_detail(site_title=site_title, entry=entry))


def _collect_categories(registry: Registry) -> list[str]:
    """Snapshot the union of categories across the catalog."""
    seen: set[str] = set()
    for entry in list(registry):
        for cat in entry.manifest.categories:
            seen.add(cat)
    return sorted(seen)
