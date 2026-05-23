"""PagerOS Hacker News reader.

Renders the current Hacker News top stories on the pager. Two screens:

* ``/`` — heading + list of the top 15 stories.
* ``/story?id=<hn-item-id>`` — story metadata + an "Open external" button
  pointing at the upstream article URL.

The Firebase HN API is polled on demand; results are memoised in process
for 5 minutes so back-to-back screen renders don't hammer it. If the
network is unavailable (common in dev), we fall back to a small
hardcoded story list so the screens still render.
"""

from __future__ import annotations

import json
import logging
import threading
import time
import urllib.parse
import urllib.request
from typing import Any

from pageros import App, Button, List, ListItem, Screen, Text

log = logging.getLogger("examples.news")

HN_TOP_URL = "https://hacker-news.firebaseio.com/v0/topstories.json"
HN_ITEM_URL = "https://hacker-news.firebaseio.com/v0/item/{id}.json"

HTTP_TIMEOUT = 5.0
CACHE_TTL = 300  # 5 minutes
TOP_N = 15
TITLE_MAX = 70

# Fallback stories used when the HN API is unreachable. Five canned rows
# keeps the home screen useful even on an air-gapped dev pager.
_FALLBACK_STORIES: list[dict[str, Any]] = [
    {
        "id": 1,
        "title": "PagerOS: a server-rendered app OS for a tiny QWERTY device",
        "score": 420,
        "descendants": 64,
        "url": "https://news.pageros.demo/post/1",
    },
    {
        "id": 2,
        "title": "Show HN: I put Hacker News on a pager",
        "score": 311,
        "descendants": 42,
        "url": "https://news.pageros.demo/post/2",
    },
    {
        "id": 3,
        "title": "Why CBOR beats JSON over LoRa",
        "score": 198,
        "descendants": 51,
        "url": "https://news.pageros.demo/post/3",
    },
    {
        "id": 4,
        "title": "The case for declarative UIs on constrained hardware",
        "score": 142,
        "descendants": 28,
        "url": "https://news.pageros.demo/post/4",
    },
    {
        "id": 5,
        "title": "Ask HN: What's on your pager home screen?",
        "score": 97,
        "descendants": 73,
        "url": "https://news.pageros.demo/post/5",
    },
]


# --------------------------------------------------------------------------- #
# In-memory cache
# --------------------------------------------------------------------------- #


_cache_lock = threading.Lock()
# key -> (timestamp, value). Both the top-id list and each item dict
# share this table; keys are URL strings to keep things simple.
_cache: dict[str, tuple[float, Any]] = {}


def _cache_get(key: str) -> Any | None:
    now = time.time()
    with _cache_lock:
        entry = _cache.get(key)
        if entry is None:
            return None
        ts, value = entry
        if now - ts > CACHE_TTL:
            _cache.pop(key, None)
            return None
        return value


def _cache_put(key: str, value: Any) -> None:
    with _cache_lock:
        _cache[key] = (time.time(), value)


def _fetch_json(url: str) -> Any | None:
    cached = _cache_get(url)
    if cached is not None:
        return cached
    req = urllib.request.Request(
        url, headers={"User-Agent": "pageros-news-example/0.1"}
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            data = json.loads(resp.read())
    except (OSError, ValueError) as exc:
        log.warning("HN fetch failed for %s: %s", url, exc)
        return None
    _cache_put(url, data)
    return data


# --------------------------------------------------------------------------- #
# HN helpers
# --------------------------------------------------------------------------- #


def _top_stories(limit: int = TOP_N) -> list[dict[str, Any]]:
    ids = _fetch_json(HN_TOP_URL)
    if not isinstance(ids, list):
        return list(_FALLBACK_STORIES)
    out: list[dict[str, Any]] = []
    for sid in ids[:limit]:
        item = _fetch_json(HN_ITEM_URL.format(id=sid))
        if isinstance(item, dict):
            out.append(item)
    if not out:
        return list(_FALLBACK_STORIES)
    return out


def _story(sid: int) -> dict[str, Any] | None:
    item = _fetch_json(HN_ITEM_URL.format(id=sid))
    if isinstance(item, dict):
        return item
    for s in _FALLBACK_STORIES:
        if s["id"] == sid:
            return s
    return None


def _domain(url: str | None) -> str:
    if not url:
        return ""
    try:
        host = urllib.parse.urlparse(url).hostname or ""
    except ValueError:
        return ""
    return host[4:] if host.startswith("www.") else host


def _trim(text: str, limit: int = TITLE_MAX) -> str:
    text = text.strip()
    if len(text) <= limit:
        return text
    return text[: max(1, limit - 1)].rstrip() + "…"


def _sub_line(story: dict[str, Any]) -> str:
    score = int(story.get("score") or 0)
    comments = int(story.get("descendants") or 0)
    dom = _domain(story.get("url"))
    parts = [f"↑{score}", f"\U0001f4ac{comments}"]
    if dom:
        parts.append(dom)
    return "  ".join(parts)


# --------------------------------------------------------------------------- #
# App
# --------------------------------------------------------------------------- #


app = App(name="news")


@app.screen("/")
def home():
    stories = _top_stories(TOP_N)
    items: list[ListItem] = []
    for s in stories:
        sid = s.get("id")
        title = s.get("title") or "(untitled)"
        items.append(
            ListItem(
                label=_trim(str(title)),
                sub=_sub_line(s),
                href=f"/story?id={sid}",
            )
        )
    return Screen(
        id="news_home",
        title="News",
        body=[
            Text("Hacker News", style="heading"),
            List(items=items),
            Button(label="Refresh", href="/"),
        ],
        ttl=CACHE_TTL,
    )


@app.screen("/story")
def story(request):
    ids = request.query.get("id", [])
    try:
        sid = int(ids[0]) if ids else 0
    except (TypeError, ValueError):
        sid = 0

    s = _story(sid)
    if s is None:
        return Screen(
            id="news_story_missing",
            title="News",
            body=[
                Text("Story not found", style="heading"),
                Text(f"id={sid}", style="dim"),
                Button(label="Back", href="/"),
            ],
            ttl=0,
        )

    title = str(s.get("title") or "(untitled)")
    score = int(s.get("score") or 0)
    comments = int(s.get("descendants") or 0)
    url = s.get("url") or ""
    dom = _domain(url) or "(no link)"

    body: list[Any] = [
        Text(_trim(title, 90), style="heading"),
        Text(f"{score} points · {comments} comments", style="dim"),
        Text(dom, style="body"),
    ]
    if url:
        body.append(Button(label="Open external", href=url, method="GET"))
    body.append(Button(label="Back", href="/"))

    return Screen(
        id=f"news_story_{sid}",
        title="News",
        body=body,
        ttl=CACHE_TTL,
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
