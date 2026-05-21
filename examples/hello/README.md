# `hello` — Single-screen text

The minimal PagerOS app: one `GET /` handler that returns a single `text` widget.

Implemented in **both** SDKs (DOCS-005). The source for each is intentionally ≤ 20 LOC:

| Language | File     | LOC |
| -------- | -------- | --- |
| Python   | `app.py` | 11  |
| TS / JS  | `app.ts` | 10  |

Both produce the **same 48-byte canonical CBOR Frame** when curled — they're literally interchangeable from the device's point of view.

## Run it (Python)

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt           # installs `pageros`
python app.py --host 127.0.0.1 --port 8000
```

While iterating, use the auto-reloading dev server instead:

```sh
python -m pageros.devserver --app app.py --host 127.0.0.1 --port 8000
```

## Run it (TypeScript)

The example pulls `@pageros/sdk` from the in-repo `sdk/js/` via a `file:` dependency, so `npm install` works without a published npm registry entry.

```sh
npm install                               # installs @pageros/sdk + tsc
npm run build                             # tsc → dist/app.js
npm start -- --host 127.0.0.1 --port 8000
```

`npm start` forwards CLI args after `--` to the app, which uses the same `--host` / `--port` flags as the Python entry point.

## Confirm a Frame comes back

Either server returns the same response. Hit it with the device's `Accept` header:

```sh
curl -sS -o frame.cbor -w 'status:%{http_code} bytes:%{size_download}\n' \
  -H 'Accept: application/cbor; pagerOS=1' \
  http://127.0.0.1:8000/
xxd frame.cbor | head
```

Expected: `status:200 bytes:48`, with the dump showing `id=scr_home`, `body=[{s: "Hello, PagerOS!", t: "text"}]` in canonical CBOR map order.

## Render it in the simulator

Run the simulator (`cd simulator && cargo tauri dev`) and point its URL bar at `http://127.0.0.1:8000/`. The text widget renders on the 480×222 pager screen. Keyboard mapping is in `simulator/KEYMAP.md`.

## Run it on a real device

A flashed PagerOS pager renders this app the same way it renders any other — install it from the Marketplace once the manifest is published, or sideload by URL from **Settings → Apps → Add by URL** with your dev server's reachable URL (see `docs/user/apps.md`).

For local-network testing without publishing, expose the dev server (Tailscale, a private LAN bind, an `ngrok`-style tunnel) and sideload that URL.

## What's next

`hello` deliberately stops at "a screen renders." For the next features — forms, persistence, multiple screens, push notifications — see the other reference apps under `examples/` (`notes`, `weather`, `chat`, `push-reminder`, …) and the full walkthrough in `docs/dev/index.md`.
