"""PagerOS translate example.

A small 3-screen app that translates short text snippets between 12
languages using a LibreTranslate-compatible HTTP endpoint. Per-device
history of the last 10 translations is stored on disk next to this
file.

Environment:

- ``LIBRETRANSLATE_URL`` — base URL of the LibreTranslate service.
  Defaults to ``https://libretranslate.com``.
- ``LIBRETRANSLATE_API_KEY`` — optional API key forwarded as the
  ``api_key`` field in the translate POST body.

If the upstream service is unreachable the app degrades to a stub
translator that just echoes the input so the screens still render on
the 480x222 pager.
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

import requests

from pageros import (
    App,
    Button,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

log = logging.getLogger("examples.translate")


# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #

LANGS: list[tuple[str, str]] = [
    ("en", "English"),
    ("es", "Spanish"),
    ("fr", "French"),
    ("de", "German"),
    ("it", "Italian"),
    ("pt", "Portuguese"),
    ("nl", "Dutch"),
    ("ja", "Japanese"),
    ("zh", "Chinese"),
    ("hi", "Hindi"),
    ("ar", "Arabic"),
    ("ru", "Russian"),
]

LANG_CODES = [code for code, _name in LANGS]
LANG_CODES_LINE = " ".join(LANG_CODES)
VALID_CODES = set(LANG_CODES)

LIBRETRANSLATE_BASE = os.environ.get(
    "LIBRETRANSLATE_URL", "https://libretranslate.com"
).rstrip("/")
LIBRETRANSLATE_API_KEY = os.environ.get("LIBRETRANSLATE_API_KEY")
HTTP_TIMEOUT = 6.0

HISTORY_PATH = Path(__file__).with_name("translate_history.json")
HISTORY_MAX = 10
TEXT_MAX = 300


# --------------------------------------------------------------------------- #
# Per-device history store
# --------------------------------------------------------------------------- #


class HistoryStore:
    """Tiny JSON-backed per-device history of the last N translations."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.Lock()
        self._data: dict[str, list[dict[str, Any]]] = self._load()

    def _load(self) -> dict[str, list[dict[str, Any]]]:
        if not self.path.exists():
            return {}
        try:
            raw = json.loads(self.path.read_text("utf-8"))
        except (OSError, ValueError):
            log.warning("history %s malformed; starting fresh", self.path)
            return {}
        if not isinstance(raw, dict):
            return {}
        return {k: list(v) for k, v in raw.items() if isinstance(v, list)}

    def _flush_unlocked(self) -> None:
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(
            json.dumps(self._data, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        os.replace(tmp, self.path)

    def list(self, device_id: str) -> list[dict[str, Any]]:
        with self._lock:
            return list(self._data.get(device_id, ()))

    def add(
        self,
        device_id: str,
        src: str,
        tgt: str,
        original: str,
        translated: str,
    ) -> None:
        entry = {
            "src": src,
            "tgt": tgt,
            "original": original,
            "translated": translated,
            "ts": int(time.time()),
        }
        with self._lock:
            bucket = self._data.setdefault(device_id, [])
            bucket.insert(0, entry)
            del bucket[HISTORY_MAX:]
            self._flush_unlocked()

    def clear(self, device_id: str) -> None:
        with self._lock:
            if device_id in self._data:
                self._data.pop(device_id, None)
                self._flush_unlocked()


history = HistoryStore(HISTORY_PATH)
app = App(name="translate")


# --------------------------------------------------------------------------- #
# Translation
# --------------------------------------------------------------------------- #


class TranslateError(Exception):
    """Raised when the upstream translator returns no usable text."""


def _translate(text: str, src: str, tgt: str) -> str:
    """Call LibreTranslate. Raises on any failure."""
    payload: dict[str, Any] = {"q": text, "source": src, "target": tgt}
    if LIBRETRANSLATE_API_KEY:
        payload["api_key"] = LIBRETRANSLATE_API_KEY
    resp = requests.post(
        f"{LIBRETRANSLATE_BASE}/translate",
        json=payload,
        timeout=HTTP_TIMEOUT,
    )
    resp.raise_for_status()
    data = resp.json()
    out = data.get("translatedText") if isinstance(data, dict) else None
    if not isinstance(out, str) or not out:
        raise TranslateError("empty translation in response")
    return out


def _translate_or_stub(text: str, src: str, tgt: str) -> tuple[str, bool]:
    """Return (translated_text, was_stub)."""
    try:
        return _translate(text, src, tgt), False
    except (requests.RequestException, ValueError, TranslateError) as exc:
        log.warning("translate fallback to stub: %s", exc)
        return f"[stub] {text}", True


# --------------------------------------------------------------------------- #
# Screen helpers
# --------------------------------------------------------------------------- #


def _trim(value: str, n: int) -> str:
    value = value or ""
    return value if len(value) <= n else value[: n - 1] + "…"


def _home_screen(
    *,
    src: str = "en",
    tgt: str = "es",
    q: str = "",
    flash: Notification | None = None,
) -> Screen:
    body: list[Any] = [Text("Translate", style="heading")]
    if flash is not None:
        body.append(flash)
    body.extend(
        [
            Form(
                action="/do",
                method="POST",
                submit="Translate",
                fields=[
                    Input(name="q", label="Text", value=q, max=TEXT_MAX),
                    Input(name="src", label="From (code)", value=src),
                    Input(name="tgt", label="To (code)", value=tgt),
                ],
            ),
            Text(LANG_CODES_LINE, style="dim"),
            Button(label="History", href="/history", method="GET"),
        ]
    )
    return Screen(id="tr_home", title="Translate", body=body, ttl=0)


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #


@app.screen("/")
def home(_request=None):
    return _home_screen()


@app.handler("/do", method="POST")
def do_translate(request):
    body = request.body if isinstance(request.body, dict) else {}
    q = (body.get("q") or "").strip()
    src = (body.get("src") or "en").strip().lower()
    tgt = (body.get("tgt") or "es").strip().lower()

    if not q:
        return _home_screen(
            src=src,
            tgt=tgt,
            q=q,
            flash=Notification(s="Enter text to translate.", level="warn"),
        )
    if src not in VALID_CODES or tgt not in VALID_CODES:
        return _home_screen(
            src=src,
            tgt=tgt,
            q=q,
            flash=Notification(
                s="Unknown language code.", level="error"
            ),
        )

    translated, was_stub = _translate_or_stub(q[:TEXT_MAX], src, tgt)
    history.add(request.ctx.device_id, src, tgt, q[:TEXT_MAX], translated)

    body_widgets: list[Any] = [Text("Translation", style="heading")]
    if was_stub:
        body_widgets.append(
            Notification(
                s="Upstream unreachable — showing stub.", level="error"
            )
        )
    body_widgets.extend(
        [
            Text(f"{src} -> {tgt}", style="dim"),
            Text(q[:TEXT_MAX], style="body"),
            Text(translated, style="heading"),
            Button(label="Translate another", href="/", method="GET"),
            Button(label="History", href="/history", method="GET"),
        ]
    )
    return Screen(
        id="tr_result",
        title="Translation",
        body=body_widgets,
        ttl=0,
    )


@app.screen("/history")
def history_screen(request):
    entries = history.list(request.ctx.device_id)
    body: list[Any] = [Text("Recent", style="heading")]
    if not entries:
        body.append(Text("No translations yet.", style="dim"))
    else:
        body.append(
            List(
                items=[
                    ListItem(
                        label=_trim(e.get("translated", ""), 50)
                        or "(empty)",
                        sub=f"{e.get('src', '?')}->{e.get('tgt', '?')} · "
                        f"{_trim(e.get('original', ''), 30)}",
                    )
                    for e in entries
                ]
            )
        )
    body.extend(
        [
            Button(
                label="Clear",
                href="/clear",
                method="POST",
                confirm="Clear history?",
            ),
            Button(label="Back", href="/", method="GET"),
        ]
    )
    return Screen(id="tr_history", title="Recent", body=body, ttl=0)


@app.handler("/clear", method="POST")
def clear(request):
    history.clear(request.ctx.device_id)
    return _home_screen(
        flash=Notification(s="History cleared.", level="info")
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
