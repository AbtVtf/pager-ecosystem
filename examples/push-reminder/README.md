# push-reminder

User sets a delay + body; the app schedules a push notification that fires at the requested time and plays a tone on the device. DOCS-011.

Demonstrates:

- the form → schedule → push flow end to end
- the Push Relay client (`send_push`) wired with an `AppKeypair` and Ed25519 signing key (SPEC §6.6, PY-006)
- the `tone` payload field that the firmware audio driver maps to a bundled PCM clip (SPEC §7.8, FW-032)

## Run

```bash
pip install -r requirements.txt
python app.py
```

For a **dev-only** local run the app generates ephemeral signing + X25519 keys on startup. The Push Relay will reject pushes unless these keys match the app's marketplace manifest, so for real device delivery set:

```bash
export PAGEROS_REMINDER_APP_ID=your.app.id
export PAGEROS_REMINDER_SIGNING_KEY=<32-byte ed25519 hex>
export PAGEROS_REMINDER_X25519_PRIVATE=<32-byte x25519 hex>
export PAGEROS_RELAY_URL=https://push.pageros.org   # or your relay
```

## Limits in this v1 example

- Reminders live in memory — restarting the app loses pending ones. A production app would persist + re-arm on boot.
- The scheduler is a `threading.Timer` per reminder. Fine up to thousands of reminders; switch to a real scheduler for production.
