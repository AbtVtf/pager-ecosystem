# gps-tracker

Live position renderer + breadcrumb tracker for PagerOS (DOCS-008).

Demonstrates the `map` widget (SPEC §5.3.7) centred on the device's GPS fix, the `location` event subscription so the device pushes fresh fixes whenever it moves (FW-022 — no polling), and per-device track storage isolated by `ctx.device_id`.

## Run (Python)

```bash
pip install -r requirements.txt
python app.py
```

The dev server listens on `http://localhost:8080`. Open the simulator (`pagerctl simulate http://localhost:8080`) and grant the `location` permission on first prompt. As the simulated location changes, the screen updates in place; the trail persists to `tracks.json`.

## Run (TypeScript)

```bash
npm install
npm start
```

## Storage

Per-device trails live in `tracks.json` next to `app.py` (override with `PAGEROS_TRACKS_STORE`). New points are only appended when the device has moved at least 25 m from the previous point to keep the file small. Each device's trail is capped at 200 points.
