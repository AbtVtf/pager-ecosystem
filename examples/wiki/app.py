"""PagerOS Wikipedia reader (wiki.pageros.app).

Three screens:

* ``/``        — search form + "Surprise me" random-article button.
* ``/search``  — OpenSearch results list (title + short description).
* ``/article`` — REST page summary (title, description, extract).
* ``/random``  — random REST page summary, same shape as /article.

The pager has a 480x222 screen; extracts are truncated to roughly
1200 characters so the renderer doesn't have to paginate huge blobs.
All outbound calls use a polite ``User-Agent`` per Wikipedia's API
etiquette and are wrapped in ``try/except`` so transient network
failures degrade to a friendly Notification instead of a 500.
"""

from __future__ import annotations

import json
import logging
import urllib.parse
import urllib.request

from pageros import App
from pageros.widgets import (
    Button,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

log = logging.getLogger("examples.wiki")

# Wikipedia API endpoints.
OPENSEARCH_URL = "https://en.wikipedia.org/w/api.php"
SUMMARY_URL = "https://en.wikipedia.org/api/rest_v1/page/summary/"
RANDOM_URL = "https://en.wikipedia.org/api/rest_v1/page/random/summary"

# Polite UA per https://meta.wikimedia.org/wiki/User-Agent_policy
USER_AGENT = "pageros-wiki/1.0 (demo@pageros.test)"
HTTP_TIMEOUT = 6.0

# Pager screen is 480x222 — keep extracts tight.
EXTRACT_MAX_CHARS = 1200
DESC_MAX_CHARS = 60

app = App(name="wiki")


# --------------------------------------------------------------------------- #
# HTTP helpers
# --------------------------------------------------------------------------- #


def _fetch_json(url: str) -> dict | list | None:
    """GET ``url`` and decode JSON. Returns ``None`` on any failure."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            return json.loads(resp.read())
    except (OSError, ValueError) as exc:
        log.warning("wiki fetch failed for %s: %s", url, exc)
        return None


def _trim(s: str | None, max_chars: int) -> str:
    if not s:
        return ""
    s = s.strip()
    if len(s) <= max_chars:
        return s
    return s[: max_chars - 1].rstrip() + "…"


# --------------------------------------------------------------------------- #
# Frame builders
# --------------------------------------------------------------------------- #


def _summary_screen(screen_id: str, payload: dict | None) -> Screen:
    """Render a REST page-summary payload as an article screen."""
    if not payload or not isinstance(payload, dict):
        return Screen(
            id=screen_id,
            title="Wikipedia",
            body=[
                Notification("Couldn't reach Wikipedia.", level="error"),
                Button(label="Back", href="/", method="GET"),
            ],
            ttl=0,
        )

    title = str(payload.get("title") or "Untitled")
    description = payload.get("description")
    extract = _trim(payload.get("extract"), EXTRACT_MAX_CHARS)

    body: list = [Text(title, style="heading")]
    if description:
        body.append(Text(str(description), style="dim"))
    if extract:
        body.append(Text(extract, style="body"))
    else:
        body.append(Text("No summary available.", style="dim"))
    body.append(Button(label="Open another", href="/", method="GET"))
    body.append(Button(label="Back", href="/", method="GET"))

    return Screen(id=screen_id, title="Wikipedia", body=body, ttl=300)


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #


@app.screen("/")
def home():
    return Screen(
        id="wiki_home",
        title="Wikipedia",
        body=[
            Text("Wikipedia", style="heading"),
            Form(
                action="/search",
                method="GET",
                fields=[Input(name="q", label="Search", max=120)],
                submit="Search",
            ),
            Text("Random article", style="dim"),
            Button(label="Surprise me", href="/random", method="GET"),
        ],
        ttl=0,
    )


@app.screen("/search")
def search(request):
    q_values = request.query.get("q", [])
    q = (q_values[0] if q_values else "").strip()
    if not q:
        return Screen(
            id="wiki_search_empty",
            title="Wikipedia",
            body=[
                Notification("Type something to search.", level="warn"),
                Button(label="Back", href="/", method="GET"),
            ],
            ttl=0,
        )

    qs = urllib.parse.urlencode(
        {
            "action": "opensearch",
            "search": q,
            "limit": 10,
            "format": "json",
        }
    )
    payload = _fetch_json(f"{OPENSEARCH_URL}?{qs}")

    # OpenSearch shape: [query, [titles], [descs], [urls]]
    titles: list[str] = []
    descs: list[str] = []
    if (
        isinstance(payload, list)
        and len(payload) >= 3
        and isinstance(payload[1], list)
        and isinstance(payload[2], list)
    ):
        titles = [str(t) for t in payload[1]]
        descs = [str(d) for d in payload[2]]

    if not titles:
        return Screen(
            id="wiki_search_none",
            title="Wikipedia",
            body=[
                Notification("No matches", level="info"),
                Text(f"No results for \"{_trim(q, 40)}\".", style="dim"),
                Button(label="Back", href="/", method="GET"),
            ],
            ttl=0,
        )

    items: list[ListItem] = []
    for i, title in enumerate(titles):
        sub = _trim(descs[i] if i < len(descs) else "", DESC_MAX_CHARS)
        href = "/article?" + urllib.parse.urlencode({"t": title})
        items.append(
            ListItem(label=_trim(title, 60), sub=sub or None, href=href)
        )

    return Screen(
        id="wiki_search_results",
        title="Wikipedia",
        body=[
            Text(f"Results for \"{_trim(q, 40)}\"", style="dim"),
            List(items=items),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=60,
    )


@app.screen("/article")
def article(request):
    t_values = request.query.get("t", [])
    title = (t_values[0] if t_values else "").strip()
    if not title:
        return Screen(
            id="wiki_article_missing",
            title="Wikipedia",
            body=[
                Notification("No article title given.", level="warn"),
                Button(label="Back", href="/", method="GET"),
            ],
            ttl=0,
        )

    # The REST summary endpoint expects the title in path position with
    # spaces as underscores and the rest URL-encoded.
    slug = urllib.parse.quote(title.replace(" ", "_"), safe="")
    payload = _fetch_json(f"{SUMMARY_URL}{slug}")
    return _summary_screen("wiki_article", payload if isinstance(payload, dict) else None)


@app.screen("/random")
def random_article():
    payload = _fetch_json(RANDOM_URL)
    return _summary_screen(
        "wiki_random", payload if isinstance(payload, dict) else None
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
