# `notes` — list, detail, forms, persistence

A minimal CRUD reference app: add, view, edit, and delete notes, with
per-device storage. Demonstrates the half-dozen widgets you'll reach
for in any data-entry app.

| Widget / API | Used here |
| --- | --- |
| `list` + `list_item` (SPEC §5.3.2) | The index screen at `/`. |
| `form` + `input` (SPEC §5.3.3, §5.3.4) | New / edit screens. |
| `button` with `confirm` (SPEC §5.3.5) | Delete with a confirm prompt. |
| `notification` (SPEC §5.3.8) | Inline flash messages ("Saved.", "Deleted."). |
| `ctx.device_id` (SPEC §8.3) | Per-device isolation of the note list. |

## Routes

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | List notes (newest first) + "New note" button. |
| `GET` | `/new` | Empty `Form` → `POST /notes`. |
| `POST` | `/notes` | Create a note; redirect back to the list. |
| `GET` | `/notes?id=<id>` | Detail view with Edit / Delete / Back buttons. |
| `GET` | `/edit?id=<id>` | Pre-filled `Form` → `POST /notes/update?id=<id>`. |
| `POST` | `/notes/update?id=<id>` | Save changes; return to the list. |
| `POST` | `/notes/delete?id=<id>` | Delete (confirmed in-device); return to the list. |

Note ids are short random hex strings; they travel as a query parameter
because the SDK's router is exact-match (PY-001).

## Storage

A single JSON file lives next to `app.py` (override with the
`PAGEROS_NOTES_STORE` env var). It maps **device id** → **list of
notes**:

```json
{
  "uK1...QwA": [
    {"id": "ab12cd34", "title": "Groceries", "body": "milk", "ts": 1700000000}
  ]
}
```

Anonymous dev-server requests (no `PagerOS-Device` header) land under
the empty-string key, so you can drive the app from curl without
standing up the signature middleware.

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

## Drive the API by hand

Listing is a plain GET — the device's `Accept` header isn't required for
the dev server, but include it to match the on-device shape:

```sh
curl -sS -o frame.cbor -w 'status:%{http_code} bytes:%{size_download}\n' \
  -H 'Accept: application/cbor; pagerOS=1' \
  http://127.0.0.1:8000/
```

Form submissions are CBOR maps (`{name: value}`) — devices use the SDK
codec, and so should you:

```sh
python - <<'PY' | curl -sS --data-binary @- \
    -H 'Content-Type: application/cbor' \
    -H 'Accept: application/cbor; pagerOS=1' \
    -o frame.cbor -w 'status:%{http_code}\n' \
    http://127.0.0.1:8000/notes
import sys
from pageros.codec import encode_frame
sys.stdout.buffer.write(encode_frame({"title": "Groceries", "body": "milk\neggs"}))
PY
```

Decode the returned Frame to see the updated list:

```sh
python -c "from pageros.codec import decode_frame; import pprint, sys; \
    pprint.pp(decode_frame(open('frame.cbor','rb').read()))"
```

Multi-device behaviour comes "for free" once you set `PagerOS-Device`
on the request:

```sh
curl -sS -H 'PagerOS-Device: alice-pubkey-b64' http://127.0.0.1:8000/
curl -sS -H 'PagerOS-Device: bob-pubkey-b64'   http://127.0.0.1:8000/
# Each device sees its own list.
```

## Render it in the simulator

Run the simulator (`cd simulator && cargo tauri dev`) and point its URL
bar at `http://127.0.0.1:8000/`. The list, form, detail, and confirm
prompt all render on the 480 × 222 screen.

## Run it on a real device

Sideload as usual — **Settings → Apps → Add by URL** with your dev
server's reachable URL. Each pager will see its own private list
because the SDK derives `ctx.device_id` from the verified X25519
pubkey (PY-008 / SPEC §8.3); two pagers using the same dev server
never see each other's notes.

## What this example deliberately skips

- **Real database.** JSON is fine for a few hundred notes but won't
  hold up under heavy concurrent writes. Swap `NoteStore` for sqlite
  or your storage of choice.
- **Pagination.** The list endpoint returns every note — fine for
  small counts but breaks the LoRa budget (`pageros.lora_budget`) past
  a few dozen rows. Add `?page=N` for production.
- **Search / tags.** No filtering. Add an `input` field at the top of
  the list and filter in the handler.
- **Migrations.** A schema change (new field) requires hand-editing the
  JSON file. Production apps should version the store.

## What's next

`weather` (DOCS-007) shows how to combine `image` + `text` with a
public HTTP API. `gps-tracker` (DOCS-008) introduces the `location`
event. The full walkthrough for building apps is in `docs/dev/index.md`.
