"""PagerOS weather example (DOCS-007).

Renders the current temperature + condition for the device's GPS
location. Pipeline per request to ``/``:

1. Read ``request.ctx.location`` — populated from the ``PagerOS-Location``
   header that v1 firmware sends on every request once the user has
   granted ``location`` permission (SPEC §8.3, FW-011).
2. Call Open-Meteo's free public forecast endpoint.
3. Return a ``Screen`` whose body is an ``image`` widget (weather icon)
   plus ``text`` widgets for temperature, condition, and the fix that
   produced them.

The screen ``subscribe``s to the ``location`` event so the device pushes
a fresh fix whenever its position changes by more than the
firmware-defined delta (FW-022). Icons are content-addressed under
``/img/<sha256>`` per SPEC §5.6 — the bytes are generated once at import
time so the example needs no checked-in assets.
"""

from __future__ import annotations

import hashlib
import json
import logging
import struct
import urllib.parse
import urllib.request
import zlib
from typing import Mapping

from pageros import App, Image, Response, Screen, Text

log = logging.getLogger("examples.weather")


# --------------------------------------------------------------------------- #
# Public weather API
# --------------------------------------------------------------------------- #

OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"
HTTP_TIMEOUT = 5.0

# Open-Meteo WMO weather code → (short label, icon slug). Pruned to the
# codes Open-Meteo actually emits on the `current` endpoint.
_WEATHER_CODES: Mapping[int, tuple[str, str]] = {
    0:  ("Clear sky",        "sun"),
    1:  ("Mainly clear",     "sun"),
    2:  ("Partly cloudy",    "partly"),
    3:  ("Overcast",         "cloud"),
    45: ("Fog",              "cloud"),
    48: ("Rime fog",         "cloud"),
    51: ("Light drizzle",    "rain"),
    53: ("Drizzle",          "rain"),
    55: ("Dense drizzle",    "rain"),
    61: ("Light rain",       "rain"),
    63: ("Rain",             "rain"),
    65: ("Heavy rain",       "rain"),
    71: ("Light snow",       "snow"),
    73: ("Snow",             "snow"),
    75: ("Heavy snow",       "snow"),
    80: ("Rain showers",     "rain"),
    81: ("Heavy showers",    "rain"),
    82: ("Violent showers",  "rain"),
    95: ("Thunderstorm",     "storm"),
    96: ("Storm + hail",     "storm"),
    99: ("Severe storm",     "storm"),
}


# --------------------------------------------------------------------------- #
# Icons — solid-colour PNGs built at import time
# --------------------------------------------------------------------------- #
#
# Real apps would ship richer artwork; these solid 96×96 PNGs keep the
# example self-contained (no binary blobs in the repo) while still
# exercising the full Image / `/img/<sha256>` round trip from SPEC §5.6.

ICON_SIZE = 96  # px; well inside the SPEC §5.6 cap of 480×222 / 8 KB.

_ICON_PALETTE: Mapping[str, tuple[int, int, int]] = {
    "sun":    (0xF4, 0xC0, 0x30),
    "partly": (0xC8, 0xD3, 0xE1),
    "cloud":  (0x90, 0x9A, 0xA8),
    "rain":   (0x48, 0x86, 0xC8),
    "snow":   (0xE8, 0xEE, 0xF6),
    "storm":  (0x36, 0x44, 0x66),
}


def _make_solid_png(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    """Encode a solid-colour 8-bit RGB PNG using stdlib only."""

    def _chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    row = b"\x00" + (bytes(rgb) * width)  # filter byte + raw pixels
    idat = zlib.compress(row * height)
    return (
        sig
        + _chunk(b"IHDR", ihdr)
        + _chunk(b"IDAT", idat)
        + _chunk(b"IEND", b"")
    )


# slug → (full-sha256 hex, PNG bytes). Hashes feed both the Image.src
# field and the /img/<hash> route registered below.
ICONS: dict[str, tuple[str, bytes]] = {}
for _slug, _rgb in _ICON_PALETTE.items():
    _png = _make_solid_png(ICON_SIZE, ICON_SIZE, _rgb)
    ICONS[_slug] = (hashlib.sha256(_png).hexdigest(), _png)


# --------------------------------------------------------------------------- #
# App
# --------------------------------------------------------------------------- #

app = App(name="weather")


def _fetch_current(lat: float, lon: float) -> dict | None:
    qs = urllib.parse.urlencode(
        {
            "latitude": f"{lat:.4f}",
            "longitude": f"{lon:.4f}",
            "current": "temperature_2m,weather_code",
            "temperature_unit": "celsius",
        }
    )
    req = urllib.request.Request(
        f"{OPEN_METEO_URL}?{qs}",
        headers={"User-Agent": "pageros-weather-example/0.1"},
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            payload = json.loads(resp.read())
    except (OSError, ValueError) as exc:
        log.warning("open-meteo fetch failed: %s", exc)
        return None
    current = payload.get("current")
    return current if isinstance(current, dict) else None


@app.screen("/")
def home(request):
    loc = request.ctx.location
    if loc is None:
        return Screen(
            id="weather_nofix",
            title="Weather",
            body=[
                Text("Waiting for GPS fix…", style="heading"),
                Text("Grant location in Settings → Apps.", style="dim"),
            ],
            subscribe=["location"],
            ttl=0,  # never cache the no-fix screen
        )

    current = _fetch_current(loc.lat, loc.lon)
    if current is None:
        return Screen(
            id="weather_err",
            title="Weather",
            body=[
                Text("Couldn't reach Open-Meteo.", style="heading"),
                Text("Will retry on the next refresh.", style="dim"),
            ],
            subscribe=["location"],
            ttl=60,
        )

    code = int(current.get("weather_code") or 0)
    temp = float(current.get("temperature_2m") or 0.0)
    label, icon_slug = _WEATHER_CODES.get(code, ("Unknown", "cloud"))
    icon_hash, _ = ICONS[icon_slug]

    return Screen(
        id="weather_now",
        title="Weather",
        body=[
            Image(src=f"img:{icon_hash}", w=ICON_SIZE, h=ICON_SIZE, alt=label),
            Text(f"{temp:.0f}°C", style="heading"),
            Text(label, style="body"),
            Text(f"{loc.lat:.3f}, {loc.lon:.3f}", style="dim"),
        ],
        subscribe=["location"],
        ttl=300,  # 5 min between background refreshes
    )


# --------------------------------------------------------------------------- #
# Image-fetch endpoint (SPEC §5.6)
# --------------------------------------------------------------------------- #


def _register_icon_route(full_hash: str, png_bytes: bytes) -> None:
    @app.handler(f"/img/{full_hash}", method="GET")
    def _serve():
        return Response(
            status=200,
            body=png_bytes,
            headers={"Content-Type": "image/png"},
        )


for _hash, _png in ICONS.values():
    _register_icon_route(_hash, _png)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
