"""PagerOS nfc-counter example (DOCS-009).

Counts NFC tag scans per UID. Demonstrates:

- the ``nfc_scan`` event subscription (SPEC §5.4.1, FW-010),
- a ``list`` view of distinct tags sorted by scan count,
- the ``notification`` widget for inline flash messages after a scan,
- per-device storage isolated by ``ctx.device_id``.

Each scan is dispatched by firmware to the foregrounded app's
``nfc_scan`` event handler with the tag UID and any NDEF records. The
handler bumps the per-(device, UID) counter and re-renders.
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path

from pageros import (
    App,
    Button,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)


def _store_path() -> Path:
    return Path(os.environ.get("PAGEROS_NFC_STORE", Path(__file__).with_name("scans.json")))


_lock = threading.Lock()


def _load() -> dict[str, dict[str, int]]:
    p = _store_path()
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text())
    except json.JSONDecodeError:
        return {}


def _save(state: dict[str, dict[str, int]]) -> None:
    _store_path().write_text(json.dumps(state, indent=2, sort_keys=True))


def _bump(device_id: str, uid: str) -> int:
    with _lock:
        state = _load()
        counts = state.setdefault(device_id, {})
        counts[uid] = counts.get(uid, 0) + 1
        _save(state)
        return counts[uid]


def _for_device(device_id: str) -> dict[str, int]:
    with _lock:
        return dict(_load().get(device_id, {}))


app = App(name="nfc-counter")


def _home_screen(device_id: str, flash: str | None = None) -> Screen:
    counts = _for_device(device_id)
    ranked = sorted(counts.items(), key=lambda kv: -kv[1])
    body = []
    if flash:
        body.append(Notification(s=flash, level="info"))
    if ranked:
        body.append(List(items=[
            ListItem(label=uid, sub=f"{n} scan{'s' if n != 1 else ''}")
            for uid, n in ranked
        ]))
    else:
        body.append(Text(s="Tap any NFC tag to count it.", style="dim"))
    body.append(Button(label="Reset all", href="/reset", method="POST", confirm="Reset all counters?"))
    return Screen(
        id="scr_home",
        title="NFC counter",
        body=body,
        subscribe=["nfc_scan"],
    )


@app.screen("/")
def home(request):
    return _home_screen(request.ctx.device_id)


@app.handler("/event/nfc_scan", method="POST")
def on_nfc_scan(request):
    """Triggered when the device's NFC chip detects a tag (FW-010).

    The payload shape mirrors SPEC §5.4.1: ``{"uid": "<hex>", "records":
    [...]}``. We ignore the NDEF records for the v1 counter.
    """
    payload = request.body if isinstance(request.body, dict) else {}
    uid = (payload.get("uid") or "").upper()
    if not uid:
        return _home_screen(request.ctx.device_id, flash="Empty UID")
    n = _bump(request.ctx.device_id, uid)
    return _home_screen(request.ctx.device_id, flash=f"Scanned {uid} (x{n})")


@app.handler("/reset", method="POST")
def reset(request):
    with _lock:
        state = _load()
        state.pop(request.ctx.device_id, None)
        _save(state)
    return _home_screen(request.ctx.device_id)


if __name__ == "__main__":
    app.run()
