"""PagerOS gps-tracker example (DOCS-008).

Live position renderer + breadcrumb tracker. Demonstrates:

- the ``map`` widget (SPEC §5.3.7) centred on the device's GPS fix,
- the ``location`` event subscription so the device pushes fresh fixes
  whenever it moves (FW-022) — no polling, no battery burn,
- per-device track storage isolated by ``ctx.device_id`` (SPEC §8.3),
- a tiny ``button`` to clear the trail.

Tracks are persisted to a JSON file next to ``app.py`` (override with
``PAGEROS_TRACKS_STORE``). Each fix is appended only when it has moved
more than ``MIN_MOVE_METERS`` from the previous point to keep the file
small.
"""

from __future__ import annotations

import json
import math
import os
import threading
import time
from pathlib import Path

from pageros import (
    App,
    Button,
    Map,
    MapMarker,
    Screen,
    Text,
)

MIN_MOVE_METERS = 25.0


def _store_path() -> Path:
    return Path(os.environ.get("PAGEROS_TRACKS_STORE", Path(__file__).with_name("tracks.json")))


_lock = threading.Lock()


def _load() -> dict[str, list[dict[str, float]]]:
    p = _store_path()
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text())
    except json.JSONDecodeError:
        return {}


def _save(state: dict[str, list[dict[str, float]]]) -> None:
    _store_path().write_text(json.dumps(state, indent=2, sort_keys=True))


def _haversine_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    """Great-circle distance between two (lat, lon) points, in metres."""
    r = 6_371_000.0
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dp = math.radians(b[0] - a[0])
    dl = math.radians(b[1] - a[1])
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def _append_if_moved(device_id: str, lat: float, lon: float) -> list[dict[str, float]]:
    with _lock:
        state = _load()
        trail = state.get(device_id, [])
        last = (trail[-1]["lat"], trail[-1]["lon"]) if trail else None
        if last is None or _haversine_m(last, (lat, lon)) >= MIN_MOVE_METERS:
            trail.append({"lat": lat, "lon": lon, "ts": time.time()})
            state[device_id] = trail[-200:]  # cap per-device trail
            _save(state)
        return state.get(device_id, [])


app = App(name="gps-tracker")


def _home_screen(device_id: str, loc) -> Screen:
    if loc is None:
        return Screen(
            id="scr_nofix",
            title="GPS tracker",
            body=[Text(s="Waiting for GPS fix…", style="dim")],
            subscribe=["location"],
        )
    trail = _append_if_moved(device_id, loc.lat, loc.lon)
    markers = [MapMarker(lat=p["lat"], lon=p["lon"]) for p in trail]
    return Screen(
        id="scr_track",
        title="GPS tracker",
        body=[
            Map(lat=loc.lat, lon=loc.lon, zoom=15, markers=markers),
            Text(s=f"{len(trail)} points · ±{int(loc.accuracy or 0)} m", style="dim"),
            Button(label="Clear trail", href="/clear", method="POST", confirm="Erase trail?"),
        ],
        subscribe=["location"],
    )


@app.screen("/")
def home(request):
    return _home_screen(request.ctx.device_id, request.ctx.location)


@app.handler("/clear", method="POST")
def clear(request):
    with _lock:
        state = _load()
        state.pop(request.ctx.device_id, None)
        _save(state)
    return _home_screen(request.ctx.device_id, request.ctx.location)


if __name__ == "__main__":
    app.run()
