"""PagerOS direct-message example.

1:1 direct messages between two pagers — no group invite codes
needed. Each device picks up a short "DM handle" derived from the
first six hex chars of its ``device_id`` (uppercased), and conversations
key off a canonical thread id formed by sorting the two handles.

The wire model is intentionally a thin specialisation of the group-chat
pattern in ``examples/chat/app.py``:

- The :class:`Chat` widget renders scrollback + composer per-thread.
- A long-poll endpoint (``/events``) wakes on a per-thread
  :class:`threading.Event` whenever a new message lands.
- State persists to ``dms.json`` next to ``app.py`` (override with
  ``PAGEROS_DM_STORE``). Schema:

  .. code-block:: json

      {
        "handles": {"<HANDLE>": "<device_id>"},
        "threads": {"<HANDLE>|<HANDLE>": {
            "peers": ["<HANDLE>", "<HANDLE>"],
            "history": [{"from": "<HANDLE>", "s": "...", "ts": 0}]
        }},
        "inbox": {"<HANDLE>": ["<thread_key>", ...]}
      }

A peer handle you've never seen is auto-registered (the typed handle
becomes the canonical key, with no backing ``device_id`` yet). This
keeps the demo runnable from a single device by DMing yourself.
"""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Any

from pageros import (
    App,
    Button,
    Chat,
    ChatCompose,
    ChatMessage,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

HANDLE_LEN = 6
MAX_HISTORY = 200
PREVIEW_LEN = 32
MAX_MSG_LEN = 200


def _store_path() -> Path:
    return Path(os.environ.get("PAGEROS_DM_STORE", Path(__file__).with_name("dms.json")))


_lock = threading.Lock()
# Per-thread long-poll wake events. Reset to a fresh Event after each
# wake so subsequent waiters block again instead of busy-looping on a
# permanently-set flag (same trick as examples/chat/app.py).
_wake: dict[str, threading.Event] = {}


def _empty_state() -> dict[str, Any]:
    return {"handles": {}, "threads": {}, "inbox": {}}


def _load() -> dict[str, Any]:
    p = _store_path()
    if not p.exists():
        return _empty_state()
    try:
        return json.loads(p.read_text())
    except json.JSONDecodeError:
        return _empty_state()


def _save(state: dict[str, Any]) -> None:
    _store_path().write_text(json.dumps(state, indent=2, sort_keys=True))


def _wake_thread(thread_key: str) -> None:
    ev = _wake.setdefault(thread_key, threading.Event())
    ev.set()
    _wake[thread_key] = threading.Event()


def _handle_for(device_id: str) -> str:
    """Derive the 6-char DM handle for a device id."""
    return (device_id or "anon")[:HANDLE_LEN].upper().ljust(HANDLE_LEN, "X")


def _register(state: dict[str, Any], device_id: str) -> str:
    handle = _handle_for(device_id)
    if state["handles"].get(handle) != device_id:
        state["handles"][handle] = device_id
    state["inbox"].setdefault(handle, [])
    return handle


def _ensure_peer(state: dict[str, Any], peer_handle: str) -> str:
    """Auto-register an unknown peer so single-device demos work."""
    peer_handle = peer_handle.upper()
    state["handles"].setdefault(peer_handle, "")
    state["inbox"].setdefault(peer_handle, [])
    return peer_handle


def _thread_key(a: str, b: str) -> str:
    return "|".join(sorted([a.upper(), b.upper()]))


def _peer_of(thread_key: str, me: str) -> str:
    a, b = thread_key.split("|", 1)
    return b if a == me else a


def _ensure_thread(state: dict[str, Any], me: str, peer: str) -> str:
    tkey = _thread_key(me, peer)
    if tkey not in state["threads"]:
        state["threads"][tkey] = {"peers": sorted([me, peer]), "history": []}
    for h in (me, peer):
        inbox = state["inbox"].setdefault(h, [])
        if tkey not in inbox:
            inbox.append(tkey)
    return tkey


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _preview(history: list[dict[str, Any]]) -> str:
    if not history:
        return "(no messages yet)"
    last = history[-1]
    s = last.get("s", "")
    if len(s) > PREVIEW_LEN:
        s = s[: PREVIEW_LEN - 1] + "..."
    return f"{last.get('from', '?')}: {s}"


app = App(name="dm")


def _home_screen(device_id: str) -> Screen:
    with _lock:
        state = _load()
        me = _register(state, device_id)
        _save(state)
    inbox = state["inbox"].get(me, [])
    items: list[ListItem] = []
    for tkey in reversed(inbox):
        peer = _peer_of(tkey, me)
        history = state["threads"].get(tkey, {}).get("history", [])
        items.append(
            ListItem(label=peer, sub=_preview(history), href=f"/thread?with={peer}")
        )
    body: list[Any] = [
        Text(s=f"Your handle: {me}", style="dim"),
    ]
    if items:
        body.append(List(items=items))
    else:
        body.append(Text(s="No conversations yet.", style="dim"))
    body.append(Button(label="Start new DM", href="/new"))
    return Screen(id="scr_home", title="DMs", body=body)


def _thread_screen(device_id: str, peer_handle: str) -> Screen:
    peer_handle = peer_handle.upper()
    with _lock:
        state = _load()
        me = _register(state, device_id)
        _ensure_peer(state, peer_handle)
        tkey = _ensure_thread(state, me, peer_handle)
        _save(state)
    room = state["threads"][tkey]
    history = room["history"][-50:]
    chat = Chat(
        group_id=tkey,
        messages=[
            ChatMessage(from_=m["from"], s=m["s"], ts=int(m["ts"])) for m in history
        ],
        compose=ChatCompose(name="msg", submit=f"/send?with={peer_handle}"),
    )
    return Screen(
        id=f"scr_thread_{peer_handle}",
        title=f"DM {peer_handle}",
        body=[chat, Button(label="Back", href="/")],
        subscribe_groups=[tkey],
    )


@app.screen("/")
def home(request):
    return _home_screen(request.ctx.device_id)


@app.screen("/new")
def new_form(request):
    return Screen(
        id="scr_new",
        title="New DM",
        body=[
            Form(
                action="/new",
                method="POST",
                fields=[
                    Input(name="to", label="Peer handle", max=HANDLE_LEN),
                    Input(name="msg", label="First message", max=MAX_MSG_LEN),
                ],
                submit="Send",
            ),
            Button(label="Back", href="/"),
        ],
    )


@app.handler("/new", method="POST")
def new_send(request):
    payload = request.body if isinstance(request.body, dict) else {}
    peer = (payload.get("to") or "").strip().upper()
    text = (payload.get("msg") or "").strip()
    if not peer:
        return Screen(
            id="scr_new",
            title="New DM",
            body=[
                Notification(s="Peer handle is required.", level="error"),
                Button(label="Back", href="/new"),
            ],
        )
    if not text:
        return Screen(
            id="scr_new",
            title="New DM",
            body=[
                Notification(s="Message cannot be empty.", level="error"),
                Button(label="Back", href="/new"),
            ],
        )
    with _lock:
        state = _load()
        me = _register(state, request.ctx.device_id)
        _ensure_peer(state, peer)
        tkey = _ensure_thread(state, me, peer)
        msg = {"from": me, "s": text[:MAX_MSG_LEN], "ts": int(time.time())}
        state["threads"][tkey]["history"].append(msg)
        state["threads"][tkey]["history"] = state["threads"][tkey]["history"][
            -MAX_HISTORY:
        ]
        _save(state)
    _wake_thread(tkey)
    return _thread_screen(request.ctx.device_id, peer)


@app.screen("/thread")
def thread(request):
    peer = _first_q(request, "with")
    if not peer:
        return _home_screen(request.ctx.device_id)
    return _thread_screen(request.ctx.device_id, peer)


@app.handler("/send", method="POST")
def send(request):
    peer = _first_q(request, "with")
    payload = request.body if isinstance(request.body, dict) else {}
    text = (payload.get("msg") or "").strip()
    if not peer:
        return _home_screen(request.ctx.device_id)
    if not text:
        return _thread_screen(request.ctx.device_id, peer)
    with _lock:
        state = _load()
        me = _register(state, request.ctx.device_id)
        _ensure_peer(state, peer)
        tkey = _ensure_thread(state, me, peer)
        msg = {"from": me, "s": text[:MAX_MSG_LEN], "ts": int(time.time())}
        state["threads"][tkey]["history"].append(msg)
        state["threads"][tkey]["history"] = state["threads"][tkey]["history"][
            -MAX_HISTORY:
        ]
        _save(state)
    _wake_thread(tkey)
    return _thread_screen(request.ctx.device_id, peer)


@app.screen("/events")
def long_poll(request):
    """Long-poll for new messages while the user has a thread open."""
    peer = _first_q(request, "with")
    if not peer:
        return {"events": []}
    me = _handle_for(request.ctx.device_id)
    tkey = _thread_key(me, peer)
    ev = _wake.setdefault(tkey, threading.Event())
    ev.wait(timeout=25.0)
    state = _load()
    room = state["threads"].get(tkey, {})
    last = room.get("history", [])[-1:] if room else []
    return {
        "events": [
            {"kind": "group_message", "group_id": tkey, "msg": m} for m in last
        ]
    }


if __name__ == "__main__":
    app.run()
