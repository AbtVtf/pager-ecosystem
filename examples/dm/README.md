# dm

`dm.pageros.app` — 1:1 direct messages between two pagers. No group invite codes; each device gets a short 6-char DM handle derived from its `device_id`, and you send by typing the peer's handle.

## Run

```bash
pip install -r requirements.txt
python app.py --port 8014
```

Then point a simulator or pager at `http://localhost:8014/`.

## Flow

- Home (`/`) shows your handle and a list of recent conversations. Tap "Start new DM" to begin one.
- `/new` takes a peer handle (6 chars) and a first message, then drops you into the thread.
- `/thread?with=HANDLE` is a [Chat] widget over the canonical thread key (sorted `A|B`). The screen subscribes to that key, and `/events` long-polls for new messages.

Unknown peer handles are auto-registered so the demo works on a single device — DM your own handle to see both sides.

## Storage

State persists to `dms.json` next to `app.py`. Override with `PAGEROS_DM_STORE`.
