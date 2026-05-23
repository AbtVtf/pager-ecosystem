"""PagerOS Maps example.

Search places by name, view them on a map, and look up nearby points of
interest. Three external services back the screens:

* `/search` queries the OpenStreetMap Nominatim geocoder.
* `/place` and `/near` render a `Map` widget (SPEC §5.3.7) centred on a
  chosen result or the device's current GPS fix (``ctx.location``).
* `/nearby` issues an Overpass QL ``node(around:500,…)[amenity=…]`` query
  and drops the matching POIs back onto the same map.

Network calls are wrapped in try/except — any failure returns a Screen
with a `notification` widget and a Back button so the pager never sees
a 5xx Frame.
"""

from __future__ import annotations

import logging
from typing import Any

import requests

from pageros import (
    App,
    Button,
    Form,
    Input,
    List,
    ListItem,
    Map,
    MapMarker,
    Notification,
    Screen,
    Text,
)

log = logging.getLogger("examples.maps")

NOMINATIM_URL = "https://nominatim.openstreetmap.org/search"
OVERPASS_URL = "https://overpass-api.de/api/interpreter"
USER_AGENT = "pageros-maps/1.0"
HTTP_TIMEOUT = 10.0

MAX_RESULTS = 8
MAX_NEARBY = 8
LABEL_MAX = 60

app = App(name="maps")


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _coerce_latlon(lat_s: str, lon_s: str) -> tuple[float, float] | None:
    try:
        lat = float(lat_s)
        lon = float(lon_s)
    except (TypeError, ValueError):
        return None
    if not (-90.0 <= lat <= 90.0) or not (-180.0 <= lon <= 180.0):
        return None
    return lat, lon


def _trim(text: str, limit: int = LABEL_MAX) -> str:
    text = (text or "").strip()
    if len(text) <= limit:
        return text
    return text[: max(1, limit - 1)].rstrip() + "…"


def _error_screen(message: str, back: str = "/") -> Screen:
    return Screen(
        id="maps_err",
        title="Maps",
        body=[
            Notification(s=message, level="error"),
            Button(label="Back", href=back),
        ],
    )


# --------------------------------------------------------------------------- #
# Screens
# --------------------------------------------------------------------------- #


@app.screen("/")
def home(request):
    body: list[Any] = [
        Text("Maps", style="heading"),
        Form(
            action="/search",
            method="POST",
            fields=[Input(name="q", label="Search place")],
            submit="Search",
        ),
    ]
    if request.ctx.location is not None:
        body.append(Button(label="Show near me", href="/near"))
    else:
        body.append(Text("Grant location to use 'near me'", style="dim"))
    return Screen(id="maps_home", title="Maps", body=body)


@app.handler("/search", method="POST")
def search(request):
    payload = request.body if isinstance(request.body, dict) else {}
    query = (payload.get("q") or "").strip()
    if not query:
        return _error_screen("Enter a place name.")

    try:
        resp = requests.get(
            NOMINATIM_URL,
            params={"q": query, "format": "json", "limit": MAX_RESULTS},
            headers={"User-Agent": USER_AGENT},
            timeout=HTTP_TIMEOUT,
        )
        resp.raise_for_status()
        results = resp.json()
    except (requests.RequestException, ValueError) as exc:
        log.warning("nominatim failed: %s", exc)
        return _error_screen("Search failed. Try again.")

    if not isinstance(results, list) or not results:
        return Screen(
            id="maps_search_empty",
            title="Maps",
            body=[
                Text("No results", style="heading"),
                Text(_trim(query, 40), style="dim"),
                Button(label="Back", href="/"),
            ],
        )

    items: list[ListItem] = []
    for r in results:
        try:
            lat = float(r.get("lat"))
            lon = float(r.get("lon"))
        except (TypeError, ValueError):
            continue
        name = r.get("display_name") or "(unnamed)"
        href = f"/place?lat={lat:.6f}&lon={lon:.6f}&name={requests.utils.quote(str(name))}"
        items.append(ListItem(label=_trim(str(name)), href=href))

    return Screen(
        id="maps_search",
        title="Results",
        body=[
            Text(_trim(f"Results for {query}", 40), style="heading"),
            List(items=items),
            Button(label="Back", href="/"),
        ],
    )


def _place_screen(lat: float, lon: float, name: str) -> Screen:
    safe_name = name or f"{lat:.4f}, {lon:.4f}"
    nearby_href = f"/nearby?lat={lat:.6f}&lon={lon:.6f}&type=cafe"
    return Screen(
        id="maps_place",
        title="Place",
        body=[
            Text(_trim(safe_name, 40), style="heading"),
            Text(f"{lat:.4f}, {lon:.4f}", style="dim"),
            Map(
                lat=lat,
                lon=lon,
                zoom=15,
                markers=[MapMarker(lat=lat, lon=lon, label=_trim(safe_name, 30))],
            ),
            Button(label="Find nearby cafes", href=nearby_href),
            Button(label="Back", href="/"),
        ],
    )


@app.screen("/place")
def place(request):
    coords = _coerce_latlon(_first_q(request, "lat"), _first_q(request, "lon"))
    if coords is None:
        return _error_screen("Bad coordinates.")
    name = _first_q(request, "name") or f"{coords[0]:.4f}, {coords[1]:.4f}"
    return _place_screen(coords[0], coords[1], name)


@app.screen("/near")
def near(request):
    loc = request.ctx.location
    if loc is None:
        return _error_screen("No GPS fix. Grant location permission.")
    return _place_screen(loc.lat, loc.lon, "Current location")


@app.screen("/nearby")
def nearby(request):
    coords = _coerce_latlon(_first_q(request, "lat"), _first_q(request, "lon"))
    if coords is None:
        return _error_screen("Bad coordinates.")
    lat, lon = coords
    amenity = _first_q(request, "type") or "cafe"
    # Keep amenity input tame — Overpass treats arbitrary text as a regex.
    amenity = "".join(c for c in amenity if c.isalnum() or c in ("_", "-"))[:32]
    if not amenity:
        amenity = "cafe"

    query = (
        f"[out:json][timeout:10];"
        f"node(around:500,{lat},{lon})[amenity={amenity}];"
        f"out 10;"
    )
    try:
        resp = requests.post(
            OVERPASS_URL,
            data=query,
            headers={"User-Agent": USER_AGENT},
            timeout=HTTP_TIMEOUT,
        )
        resp.raise_for_status()
        payload = resp.json()
    except (requests.RequestException, ValueError) as exc:
        log.warning("overpass failed: %s", exc)
        return _error_screen("Nearby lookup failed.")

    elements = payload.get("elements") if isinstance(payload, dict) else None
    if not isinstance(elements, list) or not elements:
        return Screen(
            id="maps_nearby_empty",
            title="Nearby",
            body=[
                Text(f"No {amenity} within 500m", style="heading"),
                Map(
                    lat=lat,
                    lon=lon,
                    zoom=16,
                    markers=[MapMarker(lat=lat, lon=lon, label="You")],
                ),
                Button(label="Back", href="/"),
            ],
        )

    markers: list[MapMarker] = [MapMarker(lat=lat, lon=lon, label="You")]
    items: list[ListItem] = []
    for el in elements[:MAX_NEARBY]:
        try:
            plat = float(el.get("lat"))
            plon = float(el.get("lon"))
        except (TypeError, ValueError):
            continue
        tags = el.get("tags") if isinstance(el.get("tags"), dict) else {}
        name = tags.get("name") or f"({amenity})"
        short = _trim(str(name), 30)
        markers.append(MapMarker(lat=plat, lon=plon, label=short))
        items.append(ListItem(label=_trim(str(name), 40)))

    return Screen(
        id="maps_nearby",
        title=f"Nearby {amenity}",
        body=[
            Text(f"{len(items)} {amenity} within 500m", style="dim"),
            Map(lat=lat, lon=lon, zoom=16, markers=markers),
            List(items=items),
            Button(label="Back", href="/"),
        ],
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
