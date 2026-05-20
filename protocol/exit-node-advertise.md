# Exit Node Advertise (LORA-005)

Cross-implementation specification of the `exit-node-advertise` payload
carried inside the LoRa envelope type `0x04` (SPEC.md §6.2.4).

Both the exit-node (Go) and the firmware client (C) must encode/decode
this exact byte layout. The Go reference lives in
`exit-node/internal/lora/advert.go`.

## 1. Purpose

Exit Nodes periodically broadcast their presence so nearby devices can
discover them without prior configuration. Devices keep a ranked list of
known Exit Nodes and select the best one for outbound LoRa requests
(SPEC.md §6.2.4, §6.3).

## 2. Cadence

- Each Exit Node broadcasts one advert every **30 s** (default).
- The interval is configurable but MUST stay between 10 s and 300 s.
- Adverts are sent as a LoRa envelope with `type=0x04`, fresh random
  `msg_id`, no ACK expected.

## 3. Wire format

The advert payload sits inside the LoRa envelope `payload` field. It is
intentionally a tiny fixed binary layout (not CBOR): adverts are sent
constantly and the payload size matters on a 250 B/s link.

```
 0       1                                33         34         35
 ┌───────┬─────────────────────────────────┬──────────┬──────────┐
 │ ver=1 │      pubkey (32B Ed25519)       │ bw_class │   load   │
 │  1B   │                                 │   1B     │   1B     │
 └───────┴─────────────────────────────────┴──────────┴──────────┘
```

Total: **35 bytes**.

### Fields

| Field      | Size | Description                                                                       |
|------------|------|-----------------------------------------------------------------------------------|
| `ver`      | 1 B  | Advert format version. Currently `0x01`. Decoders MUST reject other values.       |
| `pubkey`   | 32 B | Exit Node's long-term Ed25519 public identity key (raw, no encoding).             |
| `bw_class` | 1 B  | Internet bandwidth class (see table below).                                       |
| `load`     | 1 B  | Current load, 0 = idle, 255 = saturated. Reported by the Exit Node.               |

### Bandwidth class

| Value | Meaning                                                       |
|-------|---------------------------------------------------------------|
| `0`   | unknown / unspecified                                         |
| `1`   | low — dialup / cellular fallback (< 1 Mbps)                   |
| `2`   | medium — typical cellular / DSL (1–25 Mbps)                   |
| `3`   | high — broadband cable / VDSL (25–250 Mbps)                   |
| `4`   | gigabit / fiber (≥ 250 Mbps)                                  |

Unknown values MUST be treated as `0` (unknown) by ranking logic.

### Forward compatibility

Decoders MUST accept payloads **longer** than 35 bytes and ignore the
trailing bytes — future versions may append fields. Decoders MUST reject
payloads shorter than 35 bytes.

## 4. Ranking

Devices keep a table keyed by `pubkey` of the most recently observed
advert from each Exit Node. The table entry stores:

- `pubkey`, `bw_class`, `load` (from the advert)
- `rssi` (from the radio layer at receive time, signed integer, dBm)
- `last_seen` (local monotonic time of last advert)

### Selection order

When ranking candidates, lower is better:

1. **Load** — prefer the node with the lower `load` byte.
2. **RSSI** — tie-break with the stronger (less-negative) RSSI.
3. **Bandwidth class** — tie-break with the higher `bw_class`.

Entries with `last_seen` older than `5 × interval` (default 150 s) MUST
be dropped from the ranking.

## 5. Conformance

A reference test in `exit-node/internal/lora/tracker_test.go` simulates
three Exit Nodes with different (load, RSSI) tuples and asserts the
selection order matches §4. Any implementation (firmware, future SDKs)
MUST reproduce that order.
