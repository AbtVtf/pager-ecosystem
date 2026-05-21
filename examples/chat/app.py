"""PagerOS chat example (DOCS-010).

Multi-device group chat — the reference for SPEC §5.4.2 (group events)
and §5.3.10 (the ``chat`` widget). Demonstrates:

- the ``chat`` widget rendered with a scrollback + inline composer,
- the ``presence_list`` widget for "who's here",
- the server-side group membership model (SPEC §9.6: app owns
  membership, devices prove identity per-group),
- server-pushed ``group_message`` events to online subscribers via a
  long-polling endpoint, and (acknowledged but not exercised here) to
  offline subscribers via the Push Relay (SPEC §6.6.6).

Group membership in this v1 example is **invite-code based**: a user
joins by typing a shared 6-char code. Routes use a ``?gid=<code>`` query
parameter because the v1 SDK router matches paths exactly.

Persisted to ``rooms.json`` next to ``app.py`` (override with
``PAGEROS_CHAT_STORE``).
"""

from __future__ import annotations

import json
import os
import secrets
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
    PresenceList,
    PresenceMember,
    Screen,
    Text,
)

MAX_HISTORY = 200


def _store_path() -> Path:
    return Path(os.environ.get("PAGEROS_CHAT_STORE", Path(__file__).with_name("rooms.json")))


_lock = threading.Lock()
# Online subscribers, keyed by group_id → set of device_ids. Reset on
# restart; used to decide who'd get the long-poll event vs the push.
_online: dict[str, set[str]] = {}
# Per-group long-poll wake events.
_wake: dict[str, threading.Event] = {}


def _empty_state() -> dict[str, Any]:
    return {"rooms": {}, "memberships": {}}


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


def _wake_group(group_id: str) -> None:
    ev = _wake.setdefault(group_id, threading.Event())
    ev.set()
    _wake[group_id] = threading.Event()


def _members_for(group_id: str, state: dict[str, Any]) -> list[str]:
    return list(state["rooms"].get(group_id, {}).get("members", []))


def _first_q(request, key: str) -> str:
    """Read the first value of ``?key=…`` from the dispatcher's query map."""
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


app = App(name="chat")


def _rooms_screen(device_id: str) -> Screen:
    state = _load()
    mine = state["memberships"].get(device_id, [])
    if not mine:
        return Screen(
            id="scr_no_rooms",
            title="Chat",
            body=[
                Text(s="You aren't in any rooms yet.", style="dim"),
                Button(label="Create room", href="/new", method="POST"),
                Button(label="Join by code", href="/join"),
            ],
        )
    return Screen(
        id="scr_rooms",
        title="Chat",
        body=[
            List(items=[
                ListItem(label=gid, sub=f"{len(_members_for(gid, state))} members", href=f"/room?gid={gid}")
                for gid in mine
            ]),
            Button(label="Create room", href="/new", method="POST"),
            Button(label="Join by code", href="/join"),
        ],
    )


def _room_screen(device_id: str, group_id: str) -> Screen:
    state = _load()
    room = state["rooms"].get(group_id)
    if not room or device_id not in room["members"]:
        return Screen(id="scr_404", title="Chat",
                      body=[Notification(s="Room not found.", level="error")])
    history = room["history"][-MAX_HISTORY:]
    members = list(room["members"])
    _online.setdefault(group_id, set()).add(device_id)
    presence = PresenceList(
        group_id=group_id,
        members=[
            PresenceMember(
                id=did or "anon",
                name=(did[:8] if did else "anon"),
                online=did in _online.get(group_id, set()),
            )
            for did in members
        ],
    )
    chat = Chat(
        group_id=group_id,
        messages=[ChatMessage(from_=m["from"], s=m["s"], ts=int(m["ts"])) for m in history],
        compose=ChatCompose(name="msg", submit=f"/send?gid={group_id}"),
    )
    return Screen(
        id=f"scr_room_{group_id}",
        title=f"#{group_id}",
        body=[presence, chat],
        subscribe_groups=[group_id],
    )


@app.screen("/")
def home(request):
    return _rooms_screen(request.ctx.device_id)


@app.handler("/new", method="POST")
def new_room(request):
    code = secrets.token_hex(3).upper()
    with _lock:
        state = _load()
        state["rooms"][code] = {"members": [request.ctx.device_id], "history": []}
        state["memberships"].setdefault(request.ctx.device_id, []).append(code)
        _save(state)
    return _room_screen(request.ctx.device_id, code)


@app.screen("/join")
def join_form(request):
    return Screen(
        id="scr_join",
        title="Join a room",
        body=[
            Form(
                action="/join",
                method="POST",
                fields=[Input(name="code", label="Invite code", max=6)],
                submit="Join",
            ),
        ],
    )


@app.handler("/join", method="POST")
def join(request):
    payload = request.body if isinstance(request.body, dict) else {}
    code = (payload.get("code") or "").upper().strip()
    with _lock:
        state = _load()
        if code not in state["rooms"]:
            return Screen(
                id="scr_join",
                title="Join a room",
                body=[
                    Notification(s=f"No room with code {code!r}", level="error"),
                    Form(action="/join", method="POST",
                         fields=[Input(name="code", label="Invite code", max=6)], submit="Join"),
                ],
            )
        room = state["rooms"][code]
        if request.ctx.device_id not in room["members"]:
            room["members"].append(request.ctx.device_id)
            state["memberships"].setdefault(request.ctx.device_id, []).append(code)
            _save(state)
    return _room_screen(request.ctx.device_id, code)


@app.screen("/room")
def room(request):
    group_id = _first_q(request, "gid")
    if not group_id:
        return _rooms_screen(request.ctx.device_id)
    return _room_screen(request.ctx.device_id, group_id)


@app.handler("/send", method="POST")
def send(request):
    group_id = _first_q(request, "gid")
    payload = request.body if isinstance(request.body, dict) else {}
    text = (payload.get("msg") or "").strip()
    if not group_id:
        return _rooms_screen(request.ctx.device_id)
    if not text:
        return _room_screen(request.ctx.device_id, group_id)
    with _lock:
        state = _load()
        room = state["rooms"].get(group_id)
        if not room or request.ctx.device_id not in room["members"]:
            return Screen(id="scr_403", title="Chat",
                          body=[Notification(s="Not a member.", level="error")])
        msg = {"from": request.ctx.device_id[:8], "s": text, "ts": int(time.time())}
        room["history"].append(msg)
        room["history"] = room["history"][-MAX_HISTORY:]
        _save(state)
    _wake_group(group_id)
    return _room_screen(request.ctx.device_id, group_id)


@app.screen("/events")
def long_poll(request):
    """Long-poll for new group events while the user has the chat open."""
    group_id = _first_q(request, "gid")
    if not group_id:
        return {"events": []}
    ev = _wake.setdefault(group_id, threading.Event())
    ev.wait(timeout=25.0)
    state = _load()
    room = state["rooms"].get(group_id, {})
    last = room.get("history", [])[-1:] if room else []
    return {"events": [{"kind": "group_message", "group_id": group_id, "msg": m} for m in last]}


if __name__ == "__main__":
    app.run()
