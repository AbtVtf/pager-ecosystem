"""HTML rendering for the marketplace public web UI (MKT-004).

Server-side rendered, no JS framework. The single inline form on `/`
posts back as a plain `GET` so search + filter are bookmarkable. Pages
escape every dynamic value through ``html.escape`` to keep XSS handling
boring.
"""

from __future__ import annotations

import html
from typing import Iterable

from pageros_marketplace.registry.store import AppEntry


_BASE_CSS_HREF = "/static/style.css"


def render_browse(
    *,
    site_title: str,
    apps: Iterable[AppEntry],
    total: int,
    offset: int,
    limit: int,
    query: str,
    category: str | None,
    categories: Iterable[str],
) -> str:
    apps_list = list(apps)
    cards = "\n".join(_render_card(a) for a in apps_list) or _EMPTY_LIST
    options = "\n".join(_render_category_option(c, category) for c in sorted(set(categories)))
    summary = _render_summary(total=total, offset=offset, limit=limit, query=query)
    pagination = _render_pagination(
        total=total, offset=offset, limit=limit, query=query, category=category
    )
    return _BROWSE.format(
        title=html.escape(site_title),
        site_title=html.escape(site_title),
        query=html.escape(query),
        category_options=options,
        summary=summary,
        cards=cards,
        pagination=pagination,
        css_href=_BASE_CSS_HREF,
    )


def render_app_detail(*, site_title: str, entry: AppEntry) -> str:
    m = entry.manifest
    donate_block = ""
    if m.donate_url:
        donate_block = (
            '<a class="donate" rel="nofollow noopener"'
            f' href="{html.escape(m.donate_url)}">'
            "Tip developer</a>"
        )
    tags = "".join(
        f'<span class="tag tag-{html.escape(t)}">{html.escape(t)}</span>'
        for t in entry.tags
    ) or '<span class="tag tag-unverified">unverified</span>'
    categories = " · ".join(html.escape(c) for c in m.categories) or "—"
    maintainer = html.escape(m.maintainer.name)
    contact = html.escape(m.maintainer.contact)
    return _DETAIL.format(
        title=f"{html.escape(m.name)} · {html.escape(site_title)}",
        site_title=html.escape(site_title),
        name=html.escape(m.name),
        app_id=html.escape(m.id),
        description=html.escape(m.description),
        icon=html.escape(m.icon),
        url=html.escape(m.url),
        categories=categories,
        tags=tags,
        maintainer=maintainer,
        contact=contact,
        version=m.version,
        donate_block=donate_block,
        css_href=_BASE_CSS_HREF,
    )


def render_not_found(*, site_title: str, app_id: str) -> str:
    return _NOT_FOUND.format(
        title=f"Not found · {html.escape(site_title)}",
        site_title=html.escape(site_title),
        app_id=html.escape(app_id),
        css_href=_BASE_CSS_HREF,
    )


# --- partials ------------------------------------------------------------- #


def _render_card(entry: AppEntry) -> str:
    m = entry.manifest
    badges = "".join(
        f'<span class="tag tag-{html.escape(t)}">{html.escape(t)}</span>'
        for t in entry.tags
    )
    categories = ", ".join(html.escape(c) for c in m.categories) or "—"
    return (
        '<article class="card">'
        f'<a class="card-link" href="/app/{html.escape(m.id)}">'
        f'<img class="card-icon" src="{html.escape(m.icon)}" '
        f'alt="" loading="lazy">'
        '<div class="card-body">'
        f'<h2 class="card-title">{html.escape(m.name)}</h2>'
        f'<p class="card-id"><code>{html.escape(m.id)}</code></p>'
        f'<p class="card-desc">{html.escape(m.description)}</p>'
        f'<p class="card-meta">{categories}</p>'
        f'<p class="card-tags">{badges}</p>'
        "</div>"
        "</a>"
        "</article>"
    )


def _render_category_option(category: str, selected: str | None) -> str:
    sel = " selected" if selected == category else ""
    return f'<option value="{html.escape(category)}"{sel}>{html.escape(category)}</option>'


def _render_summary(*, total: int, offset: int, limit: int, query: str) -> str:
    if total == 0:
        if query:
            return (
                f'<p class="summary empty">No apps match '
                f"<q>{html.escape(query)}</q>.</p>"
            )
        return '<p class="summary empty">No apps registered yet.</p>'
    last = min(offset + limit, total)
    suffix = f' for <q>{html.escape(query)}</q>' if query else ""
    return (
        f'<p class="summary">Showing {offset + 1}–{last} of {total}{suffix}.</p>'
    )


def _render_pagination(
    *,
    total: int,
    offset: int,
    limit: int,
    query: str,
    category: str | None,
) -> str:
    if total <= limit:
        return ""
    parts: list[str] = []
    if offset > 0:
        prev_off = max(0, offset - limit)
        parts.append(
            f'<a class="page-link" href="{_browse_href(query, category, prev_off, limit)}">← Prev</a>'
        )
    if offset + limit < total:
        next_off = offset + limit
        parts.append(
            f'<a class="page-link" href="{_browse_href(query, category, next_off, limit)}">Next →</a>'
        )
    if not parts:
        return ""
    return '<nav class="pagination">' + " ".join(parts) + "</nav>"


def _browse_href(query: str, category: str | None, offset: int, limit: int) -> str:
    from urllib.parse import urlencode

    params: dict[str, str] = {}
    if query:
        params["q"] = query
    if category:
        params["category"] = category
    if offset:
        params["offset"] = str(offset)
    if limit != 24:
        params["limit"] = str(limit)
    qs = urlencode(params)
    return f"/?{qs}" if qs else "/"


_EMPTY_LIST = '<p class="empty">No apps to show.</p>'


# --- page templates ------------------------------------------------------- #

_BROWSE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<link rel="stylesheet" href="{css_href}">
</head>
<body>
<header class="site-header">
  <a class="brand" href="/">{site_title}</a>
  <p class="tagline">Apps for PagerOS — browse, search, install.</p>
</header>
<main>
  <form class="search" action="/" method="get" role="search">
    <label class="visually-hidden" for="q">Search apps</label>
    <input id="q" name="q" type="search" placeholder="Search apps…" value="{query}" autocomplete="off">
    <label class="visually-hidden" for="category">Filter by category</label>
    <select id="category" name="category">
      <option value="">All categories</option>
      {category_options}
    </select>
    <button type="submit">Search</button>
  </form>
  {summary}
  <section class="cards">
    {cards}
  </section>
  {pagination}
</main>
<footer class="site-footer">
  <p>PagerOS Marketplace · <a href="/healthz">status</a></p>
</footer>
</body>
</html>
"""


_DETAIL = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<link rel="stylesheet" href="{css_href}">
</head>
<body>
<header class="site-header">
  <a class="brand" href="/">{site_title}</a>
  <p class="tagline"><a href="/">← Back to browse</a></p>
</header>
<main class="detail">
  <article class="detail-card">
    <img class="detail-icon" src="{icon}" alt="">
    <header>
      <h1>{name}</h1>
      <p class="detail-id"><code>{app_id}</code> · v{version}</p>
      <p class="detail-tags">{tags}</p>
    </header>
    <p class="detail-desc">{description}</p>
    <dl class="detail-facts">
      <dt>Categories</dt><dd>{categories}</dd>
      <dt>App URL</dt><dd><a rel="nofollow noopener" href="{url}">{url}</a></dd>
      <dt>Maintainer</dt><dd>{maintainer} &lt;{contact}&gt;</dd>
    </dl>
    <p class="detail-actions">{donate_block}</p>
  </article>
</main>
<footer class="site-footer">
  <p>PagerOS Marketplace · <a href="/healthz">status</a></p>
</footer>
</body>
</html>
"""


_NOT_FOUND = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<link rel="stylesheet" href="{css_href}">
</head>
<body>
<header class="site-header">
  <a class="brand" href="/">{site_title}</a>
</header>
<main>
  <h1>App not found</h1>
  <p>No app with id <code>{app_id}</code> is registered.</p>
  <p><a href="/">← Back to browse</a></p>
</main>
</body>
</html>
"""


CSS = """\
:root {
  color-scheme: light dark;
  --fg: #15171a;
  --bg: #fbfbfd;
  --muted: #5b5f66;
  --border: #d8dde3;
  --accent: #1f6feb;
}
@media (prefers-color-scheme: dark) {
  :root { --fg:#e5e7eb; --bg:#0d1117; --muted:#9ca3af; --border:#30363d; --accent:#58a6ff; }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font: 16px/1.5 -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  color: var(--fg);
  background: var(--bg);
}
.site-header { padding: 32px 24px 8px; }
.site-header .brand { font-size: 24px; font-weight: 700; color: var(--fg); text-decoration: none; }
.site-header .tagline { margin: 4px 0 0; color: var(--muted); }
.site-footer { padding: 32px 24px; color: var(--muted); border-top: 1px solid var(--border); margin-top: 48px; font-size: 14px; }
main { max-width: 1080px; margin: 0 auto; padding: 16px 24px 48px; }
.visually-hidden { position: absolute; width: 1px; height: 1px; clip: rect(0 0 0 0); overflow: hidden; }
.search { display: flex; flex-wrap: wrap; gap: 8px; margin: 16px 0 24px; align-items: center; }
.search input[type="search"] { flex: 1 1 240px; padding: 10px 12px; border: 1px solid var(--border); border-radius: 8px; background: transparent; color: var(--fg); }
.search select { padding: 10px 12px; border: 1px solid var(--border); border-radius: 8px; background: transparent; color: var(--fg); }
.search button { padding: 10px 16px; border: 1px solid var(--accent); border-radius: 8px; background: var(--accent); color: white; cursor: pointer; }
.summary { color: var(--muted); margin: 0 0 16px; }
.summary.empty { font-style: italic; }
.cards { display: grid; gap: 16px; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); }
.card { border: 1px solid var(--border); border-radius: 12px; background: transparent; overflow: hidden; }
.card-link { display: flex; gap: 12px; padding: 12px; text-decoration: none; color: inherit; }
.card-icon { width: 64px; height: 64px; border-radius: 12px; object-fit: cover; flex: 0 0 auto; background: var(--border); }
.card-body { min-width: 0; }
.card-title { margin: 0; font-size: 17px; }
.card-id { margin: 2px 0; font-size: 12px; color: var(--muted); }
.card-id code { font-size: 12px; }
.card-desc { margin: 6px 0; font-size: 14px; color: var(--fg); display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
.card-meta { margin: 4px 0; font-size: 12px; color: var(--muted); }
.card-tags { margin: 6px 0 0; }
.tag { display: inline-block; padding: 2px 8px; border-radius: 999px; font-size: 11px; margin-right: 4px; border: 1px solid var(--border); color: var(--muted); }
.tag-featured { border-color: var(--accent); color: var(--accent); }
.tag-verified { border-color: #15803d; color: #15803d; }
.tag-flagged { border-color: #b91c1c; color: #b91c1c; }
.pagination { display: flex; gap: 12px; margin: 24px 0 0; }
.page-link { padding: 8px 12px; border: 1px solid var(--border); border-radius: 8px; text-decoration: none; color: var(--fg); }
.detail-card { display: grid; grid-template-columns: 96px 1fr; gap: 16px 24px; padding: 24px; border: 1px solid var(--border); border-radius: 16px; }
.detail-icon { width: 96px; height: 96px; border-radius: 16px; background: var(--border); object-fit: cover; }
.detail-desc { grid-column: 1 / -1; margin: 8px 0; }
.detail-facts { grid-column: 1 / -1; display: grid; grid-template-columns: max-content 1fr; gap: 4px 16px; margin: 8px 0; }
.detail-facts dt { color: var(--muted); }
.detail-facts dd { margin: 0; }
.detail-actions { grid-column: 1 / -1; margin: 16px 0 0; }
.donate { display: inline-block; padding: 10px 16px; border: 1px solid var(--accent); border-radius: 8px; background: var(--accent); color: white; text-decoration: none; }
.detail-id { color: var(--muted); margin: 2px 0; }
"""
