"""PagerOS notes example (DOCS-006).

A small CRUD app — add / view / edit / delete notes — that exercises:

- ``list`` + ``list_item`` widgets for the index screen,
- ``form`` + ``input`` widgets for new / edit screens,
- ``button`` widgets (including ``confirm``) for navigation + delete,
- ``notification`` widgets for inline flash messages,
- ``ctx.device_id`` for per-device storage isolation (SPEC §8.3).

Notes are persisted to a JSON file next to ``app.py`` (override with
``PAGEROS_NOTES_STORE``). The store is intentionally simple — production
apps would back this with sqlite or the host device's own storage hooks,
but a JSON blob is plenty for showing the shape of a CRUD Frame app.
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
import uuid
from pathlib import Path
from typing import Any

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

log = logging.getLogger("examples.notes")


# --------------------------------------------------------------------------- #
# Per-device note store
# --------------------------------------------------------------------------- #
#
# Layout on disk:
#   {
#     "<device_id_b64url>": [
#       {"id": "...", "title": "...", "body": "...", "ts": 1700000000},
#       ...
#     ]
#   }
#
# Anonymous requests (no PagerOS-Device header) land under the empty
# string key so the dev server is usable without a verifier wired up.

DEFAULT_STORE = Path(__file__).with_name("notes-store.json")
STORE_PATH = Path(os.environ.get("PAGEROS_NOTES_STORE", str(DEFAULT_STORE)))

TITLE_MAX = 80
BODY_MAX = 480


class NoteStore:
    """Tiny JSON-backed per-device note store."""

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
            log.warning("notes store %s is malformed; starting fresh", self.path)
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
            return sorted(
                self._data.get(device_id, ()),
                key=lambda n: -int(n.get("ts", 0)),
            )

    def get(self, device_id: str, note_id: str) -> dict[str, Any] | None:
        with self._lock:
            for n in self._data.get(device_id, ()):
                if n.get("id") == note_id:
                    return dict(n)
        return None

    def add(self, device_id: str, title: str, body: str) -> dict[str, Any]:
        note = {
            "id": uuid.uuid4().hex[:12],
            "title": title,
            "body": body,
            "ts": int(time.time()),
        }
        with self._lock:
            self._data.setdefault(device_id, []).append(note)
            self._flush_unlocked()
        return note

    def update(
        self, device_id: str, note_id: str, title: str, body: str
    ) -> bool:
        with self._lock:
            for n in self._data.get(device_id, ()):
                if n.get("id") == note_id:
                    n["title"] = title
                    n["body"] = body
                    n["ts"] = int(time.time())
                    self._flush_unlocked()
                    return True
        return False

    def delete(self, device_id: str, note_id: str) -> bool:
        with self._lock:
            notes = self._data.get(device_id)
            if not notes:
                return False
            remaining = [n for n in notes if n.get("id") != note_id]
            if len(remaining) == len(notes):
                return False
            self._data[device_id] = remaining
            self._flush_unlocked()
            return True


store = NoteStore(STORE_PATH)
app = App(name="notes")


# --------------------------------------------------------------------------- #
# Screen builders
# --------------------------------------------------------------------------- #


def _trim(value: object, n: int = 80) -> str:
    if not isinstance(value, str):
        return ""
    value = value.strip()
    return value if len(value) <= n else value[: n - 1] + "…"


def _format_ts(ts: int) -> str:
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))


def _query_id(request) -> str:
    values = request.query.get("id") or []
    return values[0] if values else ""


def _list_screen(device_id: str, *, flash: str | None = None) -> Screen:
    notes = store.list(device_id)
    body: list[Any] = []
    if flash:
        body.append(Notification(s=flash))
    if not notes:
        body.append(Text("No notes yet — add one to get started.", style="dim"))
    else:
        body.append(
            List(
                items=[
                    ListItem(
                        label=_trim(n.get("title") or "(untitled)"),
                        sub=_trim(n.get("body") or "", 60),
                        href=f"/notes?id={n['id']}",
                    )
                    for n in notes
                ]
            )
        )
    body.append(Button(label="New note", href="/new", method="GET"))
    return Screen(id="notes_list", title="Notes", body=body, ttl=0)


def _note_form(
    action: str,
    *,
    title: str = "",
    body: str = "",
    submit_label: str = "Save",
) -> Form:
    return Form(
        action=action,
        method="POST",
        submit=submit_label,
        fields=[
            Input(name="title", label="Title", value=title, max=TITLE_MAX),
            Input(name="body", label="Body", value=body, max=BODY_MAX),
        ],
    )


def _missing_screen() -> Screen:
    return Screen(
        id="notes_missing",
        title="Notes",
        body=[
            Notification(s="That note no longer exists.", level="warn"),
            Button(label="Back to list", href="/", method="GET"),
        ],
        ttl=0,
    )


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #


@app.screen("/")
def home(request):
    return _list_screen(request.ctx.device_id)


@app.screen("/new")
def new_form(_request=None):
    return Screen(
        id="notes_new",
        title="New note",
        body=[_note_form("/notes")],
        ttl=0,
    )


@app.handler("/notes", method="POST")
def create(request):
    body = request.body if isinstance(request.body, dict) else {}
    title = (body.get("title") or "").strip()
    text = (body.get("body") or "").strip()
    if not title and not text:
        return Screen(
            id="notes_empty",
            title="New note",
            body=[
                Notification(
                    s="Title and body are both empty.", level="warn"
                ),
                _note_form("/notes", title=title, body=text),
            ],
            ttl=0,
        )
    note = store.add(
        request.ctx.device_id, title or "(untitled)", text
    )
    return _list_screen(
        request.ctx.device_id,
        flash=f"Saved “{_trim(note['title'], 40)}”.",
    )


@app.screen("/notes")
def detail(request):
    note = store.get(request.ctx.device_id, _query_id(request))
    if note is None:
        return _missing_screen()
    return Screen(
        id="notes_detail",
        title=_trim(note["title"], 40) or "Note",
        body=[
            Text(note["title"] or "(untitled)", style="heading"),
            Text(note["body"] or "(empty)", style="body"),
            Text(f"Saved {_format_ts(int(note['ts']))}", style="dim"),
            Button(
                label="Edit",
                href=f"/edit?id={note['id']}",
                method="GET",
            ),
            Button(
                label="Delete",
                href=f"/notes/delete?id={note['id']}",
                method="POST",
                confirm="Delete this note?",
            ),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=0,
    )


@app.screen("/edit")
def edit_form(request):
    note = store.get(request.ctx.device_id, _query_id(request))
    if note is None:
        return _missing_screen()
    return Screen(
        id="notes_edit",
        title="Edit note",
        body=[
            _note_form(
                f"/notes/update?id={note['id']}",
                title=note["title"],
                body=note["body"],
                submit_label="Save changes",
            )
        ],
        ttl=0,
    )


@app.handler("/notes/update", method="POST")
def update(request):
    note_id = _query_id(request)
    body = request.body if isinstance(request.body, dict) else {}
    title = (body.get("title") or "").strip()
    text = (body.get("body") or "").strip()
    ok = store.update(
        request.ctx.device_id,
        note_id,
        title or "(untitled)",
        text,
    )
    return _list_screen(
        request.ctx.device_id,
        flash="Saved changes." if ok else "Couldn't find that note.",
    )


@app.handler("/notes/delete", method="POST")
def delete(request):
    ok = store.delete(request.ctx.device_id, _query_id(request))
    return _list_screen(
        request.ctx.device_id,
        flash="Deleted." if ok else "Couldn't find that note.",
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
