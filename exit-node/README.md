# exit-node

PagerOS LoRa ↔ HTTPS bridge. Reference implementation in Go (SPEC §11).

## Status

EXIT-001 (skeleton + config). Subsequent tasks layer in:

- EXIT-002 — LoRa RX/TX loop
- EXIT-003 — HTTPS proxy
- EXIT-004 — Per-device rate limiter
- EXIT-005 — Advertise packets
- EXIT-006 — Response caching
- EXIT-007 — Stats publisher
- EXIT-008 — Pi image + Docker + .deb

## Build & run

```sh
cd exit-node
go build ./...
./exit-node --config configs/exit-node.example.yaml
```

Flags:

| Flag | Default | Purpose |
|---|---|---|
| `--config` | `/etc/pager-ecosystem/exit-node.yaml` | Path to YAML config |
| `--status` | `false` | Open the device, print status, exit |
| `--version` | `false` | Print version and exit |

## Test

```sh
cd exit-node
go test ./...
```

## Layout

```
exit-node/
├── cmd/exit-node/        # binary entrypoint
├── configs/              # example YAML config
└── internal/
    ├── config/           # YAML loader + validation
    └── lora/             # envelope codec (LORA-001) + serial device wrapper
```
