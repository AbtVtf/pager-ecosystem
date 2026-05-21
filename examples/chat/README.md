# chat

Multi-device group chat — the reference for SPEC §5.4.2 (group events) and §5.3.10 (the `chat` widget). DOCS-010.

Demonstrates:

- the `chat` widget rendered with scrollback + inline composer
- the `presence_list` widget for "who's here"
- the server-side group membership model (SPEC §9.6: app owns membership, devices prove identity per-group)
- server-pushed `group_message` events to online subscribers via a long-poll endpoint
- (acknowledged but not exercised here for keying reasons) offline delivery via the Push Relay through `send_group_push`

## Run

```bash
pip install -r requirements.txt
python app.py
```

To test with two devices: open the simulator (`pagerctl simulate http://localhost:8080`), tap **Create room**, copy the 6-char code, then open a second simulator instance (SIM-007) and **Join by code** with that code. Messages typed in either window appear in both.

## Storage

Rooms + memberships persist to `rooms.json` next to `app.py` (override with `PAGEROS_CHAT_STORE`). Per-group history is capped at 200 messages.

## Limits in this v1 example

- Membership is invite-code based (no per-device join signatures, which SPEC §9.6 calls for in production).
- Offline delivery is acknowledged but skipped — `PushConfig` needs real Ed25519/X25519 keys; the SDK tests cover the wire format.
