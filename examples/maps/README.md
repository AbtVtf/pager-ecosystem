# Maps — places, pins, and nearby POIs

A small PagerOS app that searches places, drops them on a map, and
looks up nearby points of interest.

- `/` — heading "Maps" plus a search form. Shows a "Show near me"
  button when the device has granted location.
- `/search` (POST) — geocodes via the public OpenStreetMap Nominatim
  API and renders the top 8 hits as a list.
- `/place?lat=&lon=&name=` — heading, coordinates, a `Map` centred on
  the place, and buttons to look up nearby cafes or go back.
- `/nearby?lat=&lon=&type=` — Overpass QL
  `node(around:500,…)[amenity=…]` query; renders up to 8 pins plus a
  list of names.
- `/near` — uses `request.ctx.location` and behaves like `/place` for
  the device's current GPS fix.

Network calls hit `nominatim.openstreetmap.org` and
`overpass-api.de` with a custom `User-Agent`. Any failure renders a
`notification` widget with a Back button — the pager never sees a 5xx.

## Run it

```bash
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8013
```

Then point the pager (or `pageros-cli`) at `http://<host>:8013/`.
Grant the `location` permission for the "Show near me" button to
appear.
