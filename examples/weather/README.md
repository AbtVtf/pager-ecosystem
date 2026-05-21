# `weather` — image + text from a public API

A reference app showing how to combine three pieces of the PagerOS v1
stack in one Frame:

| Piece | Used here |
| --- | --- |
| `ctx.location` (SPEC §8.3, FW-011 GPS driver) | Read the device's current lat/lon. |
| Public HTTP API | Fetch live conditions from [Open-Meteo](https://open-meteo.com/) (no key required). |
| `image` widget + `/img/<sha256>` (SPEC §5.3.6, §5.6) | Render a content-addressed weather icon. |

The app is intentionally one file (`app.py`, ≈150 LOC including the
inline PNG encoder) so it can be read top-to-bottom.

## Run it

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt           # installs `pageros`
python app.py --host 127.0.0.1 --port 8000
```

While iterating, use the auto-reloading dev server instead:

```sh
python -m pageros.devserver --app app.py --host 127.0.0.1 --port 8000
```

## Test it without a real pager

The Frame depends on `ctx.location`, which comes from the
`PagerOS-Location` header the firmware adds to every device request
(format `"lat,lon,ts"`, see `sdk/python/pageros/ctx.py`). Curl can spoof
it directly:

```sh
# Helsinki, just for example. ts is seconds since epoch.
curl -sS -o frame.cbor -w 'status:%{http_code} bytes:%{size_download}\n' \
  -H 'Accept: application/cbor; pagerOS=1' \
  -H "PagerOS-Location: 60.170,24.940,$(date +%s)" \
  http://127.0.0.1:8000/
```

The response is a canonical CBOR Frame containing an `image` widget
followed by three `text` widgets. The Frame's `subscribe: ["location"]`
field tells a real device to push a refreshed Frame whenever its GPS
fix changes.

Without the header you get the "Waiting for GPS fix" Frame — useful for
checking the no-permission path.

### Fetch the icon

The `image` widget's `src` is `img:<full-sha256>`. The device fetches it
once at `GET /img/<full-sha256>` (SPEC §5.6) and caches it forever.
Confirm the route is wired up:

```sh
# Pull the hash out of the most recent Frame and request the matching icon.
python - <<'PY'
import cbor2, sys
with open("frame.cbor", "rb") as f:
    frame = cbor2.loads(f.read())
img = next(w for w in frame["body"] if w["t"] == "image")
print(img["src"].removeprefix("img:"))
PY
# → 7f3a...   (full sha256)

curl -sS -o icon.png -w 'status:%{http_code} bytes:%{size_download}\n' \
  http://127.0.0.1:8000/img/<paste-the-hash>
file icon.png   # → PNG image data, 96 x 96
```

## Render it in the simulator

Run the simulator (`cd simulator && cargo tauri dev`) and point its URL
bar at `http://127.0.0.1:8000/`. Use the simulator's location-spoof
controls (see `simulator/KEYMAP.md`) to feed a fix; the screen will
re-render with the live conditions for that lat/lon.

## Run it on a real device

A flashed PagerOS pager renders this app the same way it renders any
other — install it from the Marketplace once the manifest is published,
or sideload by URL from **Settings → Apps → Add by URL** with your dev
server's reachable URL (see `docs/user/apps.md`). The first time the
user opens the app they'll be prompted to grant the `location`
permission — without it, `ctx.location` stays `None` and the app shows
the "Waiting for GPS fix" Frame.

For local-network testing without publishing, expose the dev server
(Tailscale, a private LAN bind, an `ngrok`-style tunnel) and sideload
that URL.

## What this example deliberately skips

- **Real artwork.** Icons are solid colours generated at import time so
  the repo has no binary blobs. Replace `_ICON_PALETTE` with PNG files
  on disk for production.
- **Forecast / hourly.** Only the current observation is rendered, to
  keep the Frame under the LoRa budget (`pageros.lora_budget`) and the
  screen readable.
- **Caching.** Every refresh hits Open-Meteo. A production app would
  memoise per `(lat, lon)` for a few minutes and respect the device's
  `If-None-Match` header (SPEC §5.5).
- **Units / locale.** Hard-coded to Celsius. Add a `--units` flag or a
  per-device session preference for production.

## What's next

For a richer event-driven example, see `examples/gps-tracker` (DOCS-008,
map widget + live `location` event). For push notifications fired from
external triggers, see `examples/push-reminder` (DOCS-011). The full
walkthrough for building apps is in `docs/dev/index.md`.
