# nfc-counter

Counts NFC tag scans per UID (DOCS-009).

Demonstrates the `nfc_scan` event subscription (SPEC §5.4.1, FW-010), the `notification` widget for inline flash messages, and per-device storage isolated by `ctx.device_id`.

## Run

```bash
pip install -r requirements.txt
python app.py
```

In the simulator, the **N** key triggers a fake NFC scan with a configurable UID. Each scan bumps the per-(device, UID) counter; the home screen re-renders with the new total. The list shows distinct tags sorted by scan count.

## Storage

`scans.json` next to `app.py` (override with `PAGEROS_NFC_STORE`). Schema: `{device_id: {uid: count}}`.
