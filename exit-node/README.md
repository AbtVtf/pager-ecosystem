# exit-node

PagerOS LoRa ↔ HTTPS bridge. Reference implementation in Go (SPEC §11).

## Status

- EXIT-001 — skeleton + config ✅
- EXIT-002 — LoRa RX/TX loop ✅ (library in `internal/lora`; not yet
  wired in `cmd/exit-node` because no production `PacketTransceiver`
  framing exists for the USB modem)
- EXIT-003 — HTTPS proxy ✅ (`internal/proxy`; implements `lora.Handler`)
- EXIT-004 — Per-device rate limiter ✅ (`internal/ratelimit`)
- EXIT-005 — Advertise packets ✅ (wired in `cmd/exit-node`)
- EXIT-006 — Response caching
- EXIT-007 — Stats publisher ✅ (`internal/stats`; opt-in dashboard heartbeat)
- EXIT-008 — Packaging (Docker + .deb + Pi image instructions) ✅ — see [`packaging/README.md`](packaging/README.md)

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
├── packaging/            # EXIT-008: Dockerfile, .deb builder, systemd unit
└── internal/
    ├── config/           # YAML loader + validation
    ├── lora/             # envelope codec, fragmentation, inner envelope,
    │                     # RX/TX loop, advertiser, tracker
    ├── proxy/            # EXIT-003 HTTPS forwarder (lora.Handler impl)
    ├── ratelimit/        # EXIT-004 per-device-pubkey token bucket
    └── stats/            # EXIT-007 anonymized dashboard publisher
```

## Packaging

See [`packaging/README.md`](packaging/README.md) for Docker, `.deb`, and
Raspberry Pi install paths.
