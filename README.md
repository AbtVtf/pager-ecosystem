# PagerOS

An OS + ecosystem for the **LILYGO T-LoRa Pager** (ESP32-S3). Apps live on the internet; the device renders them through a tiny declarative UI protocol over Wi-Fi or LoRa mesh.

- **Spec:** [SPEC.md](./SPEC.md) (v0.2)
- **Tasks:** [TASKS.md](./TASKS.md)
- **Hardware:** LILYGO T-LoRa Pager (480×222 IPS, QWERTY, SX1262 LoRa, GPS, NFC)

## Repository layout

```
protocol/      # CBOR wire format spec + test vectors
firmware/      # Device OS (C/C++, ESP-IDF)
sdk/python/    # App SDK (Python, primary)
sdk/js/        # App SDK (JS/TS)
simulator/     # Tauri desktop simulator
marketplace/   # Web registry + API
push-relay/    # Project-operated push notification relay
exit-node/     # LoRa↔internet bridge (Go)
cli/           # pagerctl developer CLI
examples/      # Reference apps (hello, notes, chat, ...)
docs/          # User + developer + spec docs
```

## Status

Pre-alpha. Active development by an agent team coordinated via [Paperclip](https://github.com/paperclipai). Direct-push-to-main with conformance-test gates.

## License

TBD.
