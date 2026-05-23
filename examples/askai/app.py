"""Ask AI — chat with Claude on your pager.

A pocket assistant for the LILYGO T-LoRa pager (480x222). Each device gets
its own list of conversations persisted on the server. Messages are routed
through the Anthropic Messages API; if the API key is missing or the call
fails for any reason, the app falls back to an echo stub so the demo still
runs end-to-end.

Conversations are persisted to ``askai_convos.json`` next to this module
(override with ``ASKAI_STORE`` if you want to keep state elsewhere).
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
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

MODEL = "claude-sonnet-4-6"
MAX_TOKENS = 512
SYSTEM_PROMPT = (
    "You are an assistant on a tiny pager device with a 480x222 screen. "
    "Keep responses under 80 words. Be friendly and direct."
)
TITLE_MAX = 40
MAX_HISTORY = 40  # cap context fed back to Claude per turn


# --------------------------------------------------------------------------- #
# Anthropic client (lazy + optional)
# --------------------------------------------------------------------------- #


_client_lock = threading.Lock()
_client: Any = None
_client_tried = False


def _get_client() -> Any | None:
    """Return a cached Anthropic client, or None if unavailable.

    Returning ``None`` triggers the offline echo fallback so the demo
    keeps working without an API key or the SDK installed.
    """
    global _client, _client_tried
    with _client_lock:
        if _client_tried:
            return _client
        _client_tried = True
        if not os.environ.get("ANTHROPIC_API_KEY"):
            return None
        try:
            from anthropic import Anthropic  # type: ignore
        except Exception:
            return None
        try:
            _client = Anthropic()
        except Exception:
            _client = None
        return _client


def _ask_claude(history: list[dict[str, str]]) -> str:
    """Send ``history`` to Claude and return the assistant text.

    ``history`` is the full ordered list of {role, content} turns the
    model should consider, ending in the new user message. Falls back
    to an echo stub on any error so the screen never deadlocks.
    """
    last_user = next(
        (m["content"] for m in reversed(history) if m["role"] == "user"),
        "",
    )
    client = _get_client()
    if client is None:
        return f"[AI offline - set ANTHROPIC_API_KEY] Echo: {last_user}"
    try:
        resp = client.messages.create(
            model=MODEL,
            max_tokens=MAX_TOKENS,
            system=SYSTEM_PROMPT,
            messages=[
                {"role": m["role"], "content": m["content"]}
                for m in history[-MAX_HISTORY:]
            ],
        )
        # Concatenate any text blocks in the response (defensive: the
        # SDK usually returns a single text block but tool-use or
        # multi-block replies would otherwise drop content).
        parts: list[str] = []
        for block in getattr(resp, "content", []) or []:
            text = getattr(block, "text", None)
            if text:
                parts.append(text)
        reply = "".join(parts).strip()
        return reply or f"[AI returned empty] Echo: {last_user}"
    except Exception as exc:  # network, auth, rate-limit, decode, ...
        return f"[AI error: {type(exc).__name__}] Echo: {last_user}"


# --------------------------------------------------------------------------- #
# Persistence
# --------------------------------------------------------------------------- #


_store_lock = threading.Lock()


def _store_path() -> Path:
    return Path(
        os.environ.get(
            "ASKAI_STORE", Path(__file__).with_name("askai_convos.json")
        )
    )


def _empty() -> dict[str, Any]:
    # Top-level shape: {"devices": {device_id: [convo, ...]}}
    return {"devices": {}}


def _load() -> dict[str, Any]:
    p = _store_path()
    if not p.exists():
        return _empty()
    try:
        data = json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        return _empty()
    if not isinstance(data, dict) or "devices" not in data:
        return _empty()
    return data


def _save(state: dict[str, Any]) -> None:
    _store_path().write_text(json.dumps(state, indent=2, sort_keys=True))


def _convos_for(state: dict[str, Any], device_id: str) -> list[dict[str, Any]]:
    return state["devices"].setdefault(device_id, [])


def _find_convo(
    convos: list[dict[str, Any]], convo_id: str
) -> dict[str, Any] | None:
    for c in convos:
        if c.get("id") == convo_id:
            return c
    return None


def _title_from(text: str) -> str:
    """Trim a user message into a short conversation title."""
    cleaned = " ".join(text.split())
    if len(cleaned) <= TITLE_MAX:
        return cleaned or "(empty)"
    return cleaned[: TITLE_MAX - 1].rstrip() + "..."


def _age(ts: int) -> str:
    """Human-friendly relative age string for the home list."""
    if not ts:
        return "just now"
    delta = max(0, int(time.time()) - int(ts))
    if delta < 60:
        return f"{delta}s"
    if delta < 3600:
        return f"{delta // 60}m"
    if delta < 86400:
        return f"{delta // 3600}h"
    return f"{delta // 86400}d"


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


# --------------------------------------------------------------------------- #
# App + screens
# --------------------------------------------------------------------------- #


app = App(name="askai")


def _home_screen(device_id: str) -> Screen:
    state = _load()
    convos = _convos_for(state, device_id)
    # Newest first by the timestamp of the latest message (fall back to 0).
    ordered = sorted(
        convos,
        key=lambda c: (c.get("messages") or [{}])[-1].get("ts", 0),
        reverse=True,
    )
    body: list[Any] = [
        Text(s="Ask AI", style="heading"),
        Button(label="New chat", href="/new", method="GET"),
    ]
    if ordered:
        items = []
        for c in ordered:
            msgs = c.get("messages") or []
            last_ts = msgs[-1].get("ts", 0) if msgs else 0
            items.append(
                ListItem(
                    label=c.get("title") or "(untitled)",
                    sub=f"{len(msgs)} msgs - {_age(last_ts)}",
                    href=f"/convo?id={c['id']}",
                )
            )
        body.append(List(items=items))
    else:
        body.append(Text(s="No chats yet. Tap New chat.", style="dim"))
    return Screen(id="scr_home", title="Ask AI", body=body)


def _convo_screen(
    device_id: str, convo_id: str, *, notice: str | None = None
) -> Screen:
    state = _load()
    convo = _find_convo(_convos_for(state, device_id), convo_id)
    if convo is None:
        return Screen(
            id="scr_404",
            title="Ask AI",
            body=[
                Notification(s="Chat not found.", level="error"),
                Button(label="Back", href="/", method="GET"),
            ],
        )
    body: list[Any] = []
    if notice:
        body.append(Notification(s=notice, level="warn"))
    for msg in convo.get("messages", []):
        role = msg.get("role")
        content = msg.get("content", "")
        if role == "assistant":
            body.append(Text(s=f"AI: {content}", style="body"))
        else:
            body.append(Text(s=content or "(empty)", style="body"))
    body.append(
        Form(
            action=f"/ask?id={convo_id}",
            method="POST",
            fields=[Input(name="ask", label="Reply", max=400)],
            submit="Send",
        )
    )
    body.append(Button(label="Back", href="/", method="GET"))
    body.append(
        Button(
            label="Delete",
            href=f"/delete?id={convo_id}",
            method="POST",
            confirm="Delete this chat?",
        )
    )
    return Screen(
        id=f"scr_convo_{convo_id}",
        title=convo.get("title") or "Chat",
        body=body,
    )


@app.screen("/")
def home(request):
    return _home_screen(request.ctx.device_id)


@app.screen("/new")
def new_form(request):
    return Screen(
        id="scr_new",
        title="New chat",
        body=[
            Text(s="Ask AI", style="heading"),
            Form(
                action="/new",
                method="POST",
                fields=[
                    Input(name="ask", label="What's on your mind?", max=400)
                ],
                submit="Ask",
            ),
            Button(label="Back", href="/", method="GET"),
        ],
    )


@app.handler("/new", method="POST")
def new_submit(request):
    device_id = request.ctx.device_id
    payload = request.body if isinstance(request.body, dict) else {}
    text = (payload.get("ask") or "").strip()
    if not text:
        return Screen(
            id="scr_new",
            title="New chat",
            body=[
                Notification(s="Type something first.", level="warn"),
                Form(
                    action="/new",
                    method="POST",
                    fields=[
                        Input(name="ask", label="What's on your mind?", max=400)
                    ],
                    submit="Ask",
                ),
                Button(label="Back", href="/", method="GET"),
            ],
        )

    now = int(time.time())
    user_msg = {"role": "user", "content": text, "ts": now}
    reply = _ask_claude([{"role": "user", "content": text}])
    assistant_msg = {
        "role": "assistant",
        "content": reply,
        "ts": int(time.time()),
    }
    convo_id = secrets.token_hex(4)
    convo = {
        "id": convo_id,
        "title": _title_from(text),
        "messages": [user_msg, assistant_msg],
    }
    with _store_lock:
        state = _load()
        _convos_for(state, device_id).append(convo)
        _save(state)
    return _convo_screen(device_id, convo_id)


@app.screen("/convo")
def convo(request):
    convo_id = _first_q(request, "id")
    if not convo_id:
        return _home_screen(request.ctx.device_id)
    return _convo_screen(request.ctx.device_id, convo_id)


@app.handler("/ask", method="POST")
def ask(request):
    device_id = request.ctx.device_id
    convo_id = _first_q(request, "id")
    if not convo_id:
        return _home_screen(device_id)
    payload = request.body if isinstance(request.body, dict) else {}
    text = (payload.get("ask") or "").strip()
    if not text:
        return _convo_screen(
            device_id, convo_id, notice="Type something first."
        )

    with _store_lock:
        state = _load()
        convo = _find_convo(_convos_for(state, device_id), convo_id)
        if convo is None:
            return Screen(
                id="scr_404",
                title="Ask AI",
                body=[
                    Notification(s="Chat not found.", level="error"),
                    Button(label="Back", href="/", method="GET"),
                ],
            )
        convo["messages"].append(
            {"role": "user", "content": text, "ts": int(time.time())}
        )
        # Snapshot the history we want the model to see before releasing
        # the lock so the (potentially slow) API call doesn't block writers.
        history = [
            {"role": m["role"], "content": m["content"]}
            for m in convo["messages"]
        ]
        _save(state)

    reply = _ask_claude(history)

    with _store_lock:
        state = _load()
        convo = _find_convo(_convos_for(state, device_id), convo_id)
        if convo is None:
            # Got deleted while we were waiting on the API.
            return _home_screen(device_id)
        convo["messages"].append(
            {"role": "assistant", "content": reply, "ts": int(time.time())}
        )
        _save(state)

    return _convo_screen(device_id, convo_id)


@app.handler("/delete", method="POST")
def delete(request):
    device_id = request.ctx.device_id
    convo_id = _first_q(request, "id")
    if convo_id:
        with _store_lock:
            state = _load()
            convos = _convos_for(state, device_id)
            state["devices"][device_id] = [
                c for c in convos if c.get("id") != convo_id
            ]
            _save(state)
    return _home_screen(device_id)


if __name__ == "__main__":
    app.run()
